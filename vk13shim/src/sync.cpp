// Synchronisation and memory-requirement entry points.
//
// vkCmdPipelineBarrier2 and vkQueueSubmit2 are 1.3 core, but the same work is
// available on 1.2 through VK_KHR_synchronization2 (vkCmdPipelineBarrier2KHR,
// vkQueueSubmit2KHR). This machine has that extension, so the mapping is a
// rename; the fallback below only exists for drivers without it.
//
// maintenance4 (vkGetDevice*MemoryRequirements) has no 1.2 equivalent: it
// answers "what would this buffer/image need" without creating one. The
// approximation creates a throwaway object, queries it, and destroys it.
#include "shim.h"

#include <cstdio>
#include <vector>

// ------------------------------------------------------------------ barrier --

extern "C" VKAPI_ATTR void VKAPI_CALL vkCmdPipelineBarrier2(VkCommandBuffer command_buffer,
                                                            const VkDependencyInfo* dependency) {
    shim::DeviceState* state = shim::device_of_command_buffer(command_buffer);
    if (!state || !dependency) return;
    if (state->cmd_pipeline_barrier_2) {
        state->cmd_pipeline_barrier_2(command_buffer, dependency);
        return;
    }
    if (!state->cmd_pipeline_barrier) return;

    // 1.2 fallback: the legacy stage/access bits are the low bits of the 1.3
    // ones, so truncation is correct for everything that existed before.
    auto stage = [](VkPipelineStageFlags2 v) { return static_cast<VkPipelineStageFlags>(v & 0xFFFF); };
    auto access = [](VkAccessFlags2 v) { return static_cast<VkAccessFlags>(v & 0xFFFF); };

    std::vector<VkMemoryBarrier> memory(dependency->memoryBarrierCount);
    for (uint32_t i = 0; i < dependency->memoryBarrierCount; ++i) {
        const auto& src = dependency->pMemoryBarriers[i];
        memory[i] = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr, access(src.srcAccessMask),
                     access(src.dstAccessMask)};
    }
    std::vector<VkBufferMemoryBarrier> buffers(dependency->bufferMemoryBarrierCount);
    for (uint32_t i = 0; i < dependency->bufferMemoryBarrierCount; ++i) {
        const auto& src = dependency->pBufferMemoryBarriers[i];
        buffers[i] = {VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
                      nullptr,
                      access(src.srcAccessMask),
                      access(src.dstAccessMask),
                      src.srcQueueFamilyIndex,
                      src.dstQueueFamilyIndex,
                      src.buffer,
                      src.offset,
                      src.size};
    }
    std::vector<VkImageMemoryBarrier> images(dependency->imageMemoryBarrierCount);
    for (uint32_t i = 0; i < dependency->imageMemoryBarrierCount; ++i) {
        const auto& src = dependency->pImageMemoryBarriers[i];
        images[i] = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                     nullptr,
                     access(src.srcAccessMask),
                     access(src.dstAccessMask),
                     src.oldLayout,
                     src.newLayout,
                     src.srcQueueFamilyIndex,
                     src.dstQueueFamilyIndex,
                     src.image,
                     src.subresourceRange};
    }
    // VkDependencyInfo carries no stage masks of its own; be conservative and
    // let the per-barrier masks narrow it down.
    state->cmd_pipeline_barrier(command_buffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0,
                                static_cast<uint32_t>(memory.size()), memory.data(),
                                static_cast<uint32_t>(buffers.size()), buffers.data(),
                                static_cast<uint32_t>(images.size()), images.data());
}

// -------------------------------------------------------------------- submit --

extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkQueueSubmit2(VkQueue queue, uint32_t count,
                                                         const VkSubmitInfo2* submits,
                                                         VkFence fence) {
    // vkQueueSubmit2 has no device argument; the queue belongs to one device,
    // which the loader would normally dispatch on. The KHR path avoids needing
    // the mapping at all, and this machine has it.
    static PFN_vkQueueSubmit2KHR cached = nullptr;
    if (!cached) {
        // The loader's export table is the cheapest route, but a loader built
        // without the KHR alias only hands it out through one of the
        // proc-addr entry points (the queue is dispatchable, so its trampoline
        // works).
        cached = reinterpret_cast<PFN_vkQueueSubmit2KHR>(
            GetProcAddress(shim::loader().module, "vkQueueSubmit2KHR"));
        if (!cached) cached = reinterpret_cast<PFN_vkQueueSubmit2KHR>(
            shim::loader().instance_proc(shim::any_instance(), "vkQueueSubmit2KHR"));
        if (!cached) {
            std::fprintf(stderr, "vulkan-1 shim: no vkQueueSubmit2KHR anywhere\n");
            return VK_ERROR_FEATURE_NOT_PRESENT;
        }
    }
    return cached(queue, count, submits, fence);
}

// -------------------------------------------------------------- maintenance4 --

