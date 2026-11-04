/* Renders a fullscreen quad with a vertex and a fragment SPIR-V shader on
 * lavapipe into a 32x32 RGBA32F image and writes the pixels to a file.
 *
 *   render vert.spv frag.spv layout.txt out.raw
 *
 * layout.txt lines:
 *   ubo <set> <binding> <file>     uniform buffer with the bytes of <file>
 *   tex <set> <binding> <pattern>  combined image sampler, 8x8 RGBA8, pattern 0..7
 *   img <set> <binding> <pattern>  sampled image
 *   smp <set> <binding>            sampler (linear)
 *   pc <file>                      push constants (up to 128 bytes)
 * Vertex input: location 0 vec2 position, location 1 vec2 uv.
 */
#include <vulkan/vulkan.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define W 32
#define H 32
#define CHECK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { fprintf(stderr, "%s failed: %d\n", #x, r_); exit(1); } } while (0)

static VkDevice dev;
static VkPhysicalDevice pd;
static VkQueue q;
static uint32_t qf;

static void *read_file(const char *p, size_t *n) {
	FILE *f = fopen(p, "rb");
	if (!f) { perror(p); exit(1); }
	fseek(f, 0, SEEK_END);
	*n = (size_t)ftell(f);
	fseek(f, 0, SEEK_SET);
	void *b = malloc(*n + 4);
	if (fread(b, 1, *n, f) != *n) exit(1);
	fclose(f);
	return b;
}

static uint32_t mem_type(uint32_t bits, VkMemoryPropertyFlags want) {
	VkPhysicalDeviceMemoryProperties mp;
	vkGetPhysicalDeviceMemoryProperties(pd, &mp);
	for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
		if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
	exit(1);
}

static VkBuffer make_buffer(VkDeviceSize size, VkBufferUsageFlags usage, const void *data, VkDeviceMemory *mem) {
	VkBufferCreateInfo bi = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, size < 16 ? 16 : size, usage };
	VkBuffer b;
	CHECK(vkCreateBuffer(dev, &bi, NULL, &b));
	VkMemoryRequirements mr;
	vkGetBufferMemoryRequirements(dev, b, &mr);
	VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, mr.size,
	                            mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) };
	CHECK(vkAllocateMemory(dev, &ai, NULL, mem));
	CHECK(vkBindBufferMemory(dev, b, *mem, 0));
	void *p;
	vkMapMemory(dev, *mem, 0, mr.size, 0, &p);
	memset(p, 0, mr.size);
	if (data) memcpy(p, data, size);
	vkUnmapMemory(dev, *mem);
	return b;
}

static VkImage make_image(VkFormat fmt, uint32_t w, uint32_t h, VkImageUsageFlags usage, VkImageView *view) {
	VkImageCreateInfo ii = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, NULL, 0, VK_IMAGE_TYPE_2D, fmt, { w, h, 1 }, 1, 1,
	                         VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_OPTIMAL, usage, VK_SHARING_MODE_EXCLUSIVE, 0, NULL,
	                         VK_IMAGE_LAYOUT_UNDEFINED };
	VkImage img;
	CHECK(vkCreateImage(dev, &ii, NULL, &img));
	VkMemoryRequirements mr;
	vkGetImageMemoryRequirements(dev, img, &mr);
	VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, mr.size, mem_type(mr.memoryTypeBits, 0) };
	VkDeviceMemory m;
	CHECK(vkAllocateMemory(dev, &ai, NULL, &m));
	CHECK(vkBindImageMemory(dev, img, m, 0));
	VkImageViewCreateInfo vi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, NULL, 0, img, VK_IMAGE_VIEW_TYPE_2D, fmt,
	                             { 0 }, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
	CHECK(vkCreateImageView(dev, &vi, NULL, view));
	return img;
}

static VkCommandPool pool;

static VkCommandBuffer begin(void) {
	VkCommandBufferAllocateInfo ai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1 };
	VkCommandBuffer cb;
	CHECK(vkAllocateCommandBuffers(dev, &ai, &cb));
	VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
	vkBeginCommandBuffer(cb, &bi);
	return cb;
}

