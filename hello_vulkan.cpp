/* Copyright (c) 2014-2018, NVIDIA CORPORATION. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *  * Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *  * Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *  * Neither the name of NVIDIA CORPORATION nor the names of its
 *    contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <sstream>
#include <vulkan/vulkan.hpp>

extern std::vector<std::string> defaultSearchPaths;

#define STB_IMAGE_IMPLEMENTATION
#include "obj_loader.h"
#include "stb_image.h"

// NEW: used by saveScreenshot() to write the rendered image out to a PNG file.
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include "hello_vulkan.h"
#include "nvh//cameramanipulator.hpp"
#include "nvvk/descriptorsets_vk.hpp"
#include "nvvk/pipeline_vk.hpp"
#include "nvvk/images_vk.hpp"
#include "nvh/fileoperations.hpp"
#include "nvvk/commands_vk.hpp"
#include "nvvk/renderpasses_vk.hpp"

#include "nvh/alignment.hpp"
#include "nvvk/shaders_vk.hpp"
#include <random>

// Holding the camera matrices
struct CameraMatrices
{
  nvmath::mat4f view;
  nvmath::mat4f proj;
  nvmath::mat4f viewInverse;
  // #VKRay
  nvmath::mat4f projInverse;
};

//--------------------------------------------------------------------------------------------------
// Keep the handle on the device
// Initialize the tool to do all our allocations: buffers, images
//
void HelloVulkan::setup(const vk::Instance&       instance,
                        const vk::Device&         device,
                        const vk::PhysicalDevice& physicalDevice,
                        uint32_t                  queueFamily)
{
  AppBase::setup(instance, device, physicalDevice, queueFamily);
  m_alloc.init(device, physicalDevice);
  m_debug.setup(m_device);
}

//--------------------------------------------------------------------------------------------------
// Called at each frame to update the camera matrix
//
void HelloVulkan::updateUniformBuffer(const vk::CommandBuffer& cmdBuf)
{
  // Prepare new UBO contents on host.
  // CHANGED: was m_size (window/swapchain size); use m_renderSize (the
  // offscreen ray-traced resolution) so the raster fallback path's aspect
  // ratio matches what it's actually drawing into.
  const float aspectRatio = m_renderSize.width / static_cast<float>(m_renderSize.height);
  CameraMatrices hostUBO = {};
  hostUBO.view           = CameraManip.getMatrix();
  hostUBO.proj           = nvmath::perspectiveVK(CameraManip.getFov(), aspectRatio, 0.1f, 1000.0f);
  // hostUBO.proj[1][1] *= -1;  // Inverting Y for Vulkan (not needed with perspectiveVK).
  hostUBO.viewInverse = nvmath::invert(hostUBO.view);
  // #VKRay
  hostUBO.projInverse = nvmath::invert(hostUBO.proj);

  // UBO on the device, and what stages access it.
  vk::Buffer deviceUBO = m_cameraMat.buffer;
  auto uboUsageStages = vk::PipelineStageFlagBits::eVertexShader
                      | vk::PipelineStageFlagBits::eRayTracingShaderKHR;

  // Ensure that the modified UBO is not visible to previous frames.
  vk::BufferMemoryBarrier beforeBarrier;
  beforeBarrier.setSrcAccessMask(vk::AccessFlagBits::eShaderRead);
  beforeBarrier.setDstAccessMask(vk::AccessFlagBits::eTransferWrite);
  beforeBarrier.setBuffer(deviceUBO);
  beforeBarrier.setOffset(0);
  beforeBarrier.setSize(sizeof hostUBO);
  cmdBuf.pipelineBarrier(
    uboUsageStages,
    vk::PipelineStageFlagBits::eTransfer,
    vk::DependencyFlagBits::eDeviceGroup, {}, {beforeBarrier}, {});

  // Schedule the host-to-device upload. (hostUBO is copied into the cmd
  // buffer so it is okay to deallocate when the function returns).
  cmdBuf.updateBuffer<CameraMatrices>(m_cameraMat.buffer, 0, hostUBO);

  // Making sure the updated UBO will be visible.
  vk::BufferMemoryBarrier afterBarrier;
  afterBarrier.setSrcAccessMask(vk::AccessFlagBits::eTransferWrite);
  afterBarrier.setDstAccessMask(vk::AccessFlagBits::eShaderRead);
  afterBarrier.setBuffer(deviceUBO);
  afterBarrier.setOffset(0);
  afterBarrier.setSize(sizeof hostUBO);
  cmdBuf.pipelineBarrier(
    vk::PipelineStageFlagBits::eTransfer,
    uboUsageStages,
    vk::DependencyFlagBits::eDeviceGroup, {}, {afterBarrier}, {});
}

//--------------------------------------------------------------------------------------------------
// Describing the layout pushed when rendering
//
void HelloVulkan::createDescriptorSetLayout()
{
  using vkDS     = vk::DescriptorSetLayoutBinding;
  using vkDT     = vk::DescriptorType;
  using vkSS     = vk::ShaderStageFlagBits;
  uint32_t nbTxt = static_cast<uint32_t>(m_textures.size());
  uint32_t nbObj = static_cast<uint32_t>(m_objModel.size());
  uint32_t nbInterObj = 3;
  uint32_t nbObjPlus = nbObj + nbInterObj;

  // Camera matrices (binding = 0)
  m_descSetLayoutBind.addBinding(
      vkDS(0, vkDT::eUniformBuffer, 1, vkSS::eVertex | vkSS::eRaygenKHR));
  // Materials (binding = 1)
  m_descSetLayoutBind.addBinding(vkDS(1, vkDT::eStorageBuffer, nbObjPlus,
           vkSS::eVertex | vkSS::eFragment | vkSS::eClosestHitKHR | vkSS::eAnyHitKHR));
  // Scene description (binding = 2)
  m_descSetLayoutBind.addBinding(  //
      vkDS(2, vkDT::eStorageBuffer, 1,
           vkSS::eVertex | vkSS::eFragment | vkSS::eClosestHitKHR | vkSS::eAnyHitKHR));
  // Textures (binding = 3)
  m_descSetLayoutBind.addBinding(vkDS(3, vkDT::eCombinedImageSampler, nbTxt,
           vkSS::eFragment | vkSS::eClosestHitKHR | vkSS::eAnyHitKHR | vkSS::eMissKHR));
  // Materials Index (binding = 4)
  m_descSetLayoutBind.addBinding(vkDS(4, vkDT::eStorageBuffer, nbObjPlus,
                                      vkSS::eFragment | vkSS::eClosestHitKHR | vkSS::eAnyHitKHR));
  // Storing vertices (binding = 5)
  m_descSetLayoutBind.addBinding(  //
      vkDS(5, vkDT::eStorageBuffer, nbObj, vkSS::eClosestHitKHR | vkSS::eAnyHitKHR));
  // Storing indices (binding = 6)
  m_descSetLayoutBind.addBinding(  //
      vkDS(6, vkDT::eStorageBuffer, nbObj, vkSS::eClosestHitKHR | vkSS::eAnyHitKHR));
  // Storing spheres (binding = 7)
  m_descSetLayoutBind.addBinding(  //
      vkDS(7, vkDT::eStorageBuffer, nbInterObj,
           vkSS::eClosestHitKHR | vkSS::eIntersectionKHR | vkSS::eAnyHitKHR));


  m_descSetLayout = m_descSetLayoutBind.createLayout(m_device);
  m_descPool      = m_descSetLayoutBind.createPool(m_device, 1);
  m_descSet       = nvvk::allocateDescriptorSet(m_device, m_descPool, m_descSetLayout);
}

//--------------------------------------------------------------------------------------------------
// Setting up the buffers in the descriptor set
//
void HelloVulkan::updateDescriptorSet()
{
  std::vector<vk::WriteDescriptorSet> writes;

  // Camera matrices and scene description
  vk::DescriptorBufferInfo dbiUnif{m_cameraMat.buffer, 0, VK_WHOLE_SIZE};
  writes.emplace_back(m_descSetLayoutBind.makeWrite(m_descSet, 0, &dbiUnif));
  vk::DescriptorBufferInfo dbiSceneDesc{m_sceneDesc.buffer, 0, VK_WHOLE_SIZE};
  writes.emplace_back(m_descSetLayoutBind.makeWrite(m_descSet, 2, &dbiSceneDesc));

  // All material buffers, 1 buffer per OBJ
  std::vector<vk::DescriptorBufferInfo> dbiMat;
  std::vector<vk::DescriptorBufferInfo> dbiMatIdx;
  std::vector<vk::DescriptorBufferInfo> dbiVert;
  std::vector<vk::DescriptorBufferInfo> dbiIdx;
  for(auto& obj : m_objModel)
  {
    dbiMat.emplace_back(obj.matColorBuffer.buffer, 0, VK_WHOLE_SIZE);
    dbiMatIdx.emplace_back(obj.matIndexBuffer.buffer, 0, VK_WHOLE_SIZE);
    dbiVert.emplace_back(obj.vertexBuffer.buffer, 0, VK_WHOLE_SIZE);
    dbiIdx.emplace_back(obj.indexBuffer.buffer, 0, VK_WHOLE_SIZE);
  }

  for(int i = 0; i < 3; i++) {
    dbiMat.emplace_back(m_aabbsMatColorBuffer[i].buffer, 0, VK_WHOLE_SIZE);
    dbiMatIdx.emplace_back(m_aabbsMatIndexBuffer[i].buffer, 0, VK_WHOLE_SIZE);
  }

  writes.emplace_back(m_descSetLayoutBind.makeWriteArray(m_descSet, 1, dbiMat.data()));
  writes.emplace_back(m_descSetLayoutBind.makeWriteArray(m_descSet, 4, dbiMatIdx.data()));
  writes.emplace_back(m_descSetLayoutBind.makeWriteArray(m_descSet, 5, dbiVert.data()));
  writes.emplace_back(m_descSetLayoutBind.makeWriteArray(m_descSet, 6, dbiIdx.data()));

  std::vector<vk::DescriptorBufferInfo> dbiSpheres;
  for(int i = 0; i < 3; i++) {
    dbiSpheres.emplace_back(m_aabbsBuffer[i].buffer, 0, VK_WHOLE_SIZE);
  }
  writes.emplace_back(m_descSetLayoutBind.makeWriteArray(m_descSet, 7, dbiSpheres.data()));

  // All texture samplers
  std::vector<vk::DescriptorImageInfo> diit;
  for(auto& texture : m_textures)
  {
    diit.emplace_back(texture.descriptor);
  }
  writes.emplace_back(m_descSetLayoutBind.makeWriteArray(m_descSet, 3, diit.data()));

  // Writing the information
  m_device.updateDescriptorSets(static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
}

//--------------------------------------------------------------------------------------------------
// Creating the pipeline layout
//
void HelloVulkan::createGraphicsPipeline()
{
  using vkSS = vk::ShaderStageFlagBits;

  vk::PushConstantRange pushConstantRanges = {vkSS::eVertex | vkSS::eFragment, 0,
                                              sizeof(ObjPushConstant)};

  // Creating the Pipeline Layout
  vk::PipelineLayoutCreateInfo pipelineLayoutCreateInfo;
  vk::DescriptorSetLayout      descSetLayout(m_descSetLayout);
  pipelineLayoutCreateInfo.setSetLayoutCount(1);
  pipelineLayoutCreateInfo.setPSetLayouts(&descSetLayout);
  pipelineLayoutCreateInfo.setPushConstantRangeCount(1);
  pipelineLayoutCreateInfo.setPPushConstantRanges(&pushConstantRanges);
  m_pipelineLayout = m_device.createPipelineLayout(pipelineLayoutCreateInfo);

  // Creating the Pipeline
  std::vector<std::string>                paths = defaultSearchPaths;
  nvvk::GraphicsPipelineGeneratorCombined gpb(m_device, m_pipelineLayout, m_offscreenRenderPass);
  gpb.depthStencilState.depthTestEnable = true;
  gpb.addShader(nvh::loadFile("spv/vert_shader.vert.spv", true, paths, true), vkSS::eVertex);
  gpb.addShader(nvh::loadFile("spv/frag_shader.frag.spv", true, paths, true), vkSS::eFragment);
  gpb.addBindingDescription({0, sizeof(VertexObj)});
  gpb.addAttributeDescriptions({{0, 0, vk::Format::eR32G32B32Sfloat, offsetof(VertexObj, pos)},
                                {1, 0, vk::Format::eR32G32B32Sfloat, offsetof(VertexObj, nrm)},
                                {2, 0, vk::Format::eR32G32B32Sfloat, offsetof(VertexObj, color)},
                                {3, 0, vk::Format::eR32G32Sfloat, offsetof(VertexObj, texCoord)}});

  m_graphicsPipeline = gpb.createPipeline();
  m_debug.setObjectName(m_graphicsPipeline, "Graphics");
}

//--------------------------------------------------------------------------------------------------
// Loading the OBJ file and setting up all buffers
//
void HelloVulkan::loadModel(const std::string&              filename,
                            nvmath::mat4f                   transform,
                            const std::vector<std::string>& additionalTextures)
{
  using vkBU = vk::BufferUsageFlagBits;

  LOGI("Loading Mesh:  %s \n", filename.c_str());
  ObjLoader loader;
  loader.loadModel(filename);

  // add additional textures into vector 
  loader.m_textures.insert(loader.m_textures.end(), additionalTextures.begin(),
                           additionalTextures.end());
  
  // The obj loader is inverting the y-texture coordinate,
  // which is reverted here
  for(auto& v : loader.m_vertices) {
    v.texCoord.y = 1.0f - v.texCoord.y;
  }

  // Converting from Srgb to linear
  for(auto& m : loader.m_materials)
  {
    m.ambient  = nvmath::pow(m.ambient, 2.2f);
    m.diffuse  = nvmath::pow(m.diffuse, 2.2f);
    m.specular = nvmath::pow(m.specular, 2.2f);
  }

  ObjInstance instance;
  instance.objIndex    = static_cast<uint32_t>(m_objModel.size());
  instance.transform   = transform;
  instance.transformIT = nvmath::transpose(nvmath::invert(transform));
  instance.txtOffset   = static_cast<uint32_t>(m_textures.size());

  ObjModel model;
  model.nbIndices  = static_cast<uint32_t>(loader.m_indices.size());
  model.nbVertices = static_cast<uint32_t>(loader.m_vertices.size());

  // Create the buffers on Device and copy vertices, indices and materials
  nvvk::CommandPool cmdBufGet(m_device, m_graphicsQueueIndex);
  vk::CommandBuffer cmdBuf = cmdBufGet.createCommandBuffer();
  model.vertexBuffer =
      m_alloc.createBuffer(cmdBuf, loader.m_vertices,
                           vkBU::eVertexBuffer | vkBU::eStorageBuffer | vkBU::eShaderDeviceAddress
                               | vkBU::eAccelerationStructureBuildInputReadOnlyKHR);
  model.indexBuffer =
      m_alloc.createBuffer(cmdBuf, loader.m_indices,
                           vkBU::eIndexBuffer | vkBU::eStorageBuffer | vkBU::eShaderDeviceAddress
                               | vkBU::eAccelerationStructureBuildInputReadOnlyKHR);
  model.matColorBuffer = m_alloc.createBuffer(cmdBuf, loader.m_materials, vkBU::eStorageBuffer);
  model.matIndexBuffer = m_alloc.createBuffer(cmdBuf, loader.m_matIndx, vkBU::eStorageBuffer);
  // Creates all textures found
  createTextureImages(cmdBuf, loader.m_textures);
  cmdBufGet.submitAndWait(cmdBuf);
  m_alloc.finalizeAndReleaseStaging();

  std::string objNb = std::to_string(instance.objIndex);
  m_debug.setObjectName(model.vertexBuffer.buffer, (std::string("vertex_" + objNb).c_str()));
  m_debug.setObjectName(model.indexBuffer.buffer, (std::string("index_" + objNb).c_str()));
  m_debug.setObjectName(model.matColorBuffer.buffer, (std::string("mat_" + objNb).c_str()));
  m_debug.setObjectName(model.matIndexBuffer.buffer, (std::string("matIdx_" + objNb).c_str()));

  m_objModel.emplace_back(model);
  m_objInstance.emplace_back(instance);
}

//--------------------------------------------------------------------------------------------------
// Creating the uniform buffer holding the camera matrices
// - Buffer is host visible
//
void HelloVulkan::createUniformBuffer()
{
  using vkBU = vk::BufferUsageFlagBits;
  using vkMP = vk::MemoryPropertyFlagBits;

  m_cameraMat = m_alloc.createBuffer(sizeof(CameraMatrices),
                                     vkBU::eUniformBuffer | vkBU::eTransferDst, vkMP::eDeviceLocal);
  m_debug.setObjectName(m_cameraMat.buffer, "cameraMat");
}

//--------------------------------------------------------------------------------------------------
// Create a storage buffer containing the description of the scene elements
// - Which geometry is used by which instance
// - Transformation
// - Offset for texture
//
void HelloVulkan::createSceneDescriptionBuffer()
{
  using vkBU = vk::BufferUsageFlagBits;
  nvvk::CommandPool cmdGen(m_device, m_graphicsQueueIndex);

  auto cmdBuf = cmdGen.createCommandBuffer();
  m_sceneDesc = m_alloc.createBuffer(cmdBuf, m_objInstance, vkBU::eStorageBuffer);
  cmdGen.submitAndWait(cmdBuf);
  m_alloc.finalizeAndReleaseStaging();
  m_debug.setObjectName(m_sceneDesc.buffer, "sceneDesc");
}

std::string HelloVulkan::determineFileType(const std::string& filename) 
{
  if(filename.size() < 4){
    return filename;
  }
  
  std::string p0 = filename.substr(0, filename.size() - 4);
  std::string p1 = filename.substr(filename.size() - 4);
  if(p1 != ".???"){
    return filename;
  }

  std::string testPNG = p0 + ".png";
  FILE* filePNG = fopen(testPNG.c_str(), "r");
  if(filePNG) {
    fclose(filePNG);
    return testPNG;
  }
 
  std::string testPFM = p0 + ".pfm";
  FILE*       filePFM = fopen(testPFM.c_str(), "r");
  if(filePFM)
  {
    fclose(filePFM);
    return testPFM;
  }
  return filename;
}

// gets next string ignoring comments marked by "#"
void HelloVulkan::loadPFMgetNextHeaderLineHelper(FILE* fp, char* line)
{
  int i;
  line[0] = '\0';
  while(line[0] == '\0')
  {
    fscanf(fp, "%s", line);
    i = -1;
    do
    {
      i++;
      if(line[i] == '#')
      {
        line[i] = '\0';
        while(fgetc(fp) != '\n')
          ;
      }
    } while(line[i] != '\0');
  }
}

bool HelloVulkan::loadPFM(const std::string filename,
                          int&              width,
                          int&              height,
                          std::vector<float>& floatImg)
{
  const int identiferLength = 1000;
  char      identifer[identiferLength];
  FILE* file = fopen(filename.c_str(), "rb");
  int channels = 0;
  if(file == NULL)
  {
    std::cout << "ERROR: could not open file" << filename << std::endl;
    return false;
  }

  // read "magic number" for identifying the file type.
  char letter;
  char letter2;
  loadPFMgetNextHeaderLineHelper(file, identifer);
  sscanf(identifer, "%c%c", &letter, &letter2);

  if(letter != 'P')
  {
    fclose(file);
    return false;
  }

  bool found = false;

  if(letter2 == 'f')
  {
    channels = 1;
    found    = true;
  }
  if(letter2 == 'F')
  {
    channels = 3;
    found    = true;
  }
  if(letter2 == 'g')
  {
    sscanf(identifer, "%c%c%d", &letter, &letter2, &channels);
    if(channels > 1 && channels < 256)
    {
      found = true;
    }
  }

  if(!found)
  {
    fclose(file);
    return false;
  }

  // read width and height of the image
  loadPFMgetNextHeaderLineHelper(file, identifer);
  sscanf(identifer, "%d", &width);
  loadPFMgetNextHeaderLineHelper(file, identifer);
  sscanf(identifer, "%d", &height);
  float byteOrder;
  loadPFMgetNextHeaderLineHelper(file, identifer);
  sscanf(identifer, "%f", &byteOrder);

  if(byteOrder >= 0.0)
  {
    std::cout << "ERROR: only little-endian byte order is supported" << std::endl;
    fclose(file);
    return false;
  }

  // A single whitespace character (usually a newline).
  fgetc(file);

  float* imageData = new float[width * height * channels];
  fread(imageData, sizeof(float), width * height * channels, file);

  floatImg.resize(width * height * 4);
  float pixelmax = -1e37f;
  float pixelmin = 1e37f;
  for(int y = 0; y < height; y++){
    for(int x = 0; x < width; x++) {

      for(int z = 0; z < channels; z++){
        float value = imageData[y * width * channels + x * channels + z];
        floatImg[y * width * 4 + x * 4 + z] = value;
        if(pixelmax < value)
          pixelmax = value;
        if(pixelmin > value)
          pixelmin = value;
      }

      // fill channel data
      for(int z = channels; z < 4; z++) {
        if(z == 3) {
          floatImg[y * width * 4 + x * 4 + z] = 1.0;
        } else {
          floatImg[y * width * 4 + x * 4 + z] = 0.0;
        }
        
      }
    }
  }

  std::cout << "PFM image properties: width=" << width << ", height=" << height << ", max_value="<< pixelmax << ", min_value=" << pixelmin << std::endl;
  free(imageData);


  fclose(file);



  return true;
}


//--------------------------------------------------------------------------------------------------
// Creating all textures and samplers
//
void HelloVulkan::createTextureImages(const vk::CommandBuffer&        cmdBuf,
                                      const std::vector<std::string>& textures)
{
  using vkIU = vk::ImageUsageFlagBits;

  vk::SamplerCreateInfo samplerCreateInfo{
      {}, vk::Filter::eLinear, vk::Filter::eLinear, vk::SamplerMipmapMode::eLinear};
  samplerCreateInfo.setMaxLod(FLT_MAX);
  samplerCreateInfo.addressModeU = vk::SamplerAddressMode::eClampToEdge;
  samplerCreateInfo.addressModeV = vk::SamplerAddressMode::eClampToEdge;
  samplerCreateInfo.addressModeW = vk::SamplerAddressMode::eClampToEdge;
  //vk::Format format = vk::Format::eR8G8B8A8Srgb;
  vk::Format format = vk::Format::eR32G32B32A32Sfloat;
  // If no textures are present, create a dummy one to accommodate the pipeline layout
   if(textures.empty() && m_textures.empty())
  {
    nvvk::Texture texture;

    std::array<float, 4>   color{1.0, 1.0, 0.0, 1.0};
    vk::DeviceSize         bufferSize      = sizeof(color);
    auto                   imgSize         = vk::Extent2D(1, 1);
    auto                   imageCreateInfo = nvvk::makeImage2DCreateInfo(imgSize, format);

    // Creating the dummy texture
    nvvk::Image image = m_alloc.createImage(cmdBuf, bufferSize, color.data(), imageCreateInfo);
    vk::ImageViewCreateInfo ivInfo = nvvk::makeImageViewCreateInfo(image.image, imageCreateInfo);
    texture                        = m_alloc.createTexture(image, ivInfo, samplerCreateInfo);

    // The image format must be in VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
    nvvk::cmdBarrierImageLayout(cmdBuf, texture.image, vk::ImageLayout::eUndefined,
                                vk::ImageLayout::eShaderReadOnlyOptimal);
    m_textures.push_back(texture);
  }
  else
  {
    // Uploading all images
    for(const auto& textureFileName : textures)
    {
      std::string texture = determineFileType(textureFileName);

      std::cout << "Loading Image: " << texture << std::endl;
  
      int texWidth, texHeight, texChannels;
      float* pixels;
      std::vector<float> floatPixels;
      // NEW: Radiance (.hdr) environment map support, added for the background.hdr
      // sky texture. This loader previously only handled ".pfm" (via loadPFM) and
      // 8-bit ".png" (via stbi_load below); ".hdr" needs stbi_loadf since the data
      // is already linear floating-point and must not be divided by 255 like the
      // 8-bit path does.
      bool isHdr = texture.size() >= 4 && texture.substr(texture.size() - 4) == ".hdr";
      bool floatImageFound = !isHdr && this->loadPFM(texture, texWidth, texHeight, floatPixels);
      if(isHdr)
      {
        float* hdrPixels =
            stbi_loadf(texture.c_str(), &texWidth, &texHeight, &texChannels, STBI_rgb_alpha);
        if(!hdrPixels)
        {
          std::array<float, 4> colorBackup{0.0, 0.0, 0.0, 1.0};
          texWidth = texHeight = 1;
          pixels               = colorBackup.data();
        }
        else
        {
          // Flip vertically (same convention as the PNG path below) since the
          // miss shader samples with v=0 at the top of the image.
          texChannels = 4;
          floatPixels.resize(static_cast<size_t>(texWidth) * texHeight * texChannels);
          int nn = texWidth * texChannels;
          for(int y = 0; y < texHeight; y++) {
            for(int xx = 0; xx < nn; xx++) {
              floatPixels[y * nn + xx] = hdrPixels[(texHeight - 1 - y) * nn + xx];
            }
          }
          pixels = floatPixels.data();
          stbi_image_free(hdrPixels);
        }
      }
      else if(floatImageFound)
      {
        pixels = floatPixels.data();
      }else {

        std::stringstream o;

        //o << "media/textures/" << texture;
        //std::string txtFile = nvh::findFile(o.str(), defaultSearchPaths, true);

        stbi_uc* stbi_pixels =
            stbi_load(texture.c_str(), &texWidth, &texHeight, &texChannels, STBI_rgb_alpha);

        // Handle failure
        if(!stbi_pixels)
        {
          std::array<float, 4> colorBackup{1.0, 1.0, 1.0, 1.0};
          texWidth = texHeight = 1;
          texChannels          = 4;
          pixels               = colorBackup.data();
        }
        else
        {
          texChannels = 4;
          floatPixels.resize(texWidth * texHeight * texChannels);
          int nn      = texWidth * texChannels;
          for(int y = 0; y < texHeight; y++) {
            for(int xx = 0; xx < nn; xx++){
              floatPixels[y * nn + xx] = float(stbi_pixels[(texHeight - 1 - y) * nn + xx]) / 255.0f;
            }
          }
          pixels = floatPixels.data();
        }
        stbi_image_free(stbi_pixels);
      }

      vk::DeviceSize bufferSize = static_cast<uint64_t>(texWidth) * texHeight * sizeof(float) * 4;
      auto           imgSize    = vk::Extent2D(texWidth, texHeight);
      auto imageCreateInfo = nvvk::makeImage2DCreateInfo(imgSize, format, vkIU::eSampled, true); 

      {
        nvvk::Image image =
            m_alloc.createImage(cmdBuf, bufferSize, pixels, imageCreateInfo);
        nvvk::cmdGenerateMipmaps(cmdBuf, image.image, format, imgSize, imageCreateInfo.mipLevels);
        vk::ImageViewCreateInfo ivInfo =
            nvvk::makeImageViewCreateInfo(image.image, imageCreateInfo);
        nvvk::Texture texture = m_alloc.createTexture(image, ivInfo, samplerCreateInfo);

        m_textures.push_back(texture);
      }
    }
  }
}

//--------------------------------------------------------------------------------------------------
// Destroying all allocations
//
void HelloVulkan::destroyResources()
{
  m_device.destroy(m_graphicsPipeline);
  m_device.destroy(m_pipelineLayout);
  m_device.destroy(m_descPool);
  m_device.destroy(m_descSetLayout);
  m_alloc.destroy(m_cameraMat);
  m_alloc.destroy(m_sceneDesc);

  for(auto& m : m_objModel)
  {
    m_alloc.destroy(m.vertexBuffer);
    m_alloc.destroy(m.indexBuffer);
    m_alloc.destroy(m.matColorBuffer);
    m_alloc.destroy(m.matIndexBuffer);
  }

  for(auto& t : m_textures)
  {
    m_alloc.destroy(t);
  }

  //#Post
  m_device.destroy(m_postPipeline);
  m_device.destroy(m_postPipelineLayout);
  m_device.destroy(m_postDescPool);
  m_device.destroy(m_postDescSetLayout);
  m_alloc.destroy(m_offscreenColor);
  m_alloc.destroy(m_offscreenDepth);
  m_device.destroy(m_offscreenRenderPass);
  m_device.destroy(m_offscreenFramebuffer);

  // #VKRay
  m_rtBuilder.destroy();
  m_device.destroy(m_rtDescPool);
  m_device.destroy(m_rtDescSetLayout);
  m_device.destroy(m_rtPipeline);
  m_device.destroy(m_rtPipelineLayout);
  m_alloc.destroy(m_rtSBTBuffer);

  for(int i = 0; i < 3; i++) {
    m_alloc.destroy(m_aabbsBuffer[i]);
    m_alloc.destroy(m_aabbsAabbBuffer[i]);
    m_alloc.destroy(m_aabbsMatColorBuffer[i]);
    m_alloc.destroy(m_aabbsMatIndexBuffer[i]);
  }
}

//--------------------------------------------------------------------------------------------------
// Drawing the scene in raster mode
//
void HelloVulkan::rasterize(const vk::CommandBuffer& cmdBuf)
{
  using vkPBP = vk::PipelineBindPoint;
  using vkSS  = vk::ShaderStageFlagBits;
  vk::DeviceSize offset{0};

  m_debug.beginLabel(cmdBuf, "Rasterize");

  // Dynamic Viewport
  // CHANGED: was m_size; this draws into the offscreen framebuffer
  // (m_offscreenFramebuffer), which is now sized m_renderSize, not m_size.
  cmdBuf.setViewport(0, {vk::Viewport(0, 0, (float)m_renderSize.width, (float)m_renderSize.height, 0, 1)});
  cmdBuf.setScissor(0, {{{0, 0}, {m_renderSize.width, m_renderSize.height}}});

  // Drawing all triangles
  cmdBuf.bindPipeline(vkPBP::eGraphics, m_graphicsPipeline);
  cmdBuf.bindDescriptorSets(vkPBP::eGraphics, m_pipelineLayout, 0, {m_descSet}, {});
  for(int i = 0; i < m_objInstance.size(); ++i)
  {
    auto& inst                = m_objInstance[i];
    auto& model               = m_objModel[inst.objIndex];
    m_pushConstant.instanceId = i;  // Telling which instance is drawn
    cmdBuf.pushConstants<ObjPushConstant>(m_pipelineLayout, vkSS::eVertex | vkSS::eFragment, 0,
                                          m_pushConstant);

    cmdBuf.bindVertexBuffers(0, {model.vertexBuffer.buffer}, {offset});
    cmdBuf.bindIndexBuffer(model.indexBuffer.buffer, 0, vk::IndexType::eUint32);
    cmdBuf.drawIndexed(model.nbIndices, 1, 0, 0, 0);
  }
  m_debug.endLabel(cmdBuf);
}

//--------------------------------------------------------------------------------------------------
// Handling resize of the window
//
void HelloVulkan::onResize(int /*w*/, int /*h*/)
{
  // no resizing for now
  
  /*createOffscreenRender();
  updatePostDescriptorSet();
  updateRtDescriptorSet();*/
}