extern "C" VKAPI_ATTR void VKAPI_CALL vkGetDeviceBufferMemoryRequirements(
    VkDevice device, const VkDeviceBufferMemoryRequirements* info,
    VkMemoryRequirements2* requirements) {
    shim::DeviceState* state = shim::state_of(device);
    if (!state || !info || !requirements || !state->get_buffer_memory_requirements) return;

    auto create = reinterpret_cast<PFN_vkCreateBuffer>(
        shim::loader().device_proc(device, "vkCreateBuffer"));
    auto destroy = reinterpret_cast<PFN_vkDestroyBuffer>(
        shim::loader().device_proc(device, "vkDestroyBuffer"));
    if (!create || !destroy) return;

    VkBuffer buffer = VK_NULL_HANDLE;
    if (create(device, info->pCreateInfo, nullptr, &buffer) != VK_SUCCESS) return;
    VkMemoryRequirements plain{};
    state->get_buffer_memory_requirements(device, buffer, &plain);
    destroy(device, buffer, nullptr);
    requirements->memoryRequirements = plain;
}

extern "C" VKAPI_ATTR void VKAPI_CALL vkGetDeviceImageMemoryRequirements(
    VkDevice device, const VkDeviceImageMemoryRequirements* info,
    VkMemoryRequirements2* requirements) {
    shim::DeviceState* state = shim::state_of(device);
    if (!state || !info || !requirements || !state->get_image_memory_requirements) return;

    auto create = reinterpret_cast<PFN_vkCreateImage>(
        shim::loader().device_proc(device, "vkCreateImage"));
    auto destroy = reinterpret_cast<PFN_vkDestroyImage>(
        shim::loader().device_proc(device, "vkDestroyImage"));
    if (!create || !destroy) return;

    VkImage image = VK_NULL_HANDLE;
    if (create(device, info->pCreateInfo, nullptr, &image) != VK_SUCCESS) return;
    VkMemoryRequirements plain{};
    state->get_image_memory_requirements(device, image, &plain);
    destroy(device, image, nullptr);
    requirements->memoryRequirements = plain;
}

// KHR aliases of the four entry points above: same behaviour, different names,
// because vkd3d-proton resolves whichever exists.
extern "C" VKAPI_ATTR void VKAPI_CALL vkCmdPipelineBarrier2KHR(VkCommandBuffer command_buffer,
                                                               const VkDependencyInfo* dependency) {
    vkCmdPipelineBarrier2(command_buffer, dependency);
}

extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkQueueSubmit2KHR(VkQueue queue, uint32_t count,
                                                            const VkSubmitInfo2* submits,
                                                            VkFence fence) {
    return vkQueueSubmit2(queue, count, submits, fence);
}

extern "C" VKAPI_ATTR void VKAPI_CALL vkGetDeviceBufferMemoryRequirementsKHR(
    VkDevice device, const VkDeviceBufferMemoryRequirements* info,
    VkMemoryRequirements2* requirements) {
    vkGetDeviceBufferMemoryRequirements(device, info, requirements);
}

extern "C" VKAPI_ATTR void VKAPI_CALL vkGetDeviceImageMemoryRequirementsKHR(
    VkDevice device, const VkDeviceImageMemoryRequirements* info,
    VkMemoryRequirements2* requirements) {
    vkGetDeviceImageMemoryRequirements(device, info, requirements);
}

// maintenance4: sparse requirements for an image that does not exist yet.
extern "C" VKAPI_ATTR void VKAPI_CALL vkGetDeviceImageSparseMemoryRequirements(
    VkDevice device, const VkDeviceImageMemoryRequirements* info,
    uint32_t* count, VkSparseImageMemoryRequirements2* requirements) {
    shim::DeviceState* state = shim::state_of(device);
    if (!state || !info || !count || !info->pCreateInfo) return;

    auto create = reinterpret_cast<PFN_vkCreateImage>(
        shim::loader().device_proc(device, "vkCreateImage"));
    auto destroy = reinterpret_cast<PFN_vkDestroyImage>(
        shim::loader().device_proc(device, "vkDestroyImage"));
    auto get = reinterpret_cast<PFN_vkGetImageSparseMemoryRequirements>(
        shim::loader().device_proc(device, "vkGetImageSparseMemoryRequirements"));
    if (!create || !destroy || !get) return;

    VkImage image = VK_NULL_HANDLE;
    if (create(device, info->pCreateInfo, nullptr, &image) != VK_SUCCESS) return;

    uint32_t needed = 0;
    get(device, image, &needed, nullptr);
    if (!requirements) {
        *count = needed;
    } else if (*count >= needed) {
        std::vector<VkSparseImageMemoryRequirements> plain(needed);
        get(device, image, &needed, plain.data());
        for (uint32_t i = 0; i < needed; ++i) {
            // VkSparseImageMemoryRequirements2 wraps the plain struct.
            requirements[i].memoryRequirements = plain[i];
        }
        *count = needed;
    }
    destroy(device, image, nullptr);
}

extern "C" VKAPI_ATTR void VKAPI_CALL vkGetDeviceImageSparseMemoryRequirementsKHR(
    VkDevice device, const VkDeviceImageMemoryRequirements* info,
    uint32_t* count, VkSparseImageMemoryRequirements2* requirements) {
    vkGetDeviceImageSparseMemoryRequirements(device, info, count, requirements);
}
