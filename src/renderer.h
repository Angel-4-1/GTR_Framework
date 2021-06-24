#pragma once
#include "prefab.h"
#include "fbo.h"

//forward declarations
class Camera;
class HDRE;

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
		SHOW_GBUFFERS = 10,
		SHOW_NONE = 20
	};

	enum eRenderDeferredMode {
		DEFERRED_PBR = 0,
		DEFERRED_SHADOWMAP = 1
	};

	enum ePipelineMode {
		FORWARD,
		DEFERRED,
		NO_PIPELINE
	};

	enum eQuality {
		LOW,
		MEDIUM,
		HIGH,
		ULTRA
	};

	enum ePostFX {
		FX_MOTION_BLUR,
		FX_PIXELATED,
		FX_BLUR,
		FX_DEPTH_OF_FIELD
	};

	class renderCall {
	public:
		Mesh* mesh;
		Material* material;
		Matrix44 model;
		float distance_to_camera;
		bool isAlpha;	// has transparency?
		sReflectionProbe* nearest_reflection_probe;

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

	class toneMapper {
	public:
		float gamma = 0;
		float scale = 0;
		float white_lum = 0;
		float average_lum = 0;

		toneMapper() {
			this->init();
		}

		toneMapper(float _gamma, float _scale, float _white_lum,float _average_lum) {
			gamma = _gamma;
			scale = _scale;
			white_lum = _white_lum;
			average_lum = _average_lum;
		}

		void init() {
			gamma = 2.2;
			scale = 0.8;
			white_lum = 1.6;
			average_lum = 1.4;
		}

		void uploadToShader(Shader* shader) {
			float inverse_gamma = 1.0 / gamma;
			shader->setUniform("u_igamma", inverse_gamma);
			shader->setUniform("u_gamma", gamma);
			shader->setUniform("u_scale", scale);
			float lumwhite = white_lum * white_lum;
			shader->setUniform("u_lumwhite2", lumwhite);
			shader->setUniform("u_average_lum", average_lum);
		}
	};

	class SSAOFX {
	public:
		float intensity;
		std::vector<Vector3> points;

		SSAOFX();
		void apply(Texture* depth_buffer, Texture* normal_buffer, Camera* cam, Texture* output);
	};
	
	// This class is in charge of rendering anything in our system.
	// Separating the render from anything else makes the code cleaner
	class Renderer
	{

	public:

		FBO fbo;
		FBO shadow_singlepass;
		FBO gbuffers_fbo;
		FBO decals_fbo;
		FBO illumination_fbo;
		FBO ssao_fbo;
		FBO gamma_fbo;
		FBO* irr_fbo;
		FBO blur_fbo;
		FBO reflection_fbo;

		Texture* color_buffer;
		Texture* ao_buffer;
		Texture* blur_ao_buffer;
		Texture* probes_texture;

		eRendererCondition renderer_cond;
		eRenderMode render_mode;
		eRenderDeferredMode render_deferred_mode;
		ePipelineMode pipeline_mode;
		eQuality quality;
		ePostFX post_fx;
		std::vector< renderCall > render_calls;
		std::vector< LightEntity* > lights;
		IrradianceEntity* irr;
		ReflectionEntity* reflection_entity;
		SSAOFX ssao;
		toneMapper tone_mapper;

		//some flags
		bool show_ao = false;
		bool rendering_shadowmap = false;
		bool show_depth_camera = false;
		bool show_gbuffers = false;
		bool show_gbuffers_alpha = false;
		bool isRenderingBoundingBox = false;
		bool linear_correction = true;
		bool use_tone_mapper = true;
		bool use_dithering = false;
		bool apply_irradiance = false;
		bool use_irradiance = false;
		bool show_probes = true;
		bool apply_skybox = true;
		bool show_irradiance_coeffs = false;
		bool freeze_prev_vp = false;
		bool apply_post_fx = false;
		bool use_reflection = true;
		bool show_reflection_probes = true;
		int light_camera;	//light to show on depth camera

		//PostFX
		int pixel_size = 5;
		int blur_size = 5;

		Matrix44 vp_previous;

		float computeDistanceToCamera(Matrix44 node_model, Mesh* mesh, Vector3 cam_pos);

		Renderer();
		
		//renders several elements of the scene
		void renderScene(GTR::Scene* scene, Camera* camera);
		void renderToFBO(GTR::Scene* scene, Camera* camera);

		//create shadowmaps for each light of the scene
		void createShadowMaps(Scene* scene, Camera* camera);
		void createShadowMapsUsingForward(Scene* scene, Camera* camera);

		//obtain the shader to use
		Shader* getShader(ePipelineMode pm, eRenderMode rm);

		//create the render calls + sort them 
		void createRenderCalls(GTR::Scene* scene, Camera* camera);
	
		//to render a whole prefab (with all its nodes)
		void prefabToNode(const Matrix44& model, GTR::Prefab* prefab, Camera* camera, sReflectionProbe* _nearest_reflection_probe = NULL);

		//to render one node from the prefab and its children
		void nodeToRenderCall(const Matrix44& model, GTR::Node* node, Camera* camera, sReflectionProbe* _nearest_reflection_probe = NULL);

		void renderForward(GTR::Scene* scene, std::vector< renderCall >& data, Camera* camera, ePipelineMode pipeline = NO_PIPELINE, eRenderMode mode = SHOW_NONE);
		void renderDeferred(GTR::Scene* scene, std::vector< renderCall >& data, Camera* camera);

		//show gBuffers
		void renderGBuffers(Camera* camera);

		//upload shadrer uniforms used for the deferred
		void uploadDefferedUniforms(Shader* shader, GTR::Scene* scene, Camera* camera);

		//use gBufferes to reconstruct the scene
		void renderReconstructedScene(GTR::Scene* scene, Camera* camera);
		void renderVolumetricLights(GTR::Scene* scene, Camera* camera);

		//to render one mesh given its material and transformation matrix
		void renderMeshWithMaterial(const Matrix44 model, Mesh* mesh, GTR::Material* material, Camera* camera, Shader* sh = NULL, ePipelineMode pipeline = NO_PIPELINE,eRenderMode mode = SHOW_NONE, sReflectionProbe* _nearest_reflection_probe = NULL);

		//how to render with lights
		void renderMultiPass(Shader* shader, Mesh* mesh, bool sendShadowMap = false, sReflectionProbe* _nearest_reflection_probe = NULL);
		void renderSinglePass(Shader* shader, Mesh* mesh);
		
		//render materials with alpha on deferred
		void renderAlphaElements(std::vector< renderCall >& data, Camera* camera);

		//show debug menu using IMGUI
		void renderInMenu();

		//show lights with a basic mesh
		void renderLights(Camera* camera);

		void changeQualityFBO();

		void resizeFBOs();
		void renderSkybox(Texture* skybox, Camera* camera);

		void updateIrradianceCache(GTR::Scene* scene);
		void extractProbe(GTR::Scene* scene, sProbe& p);
		void storeIrradianceToTexture();
		void updateReflectionProbes(GTR::Scene* scene);

		void renderDecals(GTR::Scene* scene, Camera* camera);

		void renderPostFX(Camera* camera, Texture* texture);

		void readIrradiance(GTR::Scene* scene);
	};

	Texture* CubemapFromHDRE(const char* filename);

};