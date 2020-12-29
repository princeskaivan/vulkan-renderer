/*#include "Shader.h"
#include "VulkanContext.h"

#include <spirv_reflect.h>

#include <fstream>

std::vector<std::shared_ptr<Shader>> Shader::s_shader_library;

std::shared_ptr<Shader> Shader::create(const std::filesystem::path& vert_spv_path, const std::filesystem::path& frag_spv_path) {
	auto it = std::find_if(s_shader_library.begin(), s_shader_library.end(), [&](const std::shared_ptr<Shader>& s) {
		return s->m_vert_shader_path == vert_spv_path && s->m_frag_shader_path == frag_spv_path;
	});

	if (it != s_shader_library.end())
		return *it;
	
	std::shared_ptr<Shader> new_shader = std::shared_ptr<Shader>(new Shader(vert_spv_path, frag_spv_path));
	s_shader_library.push_back(new_shader);

	return new_shader;
}

Shader::Shader(const std::filesystem::path& vert_spv_path, const std::filesystem::path& frag_spv_path) :
	m_vert_shader_path(vert_spv_path), m_frag_shader_path(frag_spv_path)
{
	std::vector<uint8_t> vert_spv_code = load_spirv_code(vert_spv_path);
	std::vector<uint8_t> frag_spv_code = load_spirv_code(frag_spv_path);

	m_vertex_module = VulkanContext::create_shader_module(vert_spv_code);
	m_fragment_module = VulkanContext::create_shader_module(frag_spv_code);

	reflect_shader(vert_spv_code);
	reflect_shader(frag_spv_code);

	create_descriptor_set_layouts();

	m_vertex_create_info = create_pipeline_shader_stage_create_info(m_vertex_module, VK_SHADER_STAGE_VERTEX_BIT);
	m_fragment_create_info = create_pipeline_shader_stage_create_info(m_fragment_module, VK_SHADER_STAGE_FRAGMENT_BIT);
}

Shader::~Shader() {
	for (auto& set : m_descriptor_sets_info)
		VulkanContext::destroy_descriptor_set_layout(set.layout);

	VulkanContext::destroy_pipeline_layout(m_pipeline_layout);

	VulkanContext::destroy_shader_module(m_vertex_module);
	VulkanContext::destroy_shader_module(m_fragment_module);
}

std::vector<uint8_t> Shader::load_spirv_code(const std::filesystem::path& spv_path) {
	if (!std::filesystem::exists(spv_path))
		throw std::runtime_error("Shader doesn't exist");

	size_t code_size = std::filesystem::file_size(spv_path);
	size_t zeros_count = (4 - (code_size % 4)) % 4;

	std::vector<uint8_t> spv_code;
	spv_code.reserve(code_size + zeros_count);

	std::basic_ifstream<uint8_t> spv_file(spv_path, std::ios::binary);

	spv_code.insert(spv_code.end(), std::istreambuf_iterator<uint8_t>(spv_file), std::istreambuf_iterator<uint8_t>());
	spv_code.insert(spv_code.end(), zeros_count, 0);

	return spv_code;
}

void Shader::reflect_shader(const std::vector<uint8_t>& spv_code) {
	spv_reflect::ShaderModule reflect_module(spv_code);
	if (reflect_module.GetResult() != SPV_REFLECT_RESULT_SUCCESS)
		throw std::runtime_error("Couldn't relfect shader");

	if (reflect_module.GetShaderStage() == SPV_REFLECT_SHADER_STAGE_VERTEX_BIT)
		reflect_vertex_shader(reflect_module);
	else if (reflect_module.GetShaderStage() == SPV_REFLECT_SHADER_STAGE_FRAGMENT_BIT)
		reflect_fragment_shader(reflect_module);
	else
		throw std::runtime_error("Shader stage not supported");
}

void Shader::reflect_vertex_shader(spv_reflect::ShaderModule& shader_module) {
	uint32_t set_count = 0;
	shader_module.EnumerateDescriptorSets(&set_count, nullptr);
	std::vector<SpvReflectDescriptorSet*> sets(set_count);
	shader_module.EnumerateDescriptorSets(&set_count, sets.data());

	reflect_descriptor_sets(sets);

	uint32_t input_var_count = 0;
	shader_module.EnumerateInputVariables(&input_var_count, nullptr);
	std::vector<SpvReflectInterfaceVariable*> input_vars(input_var_count);
	shader_module.EnumerateInputVariables(&input_var_count, input_vars.data());


}

void Shader::reflect_fragment_shader(spv_reflect::ShaderModule& shader_module) {
	uint32_t set_count = 0;
	shader_module.EnumerateDescriptorSets(&set_count, nullptr);
	std::vector<SpvReflectDescriptorSet*> sets(set_count);
	shader_module.EnumerateDescriptorSets(&set_count, sets.data());

	reflect_descriptor_sets(sets);
}

void Shader::reflect_descriptor_sets(const std::vector<SpvReflectDescriptorSet*>& sets) {
	for (SpvReflectDescriptorSet* set : sets) {
		auto set_it = std::find_if(m_descriptor_sets_info.begin(), m_descriptor_sets_info.end(), [set = set->set](const auto& dsi) {
			return dsi.set_info.set == set;
		});

		if (set_it == std::end(m_descriptor_sets_info)) {
			DescriptorSetInfo dsi;
			dsi.set_info.set = set->set;

			m_descriptor_sets_info.push_back(std::move(dsi));

			std::sort(m_descriptor_sets_info.begin(), m_descriptor_sets_info.end());

			set_it = std::find_if(m_descriptor_sets_info.begin(), m_descriptor_sets_info.end(), [set = set->set](const auto& dsi) {
				return dsi.set_info.set == set;
			});
		}

		std::vector<UniformInfo>& uniforms = set_it->set_info.uniforms;
		for (size_t i = 0; i < set->binding_count; i++)
			reflect_descriptor_binding(uniforms, set->bindings[i]);

		std::sort(uniforms.begin(), uniforms.end());
	}
}

void Shader::reflect_descriptor_binding(std::vector<UniformInfo>& uniforms, SpvReflectDescriptorBinding* binding) {
	if (std::find_if(uniforms.begin(), uniforms.end(), [=](const auto& ui) { return ui.binding == binding->binding; }) != end(uniforms))
		throw std::runtime_error("Binding rebinding");

	UniformInfo uniform{
		.name = binding->name,
		.type = (UniformType)binding->descriptor_type,
		.binding = binding->binding,
		.count = binding->count
	};

	uniforms.push_back(std::move(uniform));
}

void Shader::create_descriptor_set_layouts() {
	for (DescriptorSetInfo& set_info : m_descriptor_sets_info) {
		std::vector<VkDescriptorSetLayoutBinding> bindings;
		bindings.reserve(set_info.set_info.uniforms.size());

		for (const auto& uniform_info : set_info.set_info.uniforms) {
			VkDescriptorSetLayoutBinding binding_layout{
				.binding = uniform_info.binding,
				.descriptorType = (VkDescriptorType)uniform_info.type,
				.descriptorCount = uniform_info.count
			};

			bindings.push_back(binding_layout);
		}

		set_info.layout = VulkanContext::create_descriptor_set_layout(bindings);
	}
}

void Shader::create_pipeline_layout() {
	std::vector<VkDescriptorSetLayout> layouts;
	layouts.reserve(m_descriptor_sets_info.size());

	for (const auto& set : m_descriptor_sets_info)
		layouts.push_back(set.layout);

	m_pipeline_layout = VulkanContext::create_pipeline_layout(layouts, {});
}

VkPipelineShaderStageCreateInfo Shader::create_pipeline_shader_stage_create_info(VkShaderModule shader_module, VkShaderStageFlags stage) {
	VkPipelineShaderStageCreateInfo stage_info{
		.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
		.stage = (VkShaderStageFlagBits)stage,
		.module = shader_module,
		.pName = "main"
	};

	return stage_info;
}*/