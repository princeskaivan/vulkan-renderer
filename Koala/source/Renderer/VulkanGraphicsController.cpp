#include "VulkanGraphicsController.h"

#include <spirv_reflect.h>

#include <algorithm>
#include <utility>
#include <stdexcept>

static std::vector<char> const_char_to_vector(const char* data) {
	size_t size = strlen(data);
	
	std::vector<char> v;
	v.reserve(size + 1);

	for (size_t i = 0; i < size; i++)
		v.push_back(data[i]);
	v.push_back(0);

	return v;
}

static uint32_t vk_format_to_size(VkFormat format) {
	switch (format) {
	case VK_FORMAT_R8G8B8A8_UNORM:		return 4 * 1;
	case VK_FORMAT_R8G8B8A8_SNORM:		return 4 * 1;
	case VK_FORMAT_R8G8B8A8_SRGB:		return 4 * 1;
	case VK_FORMAT_B8G8R8A8_UNORM:		return 4 * 1;
	case VK_FORMAT_R16G16B16A16_SFLOAT:	return 4 * 2;
	case VK_FORMAT_R32_UINT:			return 1 * 4;
	case VK_FORMAT_R32_SINT:			return 1 * 4;
	case VK_FORMAT_R32_SFLOAT:			return 1 * 4;
	case VK_FORMAT_R32G32_UINT:			return 2 * 4;
	case VK_FORMAT_R32G32_SINT:			return 2 * 4;
	case VK_FORMAT_R32G32_SFLOAT:		return 2 * 4;
	case VK_FORMAT_R32G32B32_UINT:		return 3 * 4;
	case VK_FORMAT_R32G32B32_SINT:		return 3 * 4;
	case VK_FORMAT_R32G32B32_SFLOAT:	return 3 * 4;
	case VK_FORMAT_R32G32B32A32_UINT:	return 4 * 4;
	case VK_FORMAT_R32G32B32A32_SINT:	return 4 * 4;
	case VK_FORMAT_R32G32B32A32_SFLOAT: return 4 * 4;
	case VK_FORMAT_D24_UNORM_S8_UINT:	return 1 * 3 + 1 * 1;
	case VK_FORMAT_D32_SFLOAT:			return 1;
	case VK_FORMAT_D32_SFLOAT_S8_UINT:	return 1 * 4 + 1 * 1;
	}

	throw std::runtime_error("Unknown format");
}

static bool format_has_stencil(VkFormat format) {
	switch (format) {
	case VK_FORMAT_S8_UINT:
	case VK_FORMAT_D16_UNORM_S8_UINT:
	case VK_FORMAT_D24_UNORM_S8_UINT:
	case VK_FORMAT_D32_SFLOAT_S8_UINT:
		return true;
	default:
		return false;
	}
}

static bool format_has_depth(VkFormat format) {
	switch (format) {
	case VK_FORMAT_D16_UNORM_S8_UINT:
	case VK_FORMAT_D24_UNORM_S8_UINT:
	case VK_FORMAT_D32_SFLOAT:
	case VK_FORMAT_D32_SFLOAT_S8_UINT:
		return true;
	default:
		return false;
	}
}

static std::tuple<VkImageLayout, VkPipelineStageFlags, VkAccessFlags> image_usage_to_layout_stage_access(ImageUsageFlags usage) {
	VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
	VkPipelineStageFlags stages = 0;
	VkAccessFlags access = 0;

	if (usage & ImageUsageNone)
		layout = VK_IMAGE_LAYOUT_UNDEFINED;
	else if (usage & ImageUsageColorAttachment) {
		layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

		stages = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
		access = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
	} else if (usage & ImageUsageDepthStencilAttachment) {
		layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

		stages = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
		access = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
	} else if (usage & ImageUsageDepthStencilReadOnly) {
		layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
	
		stages = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
		access = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
	//} else if (usage & ImageUsageSampled) {
	//	layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	//
	//	stages = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
	//	access = VK_ACCESS_SHADER_READ_BIT;
	} else if (usage & ImageUsageColorSampled) {
		layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

		stages = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
		access = VK_ACCESS_SHADER_READ_BIT;
	} else if (usage & ImageUsageDepthSampled) {
		layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

		stages = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
		access = VK_ACCESS_SHADER_READ_BIT;
	} else if (usage & ImageUsageTransferSrc) {
		layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;

		stages = VK_PIPELINE_STAGE_TRANSFER_BIT;
		access = VK_ACCESS_TRANSFER_READ_BIT;
	} else if (usage & ImageUsageTransferDst) {
		layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;

		stages = VK_PIPELINE_STAGE_TRANSFER_BIT;
		access = VK_ACCESS_TRANSFER_WRITE_BIT;
	}

	return std::make_tuple(layout, stages, access);
}

static std::pair<VkPipelineStageFlags, VkAccessFlags> image_layout_to_pipeline_stages_and_access(VkImageLayout layout) {
	VkPipelineStageFlags stages = 0;
	VkAccessFlags access = 0;
	
	if (layout == VK_IMAGE_LAYOUT_UNDEFINED) {
		stages = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
	} else if (layout == VK_IMAGE_LAYOUT_GENERAL) {
		stages = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
		access =
			VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
			VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
			VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT |
			VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_HOST_WRITE_BIT |
			VK_ACCESS_HOST_READ_BIT;
	} else if (layout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL) {
		stages = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
		access = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
	} else if (layout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL) {
		stages = VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
		access = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
	} else if (layout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL) {
		stages = VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
		access = VK_ACCESS_MEMORY_READ_BIT;
	} else if (layout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL) {
		stages = VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
		access = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
	} else if (layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
		stages = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
		access = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_INPUT_ATTACHMENT_READ_BIT;
	} else if (layout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL || layout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
		stages = VK_PIPELINE_STAGE_TRANSFER_BIT;
		access = VK_ACCESS_MEMORY_READ_BIT;
	} else if (layout == VK_IMAGE_LAYOUT_PREINITIALIZED) {
		throw std::runtime_error("Image layout not supported");
	} else {
		throw std::runtime_error("Image layout not supported");
	}

	return std::make_pair(stages, access);
}

static std::pair<VkPipelineStageFlags, VkAccessFlags> buffer_usage_to_pipeline_stages_and_access(VkBufferUsageFlags usage) {
	VkPipelineStageFlags stages = 0;
	VkAccessFlags access = 0;

	if (usage & VK_BUFFER_USAGE_TRANSFER_SRC_BIT) {
		stages |= VK_PIPELINE_STAGE_TRANSFER_BIT;
		access |= VK_ACCESS_TRANSFER_READ_BIT;
	}
	if (usage & VK_BUFFER_USAGE_TRANSFER_DST_BIT) {
		stages |= VK_PIPELINE_STAGE_TRANSFER_BIT;
		access |= VK_ACCESS_TRANSFER_WRITE_BIT;
	}
	if (usage & VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT) {
		stages |= VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
		access |= VK_ACCESS_UNIFORM_READ_BIT;
	}
	if (usage & VK_BUFFER_USAGE_INDEX_BUFFER_BIT) {
		stages |= VK_PIPELINE_STAGE_VERTEX_INPUT_BIT;
		access |= VK_ACCESS_INDEX_READ_BIT;
	}
	if (usage & VK_BUFFER_USAGE_VERTEX_BUFFER_BIT) {
		stages |= VK_PIPELINE_STAGE_VERTEX_INPUT_BIT;
		access |= VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT;
	}

	return std::make_pair(stages, access);
}

static VkImageLayout image_usage_to_optimal_image_layout(ImageUsageFlags usage) {
	if (usage & ImageUsageColorAttachment)
		return VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	if (usage & ImageUsageDepthStencilAttachment)
		return VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
	if (usage & ImageUsageDepthStencilReadOnly)
		return VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
	//if (usage & ImageUsageSampled)
	//	return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	if (usage & ImageUsageColorSampled)
		return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	if (usage & ImageUsageDepthSampled)
		return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	if (usage & ImageUsageTransferSrc)
		return VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
	if (usage & ImageUsageTransferDst)
		return VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	return VK_IMAGE_LAYOUT_GENERAL;
}

