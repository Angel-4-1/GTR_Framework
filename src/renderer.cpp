#include "renderer.h"

#include "camera.h"
#include "shader.h"
#include "mesh.h"
#include "texture.h"
#include "prefab.h"
#include "material.h"
#include "utils.h"
#include "scene.h"
#include "extra/hdre.h"
#include "application.h"
#include "sphericalharmonics.h"

#include <algorithm>

using namespace GTR;

Renderer::Renderer() {
	lights = Scene::instance->lights;
	irr = Scene::instance->irr;
	irr->placeProbes();
	reflection_entity = Scene::instance->reflection;
	reflection_entity->placeProbes();
	render_mode = eRenderMode::SHOW_SHADOWMAP;
	render_deferred_mode = eRenderDeferredMode::DEFERRED_SHADOWMAP;
	pipeline_mode = ePipelineMode::DEFERRED;
	renderer_cond = eRendererCondition::REND_COND_NONE;
	post_fx = ePostFX::FX_MOTION_BLUR;
	color_buffer = new Texture(Application::instance->window_width, Application::instance->window_height, GL_RGB, GL_HALF_FLOAT);
	quality = eQuality::LOW;
	fbo.create(1024, 1024);
	ssao_fbo.create(Application::instance->window_width, Application::instance->window_height, 1, GL_RGBA, GL_UNSIGNED_BYTE, false);
	light_camera = 0;
	//limited to 4 lights
	shadow_singlepass.create(4 * 512, 512);
	ao_buffer = NULL;
	probes_texture = NULL;
	tone_mapper.init();
	
	irr_fbo = NULL;
}

void Renderer::renderToFBO(GTR::Scene* scene, Camera* camera)
{
	if ( render_mode == SHOW_SHADOWMAP && pipeline_mode == FORWARD )
	{
		//create the shadow maps for each light
		createShadowMaps(scene,camera);

		fbo.bind();
		renderScene(scene, camera);
		fbo.unbind();

		Shader* shader = Shader::Get("depth");
		if (!shader)
		{
			//color_buffer->toViewport();
			fbo.color_textures[0]->toViewport();
			return;
		}
		fbo.color_textures[0]->toViewport();
		
		if (show_depth_camera)
		{
			if(light_camera < lights.size())
				lights[light_camera]->renderShadowFBO(shader);
		}
	}
	else {
		if (render_deferred_mode == DEFERRED_SHADOWMAP && pipeline_mode == DEFERRED)
		{
			//create the shadow maps for each light
			createShadowMapsUsingForward(scene, camera);
		}
		renderScene(scene, camera);
	}

	//render light meshes if anyone is set to visible
	renderLights(camera);

	if(!freeze_prev_vp)
		vp_previous = camera->viewprojection_matrix;
}

void Renderer::renderScene(GTR::Scene* scene, Camera* camera)
{
	createRenderCalls(scene,camera);

	if (pipeline_mode == FORWARD)
		renderForward(scene, render_calls, camera);
	else if (pipeline_mode == DEFERRED)
		renderDeferred(scene, render_calls, camera);		
}

// Calculate distance between two 3D points using the pythagoras theorem
float distance(float x1, float y1, float z1, float x2, float y2, float z2)
{
	return sqrt(pow(x2 - x1, 2.0) + pow(y2 - y1, 2.0) + pow(z2 - z1, 2.0));
}

struct compareDistanceToCamera {
	compareDistanceToCamera(){}

	bool operator ()(renderCall rc1, renderCall rc2) const {
		return (rc1.distance_to_camera > rc2.distance_to_camera);
	}
};

struct compareAlpha {

	compareAlpha() {}

	bool operator ()(renderCall rc1, renderCall rc2) const {
		return (!rc1.isAlpha && rc2.isAlpha);
	}
};

void Renderer::createRenderCalls(GTR::Scene* scene, Camera* camera) {
	// prepre the vector
	render_calls.clear();
	bool isAlpha = false;

	for (int i = 0; i < scene->entities.size(); ++i)
	{
		BaseEntity* ent = scene->entities[i];
		if (!ent->visible)
			continue;
		
		isAlpha = false;
		
		if (ent->entity_type == PREFAB)
		{
			PrefabEntity* pent = (GTR::PrefabEntity*)ent;
			if (pent->prefab)
			{
				//create the render calls
				prefabToNode(ent->model, pent->prefab, camera, pent->nearest_reflection_probe);
			}
		}
	}

	// sort render calls
	if (camera) {
		std::sort(render_calls.begin(), render_calls.end(), compareDistanceToCamera());
		std::sort(render_calls.begin(), render_calls.end(), compareAlpha());
	}
}

float Renderer::computeDistanceToCamera(Matrix44 node_model, Mesh* mesh, Vector3 cam_pos) {
	BoundingBox world_bounding = transformBoundingBox(node_model, mesh->box);
	Vector3 center = world_bounding.center;

	return distance(center.x, center.y, center.z, cam_pos.x, cam_pos.y, cam_pos.z);;
}

//renders all the prefab
void Renderer::prefabToNode(const Matrix44& model, GTR::Prefab* prefab, Camera* camera, sReflectionProbe* _nearest_reflection_probe)
{
	assert(prefab && "PREFAB IS NULL");
	//assign the model to the root node
	nodeToRenderCall(model, &prefab->root, camera, _nearest_reflection_probe);
}

//renders a node of the prefab and its children
void Renderer::nodeToRenderCall(const Matrix44& prefab_model, GTR::Node* node, Camera* camera, sReflectionProbe* _nearest_reflection_probe)
{
	if (!node->visible)
		return;

	//compute global matrix
	Matrix44 node_model = node->getGlobalMatrix(true) * prefab_model;

	//does this node have a mesh? then we must render it
	if (node->mesh && node->material)
	{
		//compute the bounding box of the object in world space (by using the mesh bounding box transformed to world space)
		BoundingBox world_bounding = transformBoundingBox(node_model,node->mesh->box);
		
		//if bounding box is inside the camera frustum then the object is probably visible
		if (!camera || camera->testBoxInFrustum(world_bounding.center, world_bounding.halfsize) )
		{
			//create render call
			renderCall rc;
			rc.set(node->mesh, node->material, node_model);
			rc.nearest_reflection_probe = _nearest_reflection_probe;
			if(camera)
				rc.distance_to_camera = computeDistanceToCamera(node_model, node->mesh, camera->eye);
			render_calls.push_back(rc);
		}
	}

	//iterate recursively with children
	for (int i = 0; i < node->children.size(); ++i)
		nodeToRenderCall(prefab_model, node->children[i], camera, _nearest_reflection_probe);
}

void Renderer::renderForward(GTR::Scene* scene, std::vector< renderCall >& data, Camera* camera, ePipelineMode pipeline, eRenderMode mode)
{
	//set the clear color (the background color)
	glClearColor(scene->background_color.x, scene->background_color.y, scene->background_color.z, 1.0);

	// Clear the color and the depth buffer
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	checkGLErrors();

	//render sky
	if (scene->environment && apply_skybox)
		renderSkybox(scene->environment, camera);

	for (int i = 0; i < data.size(); i++)
	{
		renderCall& rc = data[i];
		if ((renderer_cond == REND_COND_NO_ALPHA && !rc.isAlpha) || renderer_cond == REND_COND_NONE)
			renderMeshWithMaterial(rc.model, rc.mesh, rc.material, camera, NULL, pipeline, mode, rc.nearest_reflection_probe);
		else if (renderer_cond == REND_COND_ALPHA && rc.isAlpha)
			renderMeshWithMaterial(rc.model, rc.mesh, rc.material, camera, NULL, pipeline, mode, rc.nearest_reflection_probe);
	}
}

