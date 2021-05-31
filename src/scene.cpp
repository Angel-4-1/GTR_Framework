#include "scene.h"
#include "utils.h"

#include "prefab.h"
#include "application.h"
#include "extra/cJSON.h"

GTR::Scene* GTR::Scene::instance = NULL;

GTR::Scene::Scene()
{
	instance = this;
}

void GTR::Scene::clear()
{
	for (int i = 0; i < entities.size(); ++i)
	{
		BaseEntity* ent = entities[i];
		delete ent;
	}
	entities.resize(0);
}


void GTR::Scene::addEntity(BaseEntity* entity)
{
	entities.push_back(entity); entity->scene = this;

	if (entity->entity_type == LIGHT)
	{
		lights.push_back((GTR::LightEntity*)entity);
	}
}

bool GTR::Scene::load(const char* filename)
{
	std::string content;

	this->filename = filename;
	std::cout << " + Reading scene JSON: " << filename << "..." << std::endl;

	if (!readFile(filename, content))
	{
		std::cout << "- ERROR: Scene file not found: " << filename << std::endl;
		return false;
	}

	//parse json string 
	cJSON* json = cJSON_Parse(content.c_str());
	if (!json)
	{
		std::cout << "ERROR: Scene JSON has errors: " << filename << std::endl;
		return false;
	}

	//read global properties
	background_color = readJSONVector3(json, "background_color", background_color);
	ambient_light = readJSONVector3(json, "ambient_light", ambient_light);
	main_camera.eye = readJSONVector3(json, "camera_position", main_camera.eye);
	main_camera.center = readJSONVector3(json, "camera_target", main_camera.center);
	main_camera.fov = readJSONNumber(json, "camera_fov", main_camera.fov);

	//entities
	cJSON* entities_json = cJSON_GetObjectItemCaseSensitive(json, "entities");
	cJSON* entity_json;
	cJSON_ArrayForEach(entity_json, entities_json)
	{
		std::string type_str = cJSON_GetObjectItem(entity_json, "type")->valuestring;
		BaseEntity* ent = createEntity(type_str);
		if (!ent)
		{
			std::cout << " - ENTITY TYPE UNKNOWN: " << type_str << std::endl;
			//continue;
			ent = new BaseEntity();
		}

		addEntity(ent);

		if (cJSON_GetObjectItem(entity_json, "name"))
		{
			ent->name = cJSON_GetObjectItem(entity_json, "name")->valuestring;
			stdlog(std::string(" + entity: ") + ent->name);
		}

		//read transform
		if (cJSON_GetObjectItem(entity_json, "position"))
		{
			ent->model.setIdentity();
			Vector3 position = readJSONVector3(entity_json, "position", Vector3());
			ent->model.translate(position.x, position.y, position.z);
		}

		if (cJSON_GetObjectItem(entity_json, "angle"))
		{
			float angle = cJSON_GetObjectItem(entity_json, "angle")->valuedouble;
			ent->model.rotate(angle * DEG2RAD, Vector3(0, 1, 0));
		}

		if (cJSON_GetObjectItem(entity_json, "rotation"))
		{
			Vector4 rotation = readJSONVector4(entity_json, "rotation");
			Quaternion q(rotation.x, rotation.y, rotation.z, rotation.w);
			Matrix44 R;
			q.toMatrix(R);
			ent->model = R * ent->model;
		}

		if (cJSON_GetObjectItem(entity_json, "scale"))
		{
			Vector3 scale = readJSONVector3(entity_json, "scale", Vector3(1, 1, 1));
			ent->model.scale(scale.x, scale.y, scale.z);
		}

		ent->configure(entity_json);
	}

	//free memory
	cJSON_Delete(json);

	return true;
}

GTR::BaseEntity* GTR::Scene::createEntity(std::string type)
{
	if (type == "PREFAB")
		return new GTR::PrefabEntity();
	else if (type == "LIGHT")
		return new GTR::LightEntity();
	return NULL;
}

void GTR::BaseEntity::renderInMenu()
{
#ifndef SKIP_IMGUI
	ImGui::Text("Name: %s", name.c_str()); // Edit 3 floats representing a color
	ImGui::Checkbox("Visible", &visible); // Edit 3 floats representing a color
	//Model edit
	ImGuiMatrix44(model, "Model");
#endif
}




GTR::PrefabEntity::PrefabEntity()
{
	entity_type = PREFAB;
	prefab = NULL;
}

void GTR::PrefabEntity::configure(cJSON* json)
{
	if (cJSON_GetObjectItem(json, "filename"))
	{
		filename = cJSON_GetObjectItem(json, "filename")->valuestring;
		prefab = GTR::Prefab::Get((std::string("data/") + filename).c_str());
	}
}

