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

void Renderer::renderScene(GTR::Scene* scene, Camera* camera)
{
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
	//shader = Shader::Get("texture");
	shader = Shader::Get("light");

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
	shader->setUniform("u_ambient_light", GTR::Scene::instance->ambient_color);
	if(texture)
		shader->setUniform("u_texture", texture, 0);

	if (material->emissive_factor.length()) {
		Texture* emissive_texture = NULL;
		emissive_texture = material->emissive_texture.texture;
		shader->setUniform("u_emissive_texture", emissive_texture, 1);
	}

	//this is used to say which is the alpha threshold to what we should not paint a pixel on the screen (to cut polygons according to texture alpha)
	shader->setUniform("u_alpha_cutoff", material->alpha_mode == GTR::eAlphaMode::MASK ? material->alpha_cutoff : 0);

	//do the draw call that renders the mesh into the screen
	mesh->render(GL_TRIANGLES);

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