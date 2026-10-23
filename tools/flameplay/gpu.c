#include "gpu.h"

#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_vulkan.h>
#include <libavutil/pixdesc.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// the pass, compiled to spir-v when flameplay's built (convert.comp)
static const uint32_t shader[] =
#include "convert.spv.inc"
;

#define MAX_IMAGES 8
#define DRM_FORMAT_XBGR8888 0x34324258 // 'XB24': rgba8's bytes, alpha unused

// the device's functions this needs, through ffmpeg's loader
#define DEVICE_FUNCS(X) \
	X(vkGetDeviceQueue2) X(vkCreateImage) X(vkDestroyImage) X(vkGetImageMemoryRequirements) \
	X(vkAllocateMemory) X(vkFreeMemory) X(vkBindImageMemory) X(vkGetMemoryFdKHR) \
	X(vkGetImageDrmFormatModifierPropertiesEXT) X(vkGetImageSubresourceLayout) \
	X(vkCreateImageView) X(vkDestroyImageView) X(vkCreateSamplerYcbcrConversion) \
	X(vkDestroySamplerYcbcrConversion) X(vkCreateSampler) X(vkDestroySampler) \
	X(vkCreateDescriptorSetLayout) X(vkDestroyDescriptorSetLayout) \
	X(vkCreatePipelineLayout) X(vkDestroyPipelineLayout) X(vkCreateShaderModule) \
	X(vkDestroyShaderModule) X(vkCreateComputePipelines) X(vkDestroyPipeline) \
	X(vkCreateDescriptorPool) X(vkDestroyDescriptorPool) X(vkAllocateDescriptorSets) \
	X(vkFreeDescriptorSets) X(vkUpdateDescriptorSets) X(vkCreateCommandPool) \
	X(vkDestroyCommandPool) X(vkAllocateCommandBuffers) X(vkResetCommandBuffer) \
	X(vkBeginCommandBuffer) X(vkEndCommandBuffer) X(vkCmdPipelineBarrier2) \
	X(vkCmdBindPipeline) X(vkCmdBindDescriptorSets) X(vkCmdPushConstants) X(vkCmdDispatch) \
	X(vkQueueSubmit2) X(vkCreateFence) X(vkDestroyFence) X(vkWaitForFences) X(vkResetFences)

struct image {
	VkImage image;
	VkDeviceMemory memory;
	VkImageView view;
};

struct gpu {
	AVBufferRef *device;
	AVHWDeviceContext *ctx;
	AVVulkanDeviceContext *vk;
	VkDevice dev;
#define X(name) PFN_##name name;
	DEVICE_FUNCS(X)
#undef X
	PFN_vkGetPhysicalDeviceMemoryProperties vkGetPhysicalDeviceMemoryProperties;
	PFN_vkGetPhysicalDeviceFormatProperties2 vkGetPhysicalDeviceFormatProperties2;

	int family; // a compute queue's, shared with ffmpeg (locked around a submit)
	bool queues_locked; // the driver locks queues itself (internally synchronized)
	VkQueue queue;
	VkCommandPool pool;
	VkCommandBuffer cmd;
	VkFence fence;
	bool pending; // the last pass submitted, not seen done yet
	VkImageView source; // the frame it read, a view of its own
	VkDescriptorPool descriptors;
	VkShaderModule module;

	// the conversion for one kind of frame: made again when frames change
	bool built;
	VkFormat format;
	VkSamplerYcbcrModelConversion model;
	VkSamplerYcbcrRange range;
	VkChromaLocation x_siting, y_siting;
	VkSamplerYcbcrConversion conversion;
	VkSampler sampler;
	VkDescriptorSetLayout set_layout;
	VkPipelineLayout layout;
	VkPipeline pipeline;
	VkDescriptorSet set;

	struct image images[MAX_IMAGES];
	int count, width, height;
};

static bool has_extension(const AVVulkanDeviceContext *vk, const char *name) {
	for (int i = 0; i < vk->nb_enabled_dev_extensions; i++) {
		if (!strcmp(vk->enabled_dev_extensions[i], name)) {
			return true;
		}
	}
	return false;
}

