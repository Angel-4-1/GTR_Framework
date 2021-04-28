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

#include <algorithm>

using namespace GTR;

Renderer::Renderer() {
	render_mode = eRenderMode::SHOW_DEFAULT;
}

void Renderer::renderScene(GTR::Scene* scene, Camera* camera)
{
	//set the clear color (the background color)
	glClearColor(scene->background_color.x, scene->background_color.y, scene->background_color.z, 1.0);

	// Clear the color and the depth buffer
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	checkGLErrors();

	createRenderCalls(scene,camera);
	
	// render the nodes
	renderRenderCalls(render_calls, camera);
}

void Renderer::renderRenderCalls(std::vector< renderCall > data, Camera* camera) {

	for (int i = 0; i < data.size(); ++i)
	{
		GTR::Node* node = data[i].node;
		if (node == nullptr)
			continue;

		renderSingleNode(*data[i].prefab_model, node, camera, data[i].isAlpha);
	}
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
	lights.clear();
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
				GTR::Prefab* prefab = pent->prefab;
				GTR::Node* node = &prefab->root;
				// Check if the node has an alpha blending property
				renderCall rc;
				rc.node = node; rc.prefab_model = &pent->model;
				
				if (node->mesh && node->material) {
					if (node->material->alpha_mode == GTR::eAlphaMode::BLEND) {
						rc.isAlpha = true;
					}
					rc.distance_to_camera = computeDistanceToCamera(node, &pent->model, camera->eye);
				}

				render_calls.push_back(rc);

				checkAlphaComponent(node, &ent->model, camera->eye);
			}
		}
		else if (ent->entity_type == LIGHT)
		{
			lights.push_back( (GTR::LightEntity*)ent );
		}
	}

	// sort alpha elements
	std::sort(render_calls.begin(), render_calls.end(), compareDistanceToCamera());
	std::sort(render_calls.begin(), render_calls.end(), compareAlpha());
}

void Renderer::checkAlphaComponent(GTR::Node* node, Matrix44* prefab_model, Vector3 cam_pos) {
	// Check all the children of a node until one with alpha blending property is found
	for (int i = 0; i < node->children.size(); ++i)
	{
		GTR::Node* node_child = node->children[i];
		renderCall rc; //ew renderCall(node, Vector3(), &ent->model);
		rc.node = node_child; rc.prefab_model = prefab_model;
		if (node_child->mesh && node_child->material) {
			if (node_child->material->alpha_mode == GTR::eAlphaMode::BLEND) {
				rc.isAlpha = true;
			}
			rc.distance_to_camera = computeDistanceToCamera(node_child, prefab_model, cam_pos);
		}

		render_calls.push_back(rc);
		checkAlphaComponent(node_child, prefab_model, cam_pos);
	}
}

float Renderer::computeDistanceToCamera(GTR::Node* node, Matrix44* prefab_model, Vector3 cam_pos) {
	Matrix44 node_model = node->getGlobalMatrix(true) * (*prefab_model);
	BoundingBox world_bounding = transformBoundingBox(node_model, node->mesh->box);
	Vector3 center = world_bounding.center;

	return distance(center.x, center.y, center.z, cam_pos.x, cam_pos.y, cam_pos.z);;
}

//renders all the prefab
void Renderer::renderPrefab(const Matrix44& model, GTR::Prefab* prefab, Camera* camera)
{
	assert(prefab && "PREFAB IS NULL");
	//assign the model to the root node
	renderNode(model, &prefab->root, camera);
}

//renders a node of the prefab and its children
void Renderer::renderNode(const Matrix44& prefab_model, GTR::Node* node, Camera* camera)
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
			//render node mesh
			renderMeshWithMaterial( node_model, node->mesh, node->material, camera );
			if (isRenderingBoundingBox) {
				node->mesh->renderBounding(node_model, true);
			}
		}
	}

	//iterate recursively with children
	for (int i = 0; i < node->children.size(); ++i)
		renderNode(prefab_model, node->children[i], camera);
}

