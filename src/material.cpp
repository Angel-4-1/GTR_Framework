
#include "material.h"

#include "includes.h"
#include "texture.h"

using namespace GTR;

std::map<std::string, Material*> Material::sMaterials;

Material* Material::Get(const char* name)
{
	assert(name);
	std::map<std::string, Material*>::iterator it = sMaterials.find(name);
	if (it != sMaterials.end())
		return it->second;
	return NULL;
}

void Material::registerMaterial(const char* name)
{
	this->name = name;
	sMaterials[name] = this;

	// Ugly Hack for clouds sorting problem
	if (!strcmp(name, "Clouds"))
	{
		_zMin = 0.9f;
		_zMax = 1.0f;
	}
}

Material::~Material()
{
	if (name.size())
	{
		auto it = sMaterials.find(name);
		if (it != sMaterials.end())
			sMaterials.erase(it);
	}
}

void Material::Release()
{
	std::vector<Material *>mats;

	for (auto mp : sMaterials)
	{
		Material *m = mp.second;
		mats.push_back(m);
	}

	for (Material *m : mats)
	{
		delete m;
	}
	sMaterials.clear();
}


void Material::renderInMenu()
{
#ifndef SKIP_IMGUI
	ImGui::Text("Name: %s", name.c_str()); // Show String
	ImGui::Checkbox("Two sided", &two_sided);
	ImGui::Combo("AlphaMode", (int*)&alpha_mode, "NO_ALPHA\0MASK\0BLEND", 3);
	ImGui::SliderFloat("Alpha Cutoff", &alpha_cutoff, 0.0f, 1.0f);
	ImGui::ColorEdit4("Color", color.v); // Edit 4 floats representing a color + alpha
	ImGui::ColorEdit3("Emissive", emissive_factor.v); // Edit 4 floats representing a color + alpha
	if (color_texture.texture && ImGui::TreeNode(color_texture.texture, "Color Texture"))
	{
		int w = ImGui::GetColumnWidth();
		float aspect = color_texture.texture->width / (float)color_texture.texture->height;
		ImGui::Image((void*)(intptr_t)color_texture.texture->texture_id, ImVec2(w, w * aspect));
		ImGui::TreePop();
	}
#endif
}

void GTR::Material::uploadToShader(Shader* shader, bool apply_linear_correction, float gamma)
{
	Vector4 final_color = color;
	if (apply_linear_correction)
	{
		final_color.x = pow(color.x, gamma);
		final_color.y = pow(color.x, gamma);
		final_color.z = pow(color.y, gamma);
		final_color.w = pow(color.w, gamma);
	}
	//Color
	shader->setUniform("u_color", final_color);

	//Color texture
	Texture* texture = NULL;
	texture = color_texture.texture;
	if (texture == NULL)
		texture = Texture::getWhiteTexture(); //a 1x1 white texture

	if (texture)
		shader->setUniform("u_texture", texture, 0);

	//Emissive Texture
	Texture* emissive_text = NULL;
	emissive_text = emissive_texture.texture;

	if (emissive_text) {
		shader->setUniform("u_is_emissor", true);
		shader->setUniform("u_emissive_factor", emissive_factor);
		shader->setUniform("u_emissive_texture", emissive_text, 1);
	}
	else {
		shader->setUniform("u_is_emissor", false);
	}

	//Normal texture
	Texture* normal_text = NULL;
	normal_text = normal_texture.texture;

	if (normal_text) {
		shader->setUniform("u_has_normal", true);
		shader->setUniform("u_normal_texture", normal_text, 2);
	}
	else {
		shader->setUniform("u_has_normal", false);
	}

	//Metallic roughness texture
	Texture* metallic_roughness_text = NULL;
	metallic_roughness_text = metallic_roughness_texture.texture;
	if (metallic_roughness_text) {
		shader->setUniform("u_has_metallic_roughness", true);
		shader->setUniform("u_metallic_roughness_texture", metallic_roughness_text, 3);
		shader->setUniform("u_material_shininess", roughness_factor);
		int type_property = 0;
	} 
	else {
		shader->setUniform("u_has_metallic_roughness", false);
	}

	//this is used to say which is the alpha threshold to what we should not paint a pixel on the screen (to cut polygons according to texture alpha)
	shader->setUniform("u_alpha_cutoff", alpha_mode == GTR::eAlphaMode::MASK ? alpha_cutoff : 0);
}