struct gpu *gpu_create(AVBufferRef **device) {
	AVBufferRef *dev = NULL;
	if (av_hwdevice_ctx_create(&dev, AV_HWDEVICE_TYPE_VULKAN, NULL, NULL, 0) < 0) {
		return NULL;
	}
	struct gpu *g = calloc(1, sizeof(*g));
	if (!g) {
		av_buffer_unref(&dev);
		return NULL;
	}
	g->device = dev;
	g->ctx = (AVHWDeviceContext *)dev->data;
	g->vk = g->ctx->hwctx;
	g->dev = g->vk->act_dev;
	g->family = -1;
	// sharing images, and decoding at all
	bool decodes = false;
	for (int i = 0; i < g->vk->nb_qf; i++) {
		decodes |= (g->vk->qf[i].flags & VK_QUEUE_VIDEO_DECODE_BIT_KHR) != 0;
		if (g->family < 0 && (g->vk->qf[i].flags & VK_QUEUE_COMPUTE_BIT)) {
			g->family = g->vk->qf[i].idx;
		}
	}
	if (!decodes || g->family < 0 || !has_extension(g->vk, "VK_EXT_external_memory_dma_buf")
			|| !has_extension(g->vk, "VK_KHR_external_memory_fd")
			|| !has_extension(g->vk, "VK_EXT_image_drm_format_modifier")) {
		gpu_destroy(g);
		return NULL;
	}
	PFN_vkGetInstanceProcAddr gipa = g->vk->get_proc_addr;
	PFN_vkGetDeviceProcAddr gdpa = (PFN_vkGetDeviceProcAddr)gipa(g->vk->inst,
		"vkGetDeviceProcAddr");
	g->vkGetPhysicalDeviceMemoryProperties = (PFN_vkGetPhysicalDeviceMemoryProperties)gipa(
		g->vk->inst, "vkGetPhysicalDeviceMemoryProperties");
	g->vkGetPhysicalDeviceFormatProperties2 = (PFN_vkGetPhysicalDeviceFormatProperties2)gipa(
		g->vk->inst, "vkGetPhysicalDeviceFormatProperties2");
	bool all = gdpa && g->vkGetPhysicalDeviceMemoryProperties
		&& g->vkGetPhysicalDeviceFormatProperties2;
#define X(name) \
	g->name = all ? (PFN_##name)gdpa(g->dev, #name) : NULL; \
	all = all && g->name;
	DEVICE_FUNCS(X)
#undef X
	if (!all) {
		gpu_destroy(g);
		return NULL;
	}
	// ffmpeg made its queues internally synchronized when it could: they're
	// only had back asked for that way
	g->queues_locked = has_extension(g->vk, "VK_KHR_internally_synchronized_queues");
	g->vkGetDeviceQueue2(g->dev, &(VkDeviceQueueInfo2){
		.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_INFO_2,
		.flags = g->queues_locked ? VK_DEVICE_QUEUE_CREATE_INTERNALLY_SYNCHRONIZED_BIT_KHR : 0,
		.queueFamilyIndex = (uint32_t)g->family,
		.queueIndex = 0,
	}, &g->queue);
	if (!g->queue) {
		gpu_destroy(g);
		return NULL;
	}
	VkCommandPoolCreateInfo pool = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
		.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
		.queueFamilyIndex = (uint32_t)g->family,
	};
	VkFenceCreateInfo fence = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
	// a ycbcr sampler may take a few descriptors
	VkDescriptorPoolSize sizes[] = {
		{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4},
		{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1},
	};
	VkDescriptorPoolCreateInfo descriptors = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
		.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
		.maxSets = 1,
		.poolSizeCount = 2,
		.pPoolSizes = sizes,
	};
	VkShaderModuleCreateInfo module = {
		.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
		.codeSize = sizeof(shader),
		.pCode = shader,
	};
	if (g->vkCreateCommandPool(g->dev, &pool, NULL, &g->pool) != VK_SUCCESS
			|| g->vkAllocateCommandBuffers(g->dev, &(VkCommandBufferAllocateInfo){
				.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
				.commandPool = g->pool,
				.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
				.commandBufferCount = 1,
			}, &g->cmd) != VK_SUCCESS
			|| g->vkCreateFence(g->dev, &fence, NULL, &g->fence) != VK_SUCCESS
			|| g->vkCreateDescriptorPool(g->dev, &descriptors, NULL, &g->descriptors) != VK_SUCCESS
			|| g->vkCreateShaderModule(g->dev, &module, NULL, &g->module) != VK_SUCCESS) {
		gpu_destroy(g);
		return NULL;
	}
	*device = av_buffer_ref(dev);
	return g;
}

