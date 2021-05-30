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

#include <algorithm>

using namespace GTR;

Renderer::Renderer() {
	lights = Scene::instance->lights;
	render_mode = eRenderMode::SHOW_SHADOWMAP;
	render_deferred_mode = eRenderDeferredMode::DEFERRED_SHADOWMAP;
	pipeline_mode = ePipelineMode::DEFERRED;
	renderer_cond = eRendererCondition::REND_COND_NONE;
	color_buffer = new Texture(Application::instance->window_width, Application::instance->window_height, GL_RGB, GL_HALF_FLOAT);
	quality = eQuality::LOW;
	fbo.create(1024, 1024);
	ssao_fbo.create(Application::instance->window_width, Application::instance->window_height, 1, GL_RGBA, GL_UNSIGNED_BYTE, false);
	light_camera = 0;
	//limited to 4 lights
	shadow_singlepass.create(4 * 512, 512);
	ao_buffer = NULL;
	show_ao = false;
	tone_mapper.init();
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
				prefabToNode(ent->model, pent->prefab, camera);
			}
		}
	}

	// sort render calls
	std::sort(render_calls.begin(), render_calls.end(), compareDistanceToCamera());
	std::sort(render_calls.begin(), render_calls.end(), compareAlpha());
}

float Renderer::computeDistanceToCamera(Matrix44 node_model, Mesh* mesh, Vector3 cam_pos) {
	BoundingBox world_bounding = transformBoundingBox(node_model, mesh->box);
	Vector3 center = world_bounding.center;

	return distance(center.x, center.y, center.z, cam_pos.x, cam_pos.y, cam_pos.z);;
}

//renders all the prefab
void Renderer::prefabToNode(const Matrix44& model, GTR::Prefab* prefab, Camera* camera)
{
	assert(prefab && "PREFAB IS NULL");
	//assign the model to the root node
	nodeToRenderCall(model, &prefab->root, camera);
}

//renders a node of the prefab and its children
void Renderer::nodeToRenderCall(const Matrix44& prefab_model, GTR::Node* node, Camera* camera)
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
		if (camera->testBoxInFrustum(world_bounding.center, world_bounding.halfsize) )
		{
			//create render call
			renderCall rc;
			rc.set(node->mesh, node->material, node_model);
			rc.distance_to_camera = computeDistanceToCamera(node_model, node->mesh, camera->eye);
			render_calls.push_back(rc);
		}
	}

	//iterate recursively with children
	for (int i = 0; i < node->children.size(); ++i)
		nodeToRenderCall(prefab_model, node->children[i], camera);
}

void Renderer::renderForward(GTR::Scene* scene, std::vector< renderCall >& data, Camera* camera)
{
	//set the clear color (the background color)
	glClearColor(scene->background_color.x, scene->background_color.y, scene->background_color.z, 1.0);

	// Clear the color and the depth buffer
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	checkGLErrors();

	for (int i = 0; i < data.size(); i++)
	{
		renderCall& rc = data[i];
		if ( (renderer_cond == REND_COND_NO_ALPHA && !rc.isAlpha) || renderer_cond == REND_COND_NONE)
			renderMeshWithMaterial(rc.model, rc.mesh, rc.material, camera);
		else if (renderer_cond == REND_COND_ALPHA && rc.isAlpha)
			renderMeshWithMaterial(rc.model, rc.mesh, rc.material, camera);
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

	checkGLErrors();

	for (int i = 0; i < data.size(); i++)
	{
		renderCall& rc = data[i];
		renderMeshWithMaterial(rc.model, rc.mesh, rc.material, camera);
	}

	gbuffers_fbo.unbind();
	
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
	else {
	//	gbuffers_fbo.depth_texture->copyTo(illumination_fbo.depth_texture);
		
		illumination_fbo.bind();
		glClearColor(scene->background_color.x, scene->background_color.y, scene->background_color.z, 1.0);
		glClear(GL_COLOR_BUFFER_BIT);
		renderReconstructedScene(scene, camera);
		if(!use_dithering)
			renderAlphaElements(data, camera);
		illumination_fbo.unbind();

		glDisable(GL_BLEND);


		if (!linear_correction) {
			illumination_fbo.color_textures[0]->toViewport();
		}
		else {
			if (gamma_fbo.fbo_id == 0) {
				gamma_fbo.create(Application::instance->window_width, Application::instance->window_height,
					1, 			//three textures
					GL_RGB, 		//three channels
					GL_FLOAT, //1 byte
					true);	//depth texture
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

			gamma_fbo.color_textures[0]->toViewport();
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
	
	glEnable(GL_BLEND);
	glBlendFunc(GL_ONE, GL_ONE);

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

//renders a mesh given its transform and material
void Renderer::renderMeshWithMaterial(const Matrix44 model, Mesh* mesh, GTR::Material* material, Camera* camera, Shader* sh, ePipelineMode pipeline, eRenderMode mode)
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

	//choose a shader
	Shader* shader;
	if (sh == NULL)
		shader = getShader();
	else
		shader = sh;

    assert(glGetError() == GL_NO_ERROR);

	//no shader? then nothing to render
	if (!shader)
		return;
	shader->enable();

	ePipelineMode _pipeline_mode;
	if (pipeline == NO_PIPELINE)
		_pipeline_mode = pipeline_mode;
	else
		_pipeline_mode = pipeline;

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

	eRenderMode rending_mode;
	if (mode == SHOW_NONE)
		rending_mode = render_mode;
	else
		rending_mode = mode;

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

	//upload material properties to the shader
	material->uploadToShader(shader);

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
				renderMultiPass(shader, mesh, true);
			}
		}
		else {	//no lights
			mesh->render(GL_TRIANGLES);
		}
	}
	//Deferred
	else {
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

void GTR::Renderer::renderMultiPass(Shader* shader, Mesh* mesh, bool sendShadowMap)
{
	for (int i = 0; i < lights.size(); i++)
	{
		LightEntity* light = lights[i];
		if (light == nullptr)
			continue;

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
Shader* GTR::Renderer::getShader()
{
	Shader* shader = NULL;

	switch (render_mode) {
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

	if (pipeline_mode == DEFERRED)
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
	ImGui::Checkbox("Show AO", &show_ao);
	if (pipeline_mode == DEFERRED)
	{
		ImGui::Checkbox("Show GBuffers", &show_gbuffers);
		if(show_gbuffers)
			ImGui::Checkbox("Show Alpha GBuffers", &show_gbuffers_alpha);
	}

	if (changed_fbo)
		changeQualityFBO();
#endif
}

Texture* GTR::CubemapFromHDRE(const char* filename)
{
	HDRE* hdre = new HDRE();
	if (!hdre->load(filename))
	{
		delete hdre;
		return NULL;
	}

	/*
	Texture* texture = new Texture();
	texture->createCubemap(hdre->width, hdre->height, (Uint8**)hdre->getFaces(0), hdre->header.numChannels == 3 ? GL_RGB : GL_RGBA, GL_FLOAT );
	for(int i = 1; i < 6; ++i)
		texture->uploadCubemap(texture->format, texture->type, false, (Uint8**)hdre->getFaces(i), GL_RGBA32F, i);
	return texture;
	*/
	return NULL;
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