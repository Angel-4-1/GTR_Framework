#ifndef SCENE_H
#define SCENE_H

#include "framework.h"
#include "shader.h"
#include "fbo.h"
#include "camera.h"
#include "mesh.h"
#include "sphericalharmonics.h"
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
		REFLECTION_ENTITY = 5,
		DECAL = 6,
		IRRADIANCE = 7
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

	//struct to store reflection probes info
	class sReflectionProbe : public GTR::BaseEntity {
	public:
		Texture* cubemap = NULL;
		float size;

		sReflectionProbe();
		virtual void configure(cJSON* json);
	};

	//represents one prefab in the scene
	class PrefabEntity : public GTR::BaseEntity
	{
	public:
		std::string filename;
		Prefab* prefab;
		sReflectionProbe* nearest_reflection_probe;

		PrefabEntity();
		virtual void renderInMenu();
		virtual void configure(cJSON* json);
		void updateNearestReflectionProbe();
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
		float ortho_cam_size = 500;
		Vector3 target;
		eLightType light_type;
		bool is_volumetric;

		//For shadows
		Camera* camera;
		FBO* shadow_fbo;
		float shadow_bias;
		bool cast_shadow = false;

		//Render
		bool render_light;

		LightEntity();
		void uploadToShader(Shader* shader, bool sendShadowMap = false);
		virtual void renderInMenu();
		virtual void configure(cJSON* json);
		void updateCamera();
		void renderShadowFBO(Shader* shader);
		void renderLight(Camera* camera);
	};


	//struct to store probes
	class sProbe {
	public:
		Vector3 pos; //where is located
		Vector3 local; //its ijk pos in the matrix
		int index; //its index in the linear array
		float size;
		SphericalHarmonics sh; //coeffs

		void render(Camera* cam);
	};

	class IrradianceEntity : public GTR::BaseEntity
	{
	public:
		int dimensions[3];	//Dimensions of axis x,y,z
		float scale;		//separation within probes
		float size;			//size of each probe

		//where to store the probes
		std::vector<sProbe> probes;

		IrradianceEntity();
		virtual void configure(cJSON* json);
		void render(Shader* shader, Camera* camera);
		void init(Vector3 dim, float _scale, float _size);
		void placeProbes();
		void renderInMenu();
		void uploadToShader(Shader* shader);

		Vector3 dim = Vector3(8, 6, 12);
		Vector3 start_pos = Vector3(-200, 10, -350);
		Vector3 end_pos = Vector3(550, 250, 450);	
		Vector3 delta;
	};


	class ReflectionEntity : public GTR::BaseEntity
	{
	public:
		std::vector<sReflectionProbe*> reflection_probes;
		float size;			//size of each probe

		ReflectionEntity();
		void placeProbes();
		void render(Camera* camera);
		virtual void configure(cJSON* json);
	};

	class DecalEntity : public GTR::BaseEntity
	{
	public:
		Texture* albedo;

		DecalEntity();
		virtual void configure(cJSON* json);
	};

	//contains all entities of the scene
	class Scene
	{
	public:
		static Scene* instance;

		Vector3 background_color;
		Vector3 ambient_light;

		std::string environment_file;
		Texture* environment;
		Scene();
		Camera main_camera;

		std::string filename;
		std::vector<BaseEntity*> entities;
		std::vector<LightEntity*> lights;
		IrradianceEntity* irr;
		ReflectionEntity* reflection;
		std::vector<sReflectionProbe*> reflect_probes;

		void clear();
		void addEntity(BaseEntity* entity);
		void updatePrefabNearestReflectionProbe();
		bool load(const char* filename);
		BaseEntity* createEntity(std::string type);
	};

};

#endif