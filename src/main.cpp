// Headless Vulkan raymarch renderer.
//
// Pipeline: fullscreen triangle in a render pass renders the raymarched
// scene into a 1024x1024 R8G8B8A8 image, which is copied to a host-visible
// buffer and written to out/render.png (in-process zlib PNG). GPU time is
// measured with timestamp queries and printed.
//
//   ./build/raymarch [vert.spv frag.spv png-out]

#include <vulkan/vulkan.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "png.h"

static const int RES = 1024;

#define VK_CHECK(expr)                                                     \
  do {                                                                     \
    VkResult r_ = (expr);                                                  \
    if (r_ != VK_SUCCESS) {                                                \
      fprintf(stderr, "Vulkan error %d at %s:%d: %s\n", (int)r_, __FILE__, \
              __LINE__, #expr);                                            \
      exit(1);                                                             \
    }                                                                      \
  } while (0)

static std::vector<uint32_t> readSpirv(const std::string& path) {
  FILE* f = fopen(path.c_str(), "rb");
  if (!f) { fprintf(stderr, "cannot open %s\n", path.c_str()); exit(1); }
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  std::vector<uint32_t> d(n / 4);
  if (fread(d.data(), 1, n, f) != (size_t)n) {
    fprintf(stderr, "short read on %s\n", path.c_str());
    exit(1);
  }
  fclose(f);
  return d;
}

// Pick the first memory type satisfying `props`.
static uint32_t memType(VkPhysicalDeviceProperties& pp,
                        VkMemoryPropertyFlags props) {
  VkPhysicalDeviceMemoryProperties mem{};
  vkGetPhysicalDeviceMemoryProperties(pp.phys, &mem);
  for (uint32_t i = 0; i < mem.memoryTypeCount; i++)
    if ((mem.memoryTypes[i].propertyFlags & props) == props) return i;
  fprintf(stderr, "no memory type for %x\n", props);
  exit(1);
}

struct Gpu {
  VkInstance inst;
  VkPhysicalDevice phys;
  VkPhysicalDeviceProperties pp{};
  VkDevice dev;
  VkQueue queue;
  uint32_t family;
  bool timestamps;
};

static Gpu openGpu() {
  Gpu g{};
  VkApplicationInfo app{};
  app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
  app.pApplicationName = "raymarch";
  app.apiVersion = VK_API_VERSION_1_0;
  VkInstanceCreateInfo ici{};
  ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
  ici.pApplicationInfo = &app;
  VK_CHECK(vkCreateInstance(&ici, nullptr, &g.inst));

  uint32_t n = 0;
  vkEnumeratePhysicalDevices(g.inst, &n, nullptr);
  std::vector<VkPhysicalDevice> gpus(n);
  vkEnumeratePhysicalDevices(g.inst, &n, gpus.data());
  g.phys = gpus[0];
  for (auto h : gpus) {
    VkPhysicalDeviceProperties p{};
    vkGetPhysicalDeviceProperties(h, &p);
    if (p.deviceType != PHYSICAL_DEVICE_TYPE_CPU) { g.phys = h; break; }
  }
  vkGetPhysicalDeviceProperties(g.phys, &g.pp);
  printf("GPU: %s\n", g.pp.deviceName);

  uint32_t qf = 0;
  vkGetPhysicalDeviceQueueFamilyProperties(g.phys, &qf, nullptr);
  std::vector<VkQueueFamilyProperties> fams(qf);
  vkGetPhysicalDeviceQueueFamilyProperties(g.phys, &qf, fams.data());
  g.family = 0;
  for (uint32_t i = 0; i < qf; i++)
    if (fams[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { g.family = i; break; }
  float prio = 1.0f;
  VkDeviceQueueCreateInfo qci{};
  qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
  qci.queueFamilyIndex = g.family;
  qci.queueCount = 1;
  qci.pQueuePriorities = &prio;
  VkPhysicalDeviceFeatures feats{};
  feats.timestampComputeAndGraphics =
      g.pp.features.timestampComputeAndGraphics;
  g.timestamps = feats.timestampComputeAndGraphics;
  VkDeviceCreateInfo dci{};
  dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
  dci.queueCreateInfoCount = 1;
  dci.pQueueCreateInfos = &qci;
  dci.pEnabledFeatures = &feats;
  VK_CHECK(vkCreateDevice(g.phys, &dci, nullptr, &g.dev));
  vkGetDeviceQueue(g.dev, g.family, 0, &g.queue);
  return g;
}

// Create a 2D image with an allocation; returns (image, imageView).
static void makeImage(Gpu& g, VkImage* img, VkImageView* view,
                      VkImageLayout layout) {
  VkImageCreateInfo ici{};
  ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  ici.imageType = VK_IMAGE_TYPE_2D;
  ici.format = VK_FORMAT_R8G8B8A8_UNORM;
  ici.extent = {RES, RES, 1};
  ici.mipLevels = 1;
  ici.arrayLayers = 1;
  ici.samples = VK_SAMPLE_COUNT_1_BIT;
  ici.tiling = VK_IMAGE_TILING_OPTIMAL;
  ici.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
  ici.initialLayout = layout;
  VK_CHECK(vkCreateImage(g.dev, &ici, nullptr, img));
  VkMemoryAllocateInfo ai{};
  ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  VkMemoryRequirements reqs;
  vkGetImageMemoryRequirements(g.dev, *img, &reqs);
  ai.allocationSize = reqs.size;
  ai.memoryTypeIndex = memType(g.pp, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  VkDeviceMemory mem;
  VK_CHECK(vkAllocateMemory(g.dev, &ai, nullptr, &mem));
  VK_CHECK(vkBindImageMemory(g.dev, *img, mem, 0));
  VkImageViewCreateInfo vci{};
  vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
  vci.image = *img;
  vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
  vci.format = VK_FORMAT_R8G8B8A8_UNORM;
  vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  VK_CHECK(vkCreateImageView(g.dev, &vci, nullptr, view));
}

static VkShaderModule makeModule(Gpu& g, const std::vector<uint32_t>& spv) {
  VkShaderModuleCreateInfo ci{};
  ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
  ci.codeSize = spv.size() * 4;
  ci.pCode = spv.data();
  VkShaderModule m;
  VK_CHECK(vkCreateShaderModule(g.dev, &ci, nullptr, &m));
  return m;
}

int main(int argc, char** argv) {
  std::string vertSpv = argc > 1 ? argv[1] : "build/raymarch.vert.spv";
  std::string fragSpv = argc > 2 ? argv[2] : "build/raymarch.frag.spv";
  std::string outPng  = argc > 3 ? argv[3] : "out/render.png";

  Gpu g = openGpu();
  auto& dev = g.dev;

  // --- render pass: single color attachment ---
  VkAttachmentDescription att{};
  att.format = VK_FORMAT_R8G8B8A8_UNORM;
  att.samples = VK_SAMPLE_COUNT_1_BIT;
  att.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  att.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  att.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  VkAttachmentReference ref = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
  VkSubpassDescription sub{};
  sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
  sub.colorAttachmentCount = 1;
  sub.pColorAttachments = &ref;
  VkRenderPassCreateInfo rpci{};
  rpci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
  rpci.attachmentCount = 1;
  rpci.pAttachments = &att;
  rpci.subpassCount = 1;
  rpci.pSubpasses = &sub;
  VkRenderPass rp;
  VK_CHECK(vkCreateRenderPass(dev, &rpci, nullptr, &rp));

  // --- graphics pipeline: fullscreen triangle ---
  VkShaderModule vs = makeModule(g, readSpirv(vertSpv));
  VkShaderModule fs = makeModule(g, readSpirv(fragSpv));
  VkPipelineShaderStageCreateInfo stages[2] = {};
  stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
  stages[0].module = vs;
  stages[0].pName = "main";
  stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
  stages[1].module = fs;
  stages[1].pName = "main";
  VkPipelineVertexInputStateCreateInfo vin{};
  vin.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
  VkPipelineInputAssemblyStateCreateInfo ia{};
  ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
  ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  VkPipelineViewportStateCreateInfo vp{};
  vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
  VkViewport view = {0, 0, RES, RES, 0, 1};
  VkRect2D scissor = {{0, 0}, {RES, RES}};
  vp.viewportCount = 1;
  vp.pViewports = &view;
  vp.scissorCount = 1;
  vp.pScissors = &scissor;
  VkPipelineRasterizationStateCreateInfo rs{};
  rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
  rs.polygonMode = VK_POLYGON_MODE_FILL;
  rs.cullMode = VK_CULL_MODE_NONE;
  rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
  rs.lineWidth = 1;
  VkPipelineMultisampleStateCreateInfo ms{};
  ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
  ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
  VkPipelineColorBlendAttachmentState cbAtt{};
  cbAtt.colorWriteMask = 0xF;
  VkPipelineColorBlendStateCreateInfo cb{};
  cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
  cb.attachmentCount = 1;
  cb.pAttachments = &cbAtt;
  VkPipelineLayoutCreateInfo plc{};
  plc.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  VkPipelineLayout layout;
  VK_CHECK(vkCreatePipelineLayout(dev, &plc, nullptr, &layout));
  VkGraphicsPipelineCreateInfo gpci{};
  gpci.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
  gpci.stageCount = 2;
  gpci.pStages = stages;
  gpci.pVertexInputState = &vin;
  gpci.pInputAssemblyState = &ia;
  gpci.pViewportState = &vp;
  gpci.pRasterizationState = &rs;
  gpci.pMultisampleState = &ms;
  gpci.pColorBlendState = &cb;
  gpci.layout = layout;
  gpci.renderPass = rp;
  gpci.subpass = 0;
  VkPipeline pipe;
  VK_CHECK(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gpci, nullptr,
                                     &pipe));
  printf("[ok] pipeline created\n");

  // --- image + host buffer ---
  VkImage img;
  VkImageView imgView;
  makeImage(g, &img, &imgView, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
  VkBuffer buf;
  VkDeviceMemory bufMem;
  VkBufferCreateInfo bci{};
  bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  bci.size = (VkDeviceSize)RES * RES * 4;
  bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  VK_CHECK(vkCreateBuffer(dev, &bci, nullptr, &buf));
  VkMemoryAllocateInfo ai{};
  ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  VkMemoryRequirements reqs;
  vkGetBufferMemoryRequirements(dev, buf, &reqs);
  ai.allocationSize = reqs.size;
  ai.memoryTypeIndex = memType(g.pp, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
  VK_CHECK(vkAllocateMemory(dev, &ai, nullptr, &bufMem));
  VK_CHECK(vkBindBufferMemory(dev, buf, bufMem, 0));

  // --- timestamp queries ---
  VkQueryPool qpool = VK_NULL_HANDLE;
  if (g.timestamps) {
    VkQueryPoolCreateInfo qci{};
    qci.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
    qci.queryType = VK_QUERY_TYPE_TIMESTAMP;
    qci.queryCount = 2;
    qci.pipelineStageFlags = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT |
                             VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
    VK_CHECK(vkCreateQueryPool(dev, &qci, nullptr, &qpool));
  }

  // --- record ---
  VkCommandPoolCreateInfo cpci{};
  cpci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
  cpci.queueFamilyIndex = g.family;
  VkCommandPool pool;
  VK_CHECK(vkCreateCommandPool(dev, &cpci, nullptr, &pool));
  VkCommandBufferAllocateInfo cbi{};
  cbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  cbi.commandPool = pool;
  cbi.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  cbi.commandBufferCount = 1;
  VkCommandBuffer cmd;
  VK_CHECK(vkAllocateCommandBuffers(dev, &cbi, &cmd));
  VkCommandBufferBeginInfo bgi{};
  bgi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  VK_CHECK(vkBeginCommandBuffer(cmd, &bgi));

  // Framebuffer (needed for render pass)
  VkFramebufferCreateInfo fci{};
  fci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
  fci.renderPass = rp;
  fci.attachmentCount = 1;
  fci.pAttachments = &imgView;
  fci.width = RES;
  fci.height = RES;
  fci.layers = 1;
  VkFramebuffer fb;
  VK_CHECK(vkCreateFramebuffer(dev, &fci, nullptr, &fb));

  VK_CHECK(vkBeginCommandBuffer(cmd, &bgi));
  if (g.timestamps)
    vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, qpool, 0);
  rbi.framebuffer = fb;
  rbi.renderArea = {{0, 0}, {RES, RES}};
  rbi.clearValueCount = 1;
  rbi.pClearValues = &cv;
  vkCmdBeginRenderPass(cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
  vkCmdDraw(cmd, 3, 1, 0, 0);
  vkCmdEndRenderPass(cmd);
  if (g.timestamps)
    vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, qpool, 1);

  // Image COLOR_ATTACHMENT_OPTIMAL -> TRANSFER_SRC_OPTIMAL, then copy out.
  VkImageMemoryBarrier imb{};
  imb.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  imb.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  imb.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  imb.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  imb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  imb.image = img;
  imb.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  imb.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
  imb.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
  vkCmdPipelineBarrier(cmd,
                       VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                           VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                       VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                       nullptr, 1, &imb);
  VkBufferImageCopy bic{};
  bic.bufferOffset = 0;
  bic.bufferRowLength = 0;
  bic.bufferImageHeight = 0;
  bic.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  bic.imageOffset = {0, 0, 0};
  bic.imageExtent = {RES, RES, 1};
  vkCmdCopyImageToBuffer(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf,
                         1, &bic);
  VK_CHECK(vkEndCommandBuffer(cmd));

  // --- submit + wait ---
  auto t0 = std::chrono::steady_clock::now();
  VkSubmitInfo si{};
  si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  si.commandBufferCount = 1;
  si.pCommandBuffers = &cmd;
  VK_CHECK(vkQueueSubmit(g.queue, 1, &si, VK_NULL_HANDLE));
  VK_CHECK(vkQueueWaitIdle(g.queue));
  auto t1 = std::chrono::steady_clock::now();

  double msWall =
      std::chrono::duration<double, std::milli>(t1 - t0).count();
  double msGpu = -1;
  if (g.timestamps) {
    uint64_t ts[2];
    VK_CHECK(vkGetQueryPoolResults(dev, qpool, 0, 2, sizeof(ts), ts,
                                   sizeof(uint64_t),
                                   VK_QUERY_RESULT_64_BIT));
    msGpu = (ts[1] - ts[0]) * g.pp.limits.timestampPeriod * 1e-3;
  }

  void* pixels;
  VK_CHECK(vkMapMemory(dev, bufMem, 0, RES * RES * 4, 0, &pixels));
  bool ok = writePng(outPng.c_str(), RES, RES, (const uint8_t*)pixels);
  vkUnmapMemory(dev, bufMem);

  printf("rendered %s: %.1f ms (wall)%.1f\n", outPng.c_str(), msWall,
         g.timestamps ? 0 : 0);
  if (g.timestamps) printf("GPU time: %.1f ms\n", msGpu);
  if (!ok) return 1;
  printf("done\n");
  return 0;
}
