#pragma once

#include <vulkan/vulkan.h>

// The overlay's Vulkan side. The backend hands over what ImGui needs in a plain struct, so SXC
// depends on the Vulkan headers only, not on the backend's own (C++20) tier.
namespace SXC::Ui {
    struct VulkanTarget {
        VkInstance instance;
        VkPhysicalDevice physical;
        VkDevice device;
        uint32_t queueFamily;
        VkQueue queue;
        VkRenderPass renderPass;  // the pass the overlay is recorded into (subpass 0, single-sampled)
        uint32_t imageCount;      // swapchain images
        PFN_vkGetInstanceProcAddr getInstanceProcAddr;  // the backend loads Vulkan dynamically; so does ImGui
    };

    // Records the overlay into `cmd`, inside `target.renderPass`, when it's showing. Initialises
    // ImGui's Vulkan backend on first use and again when the render pass changes.
    void RenderVulkan(VkCommandBuffer cmd, const VulkanTarget& target);

    // Releases ImGui's Vulkan objects. Call with the device idle, before it's destroyed.
    void ShutdownVulkan();
}