static void images_free(struct gpu *g) {
	for (int i = 0; i < g->count; i++) {
		struct image *im = &g->images[i];
		if (im->view) {
			g->vkDestroyImageView(g->dev, im->view, NULL);
		}
		if (im->image) {
			g->vkDestroyImage(g->dev, im->image, NULL);
		}
		if (im->memory) {
			g->vkFreeMemory(g->dev, im->memory, NULL);
		}
		*im = (struct image){0};
	}
	g->count = 0;
}

static void pipeline_free(struct gpu *g) {
	if (!g->built) {
		return;
	}
	g->vkFreeDescriptorSets(g->dev, g->descriptors, 1, &g->set);
	g->vkDestroyPipeline(g->dev, g->pipeline, NULL);
	g->vkDestroyPipelineLayout(g->dev, g->layout, NULL);
	g->vkDestroyDescriptorSetLayout(g->dev, g->set_layout, NULL);
	g->vkDestroySampler(g->dev, g->sampler, NULL);
	g->vkDestroySamplerYcbcrConversion(g->dev, g->conversion, NULL);
	g->built = false;
}

// the last pass done (waited for a while): its command buffer and fence
// free to use again
static bool finished(struct gpu *g) {
	if (g->pending
			&& g->vkWaitForFences(g->dev, 1, &g->fence, VK_TRUE, 2000000000ull) != VK_SUCCESS) {
		return false;
	}
	if (g->pending) {
		g->vkResetFences(g->dev, 1, &g->fence);
		g->pending = false;
	}
	if (g->source) {
		g->vkDestroyImageView(g->dev, g->source, NULL);
		g->source = VK_NULL_HANDLE;
	}
	return true;
}

void gpu_destroy(struct gpu *g) {
	if (!g) {
		return;
	}
	if (g->fence) {
		finished(g);
	}
	pipeline_free(g);
	images_free(g);
	if (g->module) {
		g->vkDestroyShaderModule(g->dev, g->module, NULL);
	}
	if (g->descriptors) {
		g->vkDestroyDescriptorPool(g->dev, g->descriptors, NULL);
	}
	if (g->fence) {
		g->vkDestroyFence(g->dev, g->fence, NULL);
	}
	if (g->pool) {
		g->vkDestroyCommandPool(g->dev, g->pool, NULL);
	}
	av_buffer_unref(&g->device);
	free(g);
}

// ---- the images flame shows ----

static int memory_type(struct gpu *g, uint32_t bits, VkMemoryPropertyFlags want) {
	VkPhysicalDeviceMemoryProperties props;
	g->vkGetPhysicalDeviceMemoryProperties(g->vk->phys_dev, &props);
	for (uint32_t i = 0; i < props.memoryTypeCount; i++) {
		if ((bits & (1u << i)) && (props.memoryTypes[i].propertyFlags & want) == want) {
			return (int)i;
		}
	}
	return -1;
}

