// VK_KHR_maintenance5 and VK_KHR_maintenance6 on a 1.2 driver.
//
// vkd3d-proton requires both feature bits (device.c: "maintenance5 and/or
// maintenance6 ... required for correct operation"), and then uses the
// entry points they add:
//
//   maintenance5: vkCmdBindIndexBuffer2KHR, vkGetRenderingAreaGranularityKHR,
//                 vkGetImageSubresourceLayout2KHR
//   maintenance6: vkCmdBindDescriptorSets2KHR, vkCmdPushDescriptorSet2KHR,
//                 vkCmdPushConstants2KHR
//
// The "2" forms are the same commands with their arguments in a struct, so
// they forward to the 1.0/1.1 versions; the two query functions answer from
// what a 1.2 device can say.
#include "shim.h"

#include <cstring>

// ----------------------------------------------------------- maintenance 5 --

extern "C" VKAPI_ATTR void VKAPI_CALL vkCmdBindIndexBuffer2KHR(VkCommandBuffer cmd,
                                                               VkBuffer buffer,
                                                               VkDeviceSize offset,
                                                               VkDeviceSize size,
                                                               VkIndexType index_type) {
    // 1.2 has no way to bind a bounded index buffer; the bound only matters for
    // out-of-range robustness, which the title does not rely on here.
    (void)size;
    shim::DeviceState* state = shim::device_of_command_buffer(cmd);
    if (!state) return;
    auto bind = reinterpret_cast<PFN_vkCmdBindIndexBuffer>(
        shim::loader().device_proc(state->device, "vkCmdBindIndexBuffer"));
    if (bind) bind(cmd, buffer, offset, index_type);
}

extern "C" VKAPI_ATTR void VKAPI_CALL vkGetRenderingAreaGranularityKHR(
    VkDevice device, const VkRenderingAreaInfoKHR* info, VkExtent2D* granularity) {
    (void)device;
    (void)info;
    // Used to align render areas; the tightest answer is always valid.
    if (granularity) *granularity = {1, 1};
}

extern "C" VKAPI_ATTR void VKAPI_CALL vkGetImageSubresourceLayout2KHR(
    VkDevice device, VkImage image, const VkImageSubresource2KHR* subresource,
    VkSubresourceLayout2KHR* layout) {
    if (!subresource || !layout) return;
    auto get = reinterpret_cast<PFN_vkGetImageSubresourceLayout>(
        shim::loader().device_proc(device, "vkGetImageSubresourceLayout"));
    if (get) get(device, image, &subresource->imageSubresource, &layout->subresourceLayout);
}

extern "C" VKAPI_ATTR void VKAPI_CALL vkGetDeviceImageSubresourceLayoutKHR(
    VkDevice device, const VkDeviceImageSubresourceInfoKHR* info,
    VkSubresourceLayout2KHR* layout) {
    if (!info || !layout || !info->pCreateInfo || !info->pSubresource) return;
    auto create = reinterpret_cast<PFN_vkCreateImage>(
        shim::loader().device_proc(device, "vkCreateImage"));
    auto destroy = reinterpret_cast<PFN_vkDestroyImage>(
        shim::loader().device_proc(device, "vkDestroyImage"));
    if (!create || !destroy) return;
    VkImage image = VK_NULL_HANDLE;
    if (create(device, info->pCreateInfo, nullptr, &image) != VK_SUCCESS) return;
    vkGetImageSubresourceLayout2KHR(device, image, info->pSubresource, layout);
    destroy(device, image, nullptr);
}

// ----------------------------------------------------------- maintenance 6 --

extern "C" VKAPI_ATTR void VKAPI_CALL vkCmdBindDescriptorSets2KHR(
    VkCommandBuffer cmd, const VkBindDescriptorSetsInfoKHR* info) {
    if (!info) return;
    shim::DeviceState* state = shim::device_of_command_buffer(cmd);
    if (!state) return;
    auto bind = reinterpret_cast<PFN_vkCmdBindDescriptorSets>(
        shim::loader().device_proc(state->device, "vkCmdBindDescriptorSets"));
    // The struct carries stage flags, not a bind point; compute is the only
    // case that is not graphics here.
    const VkPipelineBindPoint point = info->stageFlags == VK_SHADER_STAGE_COMPUTE_BIT
                                          ? VK_PIPELINE_BIND_POINT_COMPUTE
                                          : VK_PIPELINE_BIND_POINT_GRAPHICS;
    if (bind) {
        bind(cmd, point, info->layout, info->firstSet, info->descriptorSetCount,
             info->pDescriptorSets, info->dynamicOffsetCount, info->pDynamicOffsets);
    }
}

extern "C" VKAPI_ATTR void VKAPI_CALL vkCmdPushDescriptorSet2KHR(
    VkCommandBuffer cmd, const VkPushDescriptorSetInfoKHR* info) {
    if (!info) return;
    shim::DeviceState* state = shim::device_of_command_buffer(cmd);
    if (!state) return;
    const VkPipelineBindPoint point = info->stageFlags == VK_SHADER_STAGE_COMPUTE_BIT
                                          ? VK_PIPELINE_BIND_POINT_COMPUTE
                                          : VK_PIPELINE_BIND_POINT_GRAPHICS;
    shim::push_plain(state, cmd, point, info->layout, info->set,
                     info->descriptorWriteCount, info->pDescriptorWrites);
}

extern "C" VKAPI_ATTR void VKAPI_CALL vkCmdPushConstants2KHR(
    VkCommandBuffer cmd, const VkPushConstantsInfoKHR* info) {
    if (!info) return;
    shim::DeviceState* state = shim::device_of_command_buffer(cmd);
    if (!state) return;
    auto push = reinterpret_cast<PFN_vkCmdPushConstants>(
        shim::loader().device_proc(state->device, "vkCmdPushConstants"));
    if (push) push(cmd, info->layout, info->stageFlags, info->offset, info->size, info->pValues);
}

// ------------------------------------------------ extended dynamic state ---

// vkCmdBindVertexBuffers2 is 1.3 core; the driver has the EXT form on this
// machine, so forward there, and fall back to the 1.0 call if even that is
// missing (sizes and strides are only used by dynamic-state paths).
extern "C" VKAPI_ATTR void VKAPI_CALL vkCmdBindVertexBuffers2(
    VkCommandBuffer cmd, uint32_t first, uint32_t count, const VkBuffer* buffers,
    const VkDeviceSize* offsets, const VkDeviceSize* sizes, const VkDeviceSize* strides) {
    shim::DeviceState* state = shim::device_of_command_buffer(cmd);
    if (!state) return;
    if (auto ext = reinterpret_cast<PFN_vkCmdBindVertexBuffers2EXT>(
            shim::loader().device_proc(state->device, "vkCmdBindVertexBuffers2EXT"))) {
        ext(cmd, first, count, buffers, offsets, sizes, strides);
        return;
    }
    if (auto plain = reinterpret_cast<PFN_vkCmdBindVertexBuffers>(
            shim::loader().device_proc(state->device, "vkCmdBindVertexBuffers"))) {
        plain(cmd, first, count, buffers, offsets);
    }
}

extern "C" VKAPI_ATTR void VKAPI_CALL vkCmdBindVertexBuffers2EXT(
    VkCommandBuffer cmd, uint32_t first, uint32_t count, const VkBuffer* buffers,
    const VkDeviceSize* offsets, const VkDeviceSize* sizes, const VkDeviceSize* strides) {
    vkCmdBindVertexBuffers2(cmd, first, count, buffers, offsets, sizes, strides);
}
