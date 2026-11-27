// The "2" copy commands (1.3 core / VK_KHR_copy_commands2) on a 1.2 driver.
//
// Each takes the arguments in a struct and, where the driver has the KHR form,
// is a straight pass-through; otherwise the struct is unpacked into the plain
// 1.0 call, which describes exactly the same operation.
#include "shim.h"

#include <vector>

namespace {

struct Device {
    shim::DeviceState* state = nullptr;
    explicit Device(VkCommandBuffer cmd) : state(shim::device_of_command_buffer(cmd)) {}
};

}  // namespace

extern "C" VKAPI_ATTR void VKAPI_CALL vkCmdBlitImage2(VkCommandBuffer cmd,
                                                      const VkBlitImageInfo2* info) {
    Device d(cmd);
    if (!d.state || !info) return;
    if (auto khr = reinterpret_cast<PFN_vkCmdBlitImage2KHR>(
            shim::loader().device_proc(d.state->device, "vkCmdBlitImage2KHR"))) {
        khr(cmd, info);
        return;
    }
    auto plain = reinterpret_cast<PFN_vkCmdBlitImage>(
        shim::loader().device_proc(d.state->device, "vkCmdBlitImage"));
    if (!plain) return;
    std::vector<VkImageBlit> regions(info->regionCount);
    for (uint32_t i = 0; i < info->regionCount; ++i) {
        regions[i].srcSubresource = info->pRegions[i].srcSubresource;
        regions[i].dstSubresource = info->pRegions[i].dstSubresource;
        for (int j = 0; j < 2; ++j) {
            regions[i].srcOffsets[j] = info->pRegions[i].srcOffsets[j];
            regions[i].dstOffsets[j] = info->pRegions[i].dstOffsets[j];
        }
    }
    plain(cmd, info->srcImage, info->srcImageLayout, info->dstImage, info->dstImageLayout,
          info->regionCount, regions.data(), info->filter);
}

extern "C" VKAPI_ATTR void VKAPI_CALL vkCmdBlitImage2KHR(VkCommandBuffer cmd,
                                                         const VkBlitImageInfo2* info) {
    vkCmdBlitImage2(cmd, info);
}

extern "C" VKAPI_ATTR void VKAPI_CALL vkCmdCopyImage2(VkCommandBuffer cmd,
                                                      const VkCopyImageInfo2* info) {
    Device d(cmd);
    if (!d.state || !info) return;
    if (auto khr = reinterpret_cast<PFN_vkCmdCopyImage2KHR>(
            shim::loader().device_proc(d.state->device, "vkCmdCopyImage2KHR"))) {
        khr(cmd, info);
        return;
    }
    auto plain = reinterpret_cast<PFN_vkCmdCopyImage>(
        shim::loader().device_proc(d.state->device, "vkCmdCopyImage"));
    if (!plain) return;
    std::vector<VkImageCopy> regions(info->regionCount);
    for (uint32_t i = 0; i < info->regionCount; ++i) {
        regions[i].srcSubresource = info->pRegions[i].srcSubresource;
        regions[i].dstSubresource = info->pRegions[i].dstSubresource;
        for (int j = 0; j < 2; ++j) {
            regions[i].srcOffset = info->pRegions[i].srcOffset;
            regions[i].dstOffset = info->pRegions[i].dstOffset;
        }
        regions[i].extent = info->pRegions[i].extent;
    }
    plain(cmd, info->srcImage, info->srcImageLayout, info->dstImage, info->dstImageLayout,
          info->regionCount, regions.data());
}

extern "C" VKAPI_ATTR void VKAPI_CALL vkCmdCopyImage2KHR(VkCommandBuffer cmd,
                                                         const VkCopyImageInfo2* info) {
    vkCmdCopyImage2(cmd, info);
}

extern "C" VKAPI_ATTR void VKAPI_CALL vkCmdCopyBuffer2(VkCommandBuffer cmd,
                                                       const VkCopyBufferInfo2* info) {
    Device d(cmd);
    if (!d.state || !info) return;
    if (auto khr = reinterpret_cast<PFN_vkCmdCopyBuffer2KHR>(
            shim::loader().device_proc(d.state->device, "vkCmdCopyBuffer2KHR"))) {
        khr(cmd, info);
        return;
    }
    auto plain = reinterpret_cast<PFN_vkCmdCopyBuffer>(
        shim::loader().device_proc(d.state->device, "vkCmdCopyBuffer"));
    if (!plain) return;
    std::vector<VkBufferCopy> regions(info->regionCount);
    for (uint32_t i = 0; i < info->regionCount; ++i) {
        regions[i].srcOffset = info->pRegions[i].srcOffset;
        regions[i].dstOffset = info->pRegions[i].dstOffset;
        regions[i].size = info->pRegions[i].size;
    }
    plain(cmd, info->srcBuffer, info->dstBuffer, info->regionCount, regions.data());
}