// the layouts a shader can write rgba8 in that flame can read: the
// driver's own tiled ones (one plane each)
static uint32_t storage_modifiers(struct gpu *g, uint64_t *out, uint32_t max) {
	VkDrmFormatModifierPropertiesEXT mods[32];
	VkDrmFormatModifierPropertiesListEXT list = {
		.sType = VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT,
		.drmFormatModifierCount = 32,
		.pDrmFormatModifierProperties = mods,
	};
	VkFormatProperties2 props = {.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2, .pNext = &list};
	g->vkGetPhysicalDeviceFormatProperties2(g->vk->phys_dev, VK_FORMAT_R8G8B8A8_UNORM, &props);
	uint32_t n = 0;
	for (uint32_t i = 0; i < list.drmFormatModifierCount && n < max; i++) {
		if (mods[i].drmFormatModifierPlaneCount == 1
				&& (mods[i].drmFormatModifierTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT)) {
			out[n++] = mods[i].drmFormatModifier;
		}
	}
	return n;
}

static bool image_make(struct gpu *g, struct image *im, int width, int height,
		const uint64_t *mods, uint32_t nmods, struct gpu_image *out) {
	VkImageDrmFormatModifierListCreateInfoEXT list = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_LIST_CREATE_INFO_EXT,
		.drmFormatModifierCount = nmods,
		.pDrmFormatModifiers = mods,
	};
	VkExternalMemoryImageCreateInfo external = {
		.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
		.pNext = &list,
		.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
	};
	VkImageCreateInfo info = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		.pNext = &external,
		.imageType = VK_IMAGE_TYPE_2D,
		.format = VK_FORMAT_R8G8B8A8_UNORM,
		.extent = {(uint32_t)width, (uint32_t)height, 1},
		.mipLevels = 1,
		.arrayLayers = 1,
		.samples = VK_SAMPLE_COUNT_1_BIT,
		.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
		.usage = VK_IMAGE_USAGE_STORAGE_BIT,
		.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
		.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
	};
	if (g->vkCreateImage(g->dev, &info, NULL, &im->image) != VK_SUCCESS) {
		return false;
	}
	VkMemoryRequirements req;
	g->vkGetImageMemoryRequirements(g->dev, im->image, &req);
	int type = memory_type(g, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	VkMemoryDedicatedAllocateInfo dedicated = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
		.image = im->image,
	};
	VkExportMemoryAllocateInfo export = {
		.sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO,
		.pNext = &dedicated,
		.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
	};
	VkMemoryAllocateInfo alloc = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.pNext = &export,
		.allocationSize = req.size,
		.memoryTypeIndex = (uint32_t)type,
	};
	if (type < 0 || g->vkAllocateMemory(g->dev, &alloc, NULL, &im->memory) != VK_SUCCESS
			|| g->vkBindImageMemory(g->dev, im->image, im->memory, 0) != VK_SUCCESS) {
		return false;
	}
	// which layout the driver picked, and where the plane is in it
	VkImageDrmFormatModifierPropertiesEXT picked = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_PROPERTIES_EXT,
	};
	VkSubresourceLayout plane;
	g->vkGetImageSubresourceLayout(g->dev, im->image, &(VkImageSubresource){
		.aspectMask = VK_IMAGE_ASPECT_MEMORY_PLANE_0_BIT_EXT}, &plane);
	int fd = -1;
	VkMemoryGetFdInfoKHR get = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR,
		.memory = im->memory,
		.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
	};
	VkImageViewCreateInfo view = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
		.image = im->image,
		.viewType = VK_IMAGE_VIEW_TYPE_2D,
		.format = VK_FORMAT_R8G8B8A8_UNORM,
		.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
	};
	if (g->vkGetImageDrmFormatModifierPropertiesEXT(g->dev, im->image, &picked) != VK_SUCCESS
			|| g->vkGetMemoryFdKHR(g->dev, &get, &fd) != VK_SUCCESS
			|| g->vkCreateImageView(g->dev, &view, NULL, &im->view) != VK_SUCCESS) {
		if (fd >= 0) {
			close(fd);
		}
		return false;
	}
	*out = (struct gpu_image){
		.fd = fd,
		.fourcc = DRM_FORMAT_XBGR8888,
		.modifier = picked.drmFormatModifier,
		.offset = (uint32_t)plane.offset,
		.stride = (uint32_t)plane.rowPitch,
		.width = width,
		.height = height,
	};
	return true;
}

