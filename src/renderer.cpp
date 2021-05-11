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
	pipeline_mode = ePipelineMode::FORWARD;
	renderer_cond = eRendererCondition::REND_COND_NONE;
	color_buffer = new Texture(Application::instance->window_width, Application::instance->window_height);
	quality = eQuality::LOW;
	fbo.create(1024, 1024);
	light_camera = 0;
	//limited to 4 lights
	shadow_singlepass.create(4 * 512, 512);
}

void Renderer::renderToFBO(GTR::Scene* scene, Camera* camera)
{
	if (render_mode == SHOW_SHADOWMAP)
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
			/*float w = Application::instance->window_width;
			float h = Application::instance->window_height;
			glViewport(0, 0, w, h / 3);
			glScissor(0, 0, w, h / 3);
			glEnable(GL_SCISSOR_TEST);

			shader->enable();
			glDisable(GL_DEPTH_TEST);

			//upload uniforms
			shader->setUniform("u_camera_nearfar", Vector2(camera->near_plane, camera->far_plane));
			//fbo.depth_texture->toViewport(shader);
			shadow_singlepass.depth_texture->toViewport(shader);

			glEnable(GL_DEPTH_TEST);

			shader->disable();

			//restore viewport
			glDisable(GL_SCISSOR_TEST);
			glViewport(0, 0, w, h);*/

			if(light_camera < lights.size())
				lights[light_camera]->renderShadowFBO(shader);
		}
	}
	else {
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
		renderMeshWithMaterial(rc.model, rc.mesh, rc.material, camera);
	}
}

void Renderer::renderDeferred(GTR::Scene* scene, std::vector< renderCall >& data, Camera* camera)
{
	//create buffers
	if (gbuffers_fbo.fbo_id == 0)
	{
		//careful when resinig the window
		gbuffers_fbo.create(Application::instance->window_width, 
							Application::instance->window_height,
							3,	//num textures
							GL_RGBA,
							GL_UNSIGNED_BYTE);
	}

	gbuffers_fbo.bind();
	glClearColor(0, 0, 0, 1.0);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

	checkGLErrors();

	for (int i = 0; i < data.size(); i++)
	{
		renderCall& rc = data[i];
		renderMeshWithMaterial(rc.model, rc.mesh, rc.material, camera);
	}

	gbuffers_fbo.unbind();
	
	gbuffers_fbo.color_textures[0]->toViewport();

	/*if (show_gbuffers)
	{
		int w = Application::instance->window_width;
		int h = Application::instance->window_height;
		glViewport(0, h * 0.5, w * 0.5, h * 0.5);
		gbuffers_fbo.color_textures[0]->toViewport();

		glViewport(w * 0.5, h * 0.5, w * 0.5, h * 0.5);
		gbuffers_fbo.color_textures[1]->toViewport();

		glViewport(0, 0, w * 0.5, h * 0.5);
		gbuffers_fbo.color_textures[2]->toViewport();

		Shader* depth_shader = Shader::Get("depth");
		depth_shader->enable();
		depth_shader->setUniform("u_camera_nearfar", Vector2(camera->near_plane, camera->far_plane));
		glViewport(w * 0.5, 0, w * 0.5, h * 0.5);
		gbuffers_fbo.depth_texture->toViewport(depth_shader);
		depth_shader->disable();

		glViewport(0, 0, w, h);
	}*/
	
}

