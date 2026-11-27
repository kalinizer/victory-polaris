// Push descriptors on top of ordinary ones.
//
// vkd3d-proton hard-requires VK_KHR_push_descriptor (device.c: "required for
// correct operation"), and the 2021 AMD driver on this machine does not have
// it. The extension is advertised by the shim and implemented here:
//
//   * vkCreateDescriptorSetLayout clears the push flag and remembers the
//     layout's bindings (they give the descriptor-pool sizes).
//   * vkCreatePipelineLayout remembers where each set layout sits, because a
//     push names a pipeline layout plus an index, not a set layout.
//   * vkCmdPushDescriptorSetKHR allocates a set from a pool owned by the
//     recording command buffer, writes it, and binds it at that index.
//   * ...WithTemplateKHR expands the template entries into the same kind of
//     write, reading the caller's blob at the template's offsets.
//
// Sets are never updated in place: once a command buffer is submitted, its
// sets are in use. Each push takes a fresh set, and the pools are reset in
// vkBeginCommandBuffer, when nothing can be in flight.
#include "shim.h"

#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace shim {

struct LayoutInfo {
    bool push = false;
    std::vector<VkDescriptorSetLayoutBinding> bindings;
    std::vector<VkDescriptorPoolSize> pool_sizes;
};

struct TemplateInfo {
    VkDescriptorSetLayout layout = VK_NULL_HANDLE;
    VkPipelineBindPoint bind_point = VK_PIPELINE_BIND_POINT_GRAPHICS;
    std::vector<VkDescriptorUpdateTemplateEntry> entries;
};

struct PipelineLayoutInfo {
    std::vector<VkDescriptorSetLayout> sets;
};

std::mutex g_push_lock;
std::unordered_map<VkDescriptorSetLayout, LayoutInfo> g_layouts;
std::unordered_map<VkDescriptorUpdateTemplate, TemplateInfo> g_templates;
std::unordered_map<VkPipelineLayout, PipelineLayoutInfo> g_pipeline_layouts;
std::unordered_map<VkCommandBuffer, std::vector<VkDescriptorPool>> g_cmd_pools;

template <typename T>
T device_fn(VkDevice device, const char* name) {
    return reinterpret_cast<T>(loader().device_proc(device, name));
}

VkDescriptorPool make_pool(DeviceState* state, const LayoutInfo& layout, uint32_t sets) {
    std::vector<VkDescriptorPoolSize> sizes;
    sizes.reserve(layout.pool_sizes.size());
    for (const auto& base : layout.pool_sizes) {
        VkDescriptorPoolSize s = base;
        s.descriptorCount = base.descriptorCount * sets;
        sizes.push_back(s);
    }
    if (sizes.empty()) return VK_NULL_HANDLE;

    VkDescriptorPoolCreateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    info.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    info.maxSets = sets;
    info.poolSizeCount = static_cast<uint32_t>(sizes.size());
    info.pPoolSizes = sizes.data();

    auto create = device_fn<PFN_vkCreateDescriptorPool>(state->device, "vkCreateDescriptorPool");
    VkDescriptorPool pool = VK_NULL_HANDLE;
    if (!create || create(state->device, &info, nullptr, &pool) != VK_SUCCESS) return VK_NULL_HANDLE;
    return pool;
}

// g_push_lock must be held.
VkDescriptorSet allocate_set_locked(DeviceState* state, VkCommandBuffer cmd,
                                    VkDescriptorSetLayout layout) {
    auto layout_it = g_layouts.find(layout);
    if (layout_it == g_layouts.end()) return VK_NULL_HANDLE;
    const LayoutInfo& info = layout_it->second;

    auto allocate = device_fn<PFN_vkAllocateDescriptorSets>(state->device, "vkAllocateDescriptorSets");
    if (!allocate) return VK_NULL_HANDLE;

    auto& pools = g_cmd_pools[cmd];
    for (int attempt = 0; attempt < 2; ++attempt) {
        if (pools.empty() || attempt == 1) {
            VkDescriptorPool pool = make_pool(state, info, 64);
            if (pool == VK_NULL_HANDLE) return VK_NULL_HANDLE;
            pools.push_back(pool);
        }
        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool = pools.back();
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &layout;
        VkDescriptorSet set = VK_NULL_HANDLE;
        if (allocate(state->device, &ai, &set) == VK_SUCCESS) return set;
    }
    return VK_NULL_HANDLE;
}