//////////////////////////////////////////////////////////////////////////
// Post-processing
//////////////////////////////////////////////////////////////////////////

//--------------------------------------------------------------------------------------------------
// Creating an offscreen frame buffer and the associated render pass
//
void HelloVulkan::createOffscreenRender()
{
  m_alloc.destroy(m_offscreenColor);
  m_alloc.destroy(m_offscreenDepth);

  // Creating the color image
  // CHANGED: was sized m_size (window/swapchain size, clamped to the monitor);
  // now sized m_renderSize (settings.launchSizeX/Y) so the ray-traced image
  // resolution doesn't depend on the screen's resolution.
  {
    auto colorCreateInfo = nvvk::makeImage2DCreateInfo(m_renderSize, m_offscreenColorFormat,
                                                       vk::ImageUsageFlagBits::eColorAttachment
                                                           | vk::ImageUsageFlagBits::eSampled
                                                           | vk::ImageUsageFlagBits::eStorage);


    nvvk::Image             image  = m_alloc.createImage(colorCreateInfo);
    vk::ImageViewCreateInfo ivInfo = nvvk::makeImageViewCreateInfo(image.image, colorCreateInfo);
    m_offscreenColor               = m_alloc.createTexture(image, ivInfo, vk::SamplerCreateInfo());
    m_offscreenColor.descriptor.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
  }

  // Creating the depth buffer
  // CHANGED: was m_size, see color image comment above.
  auto depthCreateInfo =
      nvvk::makeImage2DCreateInfo(m_renderSize, m_offscreenDepthFormat,
                                  vk::ImageUsageFlagBits::eDepthStencilAttachment);
  {
    nvvk::Image image = m_alloc.createImage(depthCreateInfo);

    vk::ImageViewCreateInfo depthStencilView;
    depthStencilView.setViewType(vk::ImageViewType::e2D);
    depthStencilView.setFormat(m_offscreenDepthFormat);
    depthStencilView.setSubresourceRange({vk::ImageAspectFlagBits::eDepth, 0, 1, 0, 1});
    depthStencilView.setImage(image.image);

    m_offscreenDepth = m_alloc.createTexture(image, depthStencilView);
  }

  // Setting the image layout for both color and depth
  {
    nvvk::CommandPool genCmdBuf(m_device, m_graphicsQueueIndex);
    auto              cmdBuf = genCmdBuf.createCommandBuffer();
    nvvk::cmdBarrierImageLayout(cmdBuf, m_offscreenColor.image, vk::ImageLayout::eUndefined,
                                vk::ImageLayout::eGeneral);
    nvvk::cmdBarrierImageLayout(cmdBuf, m_offscreenDepth.image, vk::ImageLayout::eUndefined,
                                vk::ImageLayout::eDepthStencilAttachmentOptimal,
                                vk::ImageAspectFlagBits::eDepth);

    genCmdBuf.submitAndWait(cmdBuf);
  }

  // Creating a renderpass for the offscreen
  if(!m_offscreenRenderPass)
  {
    m_offscreenRenderPass =
        nvvk::createRenderPass(m_device, {m_offscreenColorFormat}, m_offscreenDepthFormat, 1, true,
                               true, vk::ImageLayout::eGeneral, vk::ImageLayout::eGeneral);
  }

  // Creating the frame buffer for offscreen
  std::vector<vk::ImageView> attachments = {m_offscreenColor.descriptor.imageView,
                                            m_offscreenDepth.descriptor.imageView};

  m_device.destroy(m_offscreenFramebuffer);
  vk::FramebufferCreateInfo info;
  info.setRenderPass(m_offscreenRenderPass);
  info.setAttachmentCount(2);
  info.setPAttachments(attachments.data());
  // CHANGED: was m_size; must match the m_renderSize-sized attachments above.
  info.setWidth(m_renderSize.width);
  info.setHeight(m_renderSize.height);
  info.setLayers(1);
  m_offscreenFramebuffer = m_device.createFramebuffer(info);
}