void GTR::PrefabEntity::renderInMenu()
{
	BaseEntity::renderInMenu();

#ifndef SKIP_IMGUI
	ImGui::Text("filename: %s", filename.c_str()); // Edit 3 floats representing a color
	if (prefab && ImGui::TreeNode(prefab, "Prefab Info"))
	{
		prefab->root.renderInMenu();
		ImGui::TreePop();
	}
#endif
}

GTR::LightEntity::LightEntity()
{
	entity_type = LIGHT;
	light_type = POINT;
	intensity = 1;
	max_distance = 100;
	cone_angle = 45;
	area_size = 0;
	spot_exponent = 1;
	color = Vector3(1.0, 1.0, 1.0);
	camera = new Camera();
	shadow_fbo = new FBO();
	shadow_fbo->setDepthOnly(1024, 1024);
	shadow_bias = 0.002;
	render_light = false;
}

void GTR::LightEntity::uploadToShader(Shader* shader, bool sendShadowMap)
{
	shader->setUniform("u_light_position", model.getTranslation());
	shader->setUniform("u_light_vector", model.frontVector());//directional_vector
	shader->setUniform("u_light_type", (int)light_type);
	shader->setUniform("u_light_color", color);
	shader->setUniform("u_light_intensity", intensity);
	shader->setUniform("u_light_max_distance", max_distance);
	shader->setUniform("u_light_area_size", area_size);
	float cutoff = cos((cone_angle / 180.0) * PI);
	shader->setUniform("u_spot_cosine_cutoff", cutoff );
	shader->setUniform("u_spot_exponent", spot_exponent);

	if (shadow_fbo != NULL && cast_shadow && sendShadowMap)
	{
		//pass to the shader everything needed to compute shadows
		shader->setUniform("u_cast_shadow", true);
		Texture* shadowmap = shadow_fbo->depth_texture;
		shader->setTexture("u_shadowmap_texture", shadowmap, 8);
		Matrix44 shadow_proj = camera->viewprojection_matrix;
		shader->setUniform("u_shadow_viewproj", shadow_proj);
		shader->setUniform("u_shadow_bias", shadow_bias);
	}
	else {
		//no shadow
		shader->setUniform("u_cast_shadow", false);
	}
}


void GTR::LightEntity::configure(cJSON* json)
{
	if (cJSON_GetObjectItem(json, "light_type"))
	{
		std::string category = cJSON_GetObjectItem(json, "light_type")->valuestring;
		
		if (category == "POINT") {
			light_type = POINT;
		}
		else if (category == "SPOT") {
			light_type = SPOT;
		}
		else if (category == "DIRECTIONAL") {
			light_type = DIRECTIONAL;
		}
	}

	if (cJSON_GetObjectItem(json, "color"))
	{
		color = readJSONVector3(json, "color", Vector3(1, 1, 1));
	}

	if (cJSON_GetObjectItem(json, "position"))
	{
		model.setIdentity();
		Vector3 position = readJSONVector3(json, "position", Vector3());
		model.translate(position.x, position.y, position.z);
	}

	if (cJSON_GetObjectItem(json, "intensity"))
	{
		intensity = cJSON_GetObjectItem(json, "intensity")->valuedouble;
	}

	if (cJSON_GetObjectItem(json, "max_dist"))
	{
		max_distance = cJSON_GetObjectItem(json, "max_dist")->valuedouble;
	}

	if (cJSON_GetObjectItem(json, "cone_angle"))
	{
		cone_angle = cJSON_GetObjectItem(json, "cone_angle")->valuedouble;
	}

	if (cJSON_GetObjectItem(json, "area_size"))
	{
		area_size = cJSON_GetObjectItem(json, "area_size")->valuedouble;
		ortho_cam_size = cJSON_GetObjectItem(json, "area_size")->valuedouble;
	}
	
	if (cJSON_GetObjectItem(json, "cone_exp"))
	{
		spot_exponent = cJSON_GetObjectItem(json, "cone_exp")->valuedouble;
	}
	
	if (cJSON_GetObjectItem(json, "cast_shadows"))
	{
		cast_shadow = (bool)cJSON_GetObjectItem(json, "cast_shadows")->valueint;
	}

	if (cJSON_GetObjectItem(json, "shadow_bias"))
	{
		shadow_bias = cJSON_GetObjectItem(json, "shadow_bias")->valuedouble;
	}
	
	if (cJSON_GetObjectItem(json, "target"))
	{
		target = readJSONVector3(json, "target", Vector3());
		Vector3 front = target - model.getTranslation();
		model.setFrontAndOrthonormalize(front);
		updateCamera();
	}

	if (cJSON_GetObjectItem(json, "angle"))
	{
		float angle = cJSON_GetObjectItem(json, "angle")->valuedouble;
		model.rotate(angle * DEG2RAD, Vector3(0, 1, 0));
		updateCamera();
	}
	
}