void Renderer::renderDeferred(GTR::Scene* scene, std::vector< renderCall >& data, Camera* camera)
{
	//create buffers
	if (gbuffers_fbo.fbo_id == 0)
	{
		//careful when resizing the window
		gbuffers_fbo.create(Application::instance->window_width, 
							Application::instance->window_height,
							3,	//num textures
							GL_RGBA,
							GL_FLOAT);	//precision

		decals_fbo.create(Application::instance->window_width,
			Application::instance->window_height,
			3,	//num textures
			GL_RGBA,
			GL_FLOAT);	//precision
	}

	if (illumination_fbo.fbo_id == 0)
	{
		illumination_fbo.create(Application::instance->window_width, Application::instance->window_height,
			1, 			//three textures
			GL_RGB, 		//three channels
			GL_FLOAT, //1 byte
			true);	//depth texture
	}

	gbuffers_fbo.bind();
	glClearColor(scene->background_color.x, scene->background_color.y, scene->background_color.z, 1.0);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	
	//render sky
	if (scene->environment && apply_skybox)
		renderSkybox(scene->environment, camera);

	checkGLErrors();

	for (int i = 0; i < data.size(); i++)
	{
		renderCall& rc = data[i];
		renderMeshWithMaterial(rc.model, rc.mesh, rc.material, camera, NULL, NO_PIPELINE, SHOW_NONE, rc.nearest_reflection_probe);
	}

	gbuffers_fbo.unbind();

	//--------------------
	gbuffers_fbo.color_textures[0]->copyTo(decals_fbo.color_textures[0]);
	gbuffers_fbo.color_textures[1]->copyTo(decals_fbo.color_textures[1]);
	gbuffers_fbo.color_textures[2]->copyTo(decals_fbo.color_textures[2]);
	decals_fbo.bind();
	gbuffers_fbo.depth_texture->copyTo(NULL);
	renderDecals(scene, camera);
	decals_fbo.unbind();
	decals_fbo.color_textures[0]->copyTo(gbuffers_fbo.color_textures[0]);
	decals_fbo.color_textures[1]->copyTo(gbuffers_fbo.color_textures[1]);
	decals_fbo.color_textures[2]->copyTo(gbuffers_fbo.color_textures[2]);
	//--------------------

	//compute SSAO
	if (!ao_buffer)
	{
		ao_buffer = new Texture(Application::instance->window_width * 0.5, Application::instance->window_height * 0.5, GL_LUMINANCE, GL_UNSIGNED_BYTE);
		blur_ao_buffer = new Texture(Application::instance->window_width * 0.5, Application::instance->window_height * 0.5, GL_LUMINANCE, GL_UNSIGNED_BYTE);
	}
	ssao.apply(gbuffers_fbo.depth_texture, gbuffers_fbo.color_textures[1], camera, ao_buffer);

	//apply illumination	
	glClearColor(scene->background_color.x, scene->background_color.y, scene->background_color.z, 1.0);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	glDisable(GL_BLEND);
	glDisable(GL_DEPTH_TEST);
	
	if (show_gbuffers)
	{
		renderGBuffers(camera);
	}
	else if (show_ao && ao_buffer) {
		ao_buffer->toViewport();
	}
	else if (show_irradiance_coeffs && probes_texture != NULL) {
	//	probes_texture->toViewport();
		int w = Application::instance->window_width;
		int h = Application::instance->window_height;
		Shader* irr_shader = Shader::Get("irradiance");
		irr_shader->enable();
		irr_shader->setUniform("u_iRes", Vector2(1.0 / (float)w, 1.0 / (float)h));
		irr->uploadToShader(irr_shader);
		Matrix44 inv_vp = camera->viewprojection_matrix;
		inv_vp.inverse();
		irr_shader->setUniform("u_inverse_viewprojection", inv_vp);
		irr_shader->setUniform("u_color_texture", gbuffers_fbo.color_textures[0], 0);
		irr_shader->setUniform("u_normal_texture", gbuffers_fbo.color_textures[1], 1);
		irr_shader->setUniform("u_depth_texture", gbuffers_fbo.depth_texture, 2);
		irr_shader->setUniform("u_probes_texture", probes_texture, 3);
		glDisable(GL_DEPTH_TEST);
		glDisable(GL_BLEND);
		Mesh* quad = Mesh::getQuad();
		quad->render(GL_TRIANGLES);
		irr_shader->disable();
	}
	else {
		illumination_fbo.bind();
		//copy the gbuffers depth buffer to the binded depth buffer in the FBO
		gbuffers_fbo.depth_texture->copyTo(NULL);
		glClearColor(scene->background_color.x, scene->background_color.y, scene->background_color.z, 1.0);
		glClear(GL_COLOR_BUFFER_BIT);
		glDisable(GL_DEPTH_TEST);
		renderReconstructedScene(scene, camera);
		glEnable(GL_DEPTH_TEST);
		
		if (irr && apply_irradiance && show_probes)
			irr->render(NULL, camera);

		if (use_reflection && show_reflection_probes)
			reflection_entity->render(camera);
		
		if(!use_dithering)
			renderAlphaElements(data, camera);

		renderVolumetricLights(scene, camera);

		illumination_fbo.unbind();


		if (!linear_correction) {
			illumination_fbo.color_textures[0]->toViewport();
		}
		else {
			//Gamma correction
			if (gamma_fbo.fbo_id == 0) {
				gamma_fbo.create(Application::instance->window_width, Application::instance->window_height,
					1, 			//one textures
					GL_RGB, 	//three channels
					GL_FLOAT,
					true);	    //depth texture
			}

			gamma_fbo.bind();
			glClearColor(scene->background_color.x, scene->background_color.y, scene->background_color.z, 1.0);
			glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
			Mesh* quad = Mesh::getQuad();
			Shader* shader;
			if(use_tone_mapper)
				shader = Shader::Get("tone_mapper");
			else
				shader = Shader::Get("gamma");

			shader->enable();
			int w = Application::instance->window_width;
			int h = Application::instance->window_height;
			shader->setTexture("u_texture", illumination_fbo.color_textures[0], 0);
			shader->setUniform("u_iRes", Vector2(1.0 / (float)w, 1.0 / (float)h));
			tone_mapper.uploadToShader(shader);
			quad->render(GL_TRIANGLES);
			glDisable(GL_BLEND);
			shader->disable();
			gamma_fbo.unbind();

			if (apply_post_fx) {
				renderPostFX(camera, gamma_fbo.color_textures[0]);
			}
			else {
				gamma_fbo.color_textures[0]->toViewport();
			}
		}

		if (render_deferred_mode == DEFERRED_SHADOWMAP && show_depth_camera)
		{
			Shader* shader = Shader::Get("depth");
			if (light_camera < lights.size())
				lights[light_camera]->renderShadowFBO(shader);
		}
	}
}