//--------------------------------------------------------------------------------------------------
// The pipeline is how things are rendered, which shaders, type of primitives, depth test and more
//
void HelloVulkan::createPostPipeline()
{
  // Push constants in the fragment shader
  vk::PushConstantRange pushConstantRanges = {vk::ShaderStageFlagBits::eFragment, 0, sizeof(float)};

  // Creating the pipeline layout
  vk::PipelineLayoutCreateInfo pipelineLayoutCreateInfo;
  pipelineLayoutCreateInfo.setSetLayoutCount(1);
  pipelineLayoutCreateInfo.setPSetLayouts(&m_postDescSetLayout);
  pipelineLayoutCreateInfo.setPushConstantRangeCount(1);
  pipelineLayoutCreateInfo.setPPushConstantRanges(&pushConstantRanges);
  m_postPipelineLayout = m_device.createPipelineLayout(pipelineLayoutCreateInfo);

  // Pipeline: completely generic, no vertices
  std::vector<std::string> paths = defaultSearchPaths;

  nvvk::GraphicsPipelineGeneratorCombined pipelineGenerator(m_device, m_postPipelineLayout,
                                                            m_renderPass);
  pipelineGenerator.addShader(nvh::loadFile("spv/passthrough.vert.spv", true, paths, true),
                              vk::ShaderStageFlagBits::eVertex);
  pipelineGenerator.addShader(nvh::loadFile("spv/post.frag.spv", true, paths, true),
                              vk::ShaderStageFlagBits::eFragment);
  pipelineGenerator.rasterizationState.setCullMode(vk::CullModeFlagBits::eNone);
  m_postPipeline = pipelineGenerator.createPipeline();
  m_debug.setObjectName(m_postPipeline, "post");
}

