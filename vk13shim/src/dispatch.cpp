// Dispatch table: every entry point this DLL implements itself.
//
// vkGetInstanceProcAddr and vkGetDeviceProcAddr consult this first, so a
// synthesised function is reachable both by export (link-time import) and by
// name (vkd3d-proton resolves most of its device functions this way).
#include "shim.h"

#include <cstring>

// loader.cpp
extern "C" VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance, const char*);
extern "C" VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice, const char*);
extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkCreateInstance(const VkInstanceCreateInfo*,
                                                           const VkAllocationCallbacks*, VkInstance*);
extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkCreateDevice(VkPhysicalDevice, const VkDeviceCreateInfo*,
                                                         const VkAllocationCallbacks*, VkDevice*);
extern "C" VKAPI_ATTR void VKAPI_CALL vkDestroyDevice(VkDevice, const VkAllocationCallbacks*);
extern "C" VKAPI_ATTR void VKAPI_CALL vkDestroyInstance(VkInstance, const VkAllocationCallbacks*);
extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateDeviceExtensionProperties(
    VkPhysicalDevice, const char*, uint32_t*, VkExtensionProperties*);
extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkEnumeratePhysicalDevices(VkInstance, uint32_t*, VkPhysicalDevice*);
extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateInstanceVersion(uint32_t*);
extern "C" VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceProperties2KHR(VkPhysicalDevice, VkPhysicalDeviceProperties2*);
extern "C" VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceFeatures2KHR(VkPhysicalDevice, VkPhysicalDeviceFeatures2*);
extern "C" VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceFormatProperties2(VkPhysicalDevice, VkFormat, VkFormatProperties2*);
extern "C" VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceFormatProperties2KHR(VkPhysicalDevice, VkFormat, VkFormatProperties2*);
extern "C" VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceQueueFamilyProperties2(VkPhysicalDevice, uint32_t*, VkQueueFamilyProperties2*);
extern "C" VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceQueueFamilyProperties2KHR(VkPhysicalDevice, uint32_t*, VkQueueFamilyProperties2*);
extern "C" VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceFormatProperties(VkPhysicalDevice, VkFormat, VkFormatProperties*);
extern "C" VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceQueueFamilyProperties(VkPhysicalDevice, uint32_t*, VkQueueFamilyProperties*);
extern "C" VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceProperties(VkPhysicalDevice,
                                                                    VkPhysicalDeviceProperties*);
extern "C" VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceProperties2(VkPhysicalDevice,
                                                                     VkPhysicalDeviceProperties2*);
extern "C" VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceFeatures2(VkPhysicalDevice,
                                                                   VkPhysicalDeviceFeatures2*);

// renderpass.cpp
extern "C" VKAPI_ATTR void VKAPI_CALL vkCmdBeginRendering(VkCommandBuffer,
                                                          const VkRenderingInfo*);
extern "C" VKAPI_ATTR void VKAPI_CALL vkCmdEndRendering(VkCommandBuffer);
extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkAllocateCommandBuffers(VkDevice,
                                                                   const VkCommandBufferAllocateInfo*,
                                                                   VkCommandBuffer*);
extern "C" VKAPI_ATTR void VKAPI_CALL vkFreeCommandBuffers(VkDevice, VkCommandPool, uint32_t,
                                                           const VkCommandBuffer*);
extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkCreateImageView(VkDevice, const VkImageViewCreateInfo*,
                                                            const VkAllocationCallbacks*,
                                                            VkImageView*);
extern "C" VKAPI_ATTR void VKAPI_CALL vkDestroyImageView(VkDevice, VkImageView,
                                                         const VkAllocationCallbacks*);

// sync.cpp
extern "C" VKAPI_ATTR void VKAPI_CALL vkCmdPipelineBarrier2(VkCommandBuffer,
                                                            const VkDependencyInfo*);
extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkQueueSubmit2(VkQueue, uint32_t, const VkSubmitInfo2*,
                                                         VkFence);
extern "C" VKAPI_ATTR void VKAPI_CALL vkGetDeviceBufferMemoryRequirements(
    VkDevice, const VkDeviceBufferMemoryRequirements*, VkMemoryRequirements2*);
extern "C" VKAPI_ATTR void VKAPI_CALL vkGetDeviceImageMemoryRequirements(
    VkDevice, const VkDeviceImageMemoryRequirements*, VkMemoryRequirements2*);

extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkCreateShaderModuleKHR(VkDevice, const VkShaderModuleCreateInfo*, const VkAllocationCallbacks*, VkShaderModule*);
// Declared here because the headers only declare these when the extension that
// introduced them is defined.
extern "C" VKAPI_ATTR void VKAPI_CALL vkCmdSetPatchControlPointsEXT(VkCommandBuffer, uint32_t);
extern "C" VKAPI_ATTR void VKAPI_CALL vkCmdSetRasterizerDiscardEnable(VkCommandBuffer, VkBool32);
extern "C" VKAPI_ATTR void VKAPI_CALL vkCmdSetRasterizerDiscardEnableEXT(VkCommandBuffer, VkBool32);
extern "C" VKAPI_ATTR void VKAPI_CALL vkCmdSetDepthBiasEnable(VkCommandBuffer, VkBool32);
extern "C" VKAPI_ATTR void VKAPI_CALL vkCmdSetDepthBiasEnableEXT(VkCommandBuffer, VkBool32);
extern "C" VKAPI_ATTR void VKAPI_CALL vkCmdSetLogicOp(VkCommandBuffer, VkLogicOp);
extern "C" VKAPI_ATTR void VKAPI_CALL vkCmdSetLogicOpEXT(VkCommandBuffer, VkLogicOp);
extern "C" VKAPI_ATTR void VKAPI_CALL vkCmdSetPrimitiveRestartEnable(VkCommandBuffer, VkBool32);
extern "C" VKAPI_ATTR void VKAPI_CALL vkCmdSetPrimitiveRestartEnableEXT(VkCommandBuffer, VkBool32);