//show each one of the 4 textures stored at gbuffers_fbo
void Renderer::renderGBuffers(Camera* camera)
{
	int w = Application::instance->window_width;
	int h = Application::instance->window_height;

	if (show_gbuffers_alpha) {
		Shader* shader = Shader::Get("gbuffers_alpha");
		shader->enable();
		//inverse window resolution
		shader->setUniform("u_iRes", Vector2(1.0 / (float)w, 1.0 / (float)h));
		shader->setTexture("u_texture", gbuffers_fbo.color_textures[0], 1);
		glViewport(0, h * 0.5, w * 0.5, h * 0.5);
		gbuffers_fbo.color_textures[0]->toViewport(shader);

		shader->setTexture("u_texture", gbuffers_fbo.color_textures[1], 1);
		glViewport(w * 0.5, h * 0.5, w * 0.5, h * 0.5);
		gbuffers_fbo.color_textures[1]->toViewport(shader);

		shader->setTexture("u_texture", gbuffers_fbo.color_textures[2], 1);
		glViewport(0, 0, w * 0.5, h * 0.5);
		gbuffers_fbo.color_textures[2]->toViewport(shader);

		shader->disable();
	}
	else {
		glViewport(0, h * 0.5, w * 0.5, h * 0.5);
		gbuffers_fbo.color_textures[0]->toViewport();

		glViewport(w * 0.5, h * 0.5, w * 0.5, h * 0.5);
		gbuffers_fbo.color_textures[1]->toViewport();

		glViewport(0, 0, w * 0.5, h * 0.5);
		gbuffers_fbo.color_textures[2]->toViewport();
	}	

	Shader* depth_shader = Shader::Get("depth");
	depth_shader->enable();
	depth_shader->setUniform("u_camera_nearfar", Vector2(camera->near_plane, camera->far_plane));
	glViewport(w * 0.5, 0, w * 0.5, h * 0.5);
	gbuffers_fbo.depth_texture->toViewport(depth_shader);
	depth_shader->disable();

	glViewport(0, 0, w, h);
}

void Renderer::uploadDefferedUniforms(Shader* shader, GTR::Scene* scene, Camera* camera)
{
	int w = Application::instance->window_width;
	int h = Application::instance->window_height;

	Matrix44 inv_viewproj = camera->viewprojection_matrix;
	inv_viewproj.inverse();

	//gbuffers textures
	shader->setTexture("u_color_texture", gbuffers_fbo.color_textures[0], 0);
	shader->setTexture("u_normal_texture", gbuffers_fbo.color_textures[1], 1);
	shader->setTexture("u_extra_texture", gbuffers_fbo.color_textures[2], 2);
	shader->setTexture("u_depth_texture", gbuffers_fbo.depth_texture, 3);

	bool has_environment = false;
	if (scene->environment)
	{
		shader->setTexture("u_environment_texture", scene->environment, 9);
		has_environment = true;
	}

	shader->setUniform("u_has_environment", has_environment);
	shader->setUniform("u_camera_position", camera->eye);
	shader->setUniform("u_ambient_light", scene->ambient_light);
	shader->setUniform("u_viewprojection", camera->viewprojection_matrix);
	//inverse projection of the camera to reconstruct world pos
	shader->setUniform("u_inverse_viewprojection", inv_viewproj);
	//inverse window resolution
	shader->setUniform("u_iRes", Vector2(1.0 / (float)w, 1.0 / (float)h));
	shader->setUniform("u_linear_correction", linear_correction);
	shader->setUniform("u_gamma", tone_mapper.gamma);

	//render shadows?
	bool shadows = false;
	if (render_deferred_mode == DEFERRED_SHADOWMAP)
	{
		shadows = true;
	}
	shader->setUniform("u_render_shadows", shadows);

	bool has_ao = false;
	if (ao_buffer)
	{
		has_ao = true;
		shader->setTexture("u_ssao_texture", ao_buffer, 4);
	}
	shader->setUniform("u_has_ssao", has_ao);

}

void Renderer::renderReconstructedScene(GTR::Scene* scene, Camera* camera)
{
	bool shadows = false;
	if (render_deferred_mode == DEFERRED_SHADOWMAP)
	{
		shadows = true;
	}

	/**Render directional light using a quad**/
	Mesh* quad = Mesh::getQuad();
	Shader* shader = Shader::Get("deferred");
	shader->enable();
	uploadDefferedUniforms(shader, scene, camera);
	shader->setUniform("u_is_emissor", true);

	if (irr && apply_irradiance && probes_texture != NULL && use_irradiance)
	{
		shader->setUniform("u_apply_irradiance", true);
		shader->setTexture("u_probes_texture", probes_texture, 4);
		irr->uploadToShader(shader);
	}
	else {
		shader->setUniform("u_apply_irradiance", false);
	}

	//Multipass
	for (int i = 0; i < lights.size(); i++)
	{
		LightEntity* light = lights[i];
		//check if light is inside the camera frustum
		int inside = (int)camera->testSphereInFrustum(light->model.getTranslation(), light->max_distance);
		if (inside == 0 || light->light_type != DIRECTIONAL)
			continue;

		float prev_intensity = light->intensity;
		if (linear_correction) {
			light->intensity = 6 * prev_intensity;
		}
		light->uploadToShader(shader, shadows);

		quad->render(GL_TRIANGLES);
		//restore intensity
		if (linear_correction) {
			light->intensity = prev_intensity;
		}
	}
	glDisable(GL_BLEND);
	shader->disable();

	/**Render point and spot lights using spheres**/
	Mesh* sphere = Mesh::Get("data/meshes/sphere.obj", true);
	shader = Shader::Get("deferred_ws");
	shader->enable();
	uploadDefferedUniforms(shader, scene, camera);
	
	glEnable(GL_CULL_FACE);
	//render only the backfacing triangles of the sphere
	glFrontFace(GL_CW);

	//no add ambient light --> was added by the directional light
	shader->setUniform("u_ambient_light", Vector3());
	//no add emissive again
	shader->setUniform("u_is_emissor", false);
	shader->setUniform("u_has_environment", false);

	glEnable(GL_BLEND);
	glBlendFunc(GL_ONE, GL_ONE);

	if (irr && apply_irradiance && probes_texture != NULL && use_irradiance)
	{
		shader->setUniform("u_apply_irradiance", true);
		//shader->setUniform("u_apply_irradiance", false);
		shader->setTexture("u_probes_texture", probes_texture, 4);
		irr->uploadToShader(shader);
	}
	else {
		shader->setUniform("u_apply_irradiance", false);
	}

	//Multipass
	for (int i = 0; i < lights.size(); i++)
	{
		LightEntity* light = lights[i];
		//check if light is inside the camera frustum
		int inside = (int)camera->testSphereInFrustum(light->model.getTranslation(), light->max_distance);
		if (inside == 0 || light->light_type == DIRECTIONAL)
			continue;

		float prev_intensity = light->intensity;
		if (linear_correction) {
			light->intensity = 5 * prev_intensity;
		}
		light->uploadToShader(shader, shadows);
		Vector3 lpos = light->model.getTranslation();
		Matrix44 m;
		m.setTranslation(lpos.x, lpos.y, lpos.z);
		float dist = light->max_distance;
		m.scale(dist, dist, dist);
		shader->setUniform("u_model", m);
		
		sphere->render(GL_TRIANGLES);
		//restore intensity
		if (linear_correction) {
			light->intensity = prev_intensity;
		}
	}

	//restore it
	glFrontFace(GL_CCW);
	glDisable(GL_BLEND);
	shader->disable();
}