size_t descriptor_size(VkDescriptorType type) {
    switch (type) {
        case VK_DESCRIPTOR_TYPE_SAMPLER:
        case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
        case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
        case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
        case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:
            return sizeof(VkDescriptorImageInfo);
        case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
        case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
            return sizeof(VkBufferView);
        case VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR:
            return sizeof(VkAccelerationStructureKHR);
        default:
            return sizeof(VkDescriptorBufferInfo);
    }
}

bool layout_for_set(VkPipelineLayout pipeline_layout, uint32_t set, VkDescriptorSetLayout* out) {
    auto it = g_pipeline_layouts.find(pipeline_layout);
    if (it == g_pipeline_layouts.end() || set >= it->second.sets.size()) return false;
    *out = it->second.sets[set];
    return *out != VK_NULL_HANDLE;
}

void bind_one(DeviceState* state, VkCommandBuffer cmd, VkPipelineBindPoint point,
              VkPipelineLayout pipeline_layout, uint32_t set, VkDescriptorSet handle) {
    auto bind = device_fn<PFN_vkCmdBindDescriptorSets>(state->device, "vkCmdBindDescriptorSets");
    if (bind) bind(cmd, point, pipeline_layout, set, 1, &handle, 0, nullptr);
}

void push_forget_command_buffer(VkCommandBuffer cmd) {
    std::lock_guard<std::mutex> guard(g_push_lock);
    g_cmd_pools.erase(cmd);
}

void push_reset_pools(VkCommandBuffer cmd) {
    DeviceState* state = device_of_command_buffer(cmd);
    if (!state) return;
    auto reset = device_fn<PFN_vkResetDescriptorPool>(state->device, "vkResetDescriptorPool");
    std::lock_guard<std::mutex> guard(g_push_lock);
    auto it = g_cmd_pools.find(cmd);
    if (it == g_cmd_pools.end() || !reset) return;
    for (VkDescriptorPool pool : it->second) {
        reset(state->device, pool, VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT);
    }
}

void push_plain(DeviceState* state, VkCommandBuffer cmd, VkPipelineBindPoint point,
                VkPipelineLayout pipeline_layout, uint32_t set, uint32_t write_count,
                const VkWriteDescriptorSet* writes) {
    VkDescriptorSet handle = VK_NULL_HANDLE;
    {
        std::lock_guard<std::mutex> guard(g_push_lock);
        VkDescriptorSetLayout layout = VK_NULL_HANDLE;
        if (!layout_for_set(pipeline_layout, set, &layout)) return;
        handle = allocate_set_locked(state, cmd, layout);
    }
    if (handle == VK_NULL_HANDLE) return;

    std::vector<VkWriteDescriptorSet> copies(writes, writes + write_count);
    for (auto& w : copies) w.dstSet = handle;

    if (auto update = device_fn<PFN_vkUpdateDescriptorSets>(state->device, "vkUpdateDescriptorSets"))
        update(state->device, static_cast<uint32_t>(copies.size()), copies.data(), 0, nullptr);
    bind_one(state, cmd, point, pipeline_layout, set, handle);
}

void push_with_template(DeviceState* state, VkCommandBuffer cmd,
                        VkDescriptorUpdateTemplate update_template,
                        VkPipelineLayout pipeline_layout, uint32_t set, const void* data) {
    TemplateInfo tmpl;
    {
        std::lock_guard<std::mutex> guard(g_push_lock);
        auto it = g_templates.find(update_template);
        if (it == g_templates.end()) return;
        tmpl = it->second;
    }

    VkDescriptorSet handle = VK_NULL_HANDLE;
    {
        std::lock_guard<std::mutex> guard(g_push_lock);
        handle = allocate_set_locked(state, cmd, tmpl.layout);
    }
    if (handle == VK_NULL_HANDLE || !data) return;

    // Expand the template: one write per entry, each pointing at a copy of the
    // blob slice the entry describes.
    std::vector<std::vector<unsigned char>> storage;
    storage.reserve(tmpl.entries.size());
    std::vector<VkWriteDescriptorSet> writes;
    std::vector<VkWriteDescriptorSetInlineUniformBlock> inline_blocks;
    writes.reserve(tmpl.entries.size());

    for (const auto& e : tmpl.entries) {
        const size_t element = descriptor_size(e.descriptorType);
        const size_t bytes = element * e.descriptorCount;
        storage.emplace_back(bytes);
        unsigned char* dst = storage.back().data();
        const auto* src = static_cast<const unsigned char*>(data);
        for (uint32_t i = 0; i < e.descriptorCount; ++i) {
            std::memcpy(dst + i * element, src + e.offset + i * e.stride, element);
        }

        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = handle;
        w.dstBinding = e.dstBinding;
        w.dstArrayElement = e.dstArrayElement;
        w.descriptorCount = e.descriptorCount;
        w.descriptorType = e.descriptorType;
        switch (e.descriptorType) {
            case VK_DESCRIPTOR_TYPE_SAMPLER:
            case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
            case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
            case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
            case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:
                w.pImageInfo = reinterpret_cast<const VkDescriptorImageInfo*>(storage.back().data());
                break;
            case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
            case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
                w.pTexelBufferView = reinterpret_cast<const VkBufferView*>(storage.back().data());
                break;
            default:
                w.pBufferInfo =
                    reinterpret_cast<const VkDescriptorBufferInfo*>(storage.back().data());
                break;
        }
        writes.push_back(w);
    }

    if (auto update = device_fn<PFN_vkUpdateDescriptorSets>(state->device, "vkUpdateDescriptorSets")) {
        update(state->device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
    }
    bind_one(state, cmd, tmpl.bind_point, pipeline_layout, set, handle);
}

}  // namespace shim

