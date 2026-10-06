// Headless Vulkan compute host for src/scene.comp.
//
//   ./vkmain                       -> out/render.png        (one 1024^2 frame)
//   ./vkmain --frames 300 --out out/frames   -> orbit video frames
//
// The GPU dispatch is timed with VK_QUERY_TYPE_TIMESTAMP and printed in ms.
//
// Device selection: prefers a real GPU (ANV/Intel) over llvmpipe.

#include <vulkan/vulkan.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "png.h"

static int   g_res = 1024;  // --res N: frame size in px (must be divisible by 32)
static float g_fuzz = 1.0f;
static int   g_frame = -1;

// Must match the GLSL std uniform-block layout of `Cam` in scene.comp:
// vec3 has 16-byte alignment, so ta sits at offset 16 (host pad at 12).
// GPU offsets: ro@0 ta@16 focal@28 res@32 time@36 seed@40 fuzz@44 (size 48).
struct alignas(16) CamUniform {
  float ro[3];    // 0
  float _pad0;    // 12
  float ta[3];    // 16
  float focal;    // 28
  float res;      // 32
  float time;     // 36
  float seed;     // 40
  float fuzz;     // 44
};

#define VK_CHECK(x)                                                            \
  do {                                                                         \
    VkResult _r = (x);                                                         \
    if (_r != VK_SUCCESS) {                                                    \
      fprintf(stderr, "Vulkan error %d at %s:%d\n", (int)_r, __FILE__, __LINE__); \
      return 1;                                                                \
    }                                                                          \
  } while (0)