void Renderer::renderSingleNode(const Matrix44& prefab_model, GTR::Node* node, Camera* camera, bool hasAlpha)
{
	if (!node->visible)
		return;

	if ( (renderer_cond == eRendererCondition::REND_COND_NO_ALPHA) && hasAlpha) {
		return;
	}

	if ( (renderer_cond == eRendererCondition::REND_COND_ALPHA) && !hasAlpha) {
		return;
	}

	//compute global matrix
	Matrix44 node_model = node->getGlobalMatrix(true) * prefab_model;

	//does this node have a mesh? then we must render it
	if (node->mesh && node->material)
	{
		//compute the bounding box of the object in world space (by using the mesh bounding box transformed to world space)
		BoundingBox world_bounding = transformBoundingBox(node_model, node->mesh->box);

		//if bounding box is inside the camera frustum then the object is probably visible
		if (camera->testBoxInFrustum(world_bounding.center, world_bounding.halfsize))
		{
			//render node mesh
			renderMeshWithMaterial(node_model, node->mesh, node->material, camera);
			if (isRenderingBoundingBox) {
				node->mesh->renderBounding(node_model, true);
			}
		}
	}
}
//renders a mesh given its transform and material
void Renderer::renderMeshWithMaterial(const Matrix44 model, Mesh* mesh, GTR::Material* material, Camera* camera)
{
	//in case there is nothing to do
	if (!mesh || !mesh->getNumVertices() || !material )
		return;
    assert(glGetError() == GL_NO_ERROR);

	//define locals to simplify coding
	Shader* shader = NULL;
	Texture* texture = NULL;
	GTR::Scene* scene = GTR::Scene::instance;
	
	int multiple_lights = 0;

	texture = material->color_texture.texture;
	//texture = material->metallic_roughness_texture;
	//texture = material->normal_texture;
	//texture = material->occlusion_texture;
	if (texture == NULL)
		texture = Texture::getWhiteTexture(); //a 1x1 white texture

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

	//chose a shader
	switch (render_mode) {
		case SHOW_DEFAULT:
			shader = Shader::Get("light");
			multiple_lights = 1;
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
			multiple_lights = 2;
			break;
		default:
			shader = Shader::Get("light");
			break;
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

	shader->setUniform("u_color", material->color);
	shader->setUniform("u_ambient_light", scene->ambient_light);
	if(texture)
		shader->setUniform("u_texture", texture, 0);

	Texture* emissive_texture = NULL;
	emissive_texture = material->emissive_texture.texture;
	
	if (emissive_texture) {
		shader->setUniform("u_is_emissor", true);
		shader->setUniform("u_emissive_factor", material->emissive_factor);
		shader->setUniform("u_emissive_texture", emissive_texture, 1);
	}
	else {
		shader->setUniform("u_is_emissor", false);
	}

	Texture* normal_texture = NULL;
	normal_texture = material->normal_texture.texture;

	if (normal_texture) {
		shader->setUniform("u_has_normal", true);
		shader->setUniform("u_normal_texture", normal_texture, 2);
	}
	else {
		shader->setUniform("u_has_normal", false);
	}

	Texture* metallic_roughness_texture = NULL;
	metallic_roughness_texture = material->metallic_roughness_texture.texture;
	if (metallic_roughness_texture) {
		shader->setUniform("u_has_metallic_roughness", true);
		shader->setUniform("u_metallic_roughness_texture", metallic_roughness_texture, 3);
		shader->setUniform("u_material_shininess", material->roughness_factor);
		int type_property = 0;
		switch (render_mode) {
			case SHOW_OCCLUSION:
				type_property = 0;
				break;
			case SHOW_METALLIC:
				type_property = 1;
				break;
			case SHOW_ROUGHNESS:
				type_property = 2;
				break;
			default:
				break;
		}
		shader->setUniform("u_type_property", type_property);
	}
	else {
		shader->setUniform("u_has_metallic_roughness", false);
	}


	//this is used to say which is the alpha threshold to what we should not paint a pixel on the screen (to cut polygons according to texture alpha)
	shader->setUniform("u_alpha_cutoff", material->alpha_mode == GTR::eAlphaMode::MASK ? material->alpha_cutoff : 0);

	//-----------------------
	//--- MULTIPLE LIGHTS ---
	//-----------------------
	if (multiple_lights == 1) 
	{
		//allow to render pixels that have the same depth as the one in the depth buffer
	//	glDepthFunc(GL_LEQUAL);

		//set blending mode to additive this will collide with materials with blend...
	//	glBlendFunc(GL_SRC_ALPHA, GL_ONE);	//this causes problems with the nodes with alpha blending !!!

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
				//----------------------------------
				glDepthFunc(GL_LEQUAL);
				glBlendFunc(GL_SRC_ALPHA, GL_ONE);
				//----------------------------------
				glEnable(GL_BLEND);
				//do not apply emissive texture again
				shader->setUniform("u_is_emissor", false);
				//no add ambient light
				shader->setUniform("u_ambient_light", Vector3());
			}

			//pass light to shader
			light->uploadToShader(shader);

			//do the draw call that renders the mesh into the screen
			mesh->render(GL_TRIANGLES);
		}
		glDepthFunc(GL_LESS); //as default
	}
	else if (multiple_lights == 2)
	{
		Vector3 light_position[10];
		Vector3 light_color[10];
		Vector3 light_vector[10];
		int light_type[10];
		float light_intensity[10];
		Vector2 light_spot_vars[10];	// x = cut off	y = spot exponent

		for (int i = 0; i < lights.size(); i++)
		{
			light_position[i] = lights[i]->model.getTranslation();
			light_color[i] = lights[i]->color;
			light_intensity[i] = lights[i]->intensity;
			light_type[i] = (int)lights[i]->light_type;
			light_vector[i] = lights[i]->model.frontVector();//lights[i]->directional_vector;
			light_spot_vars[i] = Vector2( cos((lights[i]->cone_angle / 180.0) * PI), lights[i]->spot_exponent);
		}
		int num_lights = lights.size();

		shader->setUniform3Array("u_light_pos", (float*)&light_position, num_lights);
		shader->setUniform3Array("u_light_color", (float*)&light_color, num_lights);
		shader->setUniform3Array("u_light_vector", (float*)&light_vector, num_lights);
		shader->setUniform1Array("u_light_type", (int*)&light_type, num_lights);
		shader->setUniform1Array("u_light_intensity", (float*)&light_intensity, num_lights);
		shader->setUniform2Array("u_light_spot_vars", (float*)&light_spot_vars, num_lights);
		shader->setUniform("u_num_lights", num_lights);

		mesh->render(GL_TRIANGLES);
	}
	else {
		mesh->render(GL_TRIANGLES);
	}
	//----------------------
	
	//disable shader
	shader->disable();

	//set the render state as it was before to avoid problems with future renders
	glDisable(GL_BLEND);
}

void Renderer::renderInMenu()
{
#ifndef SKIP_IMGUI
	ImGui::Checkbox("BoundingBox", &isRenderingBoundingBox);
	ImGui::Combo("Alpha", (int*)&renderer_cond, "NONE\0ALPHA\0NOALPHA", 3);
	ImGui::Combo("Render Mode", (int*)&render_mode, "DEFAULT\0TEXTURE\0NORMAL\0NORMALMAP\0UVS\0OCCLUSION\0METALLIC\0ROUGHNESS\0SINGLEPASS", 7);
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