static VkImageAspectFlags vk_format_to_aspect(VkFormat format) {
	VkImageAspectFlags aspect = 0;

	if (format_has_depth(format)) {
		aspect |= VK_IMAGE_ASPECT_DEPTH_BIT;

		if (format_has_stencil(format))
			aspect |= VK_IMAGE_ASPECT_STENCIL_BIT;
	} else
		aspect |= VK_IMAGE_ASPECT_COLOR_BIT;

	return aspect;
}

static VkImageUsageFlags image_usage_to_vk_image_usage(ImageUsageFlags usage) {
	VkImageUsageFlags vk_usage = 0;

	if (usage & ImageUsageColorAttachment)
		vk_usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
	if (usage & ImageUsageDepthStencilAttachment)
		vk_usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
	if (usage & ImageUsageColorSampled || usage & ImageUsageDepthSampled)
		vk_usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
	if (usage & ImageUsageDepthStencilReadOnly)
		vk_usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
	if (usage & ImageUsageTransferDst)
		vk_usage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	if (usage & ImageUsageTransferSrc)
		vk_usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;

	return vk_usage;
}

void VulkanGraphicsController::create(VulkanContext* context) {
	m_context = context;

	uint32_t frame_count = m_context->swapchain_image_count() + 1;
	m_frames.resize(frame_count);

	VkCommandPoolCreateInfo command_pool_info{
		.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
		.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
		.queueFamilyIndex = m_context->graphics_queue_index()
	};

	VkCommandBufferAllocateInfo command_buffer_info{
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
		.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
		.commandBufferCount = 1
	};

	VkFenceCreateInfo fence_info{
		.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
		.flags = VK_FENCE_CREATE_SIGNALED_BIT
	};

	VkDevice device = m_context->device();
	for (uint32_t i = 0; i < frame_count; i++) {
		if (vkCreateCommandPool(device, &command_pool_info, nullptr, &m_frames[i].command_pool) != VK_SUCCESS)
			throw std::runtime_error("Failed to create command pool");
		
		command_buffer_info.commandPool = m_frames[i].command_pool;
		if (vkAllocateCommandBuffers(device, &command_buffer_info, &m_frames[i].setup_buffer) != VK_SUCCESS ||
			vkAllocateCommandBuffers(device, &command_buffer_info, &m_frames[i].draw_buffer) != VK_SUCCESS)
			throw std::runtime_error("Failed to allocate command buffers");
	}

	m_frame_index = 0;
	VkCommandBufferBeginInfo begin_info{
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
		.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT
	};

	vkBeginCommandBuffer(m_frames[m_frame_index].setup_buffer, &begin_info);
	vkBeginCommandBuffer(m_frames[m_frame_index].draw_buffer, &begin_info);
}

void VulkanGraphicsController::destroy() {
	m_context->sync();

	vkEndCommandBuffer(m_frames[m_frame_index].setup_buffer);
	vkEndCommandBuffer(m_frames[m_frame_index].draw_buffer);

	VkDevice device = m_context->device();

	for (Buffer& buffer : m_buffers) {
		vkDestroyBuffer(device, buffer.buffer, nullptr);
		vkFreeMemory(device, buffer.memory, nullptr);
	}
	m_buffers.clear();

	for (Image& image : m_images) {
		//vkDestroyImageView(device, image.view, nullptr);
		vkDestroyImage(device, image.image, nullptr);
		vkFreeMemory(device, image.memory, nullptr);
	}
	m_images.clear();

	for (Sampler& sampler : m_samplers)
		vkDestroySampler(device, sampler.sampler, nullptr);

	for (Shader& shader : m_shaders) {
		for (VkDescriptorSetLayout set_layout : shader.set_layouts)
			vkDestroyDescriptorSetLayout(device, set_layout, nullptr);

		for (StageInfo& stage_info : shader.stages)
			vkDestroyShaderModule(device, stage_info.module, nullptr);

		//vkDestroyShaderModule(device, info.vertex_module, nullptr);
		//vkDestroyShaderModule(device, info.fragment_module, nullptr);

		vkDestroyPipelineLayout(device, shader.pipeline_layout, nullptr);
	}
	m_shaders.clear();

	for (Pipeline& pipeline : m_pipelines)
		vkDestroyPipeline(device, pipeline.pipeline, nullptr);
	m_pipelines.clear();

	for (Frame& frame : m_frames) {
		vkDestroyCommandPool(device, frame.command_pool, nullptr);

		for (StagingBuffer& buffer : frame.staging_buffers) {
			vkDestroyBuffer(device, buffer.buffer, nullptr);
			vkFreeMemory(device, buffer.memory, nullptr);
		}
	}
	m_frames.clear();

	for (Framebuffer& framebuffer : m_framebuffers) {
		for (VkImageView view : framebuffer.image_views)
			vkDestroyImageView(m_context->device(), view, nullptr);
		vkDestroyFramebuffer(device, framebuffer.framebuffer, nullptr);
	}
	m_framebuffers.clear();

	for (RenderPass& render_pass : m_render_passes)
		vkDestroyRenderPass(device, render_pass.render_pass, nullptr);
	m_render_passes.clear();
}

void VulkanGraphicsController::end_frame() {
	vkEndCommandBuffer(m_frames[m_frame_index].setup_buffer);
	vkEndCommandBuffer(m_frames[m_frame_index].draw_buffer);

	m_context->swap_buffers(m_frames[m_frame_index].setup_buffer, m_frames[m_frame_index].draw_buffer);

	m_frame_index = (m_frame_index + 1) % m_frames.size();

	VkCommandBufferBeginInfo begin_info{
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
		.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT
	};

	vkBeginCommandBuffer(m_frames[m_frame_index].setup_buffer, &begin_info);
	vkBeginCommandBuffer(m_frames[m_frame_index].draw_buffer, &begin_info);

	VkDevice device = m_context->device();
	for (StagingBuffer& buffer : m_frames[m_frame_index].staging_buffers) {
		vkDestroyBuffer(device, buffer.buffer, nullptr);
		vkFreeMemory(device, buffer.memory, nullptr);
	}
	m_frames[m_frame_index].staging_buffers.clear();
}

void VulkanGraphicsController::draw_begin(FramebufferId framebuffer_id, const ClearValue* clear_values, uint32_t count) {
	const Framebuffer& framebuffer = m_framebuffers[framebuffer_id];
	const RenderPass& render_pass = m_render_passes[framebuffer.render_pass_id];

	// Transition attchment image layout to its initial layout specified in VkRenderPass instance if it changed
	for (uint32_t i = 0; i < framebuffer.attachments.size(); i++) {
		Image& attachment_image = m_images[framebuffer.attachments[i]];

		image_should_have_layout(attachment_image, render_pass.attachments[i].initial_layout);

		attachment_image.current_layout = render_pass.attachments[i].final_layout;
	}

	VkRenderPassBeginInfo render_pass_begin_info{
		.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
		.renderPass = framebuffer.render_pass,
		.framebuffer = framebuffer.framebuffer,
		.renderArea = { { 0, 0 }, framebuffer.extent },
		.clearValueCount = count,
		.pClearValues = (VkClearValue*)clear_values
	};
	
	vkCmdBeginRenderPass(m_frames[m_frame_index].draw_buffer, &render_pass_begin_info, VK_SUBPASS_CONTENTS_INLINE);
}

void VulkanGraphicsController::draw_end() {
	vkCmdEndRenderPass(m_frames[m_frame_index].draw_buffer);
}