static void submit(VkCommandBuffer cb) {
	vkEndCommandBuffer(cb);
	VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0, NULL, NULL, 1, &cb };
	CHECK(vkQueueSubmit(q, 1, &si, VK_NULL_HANDLE));
	vkQueueWaitIdle(q);
}

static void barrier(VkCommandBuffer cb, VkImage img, VkImageLayout from, VkImageLayout to) {
	VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, VK_ACCESS_MEMORY_WRITE_BIT,
	                           VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, from, to, VK_QUEUE_FAMILY_IGNORED,
	                           VK_QUEUE_FAMILY_IGNORED, img, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
	vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &b);
}

/* 8x8 test texture: each pattern is a different mix of gradients and a checker */
static void pattern(int p, uint8_t *px) {
	for (int y = 0; y < 8; y++)
		for (int x = 0; x < 8; x++) {
			uint8_t *o = px + (y * 8 + x) * 4;
			int c = ((x + y + p) & 1) * 255;
			o[0] = (uint8_t)((x * 32 + p * 40) & 255);
			o[1] = (uint8_t)((y * 32 + p * 70) & 255);
			o[2] = (uint8_t)(p & 1 ? c : 255 - c);
			o[3] = (uint8_t)(128 + p * 16);
		}
}

static VkImageView texture(int p) {
	VkImageView view;
	VkImage img = make_image(VK_FORMAT_R8G8B8A8_UNORM, 8, 8, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, &view);
	uint8_t px[256];
	pattern(p, px);
	VkDeviceMemory m;
	VkBuffer staging = make_buffer(256, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, px, &m);
	VkCommandBuffer cb = begin();
	barrier(cb, img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
	VkBufferImageCopy c = { 0, 0, 0, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, { 0, 0, 0 }, { 8, 8, 1 } };
	vkCmdCopyBufferToImage(cb, staging, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
	barrier(cb, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	submit(cb);
	return view;
}

static VkShaderModule shader(const char *path) {
	size_t n;
	void *code = read_file(path, &n);
	VkShaderModuleCreateInfo ci = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, n, code };
	VkShaderModule m;
	CHECK(vkCreateShaderModule(dev, &ci, NULL, &m));
	return m;
}

typedef struct Res { char kind[8]; int set, binding, pattern; char file[512]; } Res;

int main(int argc, char **argv) {
	if (argc != 5) { fprintf(stderr, "usage: render vert.spv frag.spv layout.txt out.raw\n"); return 2; }
	VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "render", 1, NULL, 0, VK_API_VERSION_1_0 };
	VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app };
	VkInstance inst;
	CHECK(vkCreateInstance(&ici, NULL, &inst));
	uint32_t n = 1;
	vkEnumeratePhysicalDevices(inst, &n, &pd);
	float prio = 1;
	VkDeviceQueueCreateInfo qi = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, 0, 1, &prio };
	qf = 0;
	VkPhysicalDeviceFeatures feats = { 0 };
	VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, NULL, 0, 1, &qi, 0, NULL, 0, NULL, &feats };
	CHECK(vkCreateDevice(pd, &dci, NULL, &dev));
	vkGetDeviceQueue(dev, qf, 0, &q);
	VkCommandPoolCreateInfo pci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, 0, qf };
	CHECK(vkCreateCommandPool(dev, &pci, NULL, &pool));

	/* resources */
	Res res[64];
	int resCount = 0;
	char pcFile[512] = "";
	FILE *lf = fopen(argv[3], "r");
	if (!lf) { perror(argv[3]); return 1; }
	char line[1024];
	while (fgets(line, sizeof(line), lf)) {
		Res r = { 0 };
		if (sscanf(line, "pc %511s", pcFile) == 1) continue;
		if (sscanf(line, "ubo %d %d %511s", &r.set, &r.binding, r.file) == 3) strcpy(r.kind, "ubo");
		else if (sscanf(line, "tex %d %d %d", &r.set, &r.binding, &r.pattern) == 3) strcpy(r.kind, "tex");
		else if (sscanf(line, "img %d %d %d", &r.set, &r.binding, &r.pattern) == 3) strcpy(r.kind, "img");
		else if (sscanf(line, "smp %d %d", &r.set, &r.binding) == 2) strcpy(r.kind, "smp");
		else continue;
		res[resCount++] = r;
	}
	fclose(lf);

	VkSamplerCreateInfo sci = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
	sci.magFilter = sci.minFilter = VK_FILTER_LINEAR;
	sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
	sci.maxLod = 0;
	VkSampler sampler;
	CHECK(vkCreateSampler(dev, &sci, NULL, &sampler));

	enum { SETS = 4 };
	VkDescriptorSetLayout dsl[SETS];
	for (int s = 0; s < SETS; s++) {
		VkDescriptorSetLayoutBinding b[64];
		uint32_t bc = 0;
		for (int i = 0; i < resCount; i++) {
			if (res[i].set != s) continue;
			VkDescriptorType ty = !strcmp(res[i].kind, "ubo") ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER
			                    : !strcmp(res[i].kind, "tex") ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER
			                    : !strcmp(res[i].kind, "img") ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE : VK_DESCRIPTOR_TYPE_SAMPLER;
			b[bc++] = (VkDescriptorSetLayoutBinding){ (uint32_t)res[i].binding, ty, 1, VK_SHADER_STAGE_ALL_GRAPHICS, NULL };
		}
		VkDescriptorSetLayoutCreateInfo li = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, NULL, 0, bc, b };
		CHECK(vkCreateDescriptorSetLayout(dev, &li, NULL, &dsl[s]));
	}
	VkPushConstantRange pcr = { VK_SHADER_STAGE_ALL_GRAPHICS, 0, 128 };
	VkPipelineLayoutCreateInfo pli = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, SETS, dsl, 1, &pcr };
	VkPipelineLayout layout;
	CHECK(vkCreatePipelineLayout(dev, &pli, NULL, &layout));

	VkDescriptorPoolSize ps[3] = { { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 64 }, { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 64 },
	                               { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 64 } };
	VkDescriptorPoolSize ps2[4] = { ps[0], ps[1], ps[2], { VK_DESCRIPTOR_TYPE_SAMPLER, 64 } };
	VkDescriptorPoolCreateInfo dpi = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, NULL, 0, SETS, 4, ps2 };
	VkDescriptorPool dp;
	CHECK(vkCreateDescriptorPool(dev, &dpi, NULL, &dp));
	VkDescriptorSetAllocateInfo dai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, NULL, dp, SETS, dsl };
	VkDescriptorSet sets[SETS];
	CHECK(vkAllocateDescriptorSets(dev, &dai, sets));
	for (int i = 0; i < resCount; i++) {
		VkWriteDescriptorSet w = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, sets[res[i].set], (uint32_t)res[i].binding, 0, 1 };
		VkDescriptorBufferInfo bi;
		VkDescriptorImageInfo ii;
		if (!strcmp(res[i].kind, "ubo")) {
			size_t sz;
			void *data = read_file(res[i].file, &sz);
			VkDeviceMemory m;
			bi.buffer = make_buffer(sz, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, data, &m);
			bi.offset = 0;
			bi.range = VK_WHOLE_SIZE;
			w.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
			w.pBufferInfo = &bi;
		} else {
			ii.sampler = sampler;
			ii.imageView = !strcmp(res[i].kind, "smp") ? VK_NULL_HANDLE : texture(res[i].pattern);
			ii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
			w.descriptorType = !strcmp(res[i].kind, "tex") ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER
			                 : !strcmp(res[i].kind, "img") ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE : VK_DESCRIPTOR_TYPE_SAMPLER;
			w.pImageInfo = &ii;
		}
		vkUpdateDescriptorSets(dev, 1, &w, 0, NULL);
	}

	/* target */
	VkImageView targetView;
	VkImage target = make_image(VK_FORMAT_R32G32B32A32_SFLOAT, W, H,
	                            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, &targetView);
	VkAttachmentDescription ad = { 0, VK_FORMAT_R32G32B32A32_SFLOAT, VK_SAMPLE_COUNT_1_BIT, VK_ATTACHMENT_LOAD_OP_CLEAR,
	                               VK_ATTACHMENT_STORE_OP_STORE, VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_DONT_CARE,
	                               VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL };
	VkAttachmentReference ar = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
	VkSubpassDescription sd = { 0, VK_PIPELINE_BIND_POINT_GRAPHICS, 0, NULL, 1, &ar };
	VkRenderPassCreateInfo rpi = { VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, NULL, 0, 1, &ad, 1, &sd };
	VkRenderPass rp;
	CHECK(vkCreateRenderPass(dev, &rpi, NULL, &rp));
	VkFramebufferCreateInfo fbi = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, NULL, 0, rp, 1, &targetView, W, H, 1 };
	VkFramebuffer fb;
	CHECK(vkCreateFramebuffer(dev, &fbi, NULL, &fb));

	VkPipelineShaderStageCreateInfo st[2] = {
		{ VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_VERTEX_BIT, shader(argv[1]), "main" },
		{ VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_FRAGMENT_BIT, shader(argv[2]), "main" },
	};
	VkVertexInputBindingDescription vb = { 0, 16, VK_VERTEX_INPUT_RATE_VERTEX };
	VkVertexInputAttributeDescription va[2] = { { 0, 0, VK_FORMAT_R32G32_SFLOAT, 0 }, { 1, 0, VK_FORMAT_R32G32_SFLOAT, 8 } };
	VkPipelineVertexInputStateCreateInfo vi = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO, NULL, 0, 1, &vb, 2, va };
	VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, NULL, 0,
	                                              VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP };
	VkViewport vp = { 0, 0, W, H, 0, 1 };
	VkRect2D sc = { { 0, 0 }, { W, H } };
	VkPipelineViewportStateCreateInfo vs = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, NULL, 0, 1, &vp, 1, &sc };
	VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
	rs.lineWidth = 1;
	rs.cullMode = VK_CULL_MODE_NONE;
	VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, NULL, 0, VK_SAMPLE_COUNT_1_BIT };
	VkPipelineColorBlendAttachmentState cba = { 0 };
	cba.colorWriteMask = 15;
	VkPipelineColorBlendStateCreateInfo cb = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, NULL, 0, 0, 0, 1, &cba };
	VkGraphicsPipelineCreateInfo gpi = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, NULL, 0, 2, st, &vi, &ia, NULL, &vs,
	                                     &rs, &ms, NULL, &cb, NULL, layout, rp, 0 };
	VkPipeline pipe;
	CHECK(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gpi, NULL, &pipe));

	float verts[16] = { -1, -1, 0, 0, 1, -1, 1, 0, -1, 1, 0, 1, 1, 1, 1, 1 };
	VkDeviceMemory vm;
	VkBuffer vbuf = make_buffer(sizeof(verts), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, verts, &vm);
	VkDeviceMemory rm;
	VkBuffer readback = make_buffer(W * H * 16, VK_BUFFER_USAGE_TRANSFER_DST_BIT, NULL, &rm);

	VkCommandBuffer c = begin();
	VkClearValue clear = { .color = { { 0.25f, 0.5f, 0.75f, 1.0f } } };
	VkRenderPassBeginInfo rbi = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, NULL, rp, fb, { { 0, 0 }, { W, H } }, 1, &clear };
	vkCmdBeginRenderPass(c, &rbi, VK_SUBPASS_CONTENTS_INLINE);
	vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
	vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, SETS, sets, 0, NULL);
	if (pcFile[0]) {
		size_t sz;
		void *data = read_file(pcFile, &sz);
		vkCmdPushConstants(c, layout, VK_SHADER_STAGE_ALL_GRAPHICS, 0, (uint32_t)(sz > 128 ? 128 : sz), data);
	}
	VkDeviceSize off = 0;
	vkCmdBindVertexBuffers(c, 0, 1, &vbuf, &off);
	vkCmdDraw(c, 4, 1, 0, 0);
	vkCmdEndRenderPass(c);
	VkBufferImageCopy copy = { 0, 0, 0, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, { 0, 0, 0 }, { W, H, 1 } };
	vkCmdCopyImageToBuffer(c, target, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback, 1, &copy);
	submit(c);
	void *p;
	vkMapMemory(dev, rm, 0, W * H * 16, 0, &p);
	FILE *o = fopen(argv[4], "wb");
	fwrite(p, 1, W * H * 16, o);
	fclose(o);
	return 0;
}