void Renderer::renderVolumetricLights(GTR::Scene* scene, Camera* camera) {
	glEnable(GL_BLEND);
	//	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE);
	glDisable(GL_DEPTH_TEST);
	int w = illumination_fbo.width;
	int h = illumination_fbo.height;
	Matrix44 inv_vp = camera->viewprojection_matrix;
	inv_vp.inverse();

	Shader* shader;
	/**Render directional light using a quad**/
	shader = Shader::Get("volume_direct");
	Mesh* quad = Mesh::getQuad();
	shader->enable();
	shader->setUniform("u_inverse_viewprojection", inv_vp);
	shader->setUniform("u_camera_position", camera->eye);
	shader->setTexture("u_depth_texture", gbuffers_fbo.depth_texture, 9);
	shader->setUniform("u_iRes", Vector2(1.0 / (float)w, 1.0 / (float)h));
	for (int i = 0; i < lights.size(); i++)
	{
		LightEntity* light = lights[i];
		if (!light->is_volumetric)
			continue;
		//check if light is inside the camera frustum
		int inside = (int)camera->testSphereInFrustum(light->model.getTranslation(), light->max_distance);
		if (inside == 0 || light->light_type != DIRECTIONAL)
			continue;

		float prev_intensity = light->intensity;
		if (linear_correction) {
			light->intensity = 6 * prev_intensity;
		}
		light->uploadToShader(shader, true);

		quad->render(GL_TRIANGLES);
		//restore intensity
		if (linear_correction) {
			light->intensity = prev_intensity;
		}
	}
	shader->disable();

	/**Render point and spot lights using spheres**/
	Mesh* sphere = Mesh::Get("data/meshes/sphere.obj", false);
	shader = Shader::Get("volume_direct_ws");
	shader->enable();
	shader->setUniform("u_inverse_viewprojection", inv_vp);
	shader->setUniform("u_camera_position", camera->eye);
	shader->setUniform("u_viewprojection", camera->viewprojection_matrix);
	shader->setUniform("u_color", Vector4(1.0, 1.0, 1.0, 1.0));
	shader->setTexture("u_depth_texture", gbuffers_fbo.depth_texture, 9);
	shader->setUniform("u_iRes", Vector2(1.0 / (float)w, 1.0 / (float)h));

	//Multipass
	for (int i = 0; i < lights.size(); i++)
	{
		LightEntity* light = lights[i];
		if (!light->is_volumetric)
			continue;

		//check if light is inside the camera frustum
		int inside = (int)camera->testSphereInFrustum(light->model.getTranslation(), light->max_distance);
		if (inside == 0 || light->light_type == DIRECTIONAL)
			continue;

		float prev_intensity = light->intensity;
		if (linear_correction) {
			light->intensity = 5 * prev_intensity;
		}
		light->uploadToShader(shader, true);
		Vector3 lpos = light->model.getTranslation();
		Matrix44 m;
		m.setTranslation(lpos.x, lpos.y, lpos.z);
		float dist = light->max_distance;
		m.scale(dist, dist, dist);
		shader->setUniform("u_model", m);

		sphere->render(GL_TRIANGLES);
		//restore intensity
		if (linear_correction) {
			light->intensity = prev_intensity;
		}
	}
	shader->disable();
	glDisable(GL_BLEND);
}

//renders a mesh given its transform and material
void Renderer::renderMeshWithMaterial(const Matrix44 model, Mesh* mesh, GTR::Material* material, Camera* camera, Shader* sh, ePipelineMode pipeline, eRenderMode mode, sReflectionProbe* _nearest_reflection_probe)
{
	//in case there is nothing to do
	if (!mesh || !mesh->getNumVertices() || !material )
		return;
    assert(glGetError() == GL_NO_ERROR);

	//define locals to simplify coding
	GTR::Scene* scene = GTR::Scene::instance;
	
	//select the blending
	if (material->alpha_mode == GTR::eAlphaMode::BLEND)
	{
		glEnable(GL_BLEND);
		glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	}
	else
		glDisable(GL_BLEND);

	//select if render both sides of the triangles
	if(material->two_sided)
		glDisable(GL_CULL_FACE);
	else
		glEnable(GL_CULL_FACE);
    assert(glGetError() == GL_NO_ERROR);

	ePipelineMode _pipeline_mode = (pipeline == NO_PIPELINE) ? pipeline_mode : pipeline;
	eRenderMode rending_mode = (mode == SHOW_NONE) ? render_mode : mode;

	//choose a shader
	Shader* shader;
	if (sh == NULL)
		shader = getShader(_pipeline_mode, rending_mode);
	else
		shader = sh;

    assert(glGetError() == GL_NO_ERROR);

	//no shader? then nothing to render
	if (!shader)
		return;
	shader->enable();

	if (_pipeline_mode == DEFERRED)
	{			
		if (material->alpha_mode == GTR::eAlphaMode::BLEND)
		{
			if (use_dithering) {
				shader->setUniform("u_apply_dithering", true);
			} 
			else {
				shader->disable();
				glDisable(GL_BLEND);
				return;
			}
		}
		else {
			shader->setUniform("u_apply_dithering", false);
		}
	}

	//upload uniforms
	shader->setUniform("u_viewprojection", camera->viewprojection_matrix);
	shader->setUniform("u_camera_position", camera->eye);
	shader->setUniform("u_model", model );

	shader->setUniform("u_ambient_light", scene->ambient_light);

	int type_property = 0;	//occlusion
	if (render_mode == SHOW_METALLIC)
		type_property = 1;
	else if (render_mode == SHOW_ROUGHNESS)
		type_property = 2;
	shader->setUniform("u_type_property", type_property);
	bool apply_linear_correction = (linear_correction && pipeline_mode == DEFERRED);
	shader->setUniform("u_linear_correction", apply_linear_correction );
	shader->setUniform("u_gamma", tone_mapper.gamma);

	if(scene->environment)
		shader->setTexture("u_environment_texture", scene->environment, 9);

	//upload material properties to the shader
	material->uploadToShader(shader, apply_linear_correction, tone_mapper.gamma);

	//ssao
	/*bool has_ao = false;
	if (ao_buffer)
	{
		has_ao = true;
		shader->setTexture("u_ssao_texture", ao_buffer, 4);
	}
	shader->setUniform("u_has_ssao", has_ao);*/

	if (_pipeline_mode == FORWARD)
	{
		if (rending_mode == SHOW_MULTIPASS)
		{
			//Multi Pass
			renderMultiPass(shader, mesh);
		}
		else if (rending_mode == SHOW_SINGLEPASS)
		{
			//Single Pass
			renderSinglePass(shader, mesh);
		}
		else if (rending_mode == SHOW_SHADOWMAP) {
			//Lights with shadows
			if (rendering_shadowmap) {
				//do not care about objects with transparency when creating the shadowmap
				if (material->alpha_mode != GTR::eAlphaMode::BLEND) {
					mesh->render(GL_TRIANGLES);
				}
			}
			else {
				//Multi pass with shadows
				renderMultiPass(shader, mesh, true, _nearest_reflection_probe);
			}
		}
		else {	//no lights
			mesh->render(GL_TRIANGLES);
		}
	}
	//Deferred
	else {
		if (_nearest_reflection_probe != NULL && _nearest_reflection_probe->cubemap != NULL && use_reflection) {
			shader->setUniform("u_last_pass", true);
			shader->setTexture("u_reflection_texture", _nearest_reflection_probe->cubemap, 7);
		}
		else {
			shader->setUniform("u_last_pass", false);
		}
		mesh->render(GL_TRIANGLES);
	}
	
	
	//disable shader
	shader->disable();

	//set the render state as it was before to avoid problems with future renders
	glDisable(GL_BLEND);

	//render bounding box?
	if (isRenderingBoundingBox) {
		mesh->renderBounding(model, true);
	}
}