void VulkanGraphicsController::draw_begin_for_screen(const glm::vec4& clear_color) {
	VkClearValue clear_value{ clear_color.r, clear_color.g, clear_color.b, clear_color.a };
	
	VkRenderPassBeginInfo begin_info{
		.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
		.renderPass = m_context->swapchain_render_pass(),
		.framebuffer = m_context->swapchain_framebuffer(),
		.renderArea = { {0, 0}, m_context->swapchain_extent() },
		.clearValueCount = 1,
		.pClearValues = &clear_value
	};
	
	vkCmdBeginRenderPass(m_frames[m_frame_index].draw_buffer, &begin_info, VK_SUBPASS_CONTENTS_INLINE);
}

void VulkanGraphicsController::draw_end_for_screen() {
	vkCmdEndRenderPass(m_frames[m_frame_index].draw_buffer);
}

void VulkanGraphicsController::draw_set_viewport(float x, float y, float width, float height, float min_depth, float max_depth) {
	VkViewport viewport{
		.x = x,
		.y = y,
		.width = width,
		.height = height,
		.minDepth = min_depth,
		.maxDepth = max_depth
	};

	vkCmdSetViewport(m_frames[m_frame_index].draw_buffer, 0, 1, &viewport);
}

void VulkanGraphicsController::draw_set_scissor(int x_offset, int y_offset, uint32_t width, uint32_t height) {
	VkRect2D scissor{
		.offset = { x_offset, y_offset },
		.extent = { width, height }
	};

	vkCmdSetScissor(m_frames[m_frame_index].draw_buffer, 0, 1, &scissor);
}

void VulkanGraphicsController::draw_set_line_width(float width) {
	vkCmdSetLineWidth(m_frames[m_frame_index].draw_buffer, width);
}

void VulkanGraphicsController::draw_set_stencil_reference(StencilFaces faces, uint32_t reference) {
	vkCmdSetStencilReference(m_frames[m_frame_index].draw_buffer, (VkStencilFaceFlags)faces, reference);
}

void VulkanGraphicsController::draw_push_constants(ShaderId shader, ShaderStageFlags stage, uint32_t offset, uint32_t size, const void* data) {
	vkCmdPushConstants(m_frames[m_frame_index].draw_buffer, m_shaders[shader].pipeline_layout, (VkShaderStageFlags)stage, offset, size, data);
}

void VulkanGraphicsController::draw_bind_pipeline(PipelineId pipeline_id) {
	const Pipeline& pipeline = m_pipelines[pipeline_id];

	vkCmdBindPipeline(m_frames[m_frame_index].draw_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.pipeline);
}

void VulkanGraphicsController::draw_bind_vertex_buffer(BufferId buffer_id) {
	Buffer& buffer = m_buffers[buffer_id];

	VkDeviceSize offset = 0;
	vkCmdBindVertexBuffers(m_frames[m_frame_index].draw_buffer, 0, 1, &buffer.buffer, &offset);
}

void VulkanGraphicsController::draw_bind_index_buffer(BufferId buffer_id, IndexType index_type) {
	Buffer& buffer = m_buffers[buffer_id];

	vkCmdBindIndexBuffer(m_frames[m_frame_index].draw_buffer, buffer.buffer, 0, (VkIndexType)index_type);
}