//--------------------------------------------------------------------------------------------------
// The descriptor layout is the description of the data that is passed to the vertex or the
// fragment program.
//
void HelloVulkan::createPostDescriptor()
{
  using vkDS = vk::DescriptorSetLayoutBinding;
  using vkDT = vk::DescriptorType;
  using vkSS = vk::ShaderStageFlagBits;

  m_postDescSetLayoutBind.addBinding(vkDS(0, vkDT::eCombinedImageSampler, 1, vkSS::eFragment));
  m_postDescSetLayout = m_postDescSetLayoutBind.createLayout(m_device);
  m_postDescPool      = m_postDescSetLayoutBind.createPool(m_device);
  m_postDescSet       = nvvk::allocateDescriptorSet(m_device, m_postDescPool, m_postDescSetLayout);
}

//--------------------------------------------------------------------------------------------------
// Update the output
//
void HelloVulkan::updatePostDescriptorSet()
{
  vk::WriteDescriptorSet writeDescriptorSets =
      m_postDescSetLayoutBind.makeWrite(m_postDescSet, 0, &m_offscreenColor.descriptor);
  m_device.updateDescriptorSets(writeDescriptorSets, nullptr);
}

//--------------------------------------------------------------------------------------------------
// Draw a full screen quad with the attached image
//
void HelloVulkan::drawPost(vk::CommandBuffer cmdBuf)
{
  m_debug.beginLabel(cmdBuf, "Post");

  cmdBuf.setViewport(0, {vk::Viewport(0, 0, (float)m_size.width, (float)m_size.height, 0, 1)});
  cmdBuf.setScissor(0, {{{0, 0}, {m_size.width, m_size.height}}});

  auto aspectRatio = static_cast<float>(m_size.width) / static_cast<float>(m_size.height);
  cmdBuf.pushConstants<float>(m_postPipelineLayout, vk::ShaderStageFlagBits::eFragment, 0,
                              aspectRatio);
  cmdBuf.bindPipeline(vk::PipelineBindPoint::eGraphics, m_postPipeline);
  cmdBuf.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, m_postPipelineLayout, 0,
                            m_postDescSet, {});
  cmdBuf.draw(3, 1, 0, 0);

  m_debug.endLabel(cmdBuf);
}

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

