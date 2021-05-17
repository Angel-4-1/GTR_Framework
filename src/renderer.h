#pragma once
#include "prefab.h"
#include "fbo.h"

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
		SHOW_MULTIPASS = 0,
		SHOW_TEXTURE = 1,
		SHOW_NORMAL = 2,
		SHOW_NORMALMAP = 3,
		SHOW_UVS = 4,
		SHOW_OCCLUSION = 5,
		SHOW_METALLIC  = 6,
		SHOW_ROUGHNESS = 7,
		SHOW_SINGLEPASS = 8,
		SHOW_SHADOWMAP = 9,
		SHOW_GBUFFERS = 10
	};

	enum ePipelineMode {
		FORWARD,
		DEFERRED
	};

	enum eQuality {
		LOW,
		MEDIUM,
		HIGH,
		ULTRA
	};

	class renderCall {
	public:
		Mesh* mesh;
		Material* material;
		Matrix44 model;
		float distance_to_camera;
		bool isAlpha;	// has transparency?

		renderCall() {
			isAlpha = false;
			distance_to_camera = 9999.0;
		}

		void set(Mesh* _mesh, Material* _material, Matrix44 _model) {
			mesh = _mesh;
			material = _material;
			model = _model;

			//has transparency?
			if (material->alpha_mode == GTR::eAlphaMode::BLEND)
				isAlpha = true;
			else
				isAlpha = false;
		}
	};
	
	// This class is in charge of rendering anything in our system.
	// Separating the render from anything else makes the code cleaner
	class Renderer
	{

	public:

		FBO fbo;
		FBO shadow_singlepass;
		FBO gbuffers_fbo;
		FBO illumination_fbo;
		Texture* color_buffer;
		eRendererCondition renderer_cond;
		eRenderMode render_mode;
		ePipelineMode pipeline_mode;
		eQuality quality;
		std::vector< renderCall > render_calls;
		std::vector< LightEntity* > lights;

		//some flags
		bool rendering_shadowmap = false;
		bool show_depth_camera = false;
		bool show_gbuffers = false;
		bool isRenderingBoundingBox = false;
		int light_camera;	//light to show on depth camera

		float computeDistanceToCamera(Matrix44 node_model, Mesh* mesh, Vector3 cam_pos);

		Renderer();
		
		//renders several elements of the scene
		void renderScene(GTR::Scene* scene, Camera* camera);
		void renderToFBO(GTR::Scene* scene, Camera* camera);

		//create shadowmaps for each light of the scene
		void createShadowMaps(Scene* scene, Camera* camera, bool singlepass = false);

		//obtain the shader to use
		Shader* getShader();

		//create the render calls + sort them 
		void createRenderCalls(GTR::Scene* scene, Camera* camera);
	
		//to render a whole prefab (with all its nodes)
		void prefabToNode(const Matrix44& model, GTR::Prefab* prefab, Camera* camera);

		//to render one node from the prefab and its children
		void nodeToRenderCall(const Matrix44& model, GTR::Node* node, Camera* camera);

		void renderForward(GTR::Scene* scene, std::vector< renderCall >& data, Camera* camera);
		void renderDeferred(GTR::Scene* scene, std::vector< renderCall >& data, Camera* camera);

		//show gBuffers
		void renderGBuffers(Camera* camera);

		//upload shadrer uniforms used for the deferred
		void uploadDefferedUniforms(Shader* shader, GTR::Scene* scene, Camera* camera);

		//use gBufferes to reconstruct the scene
		void renderReconstructedScene(GTR::Scene* scene, Camera* camera);

		//to render one mesh given its material and transformation matrix
		void renderMeshWithMaterial(const Matrix44 model, Mesh* mesh, GTR::Material* material, Camera* camera);

		//how to render with lights
		void renderMultiPass(Shader* shader, Mesh* mesh, bool sendShadowMap = false);
		void renderSinglePass(Shader* shader, Mesh* mesh, bool sendShadowMap = false);
		
		//show debug menu using IMGUI
		void renderInMenu();

		//show lights with a basic mesh
		void renderLights(Camera* camera);

		void changeQualityFBO();

		void resizeFBOs();
	};

	Texture* CubemapFromHDRE(const char* filename);

};