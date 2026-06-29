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

// ImGui - standalone example application for Glfw + Vulkan, using programmable
// pipeline If you are new to ImGui, see examples/README.txt and documentation
// at the top of imgui.cpp.

#include <array>
#include <vulkan/vulkan.hpp>
VULKAN_HPP_DEFAULT_DISPATCH_LOADER_DYNAMIC_STORAGE

#include "backends/imgui_impl_glfw.h"
#include "imgui.h"

#include "hello_vulkan.h"
#include "imgui_camera_widget.h"
#include "nvh/cameramanipulator.hpp"
#include "nvh/fileoperations.hpp"
#include "nvpsystem.hpp"
#include "nvvk/appbase_vkpp.hpp"
#include "nvvk/commands_vk.hpp"
#include "nvvk/context_vk.hpp"


//////////////////////////////////////////////////////////////////////////
#define UNUSED(x) (void)(x)
//////////////////////////////////////////////////////////////////////////

// Default search path for shaders
std::vector<std::string> defaultSearchPaths;

// GLFW Callback functions
static void onErrorCallback(int error, const char* description)
{
  fprintf(stderr, "GLFW Error %d: %s\n", error, description);
}

// Extra UI
void renderUI(HelloVulkan& helloVk)
{
  ImGuiH::CameraWidget();
  if(ImGui::CollapsingHeader("Light"))
  {
    ImGui::RadioButton("Point", &helloVk.m_pushConstant.lightType, 0);
    ImGui::SameLine();
    ImGui::RadioButton("Infinite", &helloVk.m_pushConstant.lightType, 1);

    ImGui::SliderFloat3("Position", &helloVk.m_pushConstant.lightPosition.x, -20.f, 20.f);
    ImGui::SliderFloat("Intensity", &helloVk.m_pushConstant.lightIntensity, 0.f, 150.f);
  }
}

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