void VulkanGraphicsController::draw_bind_uniform_sets(PipelineId pipeline_id, uint32_t first_set, const UniformSetId* set_ids, uint32_t count) {
	std::vector<VkDescriptorSet> descriptor_sets;
	descriptor_sets.reserve(count);
	
	for (uint32_t i = 0; i < count; i++) {
		UniformSet& set = m_uniform_sets[set_ids[i]];

		for (ImageId id : set.images)
			image_should_have_layout(m_images[id], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	
		descriptor_sets.push_back(set.descriptor_set);
	}

	vkCmdBindDescriptorSets(
		m_frames[m_frame_index].draw_buffer,
		VK_PIPELINE_BIND_POINT_GRAPHICS,
		m_pipelines[pipeline_id].layout,
		first_set, count, descriptor_sets.data(),
		0, nullptr
	);
}

void VulkanGraphicsController::draw_draw_indexed(uint32_t index_count, uint32_t first_index) {
	vkCmdDrawIndexed(m_frames[m_frame_index].draw_buffer, index_count, 1, first_index, 0, 0);
}

void VulkanGraphicsController::draw_draw(uint32_t vertex_count, uint32_t first_vertex) {
	vkCmdDraw(m_frames[m_frame_index].draw_buffer, vertex_count, 1, first_vertex, 0);
}

RenderPassId VulkanGraphicsController::render_pass_create(const RenderPassAttachment* attachments, RenderId count) {
	RenderPass render_pass;
	render_pass.attachments.reserve(count);

	std::vector<VkAttachmentDescription> attachment_descriptions;
	attachment_descriptions.reserve(count);
	std::vector<VkAttachmentReference> color_attachments;
	std::vector<VkAttachmentReference> depth_stencil_attachments;
	// TODO: Resolve attachment support
	// std::vector<VkAttachmentReference> resolve_attachments;

	VkSubpassDependency external_to_subpass{
		.srcSubpass = VK_SUBPASS_EXTERNAL,
		.dstSubpass = 0
	};
	VkSubpassDependency subpass_to_external{
		.srcSubpass = 0,
		.dstSubpass = VK_SUBPASS_EXTERNAL
	};

	for (uint32_t i = 0; i < count; i++) {
		const RenderPassAttachment& attachment = attachments[i];

		auto [prev_layout, prev_stages, prev_access] = image_usage_to_layout_stage_access(attachment.previous_usage);
		auto [curr_layout, curr_stages, curr_access] = image_usage_to_layout_stage_access(attachment.current_usage);
		auto [next_layout, next_stages, next_access] = image_usage_to_layout_stage_access(attachment.next_usage);

		external_to_subpass.srcStageMask |= prev_stages;
		external_to_subpass.srcAccessMask |= prev_access;

		external_to_subpass.dstStageMask |= curr_stages;
		external_to_subpass.dstAccessMask |= curr_access;

		subpass_to_external.srcStageMask |= curr_stages;
		subpass_to_external.srcAccessMask |= curr_access;

		subpass_to_external.dstStageMask |= next_stages;
		subpass_to_external.dstAccessMask |= next_access;

		VkAttachmentDescription description{
			.format = (VkFormat)attachment.format,
			.samples = VK_SAMPLE_COUNT_1_BIT,
			.loadOp = (VkAttachmentLoadOp)attachment.initial_action,
			.storeOp = (VkAttachmentStoreOp)attachment.final_action,
			.stencilLoadOp = (VkAttachmentLoadOp)attachment.stencil_initial_action,
			.stencilStoreOp = (VkAttachmentStoreOp)attachment.stencil_final_action,
			.initialLayout = prev_layout,
			.finalLayout = next_layout
		};
				
		attachment_descriptions.push_back(description);

		VkAttachmentReference reference{
			.attachment = i,
			.layout = curr_layout
		};

		ImageUsageFlags usage = attachment.current_usage;
		if (format_has_depth((VkFormat)attachment.format))
			depth_stencil_attachments.push_back(reference);
		else
			color_attachments.push_back(reference);

		render_pass.attachments.emplace_back(attachment, prev_layout, next_layout);
	}

	if (depth_stencil_attachments.size() > 1) throw std::runtime_error("Render pass supports only one depth stencil attachment");

	VkSubpassDescription subpass{
		.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
		.colorAttachmentCount = (uint32_t)color_attachments.size(),
		.pColorAttachments = color_attachments.data(),
		//.pResolveAttachments = resolve_attachments.data(),
		.pDepthStencilAttachment = depth_stencil_attachments.data()
	};

	VkSubpassDependency dependencies[2] = { external_to_subpass, subpass_to_external };

	VkRenderPassCreateInfo render_pass_info{
		.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
		.attachmentCount = (uint32_t)attachment_descriptions.size(),
		.pAttachments = attachment_descriptions.data(),
		.subpassCount = 1,
		.pSubpasses = &subpass,
		.dependencyCount = 2,
		.pDependencies = dependencies
	};
	
	if (vkCreateRenderPass(m_context->device(), &render_pass_info, nullptr, &render_pass.render_pass) != VK_SUCCESS)
		throw std::runtime_error("Failed to create framebuffer render pass");

	m_render_passes.push_back(render_pass);
	return (RenderPassId)m_render_passes.size() - 1;
}

FramebufferId VulkanGraphicsController::framebuffer_create(RenderPassId render_pass_id, const ImageId* ids, uint32_t count) {
	RenderPass& render_pass = m_render_passes[render_pass_id];

	uint32_t width = m_images[ids[0]].info.width;
	uint32_t height = m_images[ids[0]].info.height;

	Framebuffer framebuffer{
		.render_pass_id = render_pass_id,
		.render_pass = render_pass.render_pass,
		.extent = { width, height }
	};
	framebuffer.image_views.reserve(count);

	framebuffer.attachments.reserve(count);
	for (uint32_t i = 0; i < count; i++) {
		framebuffer.attachments.push_back(ids[i]);
		
		const Image& image = m_images[ids[i]];
		
		framebuffer.image_views.push_back(image_view_create(image, image.info.usage));
	}

	VkFramebufferCreateInfo framebuffer_info{
		.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
		.renderPass = framebuffer.render_pass,
		.attachmentCount = count,
		.pAttachments = framebuffer.image_views.data(),
		.width = width,
		.height = height,
		.layers = 1
	};
	
	if (vkCreateFramebuffer(m_context->device(), &framebuffer_info, nullptr, &framebuffer.framebuffer) != VK_SUCCESS)
		throw std::runtime_error("Failed to create framebuffer");

	m_framebuffers.push_back(std::move(framebuffer));
	return (FramebufferId)m_framebuffers.size() - 1;
}

ShaderId VulkanGraphicsController::shader_create(const ShaderStage* stages, RenderId stage_count) {
	m_shaders.push_back({});
	Shader& shader = m_shaders.back();
	
	auto reflect_shader_stage = [&, this](const void* spv, size_t size) {
		spv_reflect::ShaderModule shader_module(size, spv);

		shader.stages.push_back({
			.entry = const_char_to_vector(shader_module.GetEntryPointName())
		});
		StageInfo& stage_info = shader.stages.back();

		VkShaderModuleCreateInfo module_info({
			.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
			.codeSize = (uint32_t)size,
			.pCode = (uint32_t*)spv
		});

		if (vkCreateShaderModule(m_context->device(), &module_info, nullptr, &stage_info.module) != VK_SUCCESS)
			throw std::runtime_error("Failed to create Vertex stage VkShaderModule");

		shader.stage_create_infos.push_back({
			.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
			.stage = (VkShaderStageFlagBits)shader_module.GetShaderStage(),
			.module = stage_info.module,
			.pName = stage_info.entry.data()
		});
		VkPipelineShaderStageCreateInfo& stage_create_info = shader.stage_create_infos.back();

		// Reflect Input Variables
		if (stage_create_info.stage == VK_SHADER_STAGE_VERTEX_BIT) {
			uint32_t input_var_count = 0;
			shader_module.EnumerateInputVariables(&input_var_count, nullptr);
			std::vector<SpvReflectInterfaceVariable*> input_vars(input_var_count);
			shader_module.EnumerateInputVariables(&input_var_count, input_vars.data());

			std::sort(input_vars.begin(), input_vars.end(), [](const auto& var1, const auto& var2) {
				return var1->location < var2->location;
			});

			shader.input_vars_info.attribute_descriptions.reserve(input_var_count);

			uint32_t stride = 0;
			for (uint32_t i = 0; i < input_var_count; i++) {
				SpvReflectInterfaceVariable* input_var = input_vars[i];

				VkVertexInputAttributeDescription attribute_description{
					.location = input_var->location,
					.binding = 0,
					.format = (VkFormat)input_var->format,
					.offset = stride
				};

				stride += vk_format_to_size((VkFormat)input_var->format);

				shader.input_vars_info.attribute_descriptions.push_back(attribute_description);
			}

			shader.input_vars_info.binding_description.binding = 0;
			shader.input_vars_info.binding_description.stride = stride;
		}

		// Relfect Uniforms
		uint32_t binding_count = 0;
		shader_module.EnumerateDescriptorBindings(&binding_count, nullptr);
		std::vector<SpvReflectDescriptorBinding*> bindings(binding_count);
		shader_module.EnumerateDescriptorBindings(&binding_count, bindings.data());

		for (SpvReflectDescriptorBinding* descriptor_binding : bindings) {
			uint32_t set_idx = descriptor_binding->set;
			uint32_t binding_idx = descriptor_binding->binding;
			VkDescriptorType type = (VkDescriptorType)descriptor_binding->descriptor_type;
			uint32_t count = descriptor_binding->count;

			auto set_it = shader.find_set(set_idx);
			if (set_it == shader.sets.end()) {
				SetInfo set_info{
					.set = set_idx
				};

				shader.sets.push_back(set_info);
				set_it = shader.sets.end() - 1;
			}

			auto binding_it = set_it->find_binding(binding_idx);
			if (binding_it == set_it->bindings.end()) { // Insert new binding, no problem
				VkDescriptorSetLayoutBinding layout_binding{
					.binding = binding_idx,
					.descriptorType = type,
					.descriptorCount = count,
					.stageFlags = (VkShaderStageFlags)stage_create_info.stage
				};
				
				set_it->bindings.push_back(layout_binding);
			} else { // Update binding stage and verify bindings are the same
				binding_it->stageFlags |= (VkShaderStageFlags)stage_create_info.stage;

				if (type != binding_it->descriptorType)
					throw std::runtime_error("Uniform set binding redefinition with different descriptor type");
				if (count != binding_it->descriptorCount)
					throw std::runtime_error("Uniform set binding refefinition with different count");
			}
		}

		// Reflect Push Constants
		uint32_t push_constant_count = 0;
		shader_module.EnumeratePushConstantBlocks(&push_constant_count, nullptr);
		
		if (push_constant_count) { // Only one push constant is supported per shader stage
			std::vector<SpvReflectBlockVariable*> push_constants(push_constant_count);
			shader_module.EnumeratePushConstantBlocks(&push_constant_count, push_constants.data());

			VkPushConstantRange pc_range{
				.stageFlags = (VkShaderStageFlags)stage_create_info.stage,
				.offset = push_constants[0]->members->offset,
				.size = push_constants[0]->size
			};

			shader.push_constants.push_back(pc_range);
		}
	};

	for (uint32_t i = 0; i < stage_count; i++)
		reflect_shader_stage(stages[i].spv, stages[i].spv_size);

	std::sort(shader.sets.begin(), shader.sets.end(), [](const auto& set_0, const auto& set_1) {
		return set_0.set < set_1.set;
	});

	// Create VkPipelineVertexInputStateCreateInfo
	shader.vertex_input_create_info.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
	shader.vertex_input_create_info.vertexBindingDescriptionCount = 1;
	shader.vertex_input_create_info.pVertexBindingDescriptions = &shader.input_vars_info.binding_description;
	shader.vertex_input_create_info.vertexAttributeDescriptionCount = (uint32_t)shader.input_vars_info.attribute_descriptions.size();
	shader.vertex_input_create_info.pVertexAttributeDescriptions = shader.input_vars_info.attribute_descriptions.data();

	// Create VkDescriptorSetLayouts
	size_t set_count = shader.sets.size();

	shader.set_layouts.reserve(set_count);
	for (size_t i = 0; i < set_count; i++) {
		SetInfo& set_info = shader.sets[i];

		std::sort(set_info.bindings.begin(), set_info.bindings.end(), [](const auto& bind_0, const auto& bind_1) {
			return bind_0.binding < bind_1.binding;
		});

		VkDescriptorSetLayoutCreateInfo set_layout_info{
			.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
			.bindingCount = (uint32_t)set_info.bindings.size(),
			.pBindings = set_info.bindings.data()
		};
	
		VkDescriptorSetLayout set_layout;
		if (vkCreateDescriptorSetLayout(m_context->device(), &set_layout_info, nullptr, &set_layout) != VK_SUCCESS)
			throw std::runtime_error("Failed to create descritptor set layout");

		shader.set_layouts.push_back(set_layout);
	}

	// Create pipeline layout
	VkPipelineLayoutCreateInfo pipeline_layout_info{
		.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
		.setLayoutCount = (uint32_t)shader.set_layouts.size(),
		.pSetLayouts = shader.set_layouts.data(),
		.pushConstantRangeCount = (uint32_t)shader.push_constants.size(),
		.pPushConstantRanges = shader.push_constants.data()
	};

	if (vkCreatePipelineLayout(m_context->device(), &pipeline_layout_info, nullptr, &shader.pipeline_layout) != VK_SUCCESS)
		throw std::runtime_error("Failed to create pipelien layout");

	return (ShaderId)m_shaders.size() - 1;
}

PipelineId VulkanGraphicsController::pipeline_create(const PipelineInfo& pipeline_info) {
	m_pipelines.push_back({});
	Pipeline& pipeline = m_pipelines.back();
	pipeline.info = pipeline_info;

	const Shader& shader = m_shaders[pipeline.info.shader_id];
	
	pipeline.layout = shader.pipeline_layout;

	VkPipelineInputAssemblyStateCreateInfo assembly_state{
		.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
		.topology = (VkPrimitiveTopology)pipeline_info.assembly.topology,
		.primitiveRestartEnable = pipeline_info.assembly.restart_enable
	};

	VkViewport viewport{
		.x = 0,
		.y = 0,
		.width = (float)m_context->swapchain_extent().width,
		.height = (float)m_context->swapchain_extent().height,
		.minDepth = 0.0f,
		.maxDepth = 1.0f
	};

	VkRect2D scissor{
		.offset = { 0, 0 },
		.extent = m_context->swapchain_extent()
	};

	VkPipelineViewportStateCreateInfo viewport_state{
		.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
		.viewportCount = 1,
		.pViewports = &viewport,
		.scissorCount = 1,
		.pScissors = &scissor
	};

	VkPipelineRasterizationStateCreateInfo rasterization_state{
		.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
		.depthClampEnable = pipeline_info.raster.depth_clamp_enable,
		.rasterizerDiscardEnable = pipeline_info.raster.rasterizer_discard_enable,
		.polygonMode = (VkPolygonMode)pipeline_info.raster.polygon_mode,
		.cullMode = (VkCullModeFlags)pipeline_info.raster.cull_mode,
		.frontFace = (VkFrontFace)pipeline_info.raster.front_face,
		.depthBiasEnable = pipeline_info.raster.depth_bias_enable,
		.depthBiasConstantFactor = pipeline_info.raster.depth_bias_constant_factor,
		.depthBiasClamp = pipeline_info.raster.depth_bias_clamp,
		.depthBiasSlopeFactor =  pipeline_info.raster.detpth_bias_slope_factor,
		.lineWidth = pipeline_info.raster.line_width
	};

	VkPipelineMultisampleStateCreateInfo multisample_state{
		.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
		.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
		.sampleShadingEnable = VK_FALSE
	};

	VkPipelineDepthStencilStateCreateInfo depth_stencil_state{
		.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
		.depthTestEnable = pipeline_info.depth_stencil.depth_test_enable,
		.depthWriteEnable = pipeline_info.depth_stencil.depth_write_enable,
		.depthCompareOp = (VkCompareOp)pipeline_info.depth_stencil.depth_compare_op,
		.depthBoundsTestEnable = pipeline_info.depth_stencil.depth_bounds_test_enable,
		.stencilTestEnable = pipeline_info.depth_stencil.stencil_test_enable,
		.front = {
			.failOp = (VkStencilOp)pipeline_info.depth_stencil.front.fail_op,
			.passOp = (VkStencilOp)pipeline_info.depth_stencil.front.pass_op,
			.depthFailOp = (VkStencilOp)pipeline_info.depth_stencil.front.depth_fail_op,
			.compareOp = (VkCompareOp)pipeline_info.depth_stencil.front.compare_op,
			.compareMask = pipeline_info.depth_stencil.front.compare_mask,
			.writeMask = pipeline_info.depth_stencil.front.write_mask,
			.reference = pipeline_info.depth_stencil.front.reference
		},
		.back = {
			.failOp = (VkStencilOp)pipeline_info.depth_stencil.back.fail_op,
			.passOp = (VkStencilOp)pipeline_info.depth_stencil.back.pass_op,
			.depthFailOp = (VkStencilOp)pipeline_info.depth_stencil.back.depth_fail_op,
			.compareOp = (VkCompareOp)pipeline_info.depth_stencil.back.compare_op,
			.compareMask = pipeline_info.depth_stencil.back.compare_mask,
			.writeMask = pipeline_info.depth_stencil.back.write_mask,
			.reference = pipeline_info.depth_stencil.back.reference
		},		
		.minDepthBounds = pipeline_info.depth_stencil.min_depth_bounds,
		.maxDepthBounds = pipeline_info.depth_stencil.max_depth_bounds
	};

	VkPipelineColorBlendStateCreateInfo color_blend_state{
		.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
		.logicOpEnable = pipeline_info.color_blend.logic_op_enable,
		.logicOp = (VkLogicOp)pipeline_info.color_blend.logic_op,
		.attachmentCount = pipeline_info.color_blend.attachment_count,
		.pAttachments = (VkPipelineColorBlendAttachmentState*)pipeline_info.color_blend.attachments,
		.blendConstants = {
			pipeline_info.color_blend.blend_constants[0],
			pipeline_info.color_blend.blend_constants[1],
			pipeline_info.color_blend.blend_constants[2],
			pipeline_info.color_blend.blend_constants[3]
		}
	};

	VkPipelineDynamicStateCreateInfo dynamic_state{
		.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
		.dynamicStateCount = pipeline_info.dynamic_states.dynamic_state_count,
		.pDynamicStates = (VkDynamicState*)pipeline_info.dynamic_states.dynamic_states
	};

	VkGraphicsPipelineCreateInfo pipeline_create_info{
		.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
		.stageCount = (uint32_t)shader.stage_create_infos.size(),
		.pStages = shader.stage_create_infos.data(),
		.pVertexInputState = &shader.vertex_input_create_info,
		.pInputAssemblyState = &assembly_state,
		.pTessellationState = nullptr,
		.pViewportState = &viewport_state,
		.pRasterizationState = &rasterization_state,
		.pMultisampleState = &multisample_state,
		.pDepthStencilState = &depth_stencil_state,
		// TODO: color blend state support
		.pColorBlendState = &color_blend_state,
		.pDynamicState = &dynamic_state,
		.layout = shader.pipeline_layout,
		.renderPass = pipeline_info.render_pass_id
			? m_render_passes[pipeline_info.render_pass_id.value()].render_pass
			: m_context->swapchain_render_pass(),
		.subpass = 0
	};
	
	if (vkCreateGraphicsPipelines(m_context->device(), VK_NULL_HANDLE, 1, &pipeline_create_info, nullptr, &pipeline.pipeline) != VK_SUCCESS)
		throw std::runtime_error("Failed to create graphics pipeline");

	return (PipelineId)m_pipelines.size() - 1;
}

BufferId VulkanGraphicsController::vertex_buffer_create(const void* data, size_t size) {
	Buffer buffer{
		.size = size,
		.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT
	};
	
	buffer.buffer = buffer_create(VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, size);
	buffer.memory = buffer_allocate(buffer.buffer, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

	buffer_copy(buffer.buffer, data, size);

	buffer_memory_barrier(buffer.buffer, buffer.usage, 0, size);

	m_buffers.push_back(buffer);
	return (BufferId)m_buffers.size() - 1;
}

BufferId VulkanGraphicsController::index_buffer_create(const void* data, size_t size, IndexType index_type) {
	Buffer buffer{
		.size = size,
		.usage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT
	};
	buffer.index.index_type = (VkIndexType)index_type;
	buffer.index.index_count = index_type == IndexType::Uint16 ? (uint32_t)size / 2 : (uint32_t)size / 4;

	buffer.buffer = buffer_create(VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, size);
	buffer.memory = buffer_allocate(buffer.buffer, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

	buffer_copy(buffer.buffer, data, size);

	buffer_memory_barrier(buffer.buffer, buffer.usage, 0, size);

	m_buffers.push_back(buffer);
	return (BufferId)m_buffers.size() - 1;
}

BufferId VulkanGraphicsController::uniform_buffer_create(const void* data, size_t size) {
	Buffer buffer{
		.size = size,
		.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT
	};

	buffer.buffer = buffer_create(VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, size);
	buffer.memory = buffer_allocate(buffer.buffer, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

	if (data) {
		buffer_copy(buffer.buffer, data, size);
		buffer_memory_barrier(buffer.buffer, buffer.usage, 0, size);
	}

	m_buffers.push_back(buffer);
	return (BufferId)m_buffers.size() - 1;
}

void VulkanGraphicsController::buffer_update(BufferId buffer_id, const void* data) {
	Buffer& buffer = m_buffers[buffer_id];

	buffer_memory_barrier(buffer.buffer, buffer.usage, 0, buffer.size);

	buffer_copy(buffer.buffer, data, buffer.size);

	buffer_memory_barrier(buffer.buffer, buffer.usage, 0, buffer.size);
}

ImageId VulkanGraphicsController::image_create(const ImageInfo& info) {
	VkFormat vk_format = (VkFormat)info.format;
	VkDeviceSize size = vk_format_to_size(vk_format) * info.width * info.height * info.depth * info.layer_count;

	VkImageUsageFlags image_usage = image_usage_to_vk_image_usage(info.usage);
	VkMemoryPropertyFlags mem_props = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
	VkImageTiling tiling = VK_IMAGE_TILING_OPTIMAL;
	
	// TODO: Add cpu visible image memory usage
	//if (info.usage & ImageUsageCPUVisible) {
	//	mem_props = VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
	//	tiling = VK_IMAGE_TILING_LINEAR;
	//}

	Image image{
		.info = info,
		.image = vulkan_image_create(info.view_type, vk_format, { info.width, info.height, info.depth }, info.layer_count, tiling, image_usage),
		.memory = vulkan_image_allocate(image.image, mem_props),
		.current_layout = VK_IMAGE_LAYOUT_UNDEFINED,
		.full_aspect = vk_format_to_aspect(vk_format),
		.tiling = tiling
	};

	m_images.push_back(image);
	ImageId image_id = (ImageId)m_images.size() - 1;

	return image_id;
}

void VulkanGraphicsController::image_update(ImageId image_id, const ImageDataInfo& image_data_info) {
	const Image& image = m_images[image_id];
	const ImageInfo& image_info = image.info;

	VkExtent3D extent = { image_info.width, image_info.height, image_info.depth };
	size_t image_data_size = extent.width * extent.height * extent.depth * image_info.layer_count * vk_format_to_size((VkFormat)image_data_info.format);
	VkImageLayout layout = image.current_layout;
	VkImageUsageFlags image_usage = image_usage_to_vk_image_usage(image_info.usage);
	uint32_t layer_count = image_info.layer_count;

	vulkan_image_memory_barrier(image.image, image.full_aspect, layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, layer_count);

	StagingBuffer staging_buffer = staging_buffer_create(image_data_info.data, image_data_size);

	if (image_info.format == image_data_info.format) {
		vulkan_copy_buffer_to_image(image.image, staging_buffer, extent, image.full_aspect, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, layer_count);
	} else {
		Image staging_image{
			.image = vulkan_image_create(image_info.view_type, (VkFormat)image_data_info.format, extent, layer_count, image.tiling, VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT),
			.memory = vulkan_image_allocate(staging_image.image, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)
		};

		vulkan_image_memory_barrier(staging_image.image, image.full_aspect, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, layer_count);
		vulkan_copy_buffer_to_image(staging_image.image, staging_buffer, extent, image.full_aspect, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, layer_count);
		vulkan_image_memory_barrier(staging_image.image, image.full_aspect, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, layer_count);
		
		VkOffset3D offset = { (int32_t)extent.width, (int32_t)extent.height, (int32_t)extent.depth };

		VkImageBlit region;
		region.srcSubresource = { .aspectMask = image.full_aspect, .mipLevel = 0, .baseArrayLayer = 0, .layerCount = layer_count };
		region.srcOffsets[0] = { 0, 0, 0 };
		region.srcOffsets[1] = offset;
		region.dstSubresource = { .aspectMask = image.full_aspect, .mipLevel = 0, .baseArrayLayer = 0, .layerCount = layer_count };
		region.dstOffsets[0] = { 0, 0, 0 };
		region.dstOffsets[1] = offset;

		vkCmdBlitImage(m_frames[m_frame_index].draw_buffer, staging_image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region, VkFilter::VK_FILTER_LINEAR);
	}
	
	if (layout == VK_IMAGE_LAYOUT_UNDEFINED)
		layout = image_usage_to_optimal_image_layout(image.info.usage);

	vulkan_image_memory_barrier(image.image, image.full_aspect, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, layout, layer_count);
}

SamplerId VulkanGraphicsController::sampler_create(const SamplerInfo& info) {
	VkSamplerCreateInfo sampler_info{
		.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
		.magFilter = (VkFilter)info.mag_filter,
		.minFilter = (VkFilter)info.min_filter,
		.mipmapMode = (VkSamplerMipmapMode)info.mip_map_mode,
		.addressModeU = (VkSamplerAddressMode)info.address_mode_u,
		.addressModeV = (VkSamplerAddressMode)info.address_mode_v,
		.addressModeW = (VkSamplerAddressMode)info.address_mode_w,
		.mipLodBias = info.mip_lod_bias,
		.anisotropyEnable = info.anisotropy_enable,
		.maxAnisotropy = info.max_anisotropy,
		.compareEnable = info.compare_enable,
		.compareOp = (VkCompareOp)info.comapare_op,
		.minLod = info.min_lod,
		.maxLod = info.max_lod,
		.borderColor = (VkBorderColor)info.border_color,
		.unnormalizedCoordinates = info.unnormalized_coordinates
	};

	Sampler sampler{
		.info = info
	};

	if (vkCreateSampler(m_context->device(), &sampler_info, nullptr, &sampler.sampler) != VK_SUCCESS)
		throw std::runtime_error("Failed to create sampler");

	m_samplers.push_back(sampler);

	return (SamplerId)m_samplers.size() - 1;
}

UniformSetId VulkanGraphicsController::uniform_set_create(ShaderId shader_id, uint32_t set_idx, const UniformInfo* uniforms, size_t uniform_count) {
	SetInfo& set = *m_shaders[shader_id].find_set(set_idx);

	std::vector<ImageId> images;
	std::vector<VkImageView> image_views;

	std::vector<std::vector<VkDescriptorImageInfo>> image_infos_collector;
	std::vector<std::vector<VkDescriptorBufferInfo>> buffer_infos_collector;
	std::vector<VkWriteDescriptorSet> writes;

	DescriptorPoolKey pool_key;
	for (uint32_t i = 0; i < uniform_count; i++) {
		const auto& uniform = uniforms[i];
		auto binding_it = set.find_binding(uniform.binding);
		
		if (binding_it == set.bindings.end())
			throw std::runtime_error("No binding found");

		VkWriteDescriptorSet write{
			.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
			.dstBinding = binding_it->binding,
			.dstArrayElement = 0
		};

		switch (uniform.type) {
		case UniformType::Sampler: {
			throw std::runtime_error("UniformType not supported");
		}
		case UniformType::CombinedImageSampler: {
			std::vector<VkDescriptorImageInfo> image_infos;

			for (size_t j = 0; j < uniform.id_count; j += 2) {
				Image& image = m_images[uniform.ids[j]];

				if (image.info.usage & ImageUsageDepthStencilReadOnly)
					image_should_have_layout(image, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);
				else
					image_should_have_layout(image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

				VkImageView view = image_view_create(image, uniform.image_usage);
				image_views.push_back(view);

				VkDescriptorImageInfo image_info{
					.sampler = m_samplers[uniform.ids[j + 1]].sampler,
					.imageView = view,
					.imageLayout = image.current_layout
				};

				image_infos.push_back(image_info);
				images.push_back((uint32_t)j);
			}

			write.descriptorCount = uniform.id_count / 2;
			write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
			write.pImageInfo = image_infos.data();

			image_infos_collector.push_back(std::move(image_infos));

			break;
		}
		case UniformType::SampledImage: {
			throw std::runtime_error("UniformType not supported");
		}
		case UniformType::UniformBuffer: {
			std::vector<VkDescriptorBufferInfo> buffer_infos;

			for (size_t j = 0; j < uniform.id_count; j++) {
				Buffer& buffer = m_buffers[uniform.ids[j]];

				VkDescriptorBufferInfo buffer_info{
					.buffer = buffer.buffer,
					.offset = 0,
					.range = VK_WHOLE_SIZE
				};

				buffer_infos.push_back(buffer_info);
			}

			write.descriptorCount = uniform.id_count;
			write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
			write.pBufferInfo = buffer_infos.data();
			
			buffer_infos_collector.push_back(std::move(buffer_infos));

			break;
		}
		}

		pool_key.uniform_type_counts[(uint32_t)uniform.type] += write.descriptorCount;

		writes.push_back(write);
	}

	uint32_t pool_idx = descriptor_pool_allocate(pool_key);
	const DescriptorPool& pool = m_descriptor_pools.at(pool_key)[pool_idx];

	VkDescriptorSetAllocateInfo set_allocate_info{
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
		.descriptorPool = pool.pool,
		.descriptorSetCount = 1,
		.pSetLayouts = &m_shaders[shader_id].set_layouts[set_idx]
	};
	
	VkDescriptorSet descriptor_set;
	if (vkAllocateDescriptorSets(m_context->device(), &set_allocate_info, &descriptor_set) != VK_SUCCESS)
		throw std::runtime_error("Failed to allocate descriptor set");

	UniformSet uniform_set{
		.images = std::move(images),
		.image_views = std::move(image_views),
		.pool_key = pool_key,
		.pool_idx = pool_idx,
		.shader = shader_id,
		.set_idx= set_idx,
		.descriptor_set = descriptor_set
	};

	for (VkWriteDescriptorSet& write : writes) {
		write.dstSet = descriptor_set;
	}
	vkUpdateDescriptorSets(m_context->device(), (uint32_t)writes.size(), writes.data(), 0, nullptr);

	m_uniform_sets.push_back(std::move(uniform_set));
	return (UniformSetId)m_uniform_sets.size() - 1;
}

ScreenResolution VulkanGraphicsController::screen_resolution() const {
	return { m_context->swapchain_extent().width, m_context->swapchain_extent().height };
}

VkBuffer VulkanGraphicsController::buffer_create(VkBufferUsageFlags usage, VkDeviceSize size) {
	VkBuffer buffer;

	VkBufferCreateInfo buffer_info{
		.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
		.size = size,
		.usage = usage,
		.sharingMode = VK_SHARING_MODE_EXCLUSIVE
	};

	if (vkCreateBuffer(m_context->device(), &buffer_info, nullptr, &buffer) != VK_SUCCESS)
		throw std::runtime_error("Failed to create buffer");

	return buffer;
}

VulkanGraphicsController::StagingBuffer VulkanGraphicsController::staging_buffer_create(const void* data, size_t size) {
	VkBuffer staging_buffer = buffer_create(VK_BUFFER_USAGE_TRANSFER_SRC_BIT, size);
	VkDeviceMemory staging_memory = buffer_allocate(staging_buffer, VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);

	void* staging_data = nullptr;
	vkMapMemory(m_context->device(), staging_memory, 0, size, 0, &staging_data);
	memcpy(staging_data, data, size);
	vkUnmapMemory(m_context->device(), staging_memory);

	m_frames[m_frame_index].staging_buffers.emplace_back(staging_buffer, staging_memory);

	return { staging_buffer, staging_memory };
}

VkDeviceMemory VulkanGraphicsController::buffer_allocate(VkBuffer buffer, VkMemoryPropertyFlags mem_props) {
	VkMemoryRequirements mem_requirements;
	vkGetBufferMemoryRequirements(m_context->device(), buffer, &mem_requirements);

	VkMemoryAllocateInfo allocate_info{
		.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.allocationSize = mem_requirements.size,
		.memoryTypeIndex = find_memory_type(mem_requirements.memoryTypeBits, mem_props)
	};

	VkDeviceMemory memory;
	if (vkAllocateMemory(m_context->device(), &allocate_info, nullptr, &memory) != VK_SUCCESS)
		throw std::runtime_error("Failed to allocate buffer memory");

	vkBindBufferMemory(m_context->device(), buffer, memory, 0);

	return memory;
}

void VulkanGraphicsController::buffer_copy(VkBuffer buffer, const void* data, VkDeviceSize size) {
	VkBuffer staging_buffer = buffer_create(VK_BUFFER_USAGE_TRANSFER_SRC_BIT, size);
	VkDeviceMemory staging_memory = buffer_allocate(staging_buffer, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

	void* staging_data = nullptr;
	vkMapMemory(m_context->device(), staging_memory, 0, size, 0, &staging_data);
	memcpy(staging_data, data, size);
	vkUnmapMemory(m_context->device(), staging_memory);

	VkBufferCopy region{
		.size = size
	};

	vkCmdCopyBuffer(m_frames[m_frame_index].draw_buffer, staging_buffer, buffer, 1, &region);

	m_frames[m_frame_index].staging_buffers.emplace_back(staging_buffer, staging_memory);
}

void VulkanGraphicsController::buffer_memory_barrier(VkBuffer& buffer, VkBufferUsageFlags usage, VkDeviceSize offset, VkDeviceSize size) {
	auto [src_stages, src_access] = buffer_usage_to_pipeline_stages_and_access(usage);
	auto [dst_stages, dst_access] = buffer_usage_to_pipeline_stages_and_access(usage);
	
	VkBufferMemoryBarrier barrier{
		.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
		.srcAccessMask = src_access,
		.dstAccessMask = dst_access,
		.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.buffer = buffer,
		.offset = offset,
		.size = size
	};

	vkCmdPipelineBarrier(
		m_frames[m_frame_index].draw_buffer,
		src_stages, dst_stages,
		0,
		0, nullptr,
		1, &barrier,
		0, nullptr
	);
}

VkImage VulkanGraphicsController::vulkan_image_create(ImageViewType view_type, VkFormat format, VkExtent3D extent, uint32_t layer_count, VkImageTiling tiling, VkImageUsageFlags usage) {
	VkImageCreateInfo image_info{
		.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		.flags = view_type == ImageViewType::Cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : (VkImageCreateFlags)0,
		.imageType = view_type == ImageViewType::Cube ? VK_IMAGE_TYPE_2D : (VkImageType)view_type,
		.format = format,
		.extent = extent,
		.mipLevels = 1,
		.arrayLayers = layer_count,
		.samples = VK_SAMPLE_COUNT_1_BIT,
		.tiling = tiling,
		.usage = usage,
		.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
		.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED
	};
	
	VkImage image;
	if (vkCreateImage(m_context->device(), &image_info, nullptr, &image) != VK_SUCCESS)
		throw std::runtime_error("Failed to create image");
	
	return image;
}

VkDeviceMemory VulkanGraphicsController::vulkan_image_allocate(VkImage image, VkMemoryPropertyFlags mem_props) {
	VkMemoryRequirements mem_reqs;
	vkGetImageMemoryRequirements(m_context->device(), image, &mem_reqs);
	
	VkMemoryAllocateInfo memory_info{
		.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.allocationSize = mem_reqs.size,
		.memoryTypeIndex = find_memory_type(mem_reqs.memoryTypeBits, mem_props)
	};

	VkDeviceMemory memory;
	if (vkAllocateMemory(m_context->device(), &memory_info, nullptr, &memory) != VK_SUCCESS)
		throw std::runtime_error("Failed to allocate image memory");

	vkBindImageMemory(m_context->device(), image, memory, 0);

	return memory;
}

VkImageView VulkanGraphicsController::image_view_create(const Image& image, ImageUsageFlags image_usage) {
	VkImageAspectFlags aspect = 0;

	bool has_stencil = format_has_stencil((VkFormat)image.info.format);

	if (image_usage & ImageUsageColorAttachment)
		aspect = VK_IMAGE_ASPECT_COLOR_BIT;
	else if (image_usage & ImageUsageDepthStencilAttachment) {
		aspect = VK_IMAGE_ASPECT_DEPTH_BIT;

		if (has_stencil)
			aspect |= VK_IMAGE_ASPECT_STENCIL_BIT;
	} else if (image_usage & ImageUsageDepthStencilReadOnly) {
		if (image_usage & ImageUsageDepthSampled)
			aspect |= VK_IMAGE_ASPECT_DEPTH_BIT;
		//if (image_usage & ImageUsageStelcilSampled)
		//	aspect |= VK_IMAGE_ASPECT_STENCIL_BIT;
	} else if (image_usage & ImageUsageColorSampled)
		aspect = VK_IMAGE_ASPECT_COLOR_BIT;
	else if (image_usage & ImageUsageDepthSampled)
		aspect = VK_IMAGE_ASPECT_DEPTH_BIT;
	else if (image_usage & ImageUsageTransferSrc || image_usage & ImageUsageTransferDst)
		aspect = image.full_aspect;

	VkImageViewCreateInfo view_info{
		.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
		.image = image.image,
		.viewType = (VkImageViewType)image.info.view_type,
		.format = (VkFormat)image.info.format,
		.subresourceRange = { aspect, 0, 1, 0, image.info.layer_count }
	};
	
	VkImageView image_view;
	if (vkCreateImageView(m_context->device(), &view_info, nullptr, &image_view) != VK_SUCCESS)
		throw std::runtime_error("Failed to create image view");

	return image_view;
}

void VulkanGraphicsController::vulkan_copy_buffer_to_image(VkImage image, StagingBuffer staging_buffer, VkExtent3D extent, VkImageAspectFlags aspect, VkImageLayout layout, uint32_t layer_count) {
	VkBufferImageCopy region{
		.bufferOffset = 0,
		.imageSubresource = { .aspectMask = aspect, .mipLevel = 0, .baseArrayLayer = 0, .layerCount = layer_count },
		.imageExtent = extent
	};

	vkCmdCopyBufferToImage(m_frames[m_frame_index].draw_buffer, staging_buffer.buffer, image, layout, 1, &region);
}

void VulkanGraphicsController::image_should_have_layout(Image& image, VkImageLayout layout) {
	if (image.current_layout != layout && layout != VK_IMAGE_LAYOUT_UNDEFINED) {
		vulkan_image_memory_barrier(image.image, image.full_aspect, image.current_layout, layout, image.info.layer_count);
		image.current_layout = layout;
	}
}

void VulkanGraphicsController::vulkan_image_memory_barrier(VkImage image, VkImageAspectFlags aspect, VkImageLayout old_layout, VkImageLayout new_layout, uint32_t layer_count) {
	auto [src_stages, src_access] = image_layout_to_pipeline_stages_and_access(old_layout);
	auto [dst_stages, dst_access] = image_layout_to_pipeline_stages_and_access(new_layout);

	VkImageMemoryBarrier barrier{
		.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		.srcAccessMask = src_access,
		.dstAccessMask = dst_access,
		.oldLayout = old_layout,
		.newLayout = new_layout,
		.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.image = image,
		.subresourceRange = { aspect, 0, 1, 0, layer_count }
	};

	vkCmdPipelineBarrier(
		m_frames[m_frame_index].draw_buffer,
		src_stages, dst_stages,
		0,
		0, nullptr,
		0, nullptr,
		1, &barrier
	);
}

uint32_t VulkanGraphicsController::find_memory_type(uint32_t type_filter, VkMemoryPropertyFlags properties) {
	const VkPhysicalDeviceMemoryProperties& mem_props = m_context->physical_device_mem_props();

	for (uint32_t i = 0; i < mem_props.memoryTypeCount; i++) {
		if ((type_filter & (1 << i)) && (mem_props.memoryTypes[i].propertyFlags & properties) == properties) {
			return i;
		}
	}

	throw std::runtime_error("Failed to find suitable memory type");
}

uint32_t VulkanGraphicsController::descriptor_pool_allocate(const DescriptorPoolKey& key) {
	if (!m_descriptor_pools.contains(key)) {
		m_descriptor_pools[key] = {};
	}

	std::vector<DescriptorPool>& pools = m_descriptor_pools.at(key);

	for (uint32_t i = 0; i < pools.size(); i++) {
		if (pools[i].usage_count < MAX_SETS_PER_DESCRIPTOR_POOL) {
			pools[i].usage_count++;
			return i;
		}
	}
	
	std::vector<VkDescriptorPoolSize> sizes;

	if (key.uniform_type_counts[(uint32_t)UniformType::Sampler]) {
		VkDescriptorPoolSize size{
			.type = VK_DESCRIPTOR_TYPE_SAMPLER,
			.descriptorCount = key.uniform_type_counts[(uint32_t)UniformType::Sampler] * MAX_SETS_PER_DESCRIPTOR_POOL
		};

		sizes.push_back(size);
	}
	if (key.uniform_type_counts[(uint32_t)UniformType::CombinedImageSampler]) {
		VkDescriptorPoolSize size{
			.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
			.descriptorCount = key.uniform_type_counts[(uint32_t)UniformType::CombinedImageSampler] * MAX_SETS_PER_DESCRIPTOR_POOL
		};

		sizes.push_back(size);
	}
	if (key.uniform_type_counts[(uint32_t)UniformType::SampledImage]) {
		VkDescriptorPoolSize size{
			.type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
			.descriptorCount = key.uniform_type_counts[(uint32_t)UniformType::SampledImage] * MAX_SETS_PER_DESCRIPTOR_POOL
		};

		sizes.push_back(size);
	}
	if (key.uniform_type_counts[(uint32_t)UniformType::UniformBuffer]) {
		VkDescriptorPoolSize size{
			.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
			.descriptorCount = key.uniform_type_counts[(uint32_t)UniformType::UniformBuffer] * MAX_SETS_PER_DESCRIPTOR_POOL
		};

		sizes.push_back(size);
	}

	VkDescriptorPoolCreateInfo pool_info{
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
		.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
		.maxSets = MAX_SETS_PER_DESCRIPTOR_POOL,
		.poolSizeCount = (uint32_t)sizes.size(),
		.pPoolSizes = sizes.data()
	};

	VkDescriptorPool pool;
	if (vkCreateDescriptorPool(m_context->device(), &pool_info, nullptr, &pool) != VK_SUCCESS)
		throw std::runtime_error("Failed to create descriptor pool");

	DescriptorPool desc_pool{
		.pool = pool,
		.usage_count = 1
	};

	pools.push_back(desc_pool);
	return (uint32_t)pools.size() - 1;
}

void VulkanGraphicsController::descriptor_pools_free() {

}