bool gpu_images(struct gpu *g, int width, int height, int count, struct gpu_image *out) {
	images_free(g);
	uint64_t mods[32];
	uint32_t nmods = storage_modifiers(g, mods, 32);
	if (!nmods || count > MAX_IMAGES) {
		return false;
	}
	for (int i = 0; i < count; i++) {
		g->count = i + 1;
		if (!image_make(g, &g->images[i], width, height, mods, nmods, &out[i])) {
			for (int j = 0; j < i; j++) {
				close(out[j].fd);
			}
			images_free(g);
			return false;
		}
	}
	g->width = width;
	g->height = height;
	return true;
}

// ---- the pass ----

// the queue's ffmpeg's too: one submit at a time. a driver that keeps its
// queues safe itself needs nothing, else ffmpeg's lock (which that replaces)
static void queue_lock(struct gpu *g, bool lock) {
	if (g->queues_locked) {
		return;
	}
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
	if (lock) {
		g->vk->lock_queue(g->ctx, (uint32_t)g->family, 0);
	} else {
		g->vk->unlock_queue(g->ctx, (uint32_t)g->family, 0);
	}
#pragma clang diagnostic pop
}

// the frame's colours as it says they are: its matrix (an hd one that
// doesn't say: bt.709, a full range one: bt.601, like the software path),
// its range, where its chroma sits
static void describe(const AVFrame *frame, VkSamplerYcbcrModelConversion *model,
		VkSamplerYcbcrRange *range, VkChromaLocation *x, VkChromaLocation *y) {
	bool full = frame->color_range == AVCOL_RANGE_JPEG;
	*range = full ? VK_SAMPLER_YCBCR_RANGE_ITU_FULL : VK_SAMPLER_YCBCR_RANGE_ITU_NARROW;
	switch (frame->colorspace) {
	case AVCOL_SPC_BT709:
		*model = VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_709;
		break;
	case AVCOL_SPC_BT2020_NCL:
	case AVCOL_SPC_BT2020_CL:
		*model = VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_2020;
		break;
	case AVCOL_SPC_UNSPECIFIED:
		*model = !full && frame->height >= 720 ? VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_709
			: VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_601;
		break;
	default:
		*model = VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_601;
		break;
	}
	// mpeg's default (left): beside the first luma sample across, between rows down
	*x = VK_CHROMA_LOCATION_COSITED_EVEN;
	*y = VK_CHROMA_LOCATION_MIDPOINT;
	if (frame->chroma_location == AVCHROMA_LOC_CENTER) {
		*x = VK_CHROMA_LOCATION_MIDPOINT;
	} else if (frame->chroma_location == AVCHROMA_LOC_TOPLEFT) {
		*y = VK_CHROMA_LOCATION_COSITED_EVEN;
	}
}