void GTR::LightEntity::renderInMenu()
{
#ifndef SKIP_IMGUI
	bool changed = false;
	ImGuiMatrix44(model, "Model");
	if (light_type == DIRECTIONAL)
	{
		ImGui::Checkbox("Render", &render_light);
		ImGui::SliderFloat("Area size", &ortho_cam_size, 0, 5000);
	}
	if (light_type == POINT)
	{
		ImGui::Checkbox("Render", &render_light);
	}
	ImGui::Combo("Light Type", (int*)&light_type, "DIRECTIONAL\0SPOT\0POINT", 3);
	ImGui::ColorEdit3("Color", color.v);
	ImGui::SliderFloat("Intensity", &intensity, 0, 10);
	changed |= ImGui::SliderFloat3("Target Position", &target.x, -1000, 1000);
	ImGui::SliderFloat("Max distance", &max_distance, 0, 5000);
	ImGui::Checkbox("Cast Shadow", &cast_shadow);
	ImGui::SliderFloat("Shadow Bias", &shadow_bias, 0, 0.05);
	if (light_type == SPOT)
	{
		ImGui::SliderFloat("Cone angle", &cone_angle, 0, 89);
		ImGui::SliderFloat("Spot exponent", &spot_exponent, 0, 100);
	}

	if (changed)
	{
		Vector3 front = target - model.getTranslation();
		model.setFrontAndOrthonormalize(front);
		updateCamera();
	}
#endif
}

void GTR::LightEntity::updateCamera() 
{	
	camera->lookAt(
		model.getTranslation(),
		model.getTranslation() + model.frontVector(),
		Vector3(0, 1.001, 0)); //light cannot be vertical
	
	float cam_size = 0;
	
	switch (light_type)
	{
	case SPOT:
		camera->setPerspective(2 * cone_angle, Application::instance->window_width / (float)Application::instance->window_height, 1.0f, max_distance);
		break;
	case DIRECTIONAL:
		cam_size = (float)ortho_cam_size / (float)2.0;
		camera->setOrthographic(-cam_size, cam_size, -cam_size, cam_size, 1.0f, ortho_cam_size);
	//	camera->setOrthographic(-ortho_cam_size, ortho_cam_size, -ortho_cam_size, ortho_cam_size, 1.0f, max_distance);
		break;
	case POINT:
		camera->setPerspective(90.0f, Application::instance->window_width / (float)Application::instance->window_height, 1.0f, max_distance);
		break;
	}
	
}

//render shadow fbo on the top right corner of viewport
void GTR::LightEntity::renderShadowFBO(Shader* shader)
{
	float w = Application::instance->window_width;
	float h = Application::instance->window_height;

	glViewport(w - (w / 3), h - (h / 3), w / 3, h / 3);
	glScissor(w - (w / 3), h - (h / 3), w / 3, h / 3);
	glEnable(GL_SCISSOR_TEST);

	shader->enable();
	glDisable(GL_DEPTH_TEST);

	shader->setUniform("u_camera_nearfar", Vector2(camera->near_plane, camera->far_plane));

	shadow_fbo->depth_texture->toViewport(shader);

	glEnable(GL_DEPTH_TEST);
	shader->disable();

	//restore viewport
	glDisable(GL_SCISSOR_TEST);
	glViewport(0, 0, w, h);
}

//render a basic mesh on the position of the light
void GTR::LightEntity::renderLight(Camera* camera)
{
	Mesh* mesh = new Mesh();
	if (light_type == DIRECTIONAL) {
		mesh->createPlane(50);
	}
	else {
		mesh = Mesh::Get("data/meshes/sphere.obj", false);
	}
	Shader* basic_shader = Shader::getDefaultShader("flat");
	basic_shader->enable();
	glDisable(GL_DEPTH_TEST);
	basic_shader->setUniform("u_color", Vector4(1.0, 1.0, 1.0, 1.0));
	//plane mesh was not perfectly aligned
	Matrix44 m = model;
	m.rotate(90, Vector3(1, 0, 0));
	basic_shader->setUniform("u_model", m );
	basic_shader->setUniform("u_camera_position", camera->eye);
	basic_shader->setUniform("u_viewprojection", camera->viewprojection_matrix);
	mesh->render(GL_TRIANGLES);
	glEnable(GL_DEPTH_TEST);
	basic_shader->disable();
}
