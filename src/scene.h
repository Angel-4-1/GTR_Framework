#ifndef SCENE_H
#define SCENE_H

#include "framework.h"
#include "shader.h"
#include "fbo.h"
#include "camera.h"
#include "mesh.h"
#include <string>

//forward declaration
class cJSON;


//our namespace
namespace GTR {



	enum eEntityType {
		NONE = 0,
		PREFAB = 1,
		LIGHT = 2,
		CAMERA = 3,
		REFLECTION_PROBE = 4,
		DECALL = 5
	};

	class Scene;
	class Prefab;

	//represents one element of the scene (could be lights, prefabs, cameras, etc)
	class BaseEntity
	{
	public:
		Scene* scene;
		std::string name;
		eEntityType entity_type;
		Matrix44 model;
		bool visible;
		BaseEntity() { entity_type = NONE; visible = true; }
		virtual ~BaseEntity() {}
		virtual void renderInMenu();
		virtual void configure(cJSON* json) {}
	};

	//represents one prefab in the scene
	class PrefabEntity : public GTR::BaseEntity
	{
	public:
		std::string filename;
		Prefab* prefab;

		PrefabEntity();
		virtual void renderInMenu();
		virtual void configure(cJSON* json);
	};

	enum eLightType {
		DIRECTIONAL = 0,
		SPOT = 1,
		POINT = 2
	};

	//represents one light in the scene
	class LightEntity : public GTR::BaseEntity
	{
	public:
		Vector3 color; //color of the light
		float intensity; //amount of light emitted
		float max_distance;	//how far the light can reach
		float cone_angle;	//angle in degrees of the conce spotlight
		float area_size;	//size of the volume for directional light
		float spot_exponent;
		Vector3 target;
		eLightType light_type;

		//For shadows
		Camera* camera;
		FBO* shadow_fbo;
		float shadow_bias;

		//Render
		Mesh* mesh;
		bool render_light;

		LightEntity();
		void uploadToShader(Shader* shader);
		virtual void renderInMenu();
		virtual void configure(cJSON* json);
		void updateCamera();
		void renderShadowFBO(Shader* shader);
		void renderLight(Camera* camera);
	};

	//contains all entities of the scene
	class Scene
	{
	public:
		static Scene* instance;

		Vector3 background_color;
		Vector3 ambient_light;

		Scene();

		std::string filename;
		std::vector<BaseEntity*> entities;
		std::vector<LightEntity*> lights;

		void clear();
		void addEntity(BaseEntity* entity);

		bool load(const char* filename);
		BaseEntity* createEntity(std::string type);
	};

};

#endif