//--------------------------------------------------------------------------------------------------
// Application Entry
//
int main(int argc, char** argv)
{
  UNUSED(argc);

  HelloVulkan helloVk;
  if(!helloVk.loadSettings(NVPSystem ::exePath() + PROJECT_RELDIRECTORY + std::string("./data/settings.txt")))
  {
    return 1;
  };

  // Setup GLFW window
  glfwSetErrorCallback(onErrorCallback);
  if(!glfwInit())
  {
    return 1;
  }

  int width = helloVk.settings.launchSizeX;
  int height = helloVk.settings.launchSizeY;

  glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
  GLFWwindow* window = glfwCreateWindow(width, height,
                                        PROJECT_NAME, nullptr, nullptr);



  // Setup camera
  CameraManip.setWindowSize(width, height);
  CameraManip.setLookat(nvmath::vec3f(0.0f, -4.75f, 0.0f), nvmath::vec3f(0.0, 0.0, 0.0),
                        nvmath::vec3f(0.0, 0.0, 1.0));

  // Setup Vulkan
  if(!glfwVulkanSupported())
  {
    printf("GLFW: Vulkan Not Supported\n");
    return 1;
  }

  // setup some basic things for the sample, logging file for example
  NVPSystem system(PROJECT_NAME);

  // Search path for shaders and other media
  defaultSearchPaths = {
      NVPSystem ::exePath() + PROJECT_RELDIRECTORY,
      NVPSystem ::exePath() + PROJECT_RELDIRECTORY "..",
      NVPSystem::exePath(),
      NVPSystem::exePath() + std::string(PROJECT_NAME),
  };

  // Requesting Vulkan extensions and layers
  nvvk::ContextCreateInfo contextInfo(true);
  contextInfo.setVersion(1, 2);
  contextInfo.addInstanceLayer("VK_LAYER_LUNARG_monitor", true);
  contextInfo.addInstanceExtension(VK_KHR_SURFACE_EXTENSION_NAME);
  contextInfo.addInstanceExtension(VK_EXT_DEBUG_UTILS_EXTENSION_NAME, true);
#ifdef WIN32
  contextInfo.addInstanceExtension(VK_KHR_WIN32_SURFACE_EXTENSION_NAME);
#else
  contextInfo.addInstanceExtension(VK_KHR_XLIB_SURFACE_EXTENSION_NAME);
  contextInfo.addInstanceExtension(VK_KHR_XCB_SURFACE_EXTENSION_NAME);
#endif
  contextInfo.addInstanceExtension(VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME);
  contextInfo.addDeviceExtension(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
  contextInfo.addDeviceExtension(VK_KHR_DEDICATED_ALLOCATION_EXTENSION_NAME);
  contextInfo.addDeviceExtension(VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME);
  contextInfo.addDeviceExtension(VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME);
  contextInfo.addDeviceExtension(VK_EXT_SCALAR_BLOCK_LAYOUT_EXTENSION_NAME);
  // #VKRay: Activate the ray tracing extension
  contextInfo.addDeviceExtension(VK_KHR_MAINTENANCE3_EXTENSION_NAME);
  contextInfo.addDeviceExtension(VK_KHR_PIPELINE_LIBRARY_EXTENSION_NAME);
  contextInfo.addDeviceExtension(VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME);
  contextInfo.addDeviceExtension(VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME);
  // #VKRay: Activate the ray tracing extension
  vk::PhysicalDeviceAccelerationStructureFeaturesKHR accelFeature;
  contextInfo.addDeviceExtension(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME, false,
                                 &accelFeature);
  vk::PhysicalDeviceRayTracingPipelineFeaturesKHR rtPipelineFeature;
  contextInfo.addDeviceExtension(VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME, false,
                                 &rtPipelineFeature);

  // Creating Vulkan base application
  nvvk::Context vkctx{};
  vkctx.initInstance(contextInfo);
  // Find all compatible devices
  auto compatibleDevices = vkctx.getCompatibleDevices(contextInfo);
  assert(!compatibleDevices.empty());
  // Use a compatible device
  vkctx.initDevice(compatibleDevices[0], contextInfo);

  // Window need to be opened to get the surface on which to draw
  const vk::SurfaceKHR surface = helloVk.getVkSurface(vkctx.m_instance, window);
  vkctx.setGCTQueueWithPresent(surface);

  helloVk.setup(vkctx.m_instance, vkctx.m_device, vkctx.m_physicalDevice,
                vkctx.m_queueGCT.familyIndex);
  helloVk.createSwapchain(surface, width, height);
  helloVk.createDepthBuffer();
  helloVk.createRenderPass();
  helloVk.createFrameBuffers();

  // Setup Imgui
  helloVk.initGUI(0);  // Using sub-pass 0

  nvmath::mat4f mesh0Trans(1), mesh0T(1), mesh0Rx(1), mesh0Ry(1), mesh0Rz(1), mesh0Scale(1);
  mesh0T.set_translation(nvmath::vec3f(helloVk.settings.posXBlas0, helloVk.settings.posYBlas0, helloVk.settings.posZBlas0));
  mesh0Rx.set_rot(helloVk.settings.rotXBlas0 / 180.0f * nv_pi, nvmath::vec3f(1.0, 0.0, 0.0)); 
  mesh0Ry.set_rot(helloVk.settings.rotYBlas0 / 180.0f * nv_pi, nvmath::vec3f(0.0, 1.0, 0.0)); 
  mesh0Rz.set_rot(helloVk.settings.rotZBlas0 / 180.0f * nv_pi, nvmath::vec3f(0.0, 0.0, 1.0)); 
  mesh0Scale.set_scale(nvmath::vec3f(helloVk.settings.scaleBlas0));
  mesh0Trans = mesh0T * mesh0Rx * mesh0Ry * mesh0Rz * mesh0Scale;

  nvmath::mat4f mesh1Trans(1), mesh1T(1), mesh1Rx(1), mesh1Ry(1), mesh1Rz(1), mesh1Scale(1);
  mesh1T.set_translation(nvmath::vec3f(helloVk.settings.posXBlas1, helloVk.settings.posYBlas1, helloVk.settings.posZBlas1));
  mesh1Rx.set_rot(helloVk.settings.rotXBlas1 / 180.0f * nv_pi, nvmath::vec3f(1.0, 0.0, 0.0));
  mesh1Ry.set_rot(helloVk.settings.rotYBlas1 / 180.0f * nv_pi, nvmath::vec3f(0.0, 1.0, 0.0));
  mesh1Rz.set_rot(helloVk.settings.rotZBlas1 / 180.0f * nv_pi, nvmath::vec3f(0.0, 0.0, 1.0));
  mesh1Scale.set_scale(nvmath::vec3f(helloVk.settings.scaleBlas1));
  mesh1Trans = mesh1T * mesh1Rx * mesh1Ry * mesh1Rz * mesh1Scale;

  nvmath::mat4f mesh2Trans(1), mesh2T(1), mesh2Rx(1), mesh2Ry(1), mesh2Rz(1), mesh2Scale(1);
  mesh2T.set_translation(nvmath::vec3f(helloVk.settings.posXBlas2, helloVk.settings.posYBlas2, helloVk.settings.posZBlas2));
  mesh2Rx.set_rot(helloVk.settings.rotXBlas2 / 180.0f * nv_pi, nvmath::vec3f(1.0, 0.0, 0.0));
  mesh2Ry.set_rot(helloVk.settings.rotYBlas2 / 180.0f * nv_pi, nvmath::vec3f(0.0, 1.0, 0.0));
  mesh2Rz.set_rot(helloVk.settings.rotZBlas2 / 180.0f * nv_pi, nvmath::vec3f(0.0, 0.0, 1.0));
  mesh2Scale.set_scale(nvmath::vec3f(helloVk.settings.scaleBlas2));
  mesh2Trans = mesh2T * mesh2Rx * mesh2Ry * mesh2Rz * mesh2Scale;

  // additional textures
  // ??? extension is replaced by pfm or png
  std::vector<std::string> additionalTextures;
  additionalTextures.push_back(NVPSystem ::exePath() + PROJECT_RELDIRECTORY + std::string("./data/texture0.???"));
  additionalTextures.push_back(NVPSystem ::exePath() + PROJECT_RELDIRECTORY + std::string("./data/texture1.???"));
  additionalTextures.push_back(NVPSystem ::exePath() + PROJECT_RELDIRECTORY + std::string("./data/texture2.???"));
  additionalTextures.push_back(NVPSystem ::exePath() + PROJECT_RELDIRECTORY + std::string("./data/texture3.???"));
  additionalTextures.push_back(NVPSystem ::exePath() + PROJECT_RELDIRECTORY + std::string("./data/texture4.???"));

  // loading meshes and additional textures
  helloVk.loadModel(NVPSystem ::exePath() + PROJECT_RELDIRECTORY + std::string("./data/mesh0.obj"), mesh0Trans,
                    additionalTextures);
  helloVk.loadModel(NVPSystem ::exePath() + PROJECT_RELDIRECTORY + std::string("./data/mesh1.obj"), mesh1Trans);
  helloVk.loadModel(NVPSystem ::exePath() + PROJECT_RELDIRECTORY + std::string("./data/mesh2.obj"), mesh2Trans);

  // creating intersection boxes
  helloVk.createAabbs(NVPSystem ::exePath() + PROJECT_RELDIRECTORY + std::string("./data/aabbBlas0.txt"), mesh0Trans, 0);
  helloVk.createAabbs(NVPSystem ::exePath() + PROJECT_RELDIRECTORY + std::string("./data/aabbBlas1.txt"), mesh1Trans, 1);
  helloVk.createAabbs(NVPSystem ::exePath() + PROJECT_RELDIRECTORY + std::string("./data/aabbBlas2.txt"), mesh2Trans, 2);

  helloVk.createOffscreenRender();
  helloVk.createDescriptorSetLayout();
  helloVk.createGraphicsPipeline();
  helloVk.createUniformBuffer();
  helloVk.createSceneDescriptionBuffer();
  helloVk.updateDescriptorSet();

  // #VKRay
  helloVk.initRayTracing();
  helloVk.createBottomLevelAS();
  helloVk.createTopLevelAS();
  helloVk.createRtDescriptorSet();
  helloVk.createRtPipeline();
  helloVk.createRtShaderBindingTable();

  helloVk.createPostDescriptor();
  helloVk.createPostPipeline();
  helloVk.updatePostDescriptorSet();
  helloVk.resetFrameCounter();

  nvmath::vec4f clearColor   = nvmath::vec4f(1, 1, 1, 1.00f);
  bool          useRaytracer = true;
  

  helloVk.setupGlfwCallbacks(window);
  ImGui_ImplGlfw_InitForVulkan(window, true);

  helloVk.hideGui();

  int lastMouseX, lastMouseY;
  CameraManip.getMousePosition(lastMouseX, lastMouseY);

  // Main loop
  while(!glfwWindowShouldClose(window))
  {
    glfwPollEvents();
    if(helloVk.isMinimized())
      continue;

    // Start the Dear ImGui frame
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();

    if(false){
      int currentMouseX, currentMouseY;
      CameraManip.getMousePosition(currentMouseX, currentMouseY);
      if(currentMouseX != lastMouseX || currentMouseY != lastMouseY){
        helloVk.resetFrameCounter();
        lastMouseX = currentMouseX;
        lastMouseY = currentMouseY;
        helloVk.settings.userParam0 = float(currentMouseX) / float(helloVk.settings.launchSizeX);
        helloVk.settings.userParam1 = float(currentMouseY) / float(helloVk.settings.launchSizeY);
      }
    }
    
    // Show UI window.
    if(helloVk.showGui())
    {
 
      ImGuiH::Panel::Begin();
      ImGui::ColorEdit3("Clear color", reinterpret_cast<float*>(&clearColor));
      ImGui::Checkbox("Ray Tracer mode", &useRaytracer);  // Switch between raster and ray tracing

      renderUI(helloVk);
      ImGui::Text("Application average %.3f ms/frame (%.1f FPS)",
                  1000.0f / ImGui::GetIO().Framerate, ImGui::GetIO().Framerate);
      ImGui::Text("frameID = %d", helloVk.m_frameCounter);
      ImGui::Text("userParam0 = %.3f", helloVk.settings.userParam0);
      ImGui::Text("userParam1 = %.3f", helloVk.settings.userParam1);
      ImGui::Text("userParam2 = %.3f", helloVk.settings.userParam2);
      if(helloVk.m_frameCounter == helloVk.settings.frameSize - 1)
      {
        ImGui::Text("RENDERING FINISHED");
      }
      

      ImGuiH::Control::Info("", "", "(F10) Toggle Pane", ImGuiH::Control::Flags::Disabled);
      ImGuiH::Panel::End();
    }

    // Start rendering the scene
    helloVk.prepareFrame();

    // Start command buffer of this frame
    auto                     curFrame = helloVk.getCurFrame();
    const vk::CommandBuffer& cmdBuf   = helloVk.getCommandBuffers()[curFrame];

    cmdBuf.begin({vk::CommandBufferUsageFlagBits::eOneTimeSubmit});

    // Updating camera buffer
    helloVk.updateUniformBuffer(cmdBuf);

    // Clearing screen
    vk::ClearValue clearValues[2];
    clearValues[0].setColor(
        std::array<float, 4>({clearColor[0], clearColor[1], clearColor[2], clearColor[3]}));
    clearValues[1].setDepthStencil({1.0f, 0});

    // Offscreen render pass
    {
      vk::RenderPassBeginInfo offscreenRenderPassBeginInfo;
      offscreenRenderPassBeginInfo.setClearValueCount(2);
      offscreenRenderPassBeginInfo.setPClearValues(clearValues);
      offscreenRenderPassBeginInfo.setRenderPass(helloVk.m_offscreenRenderPass);
      offscreenRenderPassBeginInfo.setFramebuffer(helloVk.m_offscreenFramebuffer);
      offscreenRenderPassBeginInfo.setRenderArea({{}, helloVk.getSize()});

      // Rendering Scene
      if(useRaytracer)
      {
        helloVk.raytrace(cmdBuf, clearColor);
      }
      else
      {
        cmdBuf.beginRenderPass(offscreenRenderPassBeginInfo, vk::SubpassContents::eInline);
        helloVk.rasterize(cmdBuf);
        cmdBuf.endRenderPass();
      }
    }

    // 2nd rendering pass: tone mapper, UI
    {
      vk::RenderPassBeginInfo postRenderPassBeginInfo;
      postRenderPassBeginInfo.setClearValueCount(2);
      postRenderPassBeginInfo.setPClearValues(clearValues);
      postRenderPassBeginInfo.setRenderPass(helloVk.getRenderPass());
      postRenderPassBeginInfo.setFramebuffer(helloVk.getFramebuffers()[curFrame]);
      postRenderPassBeginInfo.setRenderArea({{}, helloVk.getSize()});

      cmdBuf.beginRenderPass(postRenderPassBeginInfo, vk::SubpassContents::eInline);
      // Rendering tonemapper
      helloVk.drawPost(cmdBuf);
      // Rendering UI
      ImGui::Render();
      ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmdBuf);
      cmdBuf.endRenderPass();
    }

    // Submit for display
    cmdBuf.end();
    helloVk.submitFrame();
  }

  // Cleanup
  helloVk.getDevice().waitIdle();
  helloVk.destroyResources();
  helloVk.destroy();

  vkctx.deinit();

  glfwDestroyWindow(window);
  glfwTerminate();

  return 0;
}