void GTR::Renderer::renderMultiPass(Shader* shader, Mesh* mesh, bool sendShadowMap, sReflectionProbe* _nearest_reflection_probe)
{
	for (int i = 0; i < lights.size(); i++)
	{
		LightEntity* light = lights[i];
		if (light == nullptr)
			continue;

		//last one -->`apply reflection probes texture
		if (i == (lights.size() - 1) && _nearest_reflection_probe != NULL)
		{
			shader->setUniform("u_last_pass", true);
			shader->setTexture("u_reflection_texture", _nearest_reflection_probe->cubemap, 7);
		}
		else {
			shader->setUniform("u_last_pass", false);
		}

		//first pass doesn't use blending
		if (i == 0) {
			//	glDisable(GL_BLEND);	//if disabled the alpha elements wont be translucid
		}
		else {
			//allow to render pixels that have the same depth as the one in the depth buffer
			glDepthFunc(GL_LEQUAL);
			//set blending mode to additive
			glBlendFunc(GL_SRC_ALPHA, GL_ONE);
			glEnable(GL_BLEND);
			//do not apply emissive texture again
			shader->setUniform("u_is_emissor", false);
			//no add ambient light
			shader->setUniform("u_ambient_light", Vector3());
		}

		//pass light to shader
		light->uploadToShader(shader, sendShadowMap);

		//do the draw call that renders the mesh into the screen
		mesh->render(GL_TRIANGLES);
	}
	glDepthFunc(GL_LESS);
}

void GTR::Renderer::renderSinglePass(Shader* shader, Mesh* mesh)
{
	//collect info about all the lights of the scene
	Vector3 light_position[10];
	Vector3 light_color[10];
	Vector3 light_vector[10];
	int light_type[10];
	float light_intensity[10];
	float light_max_distance[10];
	Vector2 light_spot_vars[10];	// x = cut off angle  |  y = spot exponent

	int num_lights = 5; // lights.size();

	for (int i = 0; i < num_lights; i++)
	{
		LightEntity* light = lights[i];
		light_position[i] = light->model.getTranslation();
		light_color[i] = light->color;
		light_intensity[i] = light->intensity;
		light_max_distance[i] = light->max_distance;
		light_type[i] = (int)light->light_type;
		light_vector[i] = light->model.frontVector();
		light_spot_vars[i] = Vector2(cos((light->cone_angle / 180.0) * PI), light->spot_exponent);
	}
	
	//pass it to the shader
	shader->setUniform3Array("u_light_pos", (float*)&light_position, num_lights);
	shader->setUniform3Array("u_light_color", (float*)&light_color, num_lights);
	shader->setUniform3Array("u_light_vector", (float*)&light_vector, num_lights);
	shader->setUniform1Array("u_light_type", (int*)&light_type, num_lights);
	shader->setUniform1Array("u_light_intensity", (float*)&light_intensity, num_lights);
	shader->setUniform1Array("u_light_max_distance", (float*)&light_max_distance, num_lights);
	shader->setUniform2Array("u_light_spot_vars", (float*)&light_spot_vars, num_lights);
	shader->setUniform("u_num_lights", num_lights);

	mesh->render(GL_TRIANGLES);
}

void GTR::Renderer::renderAlphaElements(std::vector< renderCall >& data, Camera* camera) {
	
	Shader* sh = Shader::Get("light");
	for (int i = 0; i < data.size(); i++)
	{
		renderCall& rc = data[i];
		if (rc.isAlpha)
			renderMeshWithMaterial(rc.model, rc.mesh, rc.material, camera, sh, FORWARD, SHOW_SHADOWMAP);
	}
}

void GTR::Renderer::renderLights(Camera* camera)
{
	for (int i = 0; i < lights.size(); i++)
	{
		if (lights[i]->render_light)
			lights[i]->renderLight(camera);
	}
}

void GTR::Renderer::changeQualityFBO()
{
	fbo.freeTextures();
	int fbo_size = 1024;
	switch (quality) {
	case MEDIUM:
		fbo_size = 2048;
		break;
	case HIGH:
		fbo_size = 3072;
		break;
	case ULTRA:
		fbo_size = 4096;
		break;
	default:
		break;
	}
	fbo.create(fbo_size, fbo_size);
	

	for (int i = 0; i < lights.size(); i++)
	{
		lights[i]->shadow_fbo->freeTextures();
		lights[i]->shadow_fbo->setDepthOnly(fbo_size, fbo_size);
	}
}

//create the shadowmap of each light in the scene
void GTR::Renderer::createShadowMaps(Scene* scene, Camera* camera)
{
	for (int i = 0; i < lights.size(); i++)
	{
		if (!lights[i]->cast_shadow)
			continue;
		
		LightEntity* light = lights[i];

		//check if light is inside the camera frustum
		int inside = (int)camera->testSphereInFrustum(light->model.getTranslation(), light->max_distance);
		if (inside == 0)
			continue;

		rendering_shadowmap = true;
		light->shadow_fbo->bind();
		//disable writing to the color buffer
		glColorMask(false, false, false, false);
		//clear the depth buffer only (don't care of color)
		glClear(GL_DEPTH_BUFFER_BIT);
		light->updateCamera();
		
		renderScene(scene, light->camera);

		//disable it to render back to the screen
		light->shadow_fbo->unbind();
		//allow to render back to the color buffer
		glColorMask(true, true, true, true);
		rendering_shadowmap = false;
	}
}

//create the shadowmap of each light in the scene
void GTR::Renderer::createShadowMapsUsingForward(Scene* scene, Camera* camera)
{
	ePipelineMode prev = pipeline_mode;
	pipeline_mode = FORWARD;
	eRenderMode rend_prev = render_mode;
	render_mode = SHOW_SHADOWMAP;

	createShadowMaps(scene, camera);
	
	pipeline_mode = prev;
	render_mode = rend_prev;
}