int main(int argc, char** argv) {
  int frames = 1;
  std::string outDir = "out";
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--frames") && i + 1 < argc) frames = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--out") && i + 1 < argc) outDir = argv[++i];
    else if (!strcmp(argv[i], "--res") && i + 1 < argc) g_res = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--fuzz") && i + 1 < argc) g_fuzz = atof(argv[++i]);
    else if (!strcmp(argv[i], "--frame") && i + 1 < argc) g_frame = atoi(argv[++i]);
  }
  if (g_res % 32 != 0) { fprintf(stderr, "--res must be divisible by 32\n"); return 1; }

  // --- instance -----------------------------------------------------------
  VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
  app.pApplicationName = "raymarch-showcase";
  app.apiVersion = VK_API_VERSION_1_2;
  VkInstanceCreateInfo ic = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
  ic.pApplicationInfo = &app;
  VkInstance inst;
  VK_CHECK(vkCreateInstance(&ic, nullptr, &inst));

  // --- device selection: prefer a real GPU over llvmpipe ------------------
  uint32_t nDev = 0;
  vkEnumeratePhysicalDevices(inst, &nDev, nullptr);
  std::vector<VkPhysicalDevice> devs(nDev);
  vkEnumeratePhysicalDevices(inst, &nDev, devs.data());

  VkPhysicalDevice phys = VK_NULL_HANDLE;
  std::string physName;
  for (int pass = 0; pass < 2 && !phys; pass++) {
    for (VkPhysicalDevice d : devs) {
      VkPhysicalDeviceProperties p;
      vkGetPhysicalDeviceProperties(d, &p);
      bool lvp = strstr(p.deviceName, "LVP") != nullptr;
      if (pass == 0 && lvp) continue;
      bool haveCompute = false;
      {
        uint32_t qcnt = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(d, &qcnt, nullptr);
        if (qcnt) {
          std::vector<VkQueueFamilyProperties> qfs(qcnt);
          vkGetPhysicalDeviceQueueFamilyProperties(d, &qcnt, qfs.data());
          for (auto& qf : qfs)
            if (qf.queueFlags & VK_QUEUE_COMPUTE_BIT) haveCompute = true;
        }
      }
      if (!haveCompute) continue;
      if (p.limits.timestampComputeAndGraphics != VK_TRUE) continue;
      phys = d;
      physName = p.deviceName;
      break;
    }
  }
  if (!phys) { fprintf(stderr, "no suitable compute device with timestampComputeAndGraphics\n"); return 1; }
  VkPhysicalDeviceProperties pdev;
  vkGetPhysicalDeviceProperties(phys, &pdev);
  printf("device: %s (api %u.%u.%u)\n", physName.c_str(),
         VK_VERSION_MAJOR(pdev.apiVersion), VK_VERSION_MINOR(pdev.apiVersion), VK_VERSION_PATCH(pdev.apiVersion));

  uint32_t family = 0;
  {
    uint32_t qcnt = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &qcnt, nullptr);
    if (qcnt) {
      std::vector<VkQueueFamilyProperties> qfs(qcnt);
      vkGetPhysicalDeviceQueueFamilyProperties(phys, &qcnt, qfs.data());
      for (uint32_t i = 0; i < qcnt; i++)
        if (qfs[i].queueFlags & VK_QUEUE_COMPUTE_BIT) { family = i; break; }
    }
  }

  VkPhysicalDeviceFeatures feats;
  memset(&feats, 0, sizeof(feats));
  VkDeviceCreateInfo dc = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
  dc.pEnabledFeatures = &feats;
  const float prio = 1.0f;
  VkDeviceQueueCreateInfo qc = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
  qc.queueFamilyIndex = family;
  qc.queueCount = 1;
  qc.pQueuePriorities = &prio;
  dc.queueCreateInfoCount = 1;
  dc.pQueueCreateInfos = &qc;
  VkDevice dev;
  VK_CHECK(vkCreateDevice(phys, &dc, nullptr, &dev));

  // --- buffers ------------------------------------------------------------
  VkPhysicalDeviceMemoryProperties memProps;
  vkGetPhysicalDeviceMemoryProperties(phys, &memProps);
  auto findMem = [&](uint32_t need, uint32_t prefer) {
    for (uint32_t i = 0; i < memProps.memoryTypeCount; i++)
      if ((memProps.memoryTypes[i].propertyFlags & need) == need)
        if (prefer == 0 || (memProps.memoryTypes[i].propertyFlags & prefer) == prefer) return i;
    for (uint32_t i = 0; i < memProps.memoryTypeCount; i++)
      if ((memProps.memoryTypes[i].propertyFlags & need) == need) return i;
    return (uint32_t)-1;
  };

  auto mkBuf = [&](VkDeviceSize size, VkBufferUsageFlags usage,
                   uint32_t need, uint32_t prefer, VkBuffer* out, VkDeviceMemory* outMem) -> int {
    VkBufferCreateInfo bc = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bc.size = size;
    bc.usage = usage;
    VK_CHECK(vkCreateBuffer(dev, &bc, nullptr, out));
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(dev, *out, &mr);
    uint32_t mt = findMem(need, prefer);
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = mr.size;
    ai.memoryTypeIndex = mt;
    VK_CHECK(vkAllocateMemory(dev, &ai, nullptr, outMem));
    VK_CHECK(vkBindBufferMemory(dev, *out, *outMem, 0));
    return 0;
  };

  const VkDeviceSize imgBytes = (VkDeviceSize)g_res * g_res * 4 * 4;
  VkBuffer imgBuf;
  VkDeviceMemory imgMem;
  bool imgHostVisible;
  uint32_t both = findMem(VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, 0);
  if (both != (uint32_t)-1) {  // fast: single host-visible+device-local buffer
    if (mkBuf(imgBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
          VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, 0, &imgBuf, &imgMem) != 0) return 1;
    imgHostVisible = true;
  } else {
    if (mkBuf(imgBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
          VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, &imgBuf, &imgMem) != 0) return 1;
    imgHostVisible = false;
  }
  VkBuffer stageBuf;
  VkDeviceMemory stageMem;
  if (!imgHostVisible)
    if (mkBuf(imgBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0, &stageBuf, &stageMem) != 0) return 1;

  VkBuffer uniBuf;
  VkDeviceMemory uniMem;
  if (mkBuf(256, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0, &uniBuf, &uniMem) != 0) return 1;
  void* uniPtr;
  VK_CHECK(vkMapMemory(dev, uniMem, 0, 256, 0, &uniPtr));

  // --- pipeline ------------------------------------------------------------
  std::vector<char> spv;
  {
    std::ifstream f("build/scene.spv", std::ios::binary);
    if (!f) { fprintf(stderr, "cannot open build/scene.spv\n"); return 1; }
    spv.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
  }
  VkShaderModuleCreateInfo sc = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
  sc.codeSize = spv.size();
  sc.pCode = (const uint32_t*)spv.data();
  VkShaderModule mod;
  VK_CHECK(vkCreateShaderModule(dev, &sc, nullptr, &mod));

  VkDescriptorSetLayoutBinding dslb[2] = {
      {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT},
      {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT}};
  VkDescriptorSetLayoutCreateInfo dslc = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  dslc.bindingCount = 2;
  dslc.pBindings = dslb;
  VkDescriptorSetLayout dsl;
  VK_CHECK(vkCreateDescriptorSetLayout(dev, &dslc, nullptr, &dsl));

  VkPipelineLayoutCreateInfo plc = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  plc.setLayoutCount = 1;
  plc.pSetLayouts = &dsl;
  VkPipelineLayout layout;
  VK_CHECK(vkCreatePipelineLayout(dev, &plc, nullptr, &layout));

  VkPipelineShaderStageCreateInfo ss = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
  ss.stage = VK_SHADER_STAGE_COMPUTE_BIT;
  ss.module = mod;
  ss.pName = "main";
  VkComputePipelineCreateInfo pc = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
  pc.stage = ss;
  pc.layout = layout;
  VkPipeline pipe;
  VK_CHECK(vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &pc, nullptr, &pipe));

  VkDescriptorPoolSize dps[2] = {
      {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1}};
  VkDescriptorPoolCreateInfo dpc = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  dpc.maxSets = 1;
  dpc.poolSizeCount = 2;
  dpc.pPoolSizes = dps;
  VkDescriptorPool pool;
  VK_CHECK(vkCreateDescriptorPool(dev, &dpc, nullptr, &pool));
  VkDescriptorSetAllocateInfo dsa = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  dsa.descriptorPool = pool;
  dsa.descriptorSetCount = 1;
  dsa.pSetLayouts = &dsl;
  VkDescriptorSet dset;
  VK_CHECK(vkAllocateDescriptorSets(dev, &dsa, &dset));
  VkDescriptorBufferInfo bi0 = {uniBuf, 0, 256};
  VkDescriptorBufferInfo bi1 = {imgBuf, 0, imgBytes};
  VkWriteDescriptorSet writes[2] = {
      {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}, {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}};
  writes[0].dstSet = dset; writes[0].dstBinding = 0; writes[0].descriptorCount = 1;
  writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; writes[0].pBufferInfo = &bi0;
  writes[1].dstSet = dset; writes[1].dstBinding = 1; writes[1].descriptorCount = 1;
  writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; writes[1].pBufferInfo = &bi1;
  vkUpdateDescriptorSets(dev, 2, writes, 0, nullptr);

  VkQueryPoolCreateInfo qpc = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
  qpc.queryType = VK_QUERY_TYPE_TIMESTAMP;
  qpc.queryCount = 2;
  VkQueryPool qpool;
  VK_CHECK(vkCreateQueryPool(dev, &qpc, nullptr, &qpool));

  VkCommandPoolCreateInfo cpc = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  cpc.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
  cpc.queueFamilyIndex = family;
  VkCommandPool cpool;
  VK_CHECK(vkCreateCommandPool(dev, &cpc, nullptr, &cpool));
  VkCommandBufferAllocateInfo cba = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  cba.commandPool = cpool;
  cba.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  cba.commandBufferCount = 1;
  VkCommandBuffer cmd;
  VK_CHECK(vkAllocateCommandBuffers(dev, &cba, &cmd));

  VkFenceCreateInfo fc = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  VkFence fence;
  VK_CHECK(vkCreateFence(dev, &fc, nullptr, &fence));

  // --- render loop ----------------------------------------------------------
  const CamUniform ro0 = {{1.92f, 1.02f, 1.78f}, 0.f, {0.00f, 0.78f, 0.00f}, 1.90f, (float)g_res, 0.f, 0.f, 1.0f};
  double totalMs = 0;
  for (int li = 0; li < frames; li++) {
    int f = (g_frame >= 0) ? g_frame : li;  // --frame N renders orbit index N
    CamUniform u = ro0;
    // camera move: starts high overhead looking down at the subject,
    // drops to a level shot aimed at it with the sun in the background
    // (end bearing matches sunDir()'s azimuth). The pan completes at frame
    // 420 (7 s @ 60 fps); the final pose then holds for the last 5 s
    // (frames 420-719). p = pan progress, so `--frames 1 --frame N`
    // previews the exact video-frame camera. All terms share one
    // smoothstep ease.
    const double PAN_END = 420.0;  // frame where the pan finishes (7 s @ 60 fps)
    double p = std::min(1.0, (double)f / PAN_END);
    double e = p * p * (3.0 - 2.0 * p);
    const double sunAz = atan2(0.56, 0.82);  // = atan2(-sd.z, -sd.x) of sunDir()
    double th = sunAz + (14.0 * 3.141592653589793 / 180.0) * (1.0 - e);
    double R = 2.60 + (3.20 - 2.60) * e;               // horizontal dist (slight dolly in)
    double H = 3.30 + (0.92 - 3.30) * e;               // high -> level
    u.ro[0] = 0.02f + (float)(R * cos(th));
    u.ro[1] = (float)H;
    u.ro[2] = 0.02f + (float)(R * sin(th));
    u.time = (float)f;
    u.seed = (float)f * 101.0f;
    u.fuzz = g_fuzz;
    memcpy(uniPtr, &u, sizeof(u));

    VK_CHECK(vkResetCommandPool(dev, cpool, 0));
    VkCommandBufferBeginInfo cb = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    cb.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_CHECK(vkBeginCommandBuffer(cmd, &cb));
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &dset, 0, nullptr);
    vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, qpool, 0);
    vkCmdDispatch(cmd, g_res / 32, g_res / 32, 1);
    vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, qpool, 1);
    if (!imgHostVisible) {
      VkBufferCopy r = {0, 0, imgBytes};
      vkCmdCopyBuffer(cmd, imgBuf, stageBuf, 1, &r);
    }
    VK_CHECK(vkEndCommandBuffer(cmd));

    VkQueue q;
    vkGetDeviceQueue(dev, family, 0, &q);
    VkSubmitInfo sub = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    sub.commandBufferCount = 1;
    sub.pCommandBuffers = &cmd;
    VK_CHECK(vkQueueSubmit(q, 1, &sub, fence));
    VK_CHECK(vkWaitForFences(dev, 1, &fence, VK_TRUE, 60000000000ULL));
    VK_CHECK(vkResetFences(dev, 1, &fence));

    uint64_t ts[2];
    VK_CHECK(vkGetQueryPoolResults(dev, qpool, 0, 2, sizeof(ts), ts, sizeof(uint64_t), VK_QUERY_RESULT_64_BIT));
    double ms = (double)(ts[1] - ts[0]) * (double)pdev.limits.timestampPeriod * 1e-6;
    totalMs += ms;
    printf("frame %d: %.1f ms\n", f, ms);

    void* px;
    if (imgHostVisible) {
      VK_CHECK(vkMapMemory(dev, imgMem, 0, imgBytes, 0, &px));
    } else {
      VK_CHECK(vkMapMemory(dev, stageMem, 0, imgBytes, 0, &px));
    }
    // Shader stores linear floats in 0..1 (vec4 per pixel); convert host-side
    // to 16-bit (0..65535) — passing the raw float bits to writePng16 would be
    // garbage.
    std::vector<uint16_t> u16px((size_t)g_res * g_res * 4);
    {
      const float* fv = (const float*)px;
      for (size_t i = 0; i < (size_t)g_res * g_res; i++) {
        for (int c = 0; c < 3; c++) {
          float v = fv[i * 4 + c];
          v = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
          u16px[i * 4 + c] = (uint16_t)(v * 65535.0f + 0.5f);
        }
        u16px[i * 4 + 3] = 65535;
      }
    }
    char path[512];
    if (frames == 1) snprintf(path, sizeof(path), "%s/render.png", outDir.c_str());
    else snprintf(path, sizeof(path), "%s/%04d.png", outDir.c_str(), f);
    if (!writePng16(path, g_res, g_res, u16px.data())) {
      fprintf(stderr, "writePng16 failed: %s\n", path);
      return 1;
    }
    printf("wrote %s\n", path);
    if (imgHostVisible) vkUnmapMemory(dev, imgMem); else vkUnmapMemory(dev, stageMem);
  }
  if (frames > 1) printf("video: %d frames, GPU total %.1f ms\n", frames, totalMs);
  return 0;
}
