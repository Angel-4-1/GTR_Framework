#pragma once
#include "prefab.h"

//forward declarations
class Camera;

namespace GTR {

	class Prefab;
	class Material;

	enum eRendererCondition {
		REND_COND_NONE= 0,
		REND_COND_ALPHA = 1,
		REND_COND_NO_ALPHA = 2
	};

	enum eRenderMode {
		SHOW_DEFAULT = 0,
		SHOW_TEXTURE = 1,
		SHOW_NORMAL = 2,
		SHOW_NORMALMAP = 3,
		SHOW_UVS = 4,
		SHOW_OCCLUSION = 5,
		SHOW_METALLIC  = 6,
		SHOW_ROUGHNESS = 7
	};

	class renderCall {
	public:
		Node* node;
		float distance_to_camera;
		Matrix44* prefab_model;
		bool isAlpha;

		renderCall() {
			isAlpha = false;
			distance_to_camera = 9999.0;
		}
	};
	
	// This class is in charge of rendering anything in our system.
	// Separating the render from anything else makes the code cleaner
	class Renderer
	{

	public:

		//add here your functions
		bool isRenderingBoundingBox = false;
		eRendererCondition renderer_cond = eRendererCondition::REND_COND_NONE;
		std::vector< renderCall > render_calls;		// store nodes 
		void createRenderCalls(GTR::Scene* scene, Camera* camera);
		void checkAlphaComponent(GTR::Node* node, Matrix44* prefab_model, Vector3 cam_pos);
		void renderInMenu();
		void renderSingleNode(const Matrix44& prefab_model, GTR::Node* node, Camera* camera, bool hasAlpha);
		void renderRenderCalls(std::vector< renderCall > data, Camera* camera);
		float computeDistanceToCamera(GTR::Node* node, Matrix44* prefab_model, Vector3 cam_pos);
		LightEntity* light;
		
		Renderer();
		eRenderMode render_mode;

		//renders several elements of the scene
		void renderScene(GTR::Scene* scene, Camera* camera);
	
		//to render a whole prefab (with all its nodes)
		void renderPrefab(const Matrix44& model, GTR::Prefab* prefab, Camera* camera);

		//to render one node from the prefab and its children
		void renderNode(const Matrix44& model, GTR::Node* node, Camera* camera);

		//to render one mesh given its material and transformation matrix
		void renderMeshWithMaterial(const Matrix44 model, Mesh* mesh, GTR::Material* material, Camera* camera);
	};

	Texture* CubemapFromHDRE(const char* filename);

};