//--------------------------------------------------------------------------------------------------
// Initialize Vulkan ray tracing
// #VKRay
void HelloVulkan::initRayTracing()
{
  // Requesting ray tracing properties
  auto properties =
      m_physicalDevice.getProperties2<vk::PhysicalDeviceProperties2,
                                      vk::PhysicalDeviceRayTracingPipelinePropertiesKHR>();
  m_rtProperties = properties.get<vk::PhysicalDeviceRayTracingPipelinePropertiesKHR>();
  m_rtBuilder.setup(m_device, &m_alloc, m_graphicsQueueIndex);
}

//--------------------------------------------------------------------------------------------------
// Converting a OBJ primitive to the ray tracing geometry used for the BLAS
//
nvvk::RaytracingBuilderKHR::BlasInput HelloVulkan::objectToVkGeometryKHR(const ObjModel& model)
{
  vk::DeviceAddress vertexAddress = m_device.getBufferAddress({model.vertexBuffer.buffer});
  vk::DeviceAddress indexAddress  = m_device.getBufferAddress({model.indexBuffer.buffer});

  vk::AccelerationStructureGeometryTrianglesDataKHR triangles;
  triangles.setVertexFormat(vk::Format::eR32G32B32Sfloat);
  triangles.setVertexData(vertexAddress);
  triangles.setVertexStride(sizeof(VertexObj));
  triangles.setIndexType(vk::IndexType::eUint32);
  triangles.setIndexData(indexAddress);
  triangles.setTransformData({});
  triangles.setMaxVertex(model.nbVertices);

  // Setting up the build info of the acceleration
  vk::AccelerationStructureGeometryKHR asGeom;
  asGeom.setGeometryType(vk::GeometryTypeKHR::eTriangles);
  asGeom.setFlags(vk::GeometryFlagBitsKHR::eNoDuplicateAnyHitInvocation);  // Avoid double hits);
  asGeom.geometry.setTriangles(triangles);

  vk::AccelerationStructureBuildRangeInfoKHR offset;
  offset.setFirstVertex(0);
  offset.setPrimitiveCount(model.nbIndices / 3);  // Nb triangles
  offset.setPrimitiveOffset(0);
  offset.setTransformOffset(0);

  nvvk::RaytracingBuilderKHR::BlasInput input;
  input.asGeometry.emplace_back(asGeom);
  input.asBuildOffsetInfo.emplace_back(offset);
  return input;
}

//--------------------------------------------------------------------------------------------------
// Returning the ray tracing geometry used for the BLAS, containing all Aabbs
//
nvvk::RaytracingBuilderKHR::BlasInput HelloVulkan::aabbsToVkGeometryKHR(int no)
{
  vk::DeviceAddress dataAddress = m_device.getBufferAddress({m_aabbsAabbBuffer[no].buffer});

  vk::AccelerationStructureGeometryAabbsDataKHR aabbs;
  aabbs.setData(dataAddress);
  aabbs.setStride(sizeof(Aabb));

  // Setting up the build info of the acceleration (C version, c++ gives wrong type)
  vk::AccelerationStructureGeometryKHR asGeom;
  asGeom.geometryType   = vk::GeometryTypeKHR::eAabbs;
  asGeom.flags          = vk::GeometryFlagBitsKHR::eNoDuplicateAnyHitInvocation;
  asGeom.geometry.aabbs = aabbs;


  vk::AccelerationStructureBuildRangeInfoKHR offset;
  offset.setFirstVertex(0);
  offset.setPrimitiveCount((uint32_t)m_aabbs[no].size()); 
  offset.setPrimitiveOffset(0);
  offset.setTransformOffset(0);

  nvvk::RaytracingBuilderKHR::BlasInput input;
  input.asGeometry.emplace_back(asGeom);
  input.asBuildOffsetInfo.emplace_back(offset);
  return input;
}



// from https://stackoverflow.com/questions/16286095/similar-function-to-javas-string-split-in-c
std::vector<std::string> split(std::string str, std::string sep) {
  char* cstr = const_cast<char*>(str.c_str());
  char* current;
  std::vector<std::string> arr;
  current = strtok(cstr, sep.c_str());
  while(current != NULL) {
    arr.push_back(current);
    current = strtok(NULL, sep.c_str());
  }
  return arr;
}

bool loadAABBData(const std::string& filename, std::vector<float>& data) {

  std::ifstream input(filename.c_str());
  if(!input) {  // cast istream to bool to see if something went wrong
    std::cerr << "Can not find vertex data file " << filename << std::endl;
    return false;
  }
  int numAABB;
  double inputData;
  if(input >> numAABB) {
    int numFloats = 6 * numAABB;
    if(numFloats > 0) {
      data.resize(numFloats);
      int i = 0;
      while(input >> inputData && i < numFloats) {
        data[i] = float(inputData);
        i++;
      }
      if(i != numFloats || (numFloats % 6) != 0)
        return false;
    }
  } else {
    return false;
  }
  return true;
}