//get the shader based on the variable render_mode
Shader* GTR::Renderer::getShader(ePipelineMode pm, eRenderMode rm)
{
	Shader* shader = NULL;

	switch (rm) {
		case SHOW_MULTIPASS:
			shader = Shader::Get("light");
			break;
		case SHOW_NORMAL:
			shader = Shader::Get("normal");
			break;
		case SHOW_NORMALMAP:
			shader = Shader::Get("normalmap");
			break;
		case SHOW_UVS:
			shader = Shader::Get("uvs");
			break;
		case SHOW_TEXTURE:
			shader = Shader::Get("texture");
			break;
		case SHOW_OCCLUSION:
		case SHOW_METALLIC:
		case SHOW_ROUGHNESS:
			shader = Shader::Get("metallic");
			break;
		case SHOW_SINGLEPASS:
			shader = Shader::Get("singlepass");
			break;
		case SHOW_SHADOWMAP:
			shader = Shader::Get("light");
			break;
		case SHOW_GBUFFERS:
			shader = Shader::Get("gbuffers");
			break;
		default:
			shader = Shader::Get("light");
			break;
	}

	if (pm == DEFERRED)
	{
		shader = Shader::Get("gbuffers");
	}

	return shader;
}

void Renderer::renderInMenu()
{
#ifndef SKIP_IMGUI
	bool changed_fbo = false;
	ImGui::Checkbox("BoundingBox", &isRenderingBoundingBox);
	ImGui::Checkbox("Skybox", &apply_skybox);
	changed_fbo |= ImGui::Combo("Quality", (int*)&quality, "LOW\0MEDIUM\0HIGH\0ULTRA", 4);
	ImGui::Combo("Pipeline Mode", (int*)&pipeline_mode, "FORWARD\0DEFERRED", 2);
	
	if (pipeline_mode == FORWARD)
	{
		ImGui::Combo("Elements", (int*)&renderer_cond, "ALL\0TRANSLUCENTS\0SOLIDS");
		ImGui::Combo("Render Mode", (int*)&render_mode, "MULTIPASS\0TEXTURE\0NORMAL\0NORMALMAP\0UVS\0OCCLUSION\0METALLIC\0ROUGHNESS\0SINGLEPASS\0SHADOWMAP", 7);
		if (render_mode == SHOW_SHADOWMAP)
		{
			ImGui::Checkbox("Show Depth Cameras", &show_depth_camera);
			ImGui::SliderInt("Depth Light Camera", &light_camera, 0, lights.size() - 1);
			if(light_camera < lights.size() && light_camera >= 0)
				ImGui::Text(lights[light_camera]->name.c_str());
		}
	}
	else {
		ImGui::Combo("Render Mode", (int*)&render_deferred_mode, "PBR\0SHADOWMAP", 2);
		if (render_deferred_mode == DEFERRED_SHADOWMAP)
		{
			ImGui::Checkbox("Show Depth Cameras", &show_depth_camera);
			ImGui::SliderInt("Depth Light Camera", &light_camera, 0, lights.size() - 1);
			if (light_camera < lights.size() && light_camera >= 0)
				ImGui::Text(lights[light_camera]->name.c_str());
		}
		ImGui::Checkbox("Use dithering", &use_dithering);
		ImGui::Checkbox("Linear Correction", &linear_correction);
		if (linear_correction)
		{
			ImGui::SliderFloat("Gamma correction", &tone_mapper.gamma, 0.1, 3);
			ImGui::Checkbox("Tone Mapper", &use_tone_mapper);
			if (use_tone_mapper)
			{
				ImGui::SliderFloat("TM Scale", &tone_mapper.scale, 0.1, 5);
				ImGui::SliderFloat("TM White", &tone_mapper.white_lum, 0.1, 5);
				ImGui::SliderFloat("TM Average luminance", &tone_mapper.average_lum, 0.1, 5);
			}
		}
	}
	if (pipeline_mode == DEFERRED)
	{
		ImGui::Checkbox("Show AO", &show_ao);
		ImGui::Checkbox("Show GBuffers", &show_gbuffers);
		if(show_gbuffers)
			ImGui::Checkbox("Show Alpha GBuffers", &show_gbuffers_alpha);
		ImGui::Checkbox("Apply Post Processing Effect", &apply_post_fx);
		if (apply_post_fx) {
			ImGui::Combo("PostFX", (int*)&post_fx, "MOTION-BLUR\0PIXELATED\0BLUR\0DEPTH-OF-FIELD", 4);
			if (post_fx == FX_PIXELATED) {
				bool changed_pixel = false;
				changed_pixel |= ImGui::SliderInt("Pixel size", &pixel_size, 0, 21);
				if (changed_pixel) {
					//needs to be odd
					if (pixel_size % 2 == 0) {
						pixel_size++;
					}
				}
			}
			else if(post_fx == FX_BLUR || post_fx == FX_DEPTH_OF_FIELD) {
				ImGui::SliderInt("Blur size", &blur_size, 0, 30);
			}
		}
	}

	
	//Irradiance
	if (ImGui::TreeNode(irr, "Irradiance")) {
		ImGui::Checkbox("Activate Irradiance", &apply_irradiance);
		ImGui::Checkbox("Use Irradiance", &use_irradiance);
		if (apply_irradiance)
		{
			ImGui::Checkbox("Show Probes", &show_probes);
			if(show_probes)
				irr->renderInMenu();
		}
		bool compute_irr = false;
		compute_irr |= ImGui::Button("Compute Irradiance");
		if (compute_irr) {
			updateIrradianceCache(GTR::Scene::instance);
		}
		ImGui::Checkbox("Show coeffs", &show_irradiance_coeffs);
		bool save_irr = false;
		save_irr |= ImGui::Button("Save Irradiance to disk");
		if (save_irr) {
			GTR::Scene::instance->saveIrradianceToDisk();
		}
		bool read_irr = false;
		read_irr |= ImGui::Button("Read Irradiance from disk");
		if (read_irr) {
			readIrradiance(GTR::Scene::instance);
		}
		ImGui::TreePop();
	}
	
	//Reflection
	ImGui::Checkbox("Use Reflection", &use_reflection);
	if (use_reflection) {
		ImGui::Checkbox("Show Reflection Probes", &show_reflection_probes);
		bool compute_ref = false;
		compute_ref |= ImGui::Button("Compute Reflection");
		bool update_ref_pos = false;
		update_ref_pos |= ImGui::Button("Update Reflection values");
		if (compute_ref) {
			updateReflectionProbes(GTR::Scene::instance);
		}
		else if (update_ref_pos) {
			reflection_entity->placeProbes();
		}
	}

	if (changed_fbo)
		changeQualityFBO();
#endif
}

Texture* GTR::CubemapFromHDRE(const char* filename)
{
	HDRE* hdre = HDRE::Get(filename);
	if (!hdre)
		return NULL;

	Texture* texture = new Texture();
	
	if (hdre->getFacesf(0))
	{
		texture->createCubemap(hdre->width, hdre->height, (Uint8**)hdre->getFacesf(0),
			hdre->header.numChannels == 3 ? GL_RGB : GL_RGBA, GL_FLOAT);
		for (int i = 1; i < hdre->levels; ++i)
			texture->uploadCubemap(texture->format, texture->type, false,
				(Uint8**)hdre->getFacesf(i), GL_RGBA32F, i);
	}
	else
		if (hdre->getFacesh(0))
		{
			texture->createCubemap(hdre->width, hdre->height, (Uint8**)hdre->getFacesh(0),
				hdre->header.numChannels == 3 ? GL_RGB : GL_RGBA, GL_HALF_FLOAT);
			for (int i = 1; i < hdre->levels; ++i)
				texture->uploadCubemap(texture->format, texture->type, false,
					(Uint8**)hdre->getFacesh(i), GL_RGBA16F, i);
		}
	return texture;
}