//renders a mesh given its transform and material
void Renderer::renderMeshWithMaterial(const Matrix44 model, Mesh* mesh, GTR::Material* material, Camera* camera)
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
	Shader* shader = getShader();

	if (pipeline_mode == DEFERRED)
	{
		shader = Shader::Get("gbuffers");
		if (material->alpha_mode == GTR::eAlphaMode::BLEND)
		{
			glDisable(GL_BLEND);
			return;
		}
	}

    assert(glGetError() == GL_NO_ERROR);

	//no shader? then nothing to render
	if (!shader)
		return;
	shader->enable();

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

	if (render_mode == SHOW_MULTIPASS) 
	{
		//Multi Pass
		renderMultiPass(shader, mesh);
	}
	else if (render_mode == SHOW_SINGLEPASS)
	{
		//Single Pass
		renderSinglePass(shader, mesh);
	}
	else if (render_mode == SHOW_SHADOWMAP) {
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

void GTR::Renderer::renderSinglePass(Shader* shader, Mesh* mesh, bool sendShadowMap)
{
	//collect info about all the lights of the scene
	Vector3 light_position[10];
	Vector3 light_color[10];
	Vector3 light_vector[10];
	int light_type[10];
	float light_intensity[10];
	float light_max_distance[10];
	Vector2 light_spot_vars[10];	// x = cut off angle  |  y = spot exponent

	int num_lights = lights.size();

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

	//singlepass shadowmap?
	if (sendShadowMap)
	{
		bool light_cast_shadow[10];
		Matrix44 light_shadow_viewproj[10];
		float light_shadow_bias[10];
		float light_pos_atlas[10];

		for (int i = 0; i < num_lights; i++)
		{
			light_cast_shadow[i] = lights[i]->cast_shadow;
			if (!lights[i]->cast_shadow)
				continue;

			light_shadow_viewproj[i] = lights[i]->camera->viewprojection_matrix;
			light_shadow_bias[i] = lights[i]->shadow_bias;
			light_pos_atlas[i] = i * 512;
		//	light_pos_atlas[i] = i * 0.25;
		}
		shader->setUniform1Array("u_light_cast_shadow", (float*)&light_cast_shadow, num_lights);
		shader->setUniform4Array("u_light_shadow_viewproj", (float*)&light_shadow_viewproj, num_lights);
		shader->setUniform1Array("u_light_shadow_bias", (float*)&light_shadow_bias, num_lights);
		shader->setUniform1Array("u_light_pos_atlas", (float*)&light_pos_atlas, num_lights);
		//shadow atlas
		shader->setTexture("u_shadowmap_texture", shadow_singlepass.depth_texture, 8);
	}

	mesh->render(GL_TRIANGLES);
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
void GTR::Renderer::createShadowMaps(Scene* scene, Camera* camera, bool singlepass)
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

	if (singlepass)
	{
		
		float w = Application::instance->window_width;
		float h = Application::instance->window_height;

		for (int i = 0; i < lights.size(); i++)
		{
			if (!lights[i]->cast_shadow || lights[i]->shadow_fbo == NULL)
				continue;

			Texture* shadowmap = lights[i]->shadow_fbo->depth_texture;

			glViewport(i * 512, 0, 512, 512);
			glScissor(i * 512, 0, 512, 512);
			glEnable(GL_SCISSOR_TEST);
			
			rendering_shadowmap = true;
			shadow_singlepass.bind();
			//disable writing to the color buffer
			glColorMask(false, false, false, false);
			//clear the depth buffer only (don't care of color)
			glClear(GL_DEPTH_BUFFER_BIT);
			renderScene(scene, lights[i]->camera);
			//disable it to render back to the screen
			shadow_singlepass.unbind();
			//allow to render back to the color buffer
			glColorMask(true, true, true, true);
			rendering_shadowmap = false;

			glDisable(GL_SCISSOR_TEST);
			glViewport(0, 0, w, h);
		}
		
	}
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

	return shader;
}

void Renderer::renderInMenu()
{
#ifndef SKIP_IMGUI
	bool changed_fbo = false;
	ImGui::Checkbox("BoundingBox", &isRenderingBoundingBox);
//	ImGui::Combo("Alpha", (int*)&renderer_cond, "NONE\0ALPHA\0NOALPHA", 3);
	changed_fbo |= ImGui::Combo("Quality", (int*)&quality, "LOW\0MEDIUM\0HIGH\0ULTRA", 4);
	ImGui::Combo("Render Mode", (int*)&render_mode, "MULTIPASS\0TEXTURE\0NORMAL\0NORMALMAP\0UVS\0OCCLUSION\0METALLIC\0ROUGHNESS\0SINGLEPASS\0SHADOWMAP", 7);
//	ImGui::Combo("Pipline Mode", (int*)&pipeline_mode, "FORWARD\0DEFERRED", 2);
	
	if(pipeline_mode == DEFERRED)
		ImGui::Checkbox("Show GBuffers", &show_gbuffers);

	if (render_mode == SHOW_SHADOWMAP)
	{
		ImGui::Checkbox("Show Depth Cameras", &show_depth_camera);
		ImGui::SliderInt("Depth Light Camera", &light_camera, 0, lights.size() - 1);
		if(light_camera < lights.size() && light_camera >= 0)
			ImGui::Text(lights[light_camera]->name.c_str());
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