#include "scene.h"
#include "utils.h"

#include "prefab.h"
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
	intensity = 0;
	max_distance = 0;
	cone_angle = 45;
	area_size = 0;
	spot_exponent = 1;
}

void GTR::LightEntity::uploadToShader(Shader* shader)
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
}


void GTR::LightEntity::configure(cJSON* json)
{
	if (cJSON_GetObjectItem(json, "category"))
	{
		std::string category = cJSON_GetObjectItem(json, "category")->valuestring;
		
		if (category == "POINT")
			light_type = POINT;
		else if ( category == "SPOT")
			light_type = SPOT;
		else if (category == "DIRECTIONAL")
			light_type = DIRECTIONAL;
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

	if (cJSON_GetObjectItem(json, "max_distance"))
	{
		max_distance = cJSON_GetObjectItem(json, "max_distance")->valuedouble;
	}

	if (cJSON_GetObjectItem(json, "cone_angle"))
	{
		cone_angle = cJSON_GetObjectItem(json, "cone_angle")->valuedouble;
	}

	if (cJSON_GetObjectItem(json, "area_size"))
	{
		area_size = cJSON_GetObjectItem(json, "area_size")->valuedouble;
	}
	
	if (cJSON_GetObjectItem(json, "spot_exponent"))
	{
		spot_exponent = cJSON_GetObjectItem(json, "spot_exponent")->valuedouble;
	}
	
	if (cJSON_GetObjectItem(json, "direction"))
	{
		directional_vector = readJSONVector3(json, "direction", Vector3());
		model.setFrontAndOrthonormalize(directional_vector);
	}
}

void GTR::LightEntity::renderInMenu()
{
#ifndef SKIP_IMGUI
	bool changed = false;
	ImGuiMatrix44(model, "Model");
	ImGui::Combo("Light Type", (int*)&light_type, "DIRECTIONAL\0SPOT\0POINT", 3);
	ImGui::ColorEdit3("Color", color.v);
	ImGui::SliderFloat("Intensity", &intensity, 0, 10);
	changed |= ImGui::SliderFloat3("Direction", &directional_vector.x, -10, 10);
	ImGui::SliderFloat("Max distance", &max_distance, 0, 5000);
	ImGui::SliderFloat("Area size", &area_size, 0, 1000);
	if (light_type == SPOT)
	{
		ImGui::SliderFloat("Cone angle", &cone_angle, 0, 90);
		ImGui::SliderFloat("Spot exponent", &spot_exponent, 0, 100);
	}
	
	if(changed)
		model.setFrontAndOrthonormalize(directional_vector);
#endif
}