void GTR::Renderer::resizeFBOs()
{
	if (gbuffers_fbo.fbo_id != 0)
	{
		gbuffers_fbo.freeTextures();
		gbuffers_fbo.create(Application::instance->window_width,
			Application::instance->window_height,
			3,	//num textures
			GL_RGBA,
			GL_FLOAT);	//precision
	}

	if (illumination_fbo.fbo_id != 0)
	{
		illumination_fbo.freeTextures();
		illumination_fbo.create(Application::instance->window_width, 
			Application::instance->window_height,
			1, 			//one textures
			GL_RGB, 		//three channels
			GL_FLOAT, //1 byte
			true);	//depth texture
	}

	if (ao_buffer)
	{
		ao_buffer->clear();
		ao_buffer->create(Application::instance->window_width, Application::instance->window_height, GL_LUMINANCE, GL_UNSIGNED_BYTE);
	}

	if (gamma_fbo.fbo_id != 0) {
		gamma_fbo.create(Application::instance->window_width, Application::instance->window_height,
			1, 			//one texture
			GL_RGB, 		//three channels
			GL_FLOAT, //1 byte
			true);	//depth texture
	}

	if (blur_fbo.fbo_id != 0) {
		blur_fbo.create(Application::instance->window_width, Application::instance->window_height, 1, GL_RGBA, GL_FLOAT, false);
	}

	if (decals_fbo.fbo_id != 0)
	{
		decals_fbo.freeTextures();
		decals_fbo.create(Application::instance->window_width,
			Application::instance->window_height,
			3,	//num textures
			GL_RGBA,
			GL_FLOAT);	//precision
	}
}

void GTR::Renderer::renderSkybox(Texture* skybox, Camera* camera)
{
	Mesh* mesh = Mesh::Get("data/meshes/sphere.obj", false);
	Shader* shader = Shader::Get("skybox");
	shader->enable();
	Matrix44 m;
	m.setTranslation(camera->eye.x, camera->eye.y, camera->eye.z);
	m.scale(10, 10, 10);
	shader->setUniform("u_model", m);
	shader->setUniform("u_viewprojection", camera->viewprojection_matrix);
	shader->setUniform("u_camera_position", camera->eye);
	shader->setUniform("u_camera_pos", camera->eye);
	shader->setTexture("u_environment_texture", skybox, 9);
	shader->setUniform("u_color", Vector4(1.0,0,0,1));

	glDisable(GL_BLEND);
	glDisable(GL_CULL_FACE);
	glDisable(GL_DEPTH_TEST);
	mesh->render(GL_TRIANGLES);
	shader->disable();

	glEnable(GL_CULL_FACE);
	glEnable(GL_DEPTH_TEST);
}

void GTR::Renderer::extractProbe(GTR::Scene* scene, sProbe& p)
{
	FloatImage images[6]; //here we will store the six views
	Camera cam;

	//set the fov to 90 and the aspect to 1
	cam.setPerspective(90, 1, 0.1, 1000);

	//createRenderCalls(scene, NULL);
	if (!irr_fbo)
	{
		irr_fbo = new FBO();
		irr_fbo->create(64, 64, 1, GL_RGB, GL_FLOAT, false);
	}
	for (int i = 0; i < 6; ++i) //for every cubemap face
	{
		//compute camera orientation using defined vectors
		Vector3 eye = p.pos;
		Vector3 front = cubemapFaceNormals[i][2];
		Vector3 center = p.pos + front;
		Vector3 up = cubemapFaceNormals[i][1];
		cam.lookAt(eye, center, up);
		cam.enable();

		//render the scene from this point of view
		irr_fbo->bind();
		renderForward(scene, render_calls, &cam, FORWARD, SHOW_SHADOWMAP );
		irr_fbo->unbind();

		//read the pixels back and store in a FloatImage
		images[i].fromTexture(irr_fbo->color_textures[0]);
	}

	//compute the coefficients given the six images
	p.sh = computeSH(images, false);
}

void GTR::Renderer::updateIrradianceCache(GTR::Scene* scene)
{
	if (!irr)
	{
		std::cout << "No irradiance to update" << std::endl;
		return;
	}

	std::cout << "Updating irradiance . . .";
	//ePipelineMode prev = pipeline_mode;
	//pipeline_mode = FORWARD;
	//eRenderMode rend_prev = render_mode;
	//render_mode = SHOW_SHADOWMAP;

	int num_probes = irr->probes.size();
	float probes_done = 0;
	for (int i = 0; i < num_probes; i++)
	{
		probes_done = (float) ((i + 1) / (float)num_probes) * 100.0;
		probes_done = floor(probes_done);
		std::cout << "\r" << "Updating irradiance . . . " << probes_done  << "%";
		extractProbe(scene, irr->probes[i]);
	}

	//pipeline_mode = prev;
	//render_mode = rend_prev;
	std::cout << " Finished!" << std::endl;

	storeIrradianceToTexture();
}

void GTR::Renderer::storeIrradianceToTexture()
{
	if (probes_texture == NULL)
	{
		probes_texture = new Texture(
			9,					//9 coefficients per probe
			irr->probes.size(), //as many rows as probes
			GL_RGB,				//3 channels per coefficient
			GL_FLOAT);			//they require a high range
	}

	//we must create the color information for the texture. because every SH are 27 floats in the RGB,RGB,... order, 
	//we can create an array of SphericalHarmonics and use it as pixels of the texture
	SphericalHarmonics* sh_data = NULL;
	//sh_data = new SphericalHarmonics[irr->dimensions[0] * irr->dimensions[1] * irr->dimensions[2]];
	sh_data = new SphericalHarmonics[irr->dim.x * irr->dim.y * irr->dim.z];

	//fill the data of the array with our probes in x,y,z order...
	for (int i = 0; i < irr->probes.size(); i++)
	{
		sh_data[i] = irr->probes[i].sh;
	}

	//upload the data to the GPU
	probes_texture->upload(GL_RGB, GL_FLOAT, false, (uint8*)sh_data);

	//disable any texture filtering when reading
	probes_texture->bind();
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	probes_texture->unbind();

	//free memory after allocating it!!!
	delete[] sh_data;
}

void GTR::Renderer::updateReflectionProbes(GTR::Scene* scene)
{
	std::cout << "Updating Reflection probes . . .";

	if (reflection_fbo.fbo_id == 0)
	{
		reflection_fbo.create(64, 64, 1, GL_RGB, GL_FLOAT, false);
	}

	int ref_length = reflection_entity->reflection_probes.size();

	Camera cam;
	//set the fov to 90 and the aspect to 1
	cam.setPerspective(90, 1, 0.1, 1000);

	for (int i = 0; i < ref_length; i++)
	{
		sReflectionProbe* probe = reflection_entity->reflection_probes[i];

		//render the view from every side
		for (int i = 0; i < 6; ++i)
		{
			//assign cubemap face to FBO
			reflection_fbo.setTexture(probe->cubemap, i);

			//bind FBO
			reflection_fbo.bind();
			
			//render view
			Vector3 eye = probe->model.getTranslation();
			Vector3 center = eye + cubemapFaceNormals[i][2];
			Vector3 up = cubemapFaceNormals[i][1];
			cam.lookAt(eye, center, up);
			cam.enable();
			renderForward(scene, render_calls, &cam, FORWARD, SHOW_SHADOWMAP);
			reflection_fbo.unbind();
		}

		//generate the mipmaps
		probe->cubemap->generateMipmaps();
	}

	std::cout << " Finished!" << std::endl;
}