void HelloVulkan::createAabbs(const std::string& filename, nvmath::mat4f transform, int no)
{
  m_AabbTransforms[no] = transform;

  std::vector<float> data;
  if(!loadAABBData(filename, data))
  {
    return;
  }
  int numberOfAABB = int(data.size()) / 6;

  /**/
  // All spheres
 Aabb s;
  m_aabbs[no].resize(numberOfAABB);
  for(int i = 0; i < numberOfAABB; i++)
  {
    nvmath::vec3f center(data[i * 6 + 0], data[i * 6 + 1], data[i * 6 + 2]);
    nvmath::vec3f d(data[i * 6 + 3] / 2.0, data[i * 6 + 4] / 2.0, data[i * 6 + 5] / 2.0);
    s.minimum      = center - d;
    s.maximum      = center + d;
    m_aabbs[no][i] = std::move(s);
  }

  // Axis aligned bounding box of each sphere
  std::vector<Aabb> aabbs;
  aabbs.reserve(numberOfAABB);
  for(int i = 0; i < numberOfAABB; i++)
  {
    Aabb          aabb;
    nvmath::vec3f center(data[i * 6 + 0], data[i * 6 + 1], data[i * 6 + 2]);
    nvmath::vec3f d(data[i * 6 + 3] / 2.0, data[i * 6 + 4] / 2.0, data[i * 6 + 5] / 2.0);
    aabb.minimum = center - d;
    aabb.maximum = center + d;
    aabbs.emplace_back(aabb);
  }

  // Creating two materials
  MaterialObj mat;
  mat.diffuse = nvmath::vec3f(0, 1, 1);
  std::vector<MaterialObj> materials;
  std::vector<int>         matIdx(numberOfAABB);
  materials.emplace_back(mat);
  mat.diffuse = nvmath::vec3f(1, 1, 0);
  materials.emplace_back(mat);

  // Assign a material to each sphere
  for(int i = 0; i < numberOfAABB; i++)
  {
    matIdx[i] = i % 2;
  }

  // Creating all buffers
  using vkBU = vk::BufferUsageFlagBits;
  nvvk::CommandPool genCmdBuf(m_device, m_graphicsQueueIndex);
  auto              cmdBuf = genCmdBuf.createCommandBuffer();
  m_aabbsBuffer[no]          = m_alloc.createBuffer(cmdBuf, m_aabbs[no], vkBU::eStorageBuffer);
  m_aabbsAabbBuffer[no]     = m_alloc.createBuffer(cmdBuf, aabbs, vkBU::eShaderDeviceAddress);
  m_aabbsMatIndexBuffer[no]  = m_alloc.createBuffer(cmdBuf, matIdx, vkBU::eStorageBuffer);
  m_aabbsMatColorBuffer[no]  = m_alloc.createBuffer(cmdBuf, materials, vkBU::eStorageBuffer);
  genCmdBuf.submitAndWait(cmdBuf);

  // Debug information
  m_debug.setObjectName(m_aabbsBuffer[no].buffer, "aabbs");
  m_debug.setObjectName(m_aabbsAabbBuffer[no].buffer, "aabbsAabb");
  m_debug.setObjectName(m_aabbsMatColorBuffer[no].buffer, "aabbsMat");
  m_debug.setObjectName(m_aabbsMatIndexBuffer[no].buffer, "aabbsMatIdx");
}


void HelloVulkan::createBottomLevelAS()
{
  // BLAS - Storing each primitive in a geometry
  std::vector<nvvk::RaytracingBuilderKHR::BlasInput> allBlas;
  allBlas.reserve(m_objModel.size());
  for(const auto& obj : m_objModel)
  {
    auto blas = objectToVkGeometryKHR(obj);

    // We could add more geometry in each BLAS, but we add only one for now
    allBlas.emplace_back(blas);
  }

  // Aabbs collections
  for(int i=0; i < 3; i++) {
    auto blas = aabbsToVkGeometryKHR(i);
    allBlas.emplace_back(blas);
  }

  m_rtBuilder.buildBlas(allBlas, vk::BuildAccelerationStructureFlagBitsKHR::ePreferFastTrace);
}

void HelloVulkan::createTopLevelAS()
{

  int counter = 0;

  int switchBlas[] = {settings.modelModeBlas0, settings.modelModeBlas1, settings.modelModeBlas2};

  std::vector<VkAccelerationStructureInstanceKHR> tlas;
  tlas.reserve(m_objInstance.size());

    // Add the blas containing all aabbs
  for(int i = 0; i < 3; i++)
  {
    if(switchBlas[i] == 1)
    {
      VkAccelerationStructureInstanceKHR rayInst{};
      rayInst.transform        = nvvk::toTransformMatrixKHR(m_objInstance[i].transform);  // Position of the instance
      rayInst.instanceCustomIndex = i;                           // gl_InstanceCustomIndexEXT
      rayInst.accelerationStructureReference = m_rtBuilder.getBlasDeviceAddress(m_objInstance[i].objIndex);
      rayInst.instanceShaderBindingTableRecordOffset = 0;  // We will use the same hit group for all objects
      rayInst.flags            = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
      rayInst.mask             = (0x01 << i);
      tlas.emplace_back(rayInst);
    }
    else
    {
      VkAccelerationStructureInstanceKHR rayInst;
      rayInst.transform        = nvvk::toTransformMatrixKHR(m_AabbTransforms[i]);  // Position of the instance
      rayInst.instanceCustomIndex = i;
      static_cast<uint32_t>(tlas.size() + i);  // gl_InstanceCustomIndexEXT
      rayInst.accelerationStructureReference = m_rtBuilder.getBlasDeviceAddress(static_cast<uint32_t>(m_objModel.size() + i));
      rayInst.instanceShaderBindingTableRecordOffset = 1;  // We will use the same hit group for all objects
      rayInst.flags      = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
      if(switchBlas[i] == 0) {
        rayInst.mask = 0x00; // do not render if BLAS is set to "none" in the emulator
      }
      else{
        rayInst.mask = (0x01 << i);
      }
      
      tlas.emplace_back(rayInst);
    }
  }

  // NEW: street rat (m_objModel index 3). Always added as a plain triangle
  // instance, separate from the triangle/AABB toggle loop above, since it has
  // no associated AABB data and doesn't need the mode-switching behavior.
  // Pushed last, so it becomes gl_InstanceID == 3 in raytraceTri.rchit (the
  // gl_InstanceID branches there check the build-order index in this `tlas`
  // vector, not instanceCustomIndex).
  if(m_objModel.size() > 3)
  {
    VkAccelerationStructureInstanceKHR rayInst{};
    rayInst.transform = nvvk::toTransformMatrixKHR(m_objInstance[3].transform);
    rayInst.instanceCustomIndex             = 3;
    rayInst.accelerationStructureReference  = m_rtBuilder.getBlasDeviceAddress(m_objInstance[3].objIndex);
    rayInst.instanceShaderBindingTableRecordOffset = 0;  // raytraceTri hit group
    rayInst.flags                           = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
    rayInst.mask                            = 0x08;
    tlas.emplace_back(rayInst);
  }

  m_rtBuilder.buildTlas(tlas, vk::BuildAccelerationStructureFlagBitsKHR::ePreferFastTrace);
}