namespace shim {

PFN_vkVoidFunction synthesised_proc(const char* name) {
    struct Entry {
        const char* name;
        PFN_vkVoidFunction fn;
    };
    static const Entry entries[] = {
        // instance level
        {"vkGetInstanceProcAddr", reinterpret_cast<PFN_vkVoidFunction>(vkGetInstanceProcAddr)},
        {"vkGetDeviceProcAddr", reinterpret_cast<PFN_vkVoidFunction>(vkGetDeviceProcAddr)},
        {"vkCreateInstance", reinterpret_cast<PFN_vkVoidFunction>(vkCreateInstance)},
        {"vkEnumeratePhysicalDevices", reinterpret_cast<PFN_vkVoidFunction>(vkEnumeratePhysicalDevices)},
        {"vkEnumerateInstanceVersion", reinterpret_cast<PFN_vkVoidFunction>(vkEnumerateInstanceVersion)},
        {"vkCreateDevice", reinterpret_cast<PFN_vkVoidFunction>(vkCreateDevice)},
        {"vkDestroyDevice", reinterpret_cast<PFN_vkVoidFunction>(vkDestroyDevice)},
        {"vkDestroyInstance", reinterpret_cast<PFN_vkVoidFunction>(vkDestroyInstance)},
        {"vkEnumerateDeviceExtensionProperties",
         reinterpret_cast<PFN_vkVoidFunction>(vkEnumerateDeviceExtensionProperties)},
        {"vkGetPhysicalDeviceProperties2KHR", reinterpret_cast<PFN_vkVoidFunction>(vkGetPhysicalDeviceProperties2KHR)},
        {"vkGetPhysicalDeviceFeatures2KHR", reinterpret_cast<PFN_vkVoidFunction>(vkGetPhysicalDeviceFeatures2KHR)},
        {"vkGetPhysicalDeviceFormatProperties2", reinterpret_cast<PFN_vkVoidFunction>(vkGetPhysicalDeviceFormatProperties2)},
        {"vkGetPhysicalDeviceFormatProperties2KHR", reinterpret_cast<PFN_vkVoidFunction>(vkGetPhysicalDeviceFormatProperties2KHR)},
        {"vkGetPhysicalDeviceQueueFamilyProperties2", reinterpret_cast<PFN_vkVoidFunction>(vkGetPhysicalDeviceQueueFamilyProperties2)},
        {"vkGetPhysicalDeviceQueueFamilyProperties2KHR", reinterpret_cast<PFN_vkVoidFunction>(vkGetPhysicalDeviceQueueFamilyProperties2KHR)},
        {"vkGetPhysicalDeviceFormatProperties", reinterpret_cast<PFN_vkVoidFunction>(vkGetPhysicalDeviceFormatProperties)},
        {"vkGetPhysicalDeviceQueueFamilyProperties", reinterpret_cast<PFN_vkVoidFunction>(vkGetPhysicalDeviceQueueFamilyProperties)},
        {"vkGetPhysicalDeviceProperties",
         reinterpret_cast<PFN_vkVoidFunction>(vkGetPhysicalDeviceProperties)},
        {"vkGetPhysicalDeviceProperties2",
         reinterpret_cast<PFN_vkVoidFunction>(vkGetPhysicalDeviceProperties2)},
        {"vkGetPhysicalDeviceFeatures2",
         reinterpret_cast<PFN_vkVoidFunction>(vkGetPhysicalDeviceFeatures2)},
        // device level
        {"vkCreateDescriptorSetLayout", reinterpret_cast<PFN_vkVoidFunction>(vkCreateDescriptorSetLayout)},
        {"vkDestroyDescriptorSetLayout", reinterpret_cast<PFN_vkVoidFunction>(vkDestroyDescriptorSetLayout)},
        {"vkCreatePipelineLayout", reinterpret_cast<PFN_vkVoidFunction>(vkCreatePipelineLayout)},
        {"vkDestroyPipelineLayout", reinterpret_cast<PFN_vkVoidFunction>(vkDestroyPipelineLayout)},
        {"vkCreateDescriptorUpdateTemplate", reinterpret_cast<PFN_vkVoidFunction>(vkCreateDescriptorUpdateTemplate)},
        {"vkCreateDescriptorUpdateTemplateKHR", reinterpret_cast<PFN_vkVoidFunction>(vkCreateDescriptorUpdateTemplateKHR)},
        {"vkDestroyDescriptorUpdateTemplate", reinterpret_cast<PFN_vkVoidFunction>(vkDestroyDescriptorUpdateTemplate)},
        {"vkDestroyDescriptorUpdateTemplateKHR", reinterpret_cast<PFN_vkVoidFunction>(vkDestroyDescriptorUpdateTemplateKHR)},
        {"vkBeginCommandBuffer", reinterpret_cast<PFN_vkVoidFunction>(vkBeginCommandBuffer)},
        {"vkCmdPushDescriptorSetKHR", reinterpret_cast<PFN_vkVoidFunction>(vkCmdPushDescriptorSetKHR)},
        {"vkCmdPushDescriptorSetWithTemplateKHR", reinterpret_cast<PFN_vkVoidFunction>(vkCmdPushDescriptorSetWithTemplateKHR)},
        {"vkCmdBindIndexBuffer2KHR", reinterpret_cast<PFN_vkVoidFunction>(vkCmdBindIndexBuffer2KHR)},
        {"vkGetRenderingAreaGranularityKHR", reinterpret_cast<PFN_vkVoidFunction>(vkGetRenderingAreaGranularityKHR)},
        {"vkGetImageSubresourceLayout2KHR", reinterpret_cast<PFN_vkVoidFunction>(vkGetImageSubresourceLayout2KHR)},
        {"vkGetDeviceImageSubresourceLayoutKHR", reinterpret_cast<PFN_vkVoidFunction>(vkGetDeviceImageSubresourceLayoutKHR)},
        {"vkCmdBindDescriptorSets2KHR", reinterpret_cast<PFN_vkVoidFunction>(vkCmdBindDescriptorSets2KHR)},
        {"vkCmdPushDescriptorSet2KHR", reinterpret_cast<PFN_vkVoidFunction>(vkCmdPushDescriptorSet2KHR)},
        {"vkCmdPushConstants2KHR", reinterpret_cast<PFN_vkVoidFunction>(vkCmdPushConstants2KHR)},
        {"vkCreateShaderModule", reinterpret_cast<PFN_vkVoidFunction>(vkCreateShaderModule)},
        {"vkCreateShaderModuleKHR", reinterpret_cast<PFN_vkVoidFunction>(vkCreateShaderModuleKHR)},
        {"vkCmdSetPatchControlPointsEXT", reinterpret_cast<PFN_vkVoidFunction>(vkCmdSetPatchControlPointsEXT)},
        {"vkCmdSetRasterizerDiscardEnable", reinterpret_cast<PFN_vkVoidFunction>(vkCmdSetRasterizerDiscardEnable)},
        {"vkCmdSetRasterizerDiscardEnableEXT", reinterpret_cast<PFN_vkVoidFunction>(vkCmdSetRasterizerDiscardEnableEXT)},
        {"vkCmdSetDepthBiasEnable", reinterpret_cast<PFN_vkVoidFunction>(vkCmdSetDepthBiasEnable)},
        {"vkCmdSetDepthBiasEnableEXT", reinterpret_cast<PFN_vkVoidFunction>(vkCmdSetDepthBiasEnableEXT)},
        {"vkCmdSetLogicOp", reinterpret_cast<PFN_vkVoidFunction>(vkCmdSetLogicOp)},
        {"vkCmdSetLogicOpEXT", reinterpret_cast<PFN_vkVoidFunction>(vkCmdSetLogicOpEXT)},
        {"vkCmdSetPrimitiveRestartEnable", reinterpret_cast<PFN_vkVoidFunction>(vkCmdSetPrimitiveRestartEnable)},
        {"vkCmdSetPrimitiveRestartEnableEXT", reinterpret_cast<PFN_vkVoidFunction>(vkCmdSetPrimitiveRestartEnableEXT)},
        {"vkCmdBlitImage2", reinterpret_cast<PFN_vkVoidFunction>(vkCmdBlitImage2)},
        {"vkCmdBlitImage2KHR", reinterpret_cast<PFN_vkVoidFunction>(vkCmdBlitImage2KHR)},
        {"vkCmdCopyImage2", reinterpret_cast<PFN_vkVoidFunction>(vkCmdCopyImage2)},
        {"vkCmdCopyImage2KHR", reinterpret_cast<PFN_vkVoidFunction>(vkCmdCopyImage2KHR)},
        {"vkCmdCopyBuffer2", reinterpret_cast<PFN_vkVoidFunction>(vkCmdCopyBuffer2)},
        {"vkCmdCopyBuffer2KHR", reinterpret_cast<PFN_vkVoidFunction>(vkCmdCopyBuffer2KHR)},
        {"vkCmdCopyBufferToImage2", reinterpret_cast<PFN_vkVoidFunction>(vkCmdCopyBufferToImage2)},
        {"vkCmdCopyBufferToImage2KHR", reinterpret_cast<PFN_vkVoidFunction>(vkCmdCopyBufferToImage2KHR)},
        {"vkCmdCopyImageToBuffer2", reinterpret_cast<PFN_vkVoidFunction>(vkCmdCopyImageToBuffer2)},
        {"vkCmdCopyImageToBuffer2KHR", reinterpret_cast<PFN_vkVoidFunction>(vkCmdCopyImageToBuffer2KHR)},
        {"vkCmdResolveImage2", reinterpret_cast<PFN_vkVoidFunction>(vkCmdResolveImage2)},
        {"vkCmdResolveImage2KHR", reinterpret_cast<PFN_vkVoidFunction>(vkCmdResolveImage2KHR)},
        {"vkCmdBindVertexBuffers2", reinterpret_cast<PFN_vkVoidFunction>(vkCmdBindVertexBuffers2)},
        {"vkCmdBindVertexBuffers2EXT", reinterpret_cast<PFN_vkVoidFunction>(vkCmdBindVertexBuffers2EXT)},
        {"vkCmdBeginRendering", reinterpret_cast<PFN_vkVoidFunction>(vkCmdBeginRendering)},
        {"vkCmdEndRendering", reinterpret_cast<PFN_vkVoidFunction>(vkCmdEndRendering)},
        {"vkAllocateCommandBuffers",
         reinterpret_cast<PFN_vkVoidFunction>(vkAllocateCommandBuffers)},
        {"vkFreeCommandBuffers", reinterpret_cast<PFN_vkVoidFunction>(vkFreeCommandBuffers)},
        {"vkCreateImageView", reinterpret_cast<PFN_vkVoidFunction>(vkCreateImageView)},
        {"vkDestroyImageView", reinterpret_cast<PFN_vkVoidFunction>(vkDestroyImageView)},
        {"vkCmdPipelineBarrier2", reinterpret_cast<PFN_vkVoidFunction>(vkCmdPipelineBarrier2)},
        {"vkCmdPipelineBarrier2KHR", reinterpret_cast<PFN_vkVoidFunction>(vkCmdPipelineBarrier2)},
        {"vkQueueSubmit2", reinterpret_cast<PFN_vkVoidFunction>(vkQueueSubmit2)},
        {"vkQueueSubmit2KHR", reinterpret_cast<PFN_vkVoidFunction>(vkQueueSubmit2)},
        {"vkGetDeviceBufferMemoryRequirements",
         reinterpret_cast<PFN_vkVoidFunction>(vkGetDeviceBufferMemoryRequirements)},
        {"vkGetDeviceBufferMemoryRequirementsKHR",
         reinterpret_cast<PFN_vkVoidFunction>(vkGetDeviceBufferMemoryRequirements)},
        {"vkGetDeviceImageSparseMemoryRequirements", reinterpret_cast<PFN_vkVoidFunction>(vkGetDeviceImageSparseMemoryRequirements)},
        {"vkGetDeviceImageSparseMemoryRequirementsKHR", reinterpret_cast<PFN_vkVoidFunction>(vkGetDeviceImageSparseMemoryRequirementsKHR)},
        {"vkGetDeviceImageMemoryRequirements",
         reinterpret_cast<PFN_vkVoidFunction>(vkGetDeviceImageMemoryRequirements)},
        {"vkGetDeviceImageMemoryRequirementsKHR",
         reinterpret_cast<PFN_vkVoidFunction>(vkGetDeviceImageMemoryRequirements)},
    };
    for (const auto& e : entries) {
        if (std::strcmp(name, e.name) == 0) return e.fn;
    }
    return nullptr;
}

}  // namespace shim