// ------------------------------------------------------------ set layouts ---

extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkCreateDescriptorSetLayout(
    VkDevice device, const VkDescriptorSetLayoutCreateInfo* info,
    const VkAllocationCallbacks* alloc, VkDescriptorSetLayout* out) {
    auto create = reinterpret_cast<PFN_vkCreateDescriptorSetLayout>(
        shim::loader().device_proc(device, "vkCreateDescriptorSetLayout"));
    if (!create) return VK_ERROR_INITIALIZATION_FAILED;

    VkDescriptorSetLayoutCreateInfo filtered = *info;
    const bool push =
        (info->flags & VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR) != 0;
    filtered.flags &= ~static_cast<VkDescriptorSetLayoutCreateFlags>(
        VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR);

    VkResult result = create(device, &filtered, alloc, out);
    if (result != VK_SUCCESS) return result;

    shim::LayoutInfo record;
    record.push = push;
    record.bindings.assign(info->pBindings, info->pBindings + info->bindingCount);
    for (const auto& b : record.bindings) {
        bool found = false;
        for (auto& size : record.pool_sizes) {
            if (size.type == b.descriptorType) {
                size.descriptorCount += b.descriptorCount;
                found = true;
                break;
            }
        }
        if (!found) record.pool_sizes.push_back({b.descriptorType, b.descriptorCount});
    }
    std::lock_guard<std::mutex> guard(shim::g_push_lock);
    shim::g_layouts[*out] = std::move(record);
    return VK_SUCCESS;
}

extern "C" VKAPI_ATTR void VKAPI_CALL vkDestroyDescriptorSetLayout(
    VkDevice device, VkDescriptorSetLayout layout, const VkAllocationCallbacks* alloc) {
    {
        std::lock_guard<std::mutex> guard(shim::g_push_lock);
        shim::g_layouts.erase(layout);
    }
    if (auto destroy = reinterpret_cast<PFN_vkDestroyDescriptorSetLayout>(
            shim::loader().device_proc(device, "vkDestroyDescriptorSetLayout"))) {
        destroy(device, layout, alloc);
    }
}

// --------------------------------------------------------- pipeline layout --

extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkCreatePipelineLayout(
    VkDevice device, const VkPipelineLayoutCreateInfo* info, const VkAllocationCallbacks* alloc,
    VkPipelineLayout* out) {
    auto create = reinterpret_cast<PFN_vkCreatePipelineLayout>(
        shim::loader().device_proc(device, "vkCreatePipelineLayout"));
    if (!create) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult result = create(device, info, alloc, out);
    if (result != VK_SUCCESS) return result;

    shim::PipelineLayoutInfo record;
    if (info->setLayoutCount && info->pSetLayouts) {
        record.sets.assign(info->pSetLayouts, info->pSetLayouts + info->setLayoutCount);
    }
    std::lock_guard<std::mutex> guard(shim::g_push_lock);
    shim::g_pipeline_layouts[*out] = std::move(record);
    return VK_SUCCESS;
}