//--------------------------------------------------------------------------------------------------
// This descriptor set holds the Acceleration structure and the output image
//
void HelloVulkan::createRtDescriptorSet()
{
  using vkDT   = vk::DescriptorType;
  using vkSS   = vk::ShaderStageFlagBits;
  using vkDSLB = vk::DescriptorSetLayoutBinding;

  m_rtDescSetLayoutBind.addBinding(vkDSLB(0, vkDT::eAccelerationStructureKHR, 1,
                                          vkSS::eRaygenKHR | vkSS::eClosestHitKHR));  // TLAS
  m_rtDescSetLayoutBind.addBinding(
      vkDSLB(1, vkDT::eStorageImage, 1, vkSS::eRaygenKHR));  // Output image

  m_rtDescPool      = m_rtDescSetLayoutBind.createPool(m_device);
  m_rtDescSetLayout = m_rtDescSetLayoutBind.createLayout(m_device);
  m_rtDescSet       = m_device.allocateDescriptorSets({m_rtDescPool, 1, &m_rtDescSetLayout})[0];

  vk::AccelerationStructureKHR                   tlas = m_rtBuilder.getAccelerationStructure();
  vk::WriteDescriptorSetAccelerationStructureKHR descASInfo;
  descASInfo.setAccelerationStructureCount(1);
  descASInfo.setPAccelerationStructures(&tlas);
  vk::DescriptorImageInfo imageInfo{
      {}, m_offscreenColor.descriptor.imageView, vk::ImageLayout::eGeneral};

  std::vector<vk::WriteDescriptorSet> writes;
  writes.emplace_back(m_rtDescSetLayoutBind.makeWrite(m_rtDescSet, 0, &descASInfo));
  writes.emplace_back(m_rtDescSetLayoutBind.makeWrite(m_rtDescSet, 1, &imageInfo));
  m_device.updateDescriptorSets(static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
}


//--------------------------------------------------------------------------------------------------
// Writes the output image to the descriptor set
// - Required when changing resolution
//
void HelloVulkan::updateRtDescriptorSet()
{
  using vkDT = vk::DescriptorType;

  // (1) Output buffer
  vk::DescriptorImageInfo imageInfo{
      {}, m_offscreenColor.descriptor.imageView, vk::ImageLayout::eGeneral};
  vk::WriteDescriptorSet wds{m_rtDescSet, 1, 0, 1, vkDT::eStorageImage, &imageInfo};
  m_device.updateDescriptorSets(wds, nullptr);
}


//--------------------------------------------------------------------------------------------------
// Pipeline for the ray tracer: all shaders, raygen, chit, miss
//
void HelloVulkan::createRtPipeline()
{
  std::vector<std::string> paths = defaultSearchPaths;

  vk::ShaderModule raygenSM =
      nvvk::createShaderModule(m_device,  //
                               nvh::loadFile("spv/raytrace.rgen.spv", true, paths, true));
  vk::ShaderModule missSM =
      nvvk::createShaderModule(m_device,  //
                               nvh::loadFile("spv/raytrace.rmiss.spv", true, paths, true));

  std::vector<vk::PipelineShaderStageCreateInfo> stages;

  // Raygen
  vk::RayTracingShaderGroupCreateInfoKHR rg{vk::RayTracingShaderGroupTypeKHR::eGeneral,
                                            VK_SHADER_UNUSED_KHR, VK_SHADER_UNUSED_KHR,
                                            VK_SHADER_UNUSED_KHR, VK_SHADER_UNUSED_KHR};
  stages.push_back({{}, vk::ShaderStageFlagBits::eRaygenKHR, raygenSM, "main"});
  rg.setGeneralShader(static_cast<uint32_t>(stages.size() - 1));
  m_rtShaderGroups.push_back(rg);
  // Miss
  vk::RayTracingShaderGroupCreateInfoKHR mg{vk::RayTracingShaderGroupTypeKHR::eGeneral,
                                            VK_SHADER_UNUSED_KHR, VK_SHADER_UNUSED_KHR,
                                            VK_SHADER_UNUSED_KHR, VK_SHADER_UNUSED_KHR};
  stages.push_back({{}, vk::ShaderStageFlagBits::eMissKHR, missSM, "main"});
  mg.setGeneralShader(static_cast<uint32_t>(stages.size() - 1));
  m_rtShaderGroups.push_back(mg);

  // 2nd Miss
  stages.push_back({{}, vk::ShaderStageFlagBits::eMissKHR, missSM, "main"});
  mg.setGeneralShader(static_cast<uint32_t>(stages.size() - 1));
  m_rtShaderGroups.push_back(mg);

  // Hit Group0 - Closest Hit
  vk::ShaderModule chitSM =
      nvvk::createShaderModule(m_device,  //
                               nvh::loadFile("spv/raytraceTri.rchit.spv", true, paths, true));
  vk::ShaderModule ahitSM =
      nvvk::createShaderModule(m_device,  //
                               nvh::loadFile("spv/raytraceTri.rahit.spv", true, paths, true));

  {
    vk::RayTracingShaderGroupCreateInfoKHR hg{vk::RayTracingShaderGroupTypeKHR::eTrianglesHitGroup,
                                              VK_SHADER_UNUSED_KHR, VK_SHADER_UNUSED_KHR,
                                              VK_SHADER_UNUSED_KHR, VK_SHADER_UNUSED_KHR};
    stages.push_back({{}, vk::ShaderStageFlagBits::eClosestHitKHR, chitSM, "main"});
    hg.setClosestHitShader(static_cast<uint32_t>(stages.size() - 1));

    stages.push_back({{}, vk::ShaderStageFlagBits::eAnyHitKHR, ahitSM, "main"});
    hg.setAnyHitShader(static_cast<uint32_t>(stages.size() - 1));

    m_rtShaderGroups.push_back(hg);
  }

  // Hit Group1 - Closest Hit + Intersection (procedural)
  vk::ShaderModule chit2SM =
      nvvk::createShaderModule(m_device,  //
                               nvh::loadFile("spv/raytraceAabb.rchit.spv", true, paths, true));
  vk::ShaderModule rintSM =
      nvvk::createShaderModule(m_device,  //
                               nvh::loadFile("spv/raytraceAabb.rint.spv", true, paths, true));
  vk::ShaderModule ahit2SM =
      nvvk::createShaderModule(m_device,  //
                               nvh::loadFile("spv/raytraceAabb.rahit.spv", true, paths, true));
  {
    vk::RayTracingShaderGroupCreateInfoKHR hg{vk::RayTracingShaderGroupTypeKHR::eProceduralHitGroup,
                                              VK_SHADER_UNUSED_KHR, VK_SHADER_UNUSED_KHR,
                                              VK_SHADER_UNUSED_KHR, VK_SHADER_UNUSED_KHR};
    stages.push_back({{}, vk::ShaderStageFlagBits::eClosestHitKHR, chit2SM, "main"});
    hg.setClosestHitShader(static_cast<uint32_t>(stages.size() - 1));
    stages.push_back({{}, vk::ShaderStageFlagBits::eIntersectionKHR, rintSM, "main"});
    hg.setIntersectionShader(static_cast<uint32_t>(stages.size() - 1));
    stages.push_back({{}, vk::ShaderStageFlagBits::eAnyHitKHR, ahit2SM, "main"});
    hg.setAnyHitShader(static_cast<uint32_t>(stages.size() - 1));
    m_rtShaderGroups.push_back(hg);
  }

  vk::PipelineLayoutCreateInfo pipelineLayoutCreateInfo;

  // Push constant: we want to be able to update constants used by the shaders
  vk::PushConstantRange pushConstant{vk::ShaderStageFlagBits::eRaygenKHR
                                         | vk::ShaderStageFlagBits::eClosestHitKHR
                                         | vk::ShaderStageFlagBits::eMissKHR
                                         | vk::ShaderStageFlagBits::eAnyHitKHR
                                         | vk::ShaderStageFlagBits::eIntersectionKHR,
                                     0, sizeof(RtPushConstant)};
  pipelineLayoutCreateInfo.setPushConstantRangeCount(1);
  pipelineLayoutCreateInfo.setPPushConstantRanges(&pushConstant);

  // Descriptor sets: one specific to ray tracing, and one shared with the rasterization pipeline
  std::vector<vk::DescriptorSetLayout> rtDescSetLayouts = {m_rtDescSetLayout, m_descSetLayout};
  pipelineLayoutCreateInfo.setSetLayoutCount(static_cast<uint32_t>(rtDescSetLayouts.size()));
  pipelineLayoutCreateInfo.setPSetLayouts(rtDescSetLayouts.data());

  m_rtPipelineLayout = m_device.createPipelineLayout(pipelineLayoutCreateInfo);

  // Assemble the shader stages and recursion depth info into the ray tracing pipeline
  vk::RayTracingPipelineCreateInfoKHR rayPipelineInfo;
  rayPipelineInfo.setStageCount(static_cast<uint32_t>(stages.size()));  // Stages are shaders
  rayPipelineInfo.setPStages(stages.data());

  rayPipelineInfo.setGroupCount(static_cast<uint32_t>(
      m_rtShaderGroups.size()));  // 1-raygen, n-miss, n-(hit[+anyhit+intersect])
  rayPipelineInfo.setPGroups(m_rtShaderGroups.data());

  rayPipelineInfo.setMaxPipelineRayRecursionDepth(2);  // Ray depth
  rayPipelineInfo.setLayout(m_rtPipelineLayout);
  m_rtPipeline = m_device.createRayTracingPipelineKHR({}, {}, rayPipelineInfo).value;

  m_device.destroy(raygenSM);
  m_device.destroy(missSM);
  m_device.destroy(chitSM);
  m_device.destroy(rintSM);
  m_device.destroy(ahitSM);
  m_device.destroy(ahit2SM);
  m_device.destroy(chit2SM);
}

//--------------------------------------------------------------------------------------------------
// The Shader Binding Table (SBT)
// - getting all shader handles and writing them in a SBT buffer
// - Besides exception, this could be always done like this
//   See how the SBT buffer is used in run()
//
void HelloVulkan::createRtShaderBindingTable()
{
  auto groupCount =
      static_cast<uint32_t>(m_rtShaderGroups.size());               // 3 shaders: raygen, miss, chit
  uint32_t groupHandleSize = m_rtProperties.shaderGroupHandleSize;  // Size of a program identifier
  uint32_t groupSizeAligned =
      nvh::align_up(groupHandleSize, m_rtProperties.shaderGroupBaseAlignment);

  // Fetch all the shader handles used in the pipeline, so that they can be written in the SBT
  uint32_t sbtSize = groupCount * groupSizeAligned;

  std::vector<uint8_t> shaderHandleStorage(sbtSize);
  auto result = m_device.getRayTracingShaderGroupHandlesKHR(m_rtPipeline, 0, groupCount, sbtSize,
                                                            shaderHandleStorage.data());
  assert(result == vk::Result::eSuccess);

  // Write the handles in the SBT
  m_rtSBTBuffer = m_alloc.createBuffer(
      sbtSize,
      vk::BufferUsageFlagBits::eTransferSrc | vk::BufferUsageFlagBits::eShaderDeviceAddressKHR
          | vk::BufferUsageFlagBits::eShaderBindingTableKHR,
      vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
  m_debug.setObjectName(m_rtSBTBuffer.buffer, std::string("SBT").c_str());

  // Write the handles in the SBT
  void* mapped = m_alloc.map(m_rtSBTBuffer);
  auto* pData  = reinterpret_cast<uint8_t*>(mapped);
  for(uint32_t g = 0; g < groupCount; g++)
  {
    memcpy(pData, shaderHandleStorage.data() + g * groupHandleSize, groupHandleSize);  // raygen
    pData += groupSizeAligned;
  }
  m_alloc.unmap(m_rtSBTBuffer);


  m_alloc.finalizeAndReleaseStaging();
}

//--------------------------------------------------------------------------------------------------
// Ray Tracing the scene
//
void HelloVulkan::raytrace(const vk::CommandBuffer& cmdBuf, const nvmath::vec4f& clearColor)
{
  
  if(m_frameCounter < settings.frameSize - 1) {
    m_frameCounter++;
  } else {
    return;
  }

  m_debug.beginLabel(cmdBuf, "Ray trace");
  // Initializing push constant values
  m_rtPushConstants.clearColorRt     = clearColor;
  m_rtPushConstants.lightPositionRt  = m_pushConstant.lightPosition;
  m_rtPushConstants.lightIntensityRt = m_pushConstant.lightIntensity;
  m_rtPushConstants.lightTypeRt      = m_pushConstant.lightType;
  m_rtPushConstants.frameID         = m_frameCounter;
  m_rtPushConstants.frameSize       = settings.frameSize;
  m_rtPushConstants.userParam0      = settings.userParam0;
  m_rtPushConstants.userParam1      = settings.userParam1;
  m_rtPushConstants.userParam2      = settings.userParam2;

  cmdBuf.bindPipeline(vk::PipelineBindPoint::eRayTracingKHR, m_rtPipeline);
  cmdBuf.bindDescriptorSets(vk::PipelineBindPoint::eRayTracingKHR, m_rtPipelineLayout, 0,
                            {m_rtDescSet, m_descSet}, {});
  cmdBuf.pushConstants<RtPushConstant>(m_rtPipelineLayout,
                                       vk::ShaderStageFlagBits::eRaygenKHR
                                           | vk::ShaderStageFlagBits::eClosestHitKHR
                                           | vk::ShaderStageFlagBits::eMissKHR
                                           | vk::ShaderStageFlagBits::eAnyHitKHR 
                                           | vk::ShaderStageFlagBits::eIntersectionKHR,
                                       0, m_rtPushConstants);

  // Size of a program identifier
  uint32_t groupSize =
      nvh::align_up(m_rtProperties.shaderGroupHandleSize, m_rtProperties.shaderGroupBaseAlignment);
  uint32_t          groupStride = groupSize;
  vk::DeviceAddress sbtAddress  = m_device.getBufferAddress({m_rtSBTBuffer.buffer});

  using Stride = vk::StridedDeviceAddressRegionKHR;
  std::array<Stride, 4> strideAddresses{
      Stride{sbtAddress + 0u * groupSize, groupStride, groupSize * 1},  // raygen
      Stride{sbtAddress + 1u * groupSize, groupStride, groupSize * 2},  // miss
      Stride{sbtAddress + 3u * groupSize, groupStride, groupSize * 1},  // hit
      Stride{0u, 0u, 0u}};                                              // callable

  // CHANGED: was m_size (window/swapchain size); use m_renderSize so
  // gl_LaunchSizeEXT in the shaders matches the full requested render
  // resolution, not whatever the swapchain got clamped to for the screen.
  cmdBuf.traceRaysKHR(&strideAddresses[0], &strideAddresses[1], &strideAddresses[2],
                      &strideAddresses[3],                          //
                      m_renderSize.width, m_renderSize.height, 1);  //

  m_debug.endLabel(cmdBuf);
}

bool HelloVulkan::loadSettings(const std::string& filename)
{

  std::ifstream myfile(filename.c_str());
  if(!myfile.is_open())
  {
    std::cout << "Can not open file: " << filename << std::endl;
    return false;
  }
  std::string line;
  unsigned    lineNumber = 0;
  while(myfile.good())
  {
    std::getline(myfile, line);
    ++lineNumber;
    std::cout << line << std::endl;
    std::istringstream ss(line);
    std::string        keyword;
    ss >> keyword;
    if(keyword == "launchSizeX:")
      ss >> settings.launchSizeX;
    if(keyword == "launchSizeY:")
      ss >> settings.launchSizeY;
    if(keyword == "launchSizeZ:")
      ss >> settings.launchSizeZ;
    if(keyword == "patchSizeX:")
      ss >> settings.patchSizeX;
    if(keyword == "patchSizeY:")
      ss >> settings.patchSizeY;
    if(keyword == "frameSize:")
      ss >> settings.frameSize;
    if(keyword == "modelModeBlas0:")
      ss >> settings.modelModeBlas0;
    if(keyword == "posXBlas0:")
      ss >> settings.posXBlas0;
    if(keyword == "posYBlas0:")
      ss >> settings.posYBlas0;
    if(keyword == "posZBlas0:")
      ss >> settings.posZBlas0;
    if(keyword == "rotXBlas0:")
      ss >> settings.rotXBlas0;
    if(keyword == "rotYBlas0:")
      ss >> settings.rotYBlas0;
    if(keyword == "rotZBlas0:")
      ss >> settings.rotZBlas0;
    if(keyword == "scaleBlas0:")
      ss >> settings.scaleBlas0;
    if(keyword == "modelModeBlas1:")
      ss >> settings.modelModeBlas1;
    if(keyword == "posXBlas1:")
      ss >> settings.posXBlas1;
    if(keyword == "posYBlas1:")
      ss >> settings.posYBlas1;
    if(keyword == "posZBlas1:")
      ss >> settings.posZBlas1;
    if(keyword == "rotXBlas1:")
      ss >> settings.rotXBlas1;
    if(keyword == "rotYBlas1:")
      ss >> settings.rotYBlas1;
    if(keyword == "rotZBlas1:")
      ss >> settings.rotZBlas1;
    if(keyword == "scaleBlas1:")
      ss >> settings.scaleBlas1;
    if(keyword == "modelModeBlas2:")
      ss >> settings.modelModeBlas2;
    if(keyword == "posXBlas2:")
      ss >> settings.posXBlas2;
    if(keyword == "posYBlas2:")
      ss >> settings.posYBlas2;
    if(keyword == "posZBlas2:")
      ss >> settings.posZBlas2;
    if(keyword == "rotXBlas2:")
      ss >> settings.rotXBlas2;
    if(keyword == "rotYBlas2:")
      ss >> settings.rotYBlas2;
    if(keyword == "rotZBlas2:")
      ss >> settings.rotZBlas2;
    if(keyword == "scaleBlas2:")
      ss >> settings.scaleBlas2;
    if(keyword == "userParam0:")
      ss >> settings.userParam0;
    if(keyword == "userParam1:")
      ss >> settings.userParam1;
    if(keyword == "userParam2:")
      ss >> settings.userParam2;
  }
  myfile.close();

  return true;
}

void HelloVulkan::resetFrameCounter() {
  m_frameCounter = -1;
}

void HelloVulkan::hideGui()
{
  m_show_gui = false;
}

// NEW: copies the offscreen ray-traced image (m_offscreenColor, R32G32B32A32Sfloat,
// already gamma-corrected by the raygen shader) to a host-visible staging buffer
// and writes it out as an 8-bit PNG via stb_image_write.
void HelloVulkan::saveScreenshot(const std::string& filename)
{
  // CHANGED: was m_size throughout this function; use m_renderSize so the
  // saved PNG is the full requested render resolution, not the (possibly
  // screen-clamped) window/swapchain size.
  vk::DeviceSize bufferSize =
      static_cast<vk::DeviceSize>(m_renderSize.width) * m_renderSize.height * 4 * sizeof(float);

  nvvk::CommandPool genCmdBuf(m_device, m_graphicsQueueIndex);
  vk::CommandBuffer cmdBuf = genCmdBuf.createCommandBuffer();

  nvvk::Buffer stagingBuffer =
      m_alloc.createBuffer(bufferSize, vk::BufferUsageFlagBits::eTransferDst,
                            vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);

  nvvk::cmdBarrierImageLayout(cmdBuf, m_offscreenColor.image, vk::ImageLayout::eGeneral,
                              vk::ImageLayout::eTransferSrcOptimal);

  vk::BufferImageCopy copyRegion;
  copyRegion.setImageSubresource({vk::ImageAspectFlagBits::eColor, 0, 0, 1});
  copyRegion.setImageExtent({m_renderSize.width, m_renderSize.height, 1});
  cmdBuf.copyImageToBuffer(m_offscreenColor.image, vk::ImageLayout::eTransferSrcOptimal,
                            stagingBuffer.buffer, 1, &copyRegion);

  nvvk::cmdBarrierImageLayout(cmdBuf, m_offscreenColor.image, vk::ImageLayout::eTransferSrcOptimal,
                              vk::ImageLayout::eGeneral);

  genCmdBuf.submitAndWait(cmdBuf);

  const float*  floatData = reinterpret_cast<const float*>(m_alloc.map(stagingBuffer));
  size_t        pixelCount = static_cast<size_t>(m_renderSize.width) * m_renderSize.height * 4;
  std::vector<unsigned char> pixels(pixelCount);
  for(size_t i = 0; i < pixelCount; i++)
  {
    float v   = std::min(std::max(floatData[i], 0.0f), 1.0f);
    pixels[i] = static_cast<unsigned char>(v * 255.0f + 0.5f);
  }
  m_alloc.unmap(stagingBuffer);
  m_alloc.destroy(stagingBuffer);

  stbi_write_png(filename.c_str(), static_cast<int>(m_renderSize.width), static_cast<int>(m_renderSize.height),
                 4, pixels.data(), static_cast<int>(m_renderSize.width) * 4);
}