static bool build(struct gpu *g, VkFormat format, VkSamplerYcbcrModelConversion model,
		VkSamplerYcbcrRange range, VkChromaLocation x, VkChromaLocation y) {
	pipeline_free(g);
	VkSamplerYcbcrConversionCreateInfo conversion = {
		.sType = VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_CREATE_INFO,
		.format = format,
		.ycbcrModel = model,
		.ycbcrRange = range,
		.components = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
			VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY},
		.xChromaOffset = x,
		.yChromaOffset = y,
		.chromaFilter = VK_FILTER_LINEAR,
	};
	if (g->vkCreateSamplerYcbcrConversion(g->dev, &conversion, NULL, &g->conversion)
			!= VK_SUCCESS) {
		return false;
	}
	VkSamplerYcbcrConversionInfo with = {
		.sType = VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO,
		.conversion = g->conversion,
	};
	VkSamplerCreateInfo sampler = {
		.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
		.pNext = &with,
		.magFilter = VK_FILTER_LINEAR,
		.minFilter = VK_FILTER_LINEAR,
		.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
		.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
		.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
		.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
	};
	if (g->vkCreateSampler(g->dev, &sampler, NULL, &g->sampler) != VK_SUCCESS) {
		g->vkDestroySamplerYcbcrConversion(g->dev, g->conversion, NULL);
		return false;
	}
	// a ycbcr sampler has to be immutable, in the layout itself
	VkDescriptorSetLayoutBinding bindings[] = {
		{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT,
			&g->sampler},
		{1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
	};
	VkDescriptorSetLayoutCreateInfo set_layout = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
		.bindingCount = 2,
		.pBindings = bindings,
	};
	VkPushConstantRange push = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 4 * sizeof(float)};
	bool ok = g->vkCreateDescriptorSetLayout(g->dev, &set_layout, NULL, &g->set_layout)
		== VK_SUCCESS;
	ok = ok && g->vkCreatePipelineLayout(g->dev, &(VkPipelineLayoutCreateInfo){
		.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
		.setLayoutCount = 1,
		.pSetLayouts = &g->set_layout,
		.pushConstantRangeCount = 1,
		.pPushConstantRanges = &push,
	}, NULL, &g->layout) == VK_SUCCESS;
	ok = ok && g->vkCreateComputePipelines(g->dev, VK_NULL_HANDLE, 1,
		&(VkComputePipelineCreateInfo){
			.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
			.stage = {
				.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
				.stage = VK_SHADER_STAGE_COMPUTE_BIT,
				.module = g->module,
				.pName = "main",
			},
			.layout = g->layout,
		}, NULL, &g->pipeline) == VK_SUCCESS;
	ok = ok && g->vkAllocateDescriptorSets(g->dev, &(VkDescriptorSetAllocateInfo){
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
		.descriptorPool = g->descriptors,
		.descriptorSetCount = 1,
		.pSetLayouts = &g->set_layout,
	}, &g->set) == VK_SUCCESS;
	g->built = true; // what's there freed together, made or not
	g->format = format;
	g->model = model;
	g->range = range;
	g->x_siting = x;
	g->y_siting = y;
	if (!ok) {
		pipeline_free(g);
	}
	return ok;
}