extern "C" VKAPI_ATTR void VKAPI_CALL vkCmdCopyBuffer2KHR(VkCommandBuffer cmd,
                                                          const VkCopyBufferInfo2* info) {
    vkCmdCopyBuffer2(cmd, info);
}

extern "C" VKAPI_ATTR void VKAPI_CALL vkCmdCopyBufferToImage2(
    VkCommandBuffer cmd, const VkCopyBufferToImageInfo2* info) {
    Device d(cmd);
    if (!d.state || !info) return;
    if (auto khr = reinterpret_cast<PFN_vkCmdCopyBufferToImage2KHR>(
            shim::loader().device_proc(d.state->device, "vkCmdCopyBufferToImage2KHR"))) {
        khr(cmd, info);
        return;
    }
    auto plain = reinterpret_cast<PFN_vkCmdCopyBufferToImage>(
        shim::loader().device_proc(d.state->device, "vkCmdCopyBufferToImage"));
    if (!plain) return;
    std::vector<VkBufferImageCopy> regions(info->regionCount);
    for (uint32_t i = 0; i < info->regionCount; ++i) {
        const auto& s = info->pRegions[i];
        regions[i].bufferOffset = s.bufferOffset;
        regions[i].bufferRowLength = s.bufferRowLength;
        regions[i].bufferImageHeight = s.bufferImageHeight;
        regions[i].imageSubresource = s.imageSubresource;
        regions[i].imageOffset = s.imageOffset;
        regions[i].imageExtent = s.imageExtent;
    }
    plain(cmd, info->srcBuffer, info->dstImage, info->dstImageLayout, info->regionCount,
          regions.data());
}

extern "C" VKAPI_ATTR void VKAPI_CALL vkCmdCopyBufferToImage2KHR(
    VkCommandBuffer cmd, const VkCopyBufferToImageInfo2* info) {
    vkCmdCopyBufferToImage2(cmd, info);
}

extern "C" VKAPI_ATTR void VKAPI_CALL vkCmdCopyImageToBuffer2(
    VkCommandBuffer cmd, const VkCopyImageToBufferInfo2* info) {
    Device d(cmd);
    if (!d.state || !info) return;
    if (auto khr = reinterpret_cast<PFN_vkCmdCopyImageToBuffer2KHR>(
            shim::loader().device_proc(d.state->device, "vkCmdCopyImageToBuffer2KHR"))) {
        khr(cmd, info);
        return;
    }
    auto plain = reinterpret_cast<PFN_vkCmdCopyImageToBuffer>(
        shim::loader().device_proc(d.state->device, "vkCmdCopyImageToBuffer"));
    if (!plain) return;
    std::vector<VkBufferImageCopy> regions(info->regionCount);
    for (uint32_t i = 0; i < info->regionCount; ++i) {
        const auto& s = info->pRegions[i];
        regions[i].bufferOffset = s.bufferOffset;
        regions[i].bufferRowLength = s.bufferRowLength;
        regions[i].bufferImageHeight = s.bufferImageHeight;
        regions[i].imageSubresource = s.imageSubresource;
        regions[i].imageOffset = s.imageOffset;
        regions[i].imageExtent = s.imageExtent;
    }
    plain(cmd, info->srcImage, info->srcImageLayout, info->dstBuffer, info->regionCount,
          regions.data());
}

extern "C" VKAPI_ATTR void VKAPI_CALL vkCmdCopyImageToBuffer2KHR(
    VkCommandBuffer cmd, const VkCopyImageToBufferInfo2* info) {
    vkCmdCopyImageToBuffer2(cmd, info);
}

extern "C" VKAPI_ATTR void VKAPI_CALL vkCmdResolveImage2(VkCommandBuffer cmd,
                                                         const VkResolveImageInfo2* info) {
    Device d(cmd);
    if (!d.state || !info) return;
    if (auto khr = reinterpret_cast<PFN_vkCmdResolveImage2KHR>(
            shim::loader().device_proc(d.state->device, "vkCmdResolveImage2KHR"))) {
        khr(cmd, info);
        return;
    }
    auto plain = reinterpret_cast<PFN_vkCmdResolveImage>(
        shim::loader().device_proc(d.state->device, "vkCmdResolveImage"));
    if (!plain) return;
    std::vector<VkImageResolve> regions(info->regionCount);
    for (uint32_t i = 0; i < info->regionCount; ++i) {
        regions[i].srcSubresource = info->pRegions[i].srcSubresource;
        regions[i].dstSubresource = info->pRegions[i].dstSubresource;
        regions[i].srcOffset = info->pRegions[i].srcOffset;
        regions[i].dstOffset = info->pRegions[i].dstOffset;
        regions[i].extent = info->pRegions[i].extent;
    }
    plain(cmd, info->srcImage, info->srcImageLayout, info->dstImage, info->dstImageLayout,
          info->regionCount, regions.data());
}

extern "C" VKAPI_ATTR void VKAPI_CALL vkCmdResolveImage2KHR(VkCommandBuffer cmd,
                                                            const VkResolveImageInfo2* info) {
    vkCmdResolveImage2(cmd, info);
}
