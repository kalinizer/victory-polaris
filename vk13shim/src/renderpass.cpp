// Dynamic rendering bridge: vkCmdBeginRendering (1.3) -> vkCmdBeginRenderPass
// (1.0).
//
// The piece of information VkRenderingInfo does not carry is the attachment
// format (it holds image views only, and Vulkan has no query for a view's
// format). Every vkCreateImageView is therefore recorded on the way through;
// that is why this file also exports the image-view entry points.
//
// A render pass is built from the attachment descriptions (format, samples,
// layout, load/store) and cached per device; a framebuffer is built from the
// exact view set and cached under that pass.
#include "shim.h"

#include <cstring>
#include <mutex>
#include <unordered_map>

namespace shim {

static std::mutex g_view_lock;
static std::unordered_map<VkImageView, VkImageViewCreateInfo> g_views;

void register_view(VkImageView view, const VkImageViewCreateInfo& info) {
    std::lock_guard<std::mutex> guard(g_view_lock);
    g_views[view] = info;
}

void unregister_view(VkImageView view) {
    std::lock_guard<std::mutex> guard(g_view_lock);
    g_views.erase(view);
}

bool view_format(VkImageView view, VkFormat* format, VkSampleCountFlagBits* samples) {
    std::lock_guard<std::mutex> guard(g_view_lock);
    auto it = g_views.find(view);
    if (it == g_views.end()) return false;
    *format = it->second.format;
    // TODO: a view does not carry the image's sample count; record
    // VkImageCreateInfo in vkCreateImage to fill this in properly.
    *samples = VK_SAMPLE_COUNT_1_BIT;
    return true;
}

static uint64_t hash_views(const std::vector<VkImageView>& views) {
    uint64_t h = 1469598103934665603ull;
    for (VkImageView v : views) h = (h ^ reinterpret_cast<uint64_t>(v)) * 1099511628211ull;
    return h;
}

static VkAttachmentDescription describe(const VkRenderingAttachmentInfo& a,
                                        const AttachmentKey& key) {
    VkAttachmentDescription d{};
    d.format = key.format;
    d.samples = key.samples;
    d.loadOp = key.load;
    d.storeOp = key.store;
    d.stencilLoadOp = key.load == VK_ATTACHMENT_LOAD_OP_CLEAR ? VK_ATTACHMENT_LOAD_OP_CLEAR
                                                             : VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    d.stencilStoreOp = key.store;
    d.initialLayout = a.imageLayout;
    d.finalLayout = a.imageLayout;
    return d;
}

VkRenderPass build_pass_locked(DeviceState* state, const VkRenderingInfo& info,
                               const RenderPassKey& key, RenderPassEntry* entry) {
    std::vector<VkAttachmentDescription> attachments;
    std::vector<VkAttachmentReference> color_refs;
    VkAttachmentReference depth_ref{};
    VkAttachmentReference stencil_ref{};

    for (uint32_t i = 0; i < info.colorAttachmentCount; ++i) {
        const auto& a = info.pColorAttachments[i];
        attachments.push_back(describe(a, key.color[i]));
        color_refs.push_back({static_cast<uint32_t>(attachments.size() - 1), a.imageLayout});
        entry->views.push_back(a.imageView);
    }
    if (info.pDepthAttachment && info.pDepthAttachment->imageView != VK_NULL_HANDLE) {
        attachments.push_back(describe(*info.pDepthAttachment, key.depth));
        depth_ref = {static_cast<uint32_t>(attachments.size() - 1),
                     info.pDepthAttachment->imageLayout};
        entry->views.push_back(info.pDepthAttachment->imageView);
    }
    if (info.pStencilAttachment && info.pStencilAttachment->imageView != VK_NULL_HANDLE &&
        (!info.pDepthAttachment ||
         info.pStencilAttachment->imageView != info.pDepthAttachment->imageView)) {
        attachments.push_back(describe(*info.pStencilAttachment, key.stencil));
        stencil_ref = {static_cast<uint32_t>(attachments.size() - 1),
                       info.pStencilAttachment->imageLayout};
        entry->views.push_back(info.pStencilAttachment->imageView);
    }

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = static_cast<uint32_t>(color_refs.size());
    subpass.pColorAttachments = color_refs.empty() ? nullptr : color_refs.data();
    subpass.pDepthStencilAttachment =
        key.has_depth ? &depth_ref : key.has_stencil ? &stencil_ref : nullptr;

    uint32_t view_mask = key.view_mask;
    VkRenderPassMultiviewCreateInfo multiview{VK_STRUCTURE_TYPE_RENDER_PASS_MULTIVIEW_CREATE_INFO};
    if (view_mask != 0) {
        multiview.subpassCount = 1;
        multiview.pViewMasks = &view_mask;
    }

    VkRenderPassCreateInfo create{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    create.pNext = view_mask ? &multiview : nullptr;
    create.attachmentCount = static_cast<uint32_t>(attachments.size());
    create.pAttachments = attachments.data();
    create.subpassCount = 1;
    create.pSubpasses = &subpass;

    VkRenderPass pass = VK_NULL_HANDLE;
    if (state->create_render_pass(state->device, &create, nullptr, &pass) != VK_SUCCESS) {
        return VK_NULL_HANDLE;
    }
    entry->pass = pass;
    return pass;
}

VkFramebuffer framebuffer_locked(DeviceState* state, RenderPassEntry& entry,
                                 const VkExtent2D& area, uint32_t layers) {
    const uint64_t hash = hash_views(entry.views);
    auto it = entry.framebuffers.find(hash);
    if (it != entry.framebuffers.end()) return it->second;

    VkFramebufferCreateInfo create{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    create.renderPass = entry.pass;
    create.attachmentCount = static_cast<uint32_t>(entry.views.size());
    create.pAttachments = entry.views.data();
    create.width = area.width;
    create.height = area.height;
    create.layers = layers ? layers : 1;

    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    if (state->create_framebuffer(state->device, &create, nullptr, &framebuffer) != VK_SUCCESS) {
        return VK_NULL_HANDLE;
    }
    entry.framebuffers.emplace(hash, framebuffer);
    return framebuffer;
}

}  // namespace shim

// ------------------------------------------------------------- image views ---

extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkCreateImageView(VkDevice device,
                                                            const VkImageViewCreateInfo* info,
                                                            const VkAllocationCallbacks* alloc,
                                                            VkImageView* out) {
    auto create = reinterpret_cast<PFN_vkCreateImageView>(
        shim::loader().device_proc(device, "vkCreateImageView"));
    if (!create) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult result = create(device, info, alloc, out);
    if (result == VK_SUCCESS) shim::register_view(*out, *info);
    return result;
}

extern "C" VKAPI_ATTR void VKAPI_CALL vkDestroyImageView(VkDevice device, VkImageView view,
                                                         const VkAllocationCallbacks* alloc) {
    shim::unregister_view(view);
    if (auto destroy = reinterpret_cast<PFN_vkDestroyImageView>(
            shim::loader().device_proc(device, "vkDestroyImageView"))) {
        destroy(device, view, alloc);
    }
}

// -------------------------------------------------------- dynamic rendering --

extern "C" VKAPI_ATTR void VKAPI_CALL vkCmdBeginRendering(VkCommandBuffer command_buffer,
                                                          const VkRenderingInfo* info) {
    shim::DeviceState* state = shim::device_of_command_buffer(command_buffer);
    if (!state || !info || !state->cmd_begin_render_pass) return;

    shim::RenderPassKey key;
    key.layer_count = info->layerCount ? info->layerCount : 1;
    key.view_mask = info->viewMask;

    std::vector<VkClearValue> clears;
    clears.reserve(info->colorAttachmentCount + 2);

    for (uint32_t i = 0; i < info->colorAttachmentCount; ++i) {
        const auto& a = info->pColorAttachments[i];
        VkFormat format = VK_FORMAT_UNDEFINED;
        VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
        if (!shim::view_format(a.imageView, &format, &samples)) {
            // Unknown view: no correct description is possible, so the frame is
            // dropped rather than recorded against a wrong render pass.
            return;
        }
        key.color.push_back({format, samples, a.imageLayout, a.loadOp, a.storeOp});
        clears.push_back(a.clearValue);
    }
    if (info->pDepthAttachment && info->pDepthAttachment->imageView != VK_NULL_HANDLE) {
        VkFormat format = VK_FORMAT_UNDEFINED;
        VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
        if (!shim::view_format(info->pDepthAttachment->imageView, &format, &samples)) return;
        key.depth = {format, samples, info->pDepthAttachment->imageLayout,
                     info->pDepthAttachment->loadOp, info->pDepthAttachment->storeOp};
        key.has_depth = true;
        clears.push_back(info->pDepthAttachment->clearValue);
    }
    if (info->pStencilAttachment && info->pStencilAttachment->imageView != VK_NULL_HANDLE) {
        VkFormat format = VK_FORMAT_UNDEFINED;
        VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
        if (!shim::view_format(info->pStencilAttachment->imageView, &format, &samples)) return;
        key.stencil = {format, samples, info->pStencilAttachment->imageLayout,
                       info->pStencilAttachment->loadOp, info->pStencilAttachment->storeOp};
        key.has_stencil = true;
    }

    VkRenderPass pass = VK_NULL_HANDLE;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    {
        std::lock_guard<std::mutex> guard(state->lock);
        auto it = state->render_passes.find(key);
        if (it == state->render_passes.end()) {
            shim::RenderPassEntry entry;
            pass = shim::build_pass_locked(state, *info, key, &entry);
            if (pass == VK_NULL_HANDLE) return;
            framebuffer =
                shim::framebuffer_locked(state, entry, info->renderArea.extent, key.layer_count);
            if (framebuffer == VK_NULL_HANDLE) return;
            state->render_passes.emplace(key, std::move(entry));
        } else {
            pass = it->second.pass;
            framebuffer = shim::framebuffer_locked(state, it->second, info->renderArea.extent,
                                                  key.layer_count);
        }
    }
    if (framebuffer == VK_NULL_HANDLE) return;

    VkRenderPassBeginInfo begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    begin.renderPass = pass;
    begin.framebuffer = framebuffer;
    begin.renderArea = info->renderArea;
    begin.clearValueCount = static_cast<uint32_t>(clears.size());
    begin.pClearValues = clears.empty() ? nullptr : clears.data();
    state->cmd_begin_render_pass(command_buffer, &begin, VK_SUBPASS_CONTENTS_INLINE);
}

extern "C" VKAPI_ATTR void VKAPI_CALL vkCmdEndRendering(VkCommandBuffer command_buffer) {
    shim::DeviceState* state = shim::device_of_command_buffer(command_buffer);
    if (!state || !state->cmd_end_render_pass) return;
    state->cmd_end_render_pass(command_buffer);
}

// --------------------------------------------------------- command buffers ---

extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkAllocateCommandBuffers(
    VkDevice device, const VkCommandBufferAllocateInfo* info, VkCommandBuffer* buffers) {
    auto allocate = reinterpret_cast<PFN_vkAllocateCommandBuffers>(
        shim::loader().device_proc(device, "vkAllocateCommandBuffers"));
    if (!allocate) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult result = allocate(device, info, buffers);
    if (result != VK_SUCCESS) return result;
    shim::DeviceState* state = shim::state_of(device);
    for (uint32_t i = 0; i < info->commandBufferCount; ++i) {
        shim::register_command_buffer(buffers[i], state);
    }
    return VK_SUCCESS;
}

extern "C" VKAPI_ATTR void VKAPI_CALL vkFreeCommandBuffers(VkDevice device, VkCommandPool pool,
                                                           uint32_t count,
                                                           const VkCommandBuffer* buffers) {
    for (uint32_t i = 0; i < count; ++i) {
        shim::forget_command_buffer(buffers[i]);
        shim::push_forget_command_buffer(buffers[i]);
    }
    if (auto free_buffers = reinterpret_cast<PFN_vkFreeCommandBuffers>(
            shim::loader().device_proc(device, "vkFreeCommandBuffers"))) {
        free_buffers(device, pool, count, buffers);
    }
}