bool gpu_convert(struct gpu *g, AVFrame *frame, int index) {
	if (frame->format != AV_PIX_FMT_VULKAN || !frame->hw_frames_ctx || index < 0
			|| index >= g->count || !finished(g)) {
		return false;
	}
	AVHWFramesContext *frames = (AVHWFramesContext *)frame->hw_frames_ctx->data;
	AVVulkanFramesContext *vkf = frames->hwctx;
	AVVkFrame *f = (AVVkFrame *)frame->data[0];
	VkFormat format = vkf->format[0];
	VkSamplerYcbcrModelConversion model;
	VkSamplerYcbcrRange range;
	VkChromaLocation x, y;
	describe(frame, &model, &range, &x, &y);
	if (!g->built || g->format != format || g->model != model || g->range != range
			|| g->x_siting != x || g->y_siting != y) {
		if (!build(g, format, model, range, x, y)) {
			return false;
		}
	}
	// the frame, seen through its conversion, only sampled: the decoder's
	// image is made for more (storage...) than a yuv view can be
	VkImageViewUsageCreateInfo sampled = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO,
		.usage = VK_IMAGE_USAGE_SAMPLED_BIT,
	};
	VkSamplerYcbcrConversionInfo with = {
		.sType = VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO,
		.pNext = &sampled,
		.conversion = g->conversion,
	};
	if (g->vkCreateImageView(g->dev, &(VkImageViewCreateInfo){
			.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
			.pNext = &with,
			.image = f->img[0],
			.viewType = VK_IMAGE_VIEW_TYPE_2D,
			.format = format,
			.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
		}, NULL, &g->source) != VK_SUCCESS) {
		return false;
	}
	struct image *out = &g->images[index];
	VkDescriptorImageInfo in_info = {VK_NULL_HANDLE, g->source,
		VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
	VkDescriptorImageInfo out_info = {VK_NULL_HANDLE, out->view, VK_IMAGE_LAYOUT_GENERAL};
	VkWriteDescriptorSet writes[] = {
		{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = g->set, .dstBinding = 0,
			.descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
			.pImageInfo = &in_info},
		{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = g->set, .dstBinding = 1,
			.descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
			.pImageInfo = &out_info},
	};
	g->vkUpdateDescriptorSets(g->dev, 2, writes, 0, NULL);

	vkf->lock_frame(frames, f);
	VkCommandBuffer cmd = g->cmd;
	g->vkResetCommandBuffer(cmd, 0);
	g->vkBeginCommandBuffer(cmd, &(VkCommandBufferBeginInfo){
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
		.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
	});
	// the frame from however the decoder left it to sampled, the picture
	// back from flame (what it showed is overwritten whole)
	VkImageMemoryBarrier2 before[] = {
		{
			.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
			.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
			.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
			.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
			.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
			.oldLayout = f->layout[0],
			.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
			.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			.image = f->img[0],
			.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
		},
		{
			.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
			.srcStageMask = VK_PIPELINE_STAGE_2_NONE,
			.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
			.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
			.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
			.newLayout = VK_IMAGE_LAYOUT_GENERAL,
			.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			.image = out->image,
			.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
		},
	};
	g->vkCmdPipelineBarrier2(cmd, &(VkDependencyInfo){
		.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
		.imageMemoryBarrierCount = 2,
		.pImageMemoryBarriers = before,
	});
	g->vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g->pipeline);
	g->vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g->layout, 0, 1, &g->set,
		0, NULL);
	// the frame's part of the decoder's image (it's rounded up to whole
	// blocks), where it's cropped from
	float size[4] = {(float)frame->crop_left, (float)frame->crop_top,
		1.0f / (float)frames->width, 1.0f / (float)frames->height};
	g->vkCmdPushConstants(cmd, g->layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(size), size);
	g->vkCmdDispatch(cmd, ((uint32_t)g->width + 15) / 16, ((uint32_t)g->height + 15) / 16, 1);
	// done writing: the picture's flame's (another api's) to read now, its
	// memory let go to outside this device
	VkImageMemoryBarrier2 after = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
		.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
		.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
		.dstStageMask = VK_PIPELINE_STAGE_2_NONE,
		.oldLayout = VK_IMAGE_LAYOUT_GENERAL,
		.newLayout = VK_IMAGE_LAYOUT_GENERAL,
		.srcQueueFamilyIndex = (uint32_t)g->family,
		.dstQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL,
		.image = out->image,
		.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
	};
	g->vkCmdPipelineBarrier2(cmd, &(VkDependencyInfo){
		.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
		.imageMemoryBarrierCount = 1,
		.pImageMemoryBarriers = &after,
	});
	g->vkEndCommandBuffer(cmd);

	// after the decoder's done with the frame, and the decoder after us
	VkSemaphoreSubmitInfo wait = {
		.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
		.semaphore = f->sem[0],
		.value = f->sem_value[0],
		.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
	};
	VkSemaphoreSubmitInfo signal = {
		.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
		.semaphore = f->sem[0],
		.value = f->sem_value[0] + 1,
		.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
	};
	VkCommandBufferSubmitInfo cmd_info = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
		.commandBuffer = cmd,
	};
	queue_lock(g, true);
	VkResult r = g->vkQueueSubmit2(g->queue, 1, &(VkSubmitInfo2){
		.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
		.waitSemaphoreInfoCount = 1,
		.pWaitSemaphoreInfos = &wait,
		.commandBufferInfoCount = 1,
		.pCommandBufferInfos = &cmd_info,
		.signalSemaphoreInfoCount = 1,
		.pSignalSemaphoreInfos = &signal,
	}, g->fence);
	queue_lock(g, false);
	if (r == VK_SUCCESS) {
		g->pending = true;
		f->sem_value[0]++;
		f->layout[0] = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		f->access[0] = VK_ACCESS_SHADER_READ_BIT;
	}
	vkf->unlock_frame(frames, f);
	// done before flame's handed it: it can't tell when the gpu's finished
	return r == VK_SUCCESS && finished(g);
}