extern "C" VKAPI_ATTR void VKAPI_CALL vkDestroyPipelineLayout(VkDevice device,
                                                             VkPipelineLayout layout,
                                                             const VkAllocationCallbacks* alloc) {
    {
        std::lock_guard<std::mutex> guard(shim::g_push_lock);
        shim::g_pipeline_layouts.erase(layout);
    }
    if (auto destroy = reinterpret_cast<PFN_vkDestroyPipelineLayout>(
            shim::loader().device_proc(device, "vkDestroyPipelineLayout"))) {
        destroy(device, layout, alloc);
    }
}

// -------------------------------------------------------------- templates ---

extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkCreateDescriptorUpdateTemplate(
    VkDevice device, const VkDescriptorUpdateTemplateCreateInfo* info,
    const VkAllocationCallbacks* alloc, VkDescriptorUpdateTemplate* out) {
    auto create = reinterpret_cast<PFN_vkCreateDescriptorUpdateTemplate>(
        shim::loader().device_proc(device, "vkCreateDescriptorUpdateTemplate"));
    if (!create) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult result = create(device, info, alloc, out);
    if (result != VK_SUCCESS) return result;

    shim::TemplateInfo record;
    record.layout = info->descriptorSetLayout;
    record.bind_point = info->pipelineBindPoint;
    record.entries.assign(info->pDescriptorUpdateEntries,
                          info->pDescriptorUpdateEntries + info->descriptorUpdateEntryCount);
    std::lock_guard<std::mutex> guard(shim::g_push_lock);
    shim::g_templates[*out] = std::move(record);
    return VK_SUCCESS;
}

extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkCreateDescriptorUpdateTemplateKHR(
    VkDevice device, const VkDescriptorUpdateTemplateCreateInfo* info,
    const VkAllocationCallbacks* alloc, VkDescriptorUpdateTemplate* out) {
    return vkCreateDescriptorUpdateTemplate(device, info, alloc, out);
}

extern "C" VKAPI_ATTR void VKAPI_CALL vkDestroyDescriptorUpdateTemplate(
    VkDevice device, VkDescriptorUpdateTemplate handle, const VkAllocationCallbacks* alloc) {
    {
        std::lock_guard<std::mutex> guard(shim::g_push_lock);
        shim::g_templates.erase(handle);
    }
    if (auto destroy = reinterpret_cast<PFN_vkDestroyDescriptorUpdateTemplate>(
            shim::loader().device_proc(device, "vkDestroyDescriptorUpdateTemplate"))) {
        destroy(device, handle, alloc);
    }
}

extern "C" VKAPI_ATTR void VKAPI_CALL vkDestroyDescriptorUpdateTemplateKHR(
    VkDevice device, VkDescriptorUpdateTemplate handle, const VkAllocationCallbacks* alloc) {
    vkDestroyDescriptorUpdateTemplate(device, handle, alloc);
}

// --------------------------------------------------------------- recording --

extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkBeginCommandBuffer(
    VkCommandBuffer cmd, const VkCommandBufferBeginInfo* info) {
    shim::DeviceState* state = shim::device_of_command_buffer(cmd);
    if (!state) return VK_ERROR_INITIALIZATION_FAILED;
    auto begin = reinterpret_cast<PFN_vkBeginCommandBuffer>(
        shim::loader().device_proc(state->device, "vkBeginCommandBuffer"));
    if (!begin) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult result = begin(cmd, info);
    if (result == VK_SUCCESS) shim::push_reset_pools(cmd);
    return result;
}

extern "C" VKAPI_ATTR void VKAPI_CALL vkCmdPushDescriptorSetKHR(
    VkCommandBuffer cmd, VkPipelineBindPoint bind_point, VkPipelineLayout pipeline_layout,
    uint32_t set, uint32_t write_count, const VkWriteDescriptorSet* writes) {
    shim::DeviceState* state = shim::device_of_command_buffer(cmd);
    if (!state) return;
    shim::push_plain(state, cmd, bind_point, pipeline_layout, set, write_count, writes);
}

extern "C" VKAPI_ATTR void VKAPI_CALL vkCmdPushDescriptorSetWithTemplateKHR(
    VkCommandBuffer cmd, VkDescriptorUpdateTemplate update_template,
    VkPipelineLayout pipeline_layout, uint32_t set, const void* data) {
    shim::DeviceState* state = shim::device_of_command_buffer(cmd);
    if (!state) return;
    shim::push_with_template(state, cmd, update_template, pipeline_layout, set, data);
}