void GTR::Renderer::renderDecals(GTR::Scene* scene, Camera* camera) {

	Shader* shader = Shader::Get("decals");
	shader->enable();
	shader->setTexture("u_color_texture", gbuffers_fbo.color_textures[0], 0);
	shader->setTexture("u_normal_texture", gbuffers_fbo.color_textures[1], 1);
	shader->setTexture("u_extra_texture", gbuffers_fbo.color_textures[2], 2);
	shader->setTexture("u_depth_texture", gbuffers_fbo.depth_texture, 3);
	shader->setUniform("u_viewprojection", camera->viewprojection_matrix);
	Matrix44 inv_viewproj = camera->viewprojection_matrix;
	inv_viewproj.inverse();
	shader->setUniform("u_inverse_viewprojection", inv_viewproj);
	//inverse window resolution
	shader->setUniform("u_iRes", Vector2(1.0 / (float)gbuffers_fbo.color_textures[0]->width, 1.0 / (float)gbuffers_fbo.color_textures[0]->height));
	static Mesh* box = NULL;
	if (!box)
	{
		box = new Mesh();
		box->createCube();
	}
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_BLEND);

	//optimizar --> sacarlos antes
	for (int i = 0; i < scene->entities.size(); i++)
	{
		BaseEntity* ent = scene->entities[i];
		if (ent->entity_type != eEntityType::DECAL)
			continue;

		DecalEntity* decal = (DecalEntity*)ent;
		shader->setUniform("u_model", ent->model);
		Matrix44 inv = ent->model;
		inv.inverse();
		shader->setUniform("u_iModel", inv);
		shader->setTexture("u_decal_texture", decal->albedo, 4);

		box->render(GL_TRIANGLES);
	}
}

void GTR::Renderer::renderPostFX(Camera* camera, Texture* texture)
{
	Shader* shader = NULL;
	int w = Application::instance->window_width;
	int h = Application::instance->window_height;
	Matrix44 inv_vp = camera->viewprojection_matrix;
	inv_vp.inverse();

	switch (post_fx) {
		case FX_MOTION_BLUR:
			if (gbuffers_fbo.depth_texture) {
				shader = Shader::Get("motion_blur");
				shader->enable();
				shader->setUniform("u_prev_vp", vp_previous);
				shader->setTexture("u_depth_texture", gbuffers_fbo.depth_texture, 1);
				shader->setUniform("u_inverse_viewprojection", inv_vp);
				shader->setUniform("u_iRes", Vector2(1.0 / (float)w, 1.0 / (float)h));
			}
			break;
		case FX_PIXELATED:
			shader = Shader::Get("pixelated");
			shader->enable();
			shader->setUniform("u_pixel_size", pixel_size);
			shader->setUniform("u_iRes", Vector2(1.0 / (float)w, 1.0 / (float)h));
			break;
		case FX_BLUR:
			shader = Shader::Get("blur");
			shader->enable();
			shader->setUniform("u_blur_size", blur_size);
			shader->setUniform("u_iRes", Vector2(1.0 / (float)w, 1.0 / (float)h));
			break;
		case FX_DEPTH_OF_FIELD:
			if (blur_fbo.fbo_id == 0) {
				blur_fbo.create(w, h, 1, GL_RGBA, GL_FLOAT, false);
			}
			//blur step
			shader = Shader::Get("blur");
			shader->enable();
			shader->setUniform("u_blur_size", blur_size);
			shader->setUniform("u_iRes", Vector2(1.0 / (float)w, 1.0 / (float)h));
			blur_fbo.bind();
			glClearColor(0,0,0, 1.0);
			glClear(GL_COLOR_BUFFER_BIT);
			glDisable(GL_BLEND);
			glDisable(GL_DEPTH_TEST);
			texture->toViewport(shader);
			blur_fbo.unbind();
			shader->disable();
			//once have it the blurried texture, apply the depth of field postfx
			shader = Shader::Get("dof");
			shader->enable();
			shader->setUniform("u_blur_size", blur_size);
			shader->setUniform("u_iRes", Vector2(1.0 / (float)w, 1.0 / (float)h));
			shader->setTexture("u_blur_texture", blur_fbo.color_textures[0], 3);
			shader->setTexture("u_depth_texture", gbuffers_fbo.depth_texture, 4);
			shader->setUniform("u_inverse_viewprojection", inv_vp);
			shader->setUniform("u_camera_nearfar", Vector2(camera->near_plane, camera->far_plane));
			shader->setUniform("u_camera_position", camera->eye);
			break;
	}

	texture->toViewport(shader);
}

void GTR::Renderer::readIrradiance(GTR::Scene* scene)
{
	scene->readIrradianceFromDisk();
	//build the irradiance texture
	storeIrradianceToTexture();
}

std::vector<Vector3> generateSpherePoints(int num, float radius, bool hemi)
{
	std::vector<Vector3> points;
	points.resize(num);
	for (int i = 0; i < num; i += 3)
	{
		Vector3& p = points[i];
		float u = random();
		float v = random();
		float theta = u * 2.0 * PI;
		float phi = acos(2.0 * v - 1.0);
		float r = cbrt(random() * 0.9 + 0.1) * radius;
		float sinTheta = sin(theta);
		float cosTheta = cos(theta);
		float sinPhi = sin(phi);
		float cosPhi = cos(phi);
		p.x = r * sinPhi * cosTheta;
		p.y = r * sinPhi * sinTheta;
		p.z = r * cosPhi;
		if (hemi && p.z < 0)
			p.z *= -1.0;
	}
	return points;
}

GTR::SSAOFX::SSAOFX()
{
	intensity = 1.0;
	//random points
	points = generateSpherePoints(64, 1.0, true);
}

void GTR::SSAOFX::apply(Texture* depth_buffer, Texture* normal_buffer, Camera* cam, Texture* output)
{
	////bind the texture we want to change
	//depth_buffer->bind();

	////disable using mipmaps
	//glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);

	////enable bilinear filtering
	//glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

	//depth_buffer->unbind();

	FBO* fboS = Texture::getGlobalFBO(output);
	fboS->bind();


	Mesh* quad = Mesh::getQuad();

	glDisable(GL_DEPTH_TEST);
	glDisable(GL_BLEND);

	Shader* shader = Shader::Get("ssao");
	shader->enable();
	shader->setUniform("u_viewprojection", cam->viewprojection_matrix);
	shader->setTexture("u_normal_texture", normal_buffer, 1);
	shader->setTexture("u_depth_texture", depth_buffer, 3);
	shader->setUniform3Array("u_points", (float*)&points[0], points.size());
	//viewprojection to obtain the uv in the depthtexture of any random position of our world
	Matrix44 inv_viewproj = cam->viewprojection_matrix;
	inv_viewproj.inverse();
	shader->setUniform("u_inverse_viewprojection", inv_viewproj);
	shader->setUniform("u_iRes", Vector2(1.0 / (float)depth_buffer->width, 1.0 / (float)depth_buffer->height));

	quad->render(GL_TRIANGLES);
	
	fboS->unbind();
}
