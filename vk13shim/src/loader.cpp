#include "shim.h"

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>

namespace shim {
namespace {

std::mutex g_lock;
std::unordered_map<VkInstance, InstanceState> g_instances;
std::unordered_map<VkDevice, DeviceState*> g_devices;
std::unordered_map<VkPhysicalDevice, VkInstance> g_physical_instance;
std::unordered_map<VkCommandBuffer, DeviceState*> g_command_devices;
static VkInstance g_any_instance = VK_NULL_HANDLE;

constexpr const char* kFakeExtensions[] = {
    VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME,
    VK_KHR_MAINTENANCE_4_EXTENSION_NAME,
    // Required by vkd3d-proton before it will look at the alignment properties.
    VK_EXT_TEXEL_BUFFER_ALIGNMENT_EXTENSION_NAME,
    VK_EXT_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME,
    VK_EXT_TRANSFORM_FEEDBACK_EXTENSION_NAME,
    VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME,
    VK_KHR_MAINTENANCE_5_EXTENSION_NAME,
    VK_KHR_MAINTENANCE_6_EXTENSION_NAME,
};

template <typename T>
T* find_in_chain(void* head, VkStructureType type) {
    auto* base = static_cast<VkBaseOutStructure*>(head);
    while (base) {
        if (base->sType == type) return reinterpret_cast<T*>(base);
        base = base->pNext;
    }
    return nullptr;
}

}  // namespace

bool is_fake_extension_name(const char* name) {
    for (const char* fake : kFakeExtensions) {
        if (std::strcmp(name, fake) == 0) return true;
    }
    return false;
}

RealLoader& loader() {
    static RealLoader real;
    return real;
}

// A function in this DLL, whose address names our own module.
static void module_anchor() {}

bool RealLoader::open() {
    if (module) return true;

    // The untouched Khronos loader ships beside this DLL as vulkan-1-real.dll.
    // It has to be a different name, or the forwarders in vulkan13.def would
    // resolve back to us.
    HMODULE self = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&module_anchor), &self)) {
        return false;
    }
    wchar_t path[MAX_PATH]{};
    DWORD length = GetModuleFileNameW(self, path, MAX_PATH);
    if (length == 0 || length == MAX_PATH) return false;

    // Replace the file name, keep the directory.
    wchar_t* slash = wcsrchr(path, L'\\');
    if (!slash) return false;
    const wchar_t* loader_name = L"vulkan_loader_real.dll";
    if (static_cast<size_t>(path + MAX_PATH - slash - 1) < wcslen(loader_name) + 1) return false;
    wcscpy_s(slash + 1, MAX_PATH - (slash + 1 - path), loader_name);

    module = LoadLibraryExW(path, nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR |
                                                 LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!module) return false;
    get_instance_proc_addr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
        GetProcAddress(module, "vkGetInstanceProcAddr"));
    get_device_proc_addr =
        reinterpret_cast<PFN_vkGetDeviceProcAddr>(GetProcAddress(module, "vkGetDeviceProcAddr"));
    return get_instance_proc_addr && get_device_proc_addr;
}

// Applications resolve global entry points before they ever create an
// instance, so the real loader has to be open by then.
PFN_vkVoidFunction RealLoader::instance_proc(VkInstance instance, const char* name) const {
    if (!module) const_cast<RealLoader*>(this)->open();
    return get_instance_proc_addr ? get_instance_proc_addr(instance, name) : nullptr;
}

PFN_vkVoidFunction RealLoader::device_proc(VkDevice device, const char* name) const {
    if (!module) const_cast<RealLoader*>(this)->open();
    return get_device_proc_addr ? get_device_proc_addr(device, name) : nullptr;
}

DeviceState* state_of(VkDevice device) {
    std::lock_guard<std::mutex> guard(g_lock);
    auto it = g_devices.find(device);
    return it == g_devices.end() ? nullptr : it->second;
}

void forget_device(VkDevice device) {
    DeviceState* state = nullptr;
    {
        std::lock_guard<std::mutex> guard(g_lock);
        auto it = g_devices.find(device);
        if (it == g_devices.end()) return;
        state = it->second;
        g_devices.erase(it);
    }
    for (auto& entry : state->render_passes) {
        for (auto& fb : entry.second.framebuffers) {
            if (state->destroy_framebuffer) state->destroy_framebuffer(device, fb.second, nullptr);
        }
        if (state->destroy_render_pass) state->destroy_render_pass(device, entry.second.pass, nullptr);
    }
    delete state;
}

VkInstance any_instance() {
    std::lock_guard<std::mutex> guard(g_lock);
    return g_any_instance;
}

VkInstance instance_of_physical(VkPhysicalDevice physical) {
    std::lock_guard<std::mutex> guard(g_lock);
    auto it = g_physical_instance.find(physical);
    return it == g_physical_instance.end() ? VK_NULL_HANDLE : it->second;
}

void register_command_buffer(VkCommandBuffer buffer, DeviceState* state) {
    std::lock_guard<std::mutex> guard(g_lock);
    g_command_devices[buffer] = state;
}

void forget_command_buffer(VkCommandBuffer buffer) {
    std::lock_guard<std::mutex> guard(g_lock);
    g_command_devices.erase(buffer);
}

DeviceState* device_of_command_buffer(VkCommandBuffer buffer) {
    std::lock_guard<std::mutex> guard(g_lock);
    auto it = g_command_devices.find(buffer);
    return it == g_command_devices.end() ? nullptr : it->second;
}

InstanceState* instance_state_of(VkInstance instance) {
    std::lock_guard<std::mutex> guard(g_lock);
    auto it = g_instances.find(instance);
    return it == g_instances.end() ? nullptr : &it->second;
}

}  // namespace shim

// ---------------------------------------------------------------- exports ----

using namespace shim;

extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkCreateInstance(const VkInstanceCreateInfo*,
                                                           const VkAllocationCallbacks*,
                                                           VkInstance*);
extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkCreateDevice(VkPhysicalDevice,
                                                         const VkDeviceCreateInfo*,
                                                         const VkAllocationCallbacks*, VkDevice*);
extern "C" VKAPI_ATTR void VKAPI_CALL vkDestroyDevice(VkDevice, const VkAllocationCallbacks*);
extern "C" VKAPI_ATTR void VKAPI_CALL vkDestroyInstance(VkInstance,
                                                        const VkAllocationCallbacks*);

extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateInstanceVersion(uint32_t* api_version) {
    auto real = reinterpret_cast<PFN_vkEnumerateInstanceVersion>(
        loader().instance_proc(VK_NULL_HANDLE, "vkEnumerateInstanceVersion"));
    VkResult result = real ? real(api_version) : VK_SUCCESS;
    if (result == VK_SUCCESS && api_version && *api_version < VK_API_VERSION_1_3) {
        *api_version = VK_API_VERSION_1_3;
    }
    return result;
}

extern "C" VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance, const char*);
extern "C" VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice, const char*);
extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateDeviceExtensionProperties(
    VkPhysicalDevice, const char*, uint32_t*, VkExtensionProperties*);
namespace {

// vkd3d-proton derives the D3D12 feature level from a few limits, and AMD's
// 1.2 driver reports the D3D11-era values (8) for the per-stage UAV counts.
// D3D12_UAV_SLOT_COUNT is 64, so without this the device caps out at 11_0 and
// D3D12CreateDevice(12_0) is refused before anything is drawn.
void raise_d3d12_limits(VkPhysicalDeviceLimits& limits) {
    auto at_least = [](uint32_t& value, uint32_t want) {
        if (value < want) value = want;
    };
    at_least(limits.maxPerStageDescriptorStorageBuffers, 64);
    at_least(limits.maxPerStageDescriptorStorageImages, 64);
    at_least(limits.maxPerStageDescriptorSampledImages, 128);
    at_least(limits.maxPerStageDescriptorSamplers, 64);
    at_least(limits.maxDescriptorSetStorageBuffers, 64);
    at_least(limits.maxDescriptorSetStorageImages, 64);
}

}  // namespace



// D3D12's TypedUAVLoadAdditionalFormats (one of the 12_0 conditions) is read
// from the format support of a fixed list. AMD's driver reports the storage
// capability conservatively for some of them.

// vkd3d-proton asks for the KHR spellings of these (they were extensions when
// it started), so the core-name overrides above never ran for it.
extern "C" VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceProperties2KHR(
    VkPhysicalDevice physical, VkPhysicalDeviceProperties2* properties) {
    vkGetPhysicalDeviceProperties2(physical, properties);
}

extern "C" VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceFeatures2KHR(
    VkPhysicalDevice physical, VkPhysicalDeviceFeatures2* features) {
    vkGetPhysicalDeviceFeatures2(physical, features);
}

extern "C" VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceFormatProperties2(
    VkPhysicalDevice physical, VkFormat format, VkFormatProperties2* properties) {
    if (!properties) return;
    vkGetPhysicalDeviceFormatProperties(physical, format, &properties->formatProperties);
}

extern "C" VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceFormatProperties2KHR(
    VkPhysicalDevice physical, VkFormat format, VkFormatProperties2* properties) {
    vkGetPhysicalDeviceFormatProperties2(physical, format, properties);
}

extern "C" VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceQueueFamilyProperties2(
    VkPhysicalDevice physical, uint32_t* count, VkQueueFamilyProperties2* properties) {
    auto real = reinterpret_cast<PFN_vkGetPhysicalDeviceQueueFamilyProperties2>(
        loader().instance_proc(instance_of_physical(physical), "vkGetPhysicalDeviceQueueFamilyProperties2"));
    if (!real) return;
    real(physical, count, properties);
    if (!properties || !count || *count == 0) return;
    properties[0].queueFamilyProperties.queueFlags |= VK_QUEUE_SPARSE_BINDING_BIT;
}

extern "C" VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceQueueFamilyProperties2KHR(
    VkPhysicalDevice physical, uint32_t* count, VkQueueFamilyProperties2* properties) {
    vkGetPhysicalDeviceQueueFamilyProperties2(physical, count, properties);
}

extern "C" VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceFormatProperties(
    VkPhysicalDevice physical, VkFormat format, VkFormatProperties* properties) {
    auto real = reinterpret_cast<PFN_vkGetPhysicalDeviceFormatProperties>(
        loader().instance_proc(instance_of_physical(physical), "vkGetPhysicalDeviceFormatProperties"));
    if (!real) return;
    real(physical, format, properties);
    if (!properties) return;
    if (properties->optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) {
        properties->optimalTilingFeatures |= VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT;
    }
}

extern "C" VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceQueueFamilyProperties(
    VkPhysicalDevice physical, uint32_t* count, VkQueueFamilyProperties* properties) {
    auto real = reinterpret_cast<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(
        loader().instance_proc(instance_of_physical(physical), "vkGetPhysicalDeviceQueueFamilyProperties"));
    if (!real) return;
    real(physical, count, properties);
    if (!properties || !count || *count == 0) return;
    // vkd3d looks for a sparse-binding family to decide the tiled-resources
    // tier. The driver has one queue family for everything; mark it.
    properties[0].queueFlags |= VK_QUEUE_SPARSE_BINDING_BIT;
}

extern "C" VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceProperties(VkPhysicalDevice,
                                                                    VkPhysicalDeviceProperties*);
extern "C" VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceProperties2(VkPhysicalDevice,
                                                                     VkPhysicalDeviceProperties2*);
extern "C" VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceFeatures2(VkPhysicalDevice,
                                                                   VkPhysicalDeviceFeatures2*);

extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkCreateInstance(const VkInstanceCreateInfo* info,
                                                           const VkAllocationCallbacks* alloc,
                                                           VkInstance* out) {
    if (!loader().open()) return VK_ERROR_INITIALIZATION_FAILED;
    auto create = loader().fn<PFN_vkCreateInstance>("vkCreateInstance");
    if (!create) return VK_ERROR_INITIALIZATION_FAILED;

    // VICTORY_VK_VALIDATION=1 puts the Khronos validation layer on the
    // instance, which is the only way to hear why a driver refuses a shader.
    std::vector<const char*> layers;
    std::vector<const char*> extensions;
    VkInstanceCreateInfo augmented = *info;
    if (const char* want = std::getenv("VICTORY_VK_VALIDATION"); want && want[0] == '1') {
        if (info->ppEnabledLayerNames) {
            layers.assign(info->ppEnabledLayerNames,
                          info->ppEnabledLayerNames + info->enabledLayerCount);
        }
        layers.push_back("VK_LAYER_KHRONOS_validation");
        if (info->ppEnabledExtensionNames) {
            extensions.assign(info->ppEnabledExtensionNames,
                              info->ppEnabledExtensionNames + info->enabledExtensionCount);
        }
        augmented.enabledLayerCount = static_cast<uint32_t>(layers.size());
        augmented.ppEnabledLayerNames = layers.data();
        augmented.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
        augmented.ppEnabledExtensionNames = extensions.empty() ? nullptr : extensions.data();
    }

    VkResult result = create(&augmented, alloc, out);
    if (result != VK_SUCCESS) return result;

    InstanceState state;
    state.instance = *out;
    state.real_enumerate_device_extensions = reinterpret_cast<PFN_vkEnumerateDeviceExtensionProperties>(
        loader().instance_proc(*out, "vkEnumerateDeviceExtensionProperties"));
    state.real_physical_properties = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(
        loader().instance_proc(*out, "vkGetPhysicalDeviceProperties"));
    state.real_physical_properties2 = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(
        loader().instance_proc(*out, "vkGetPhysicalDeviceProperties2"));
    state.real_physical_features2 = reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(
        loader().instance_proc(*out, "vkGetPhysicalDeviceFeatures2"));
    {
        std::lock_guard<std::mutex> guard(g_lock);
        g_instances[*out] = state;
        if (g_any_instance == VK_NULL_HANDLE) g_any_instance = *out;
    }
    return VK_SUCCESS;
}

extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkCreateDevice(VkPhysicalDevice physical,
                                                         const VkDeviceCreateInfo* info,
                                                         const VkAllocationCallbacks* alloc,
                                                         VkDevice* out) {
    // Strip the extensions we advertise but the driver does not implement.
    std::vector<const char*> passed;
    passed.reserve(info->enabledExtensionCount);
    for (uint32_t i = 0; i < info->enabledExtensionCount; ++i) {
        if (!is_fake_extension_name(info->ppEnabledExtensionNames[i])) {
            passed.push_back(info->ppEnabledExtensionNames[i]);
        }
    }
    // Told that the API is 1.3, an application enables no KHR extension names
    // for features that are core there. The driver here is 1.2, so those names
    // have to be enabled on its behalf or the entry points come back NULL.
    if (InstanceState* instance = instance_state_of(instance_of_physical(physical))) {
        if (instance->real_enumerate_device_extensions) {
            uint32_t count = 0;
            instance->real_enumerate_device_extensions(physical, nullptr, &count, nullptr);
            std::vector<VkExtensionProperties> available(count);
            if (count && instance->real_enumerate_device_extensions(
                             physical, nullptr, &count, available.data()) == VK_SUCCESS) {
                const char* promoted[] = {VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME,
                                          VK_KHR_COPY_COMMANDS_2_EXTENSION_NAME,
                                          VK_EXT_EXTENDED_DYNAMIC_STATE_EXTENSION_NAME,
                                          VK_EXT_EXTENDED_DYNAMIC_STATE_2_EXTENSION_NAME,
                                          VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME};
                for (const char* want : promoted) {
                    bool in_driver = false;
                    for (const auto& e : available) {
                        if (std::strcmp(e.extensionName, want) == 0) in_driver = true;
                    }
                    bool enabled = false;
                    for (const char* have : passed) {
                        if (std::strcmp(have, want) == 0) enabled = true;
                    }
                    if (in_driver && !enabled) passed.push_back(want);
                }
            }
        }
    }

    VkDeviceCreateInfo filtered = *info;
    filtered.enabledExtensionCount = static_cast<uint32_t>(passed.size());
    filtered.ppEnabledExtensionNames = passed.empty() ? nullptr : passed.data();

    // Every feature struct the shim answers for is ours to honour: the driver
    // does not know these structures (or does not support them), so they come
    // off the chain for the duration of the call and go back afterwards.
    const VkStructureType ours[] = {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES_KHR,
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_6_FEATURES_KHR,
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_FEATURES_EXT,
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_FEATURES_EXT,
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TEXEL_BUFFER_ALIGNMENT_FEATURES_EXT,
    };
    struct Unlinked {
        VkBaseOutStructure* prev;
        VkBaseOutStructure* node;
    };
    std::vector<Unlinked> unlinked;
    VkBaseOutStructure* prev = nullptr;
    VkBaseOutStructure* node = reinterpret_cast<VkBaseOutStructure*>(const_cast<void*>(filtered.pNext));
    while (node) {
        bool drop = false;
        for (VkStructureType type : ours) {
            if (node->sType == type) drop = true;
        }
        VkBaseOutStructure* next = node->pNext;
        if (drop) {
            if (prev) {
                prev->pNext = next;
            } else {
                filtered.pNext = next;
            }
            unlinked.push_back({prev, node});
        } else {
            prev = node;
        }
        node = next;
    }

    VkInstance owner = VK_NULL_HANDLE;
    {
        std::lock_guard<std::mutex> guard(g_lock);
        auto it = g_physical_instance.find(physical);
        if (it != g_physical_instance.end()) owner = it->second;
    }
    auto create = reinterpret_cast<PFN_vkCreateDevice>(
        loader().instance_proc(owner, "vkCreateDevice"));
    if (!create) return VK_ERROR_INITIALIZATION_FAILED;
    if (std::getenv("VICTORY_SHIM_DEBUG")) {
        std::fprintf(stderr, "shim: vkCreateDevice with %u extensions:\n", filtered.enabledExtensionCount);
        for (uint32_t i = 0; i < filtered.enabledExtensionCount; ++i)
            std::fprintf(stderr, "  %s\n", filtered.ppEnabledExtensionNames[i]);
    }
    VkResult result = create(physical, &filtered, alloc, out);
    if (result != VK_SUCCESS) {
        std::fprintf(stderr,
                     "vulkan-1 shim: vkCreateDevice -> %d with %u extensions enabled\n",
                     int(result), filtered.enabledExtensionCount);
        for (uint32_t i = 0; i < filtered.enabledExtensionCount; ++i) {
            std::fprintf(stderr, "  ext[%u] = %s\n", i, filtered.ppEnabledExtensionNames[i]);
        }
    }

    // Put the chain back exactly as the caller had it.
    for (auto it = unlinked.rbegin(); it != unlinked.rend(); ++it) {
        if (it->prev) {
            it->prev->pNext = it->node;
        } else {
            filtered.pNext = it->node;
        }
    }
    if (result != VK_SUCCESS) return result;

    auto* state = new DeviceState();
    state->device = *out;
    state->physical = physical;
    state->cmd_begin_render_pass = reinterpret_cast<PFN_vkCmdBeginRenderPass>(
        loader().device_proc(*out, "vkCmdBeginRenderPass"));
    state->cmd_end_render_pass =
        reinterpret_cast<PFN_vkCmdEndRenderPass>(loader().device_proc(*out, "vkCmdEndRenderPass"));
    state->create_render_pass =
        reinterpret_cast<PFN_vkCreateRenderPass>(loader().device_proc(*out, "vkCreateRenderPass"));
    state->destroy_render_pass = reinterpret_cast<PFN_vkDestroyRenderPass>(
        loader().device_proc(*out, "vkDestroyRenderPass"));
    state->create_framebuffer =
        reinterpret_cast<PFN_vkCreateFramebuffer>(loader().device_proc(*out, "vkCreateFramebuffer"));
    state->destroy_framebuffer = reinterpret_cast<PFN_vkDestroyFramebuffer>(
        loader().device_proc(*out, "vkDestroyFramebuffer"));
    state->cmd_pipeline_barrier = reinterpret_cast<PFN_vkCmdPipelineBarrier>(
        loader().device_proc(*out, "vkCmdPipelineBarrier"));
    state->cmd_pipeline_barrier_2 = reinterpret_cast<PFN_vkCmdPipelineBarrier2KHR>(
        loader().device_proc(*out, "vkCmdPipelineBarrier2KHR"));
    state->queue_submit =
        reinterpret_cast<PFN_vkQueueSubmit>(loader().device_proc(*out, "vkQueueSubmit"));
    state->queue_submit_2 = reinterpret_cast<PFN_vkQueueSubmit2KHR>(
        loader().device_proc(*out, "vkQueueSubmit2KHR"));
    state->get_buffer_memory_requirements = reinterpret_cast<PFN_vkGetBufferMemoryRequirements>(
        loader().device_proc(*out, "vkGetBufferMemoryRequirements"));
    state->get_image_memory_requirements = reinterpret_cast<PFN_vkGetImageMemoryRequirements>(
        loader().device_proc(*out, "vkGetImageMemoryRequirements"));

    // Record what the driver really offers before we start claiming things.
    {
        std::lock_guard<std::mutex> guard(g_lock);
        auto it = g_physical_instance.find(physical);
        if (it != g_physical_instance.end()) state->instance = it->second;
    }
    if (InstanceState* instance = instance_state_of(state->instance)) {
        if (instance->real_physical_features2) {
            VkPhysicalDeviceSynchronization2Features sync2{
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES};
            VkPhysicalDeviceVulkan12Features vk12{
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
            vk12.pNext = &sync2;
            VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
            features.pNext = &vk12;
            instance->real_physical_features2(physical, &features);
            state->has_synchronization2 = sync2.synchronization2 == VK_TRUE;
            state->has_descriptor_indexing = vk12.descriptorIndexing == VK_TRUE;
        }
    }

    {
        std::lock_guard<std::mutex> guard(g_lock);
        g_devices[*out] = state;
        g_physical_instance[physical] = state->instance;
    }
    return VK_SUCCESS;
}

extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkEnumeratePhysicalDevices(VkInstance instance,
                                                                    uint32_t* count,
                                                                    VkPhysicalDevice* devices) {
    auto enumerate = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(
        loader().instance_proc(instance, "vkEnumeratePhysicalDevices"));
    if (!enumerate) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult result = enumerate(instance, count, devices);
    if ((result == VK_SUCCESS || result == VK_INCOMPLETE) && devices) {
        std::lock_guard<std::mutex> guard(g_lock);
        for (uint32_t i = 0; i < *count; ++i) g_physical_instance[devices[i]] = instance;
    }
    return result;
}
extern "C" VKAPI_ATTR void VKAPI_CALL vkDestroyDevice(VkDevice device,
                                                      const VkAllocationCallbacks* alloc) {
    forget_device(device);
    if (auto destroy = reinterpret_cast<PFN_vkDestroyDevice>(
            loader().device_proc(device, "vkDestroyDevice"))) {
        destroy(device, alloc);
    }
}

extern "C" VKAPI_ATTR void VKAPI_CALL vkDestroyInstance(VkInstance instance,
                                                        const VkAllocationCallbacks* alloc) {
    {
        std::lock_guard<std::mutex> guard(g_lock);
        g_instances.erase(instance);
    }
    if (auto destroy = loader().fn<PFN_vkDestroyInstance>("vkDestroyInstance")) {
        destroy(instance, alloc);
    }
}
extern "C" VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance instance,
                                                                          const char* name) {
    if (!name) return nullptr;
    if (PFN_vkVoidFunction fn = synthesised_proc(name)) return fn;
    static const struct {
        const char* name;
        PFN_vkVoidFunction fn;
    } overrides[] = {
        {"vkCreateInstance", reinterpret_cast<PFN_vkVoidFunction>(vkCreateInstance)},
        {"vkCreateDevice", reinterpret_cast<PFN_vkVoidFunction>(vkCreateDevice)},
        {"vkDestroyDevice", reinterpret_cast<PFN_vkVoidFunction>(vkDestroyDevice)},
        {"vkGetInstanceProcAddr", reinterpret_cast<PFN_vkVoidFunction>(vkGetInstanceProcAddr)},
        {"vkGetDeviceProcAddr", reinterpret_cast<PFN_vkVoidFunction>(vkGetDeviceProcAddr)},
        {"vkEnumerateDeviceExtensionProperties",
         reinterpret_cast<PFN_vkVoidFunction>(vkEnumerateDeviceExtensionProperties)},
        {"vkGetPhysicalDeviceProperties",
         reinterpret_cast<PFN_vkVoidFunction>(vkGetPhysicalDeviceProperties)},
        {"vkGetPhysicalDeviceProperties2",
         reinterpret_cast<PFN_vkVoidFunction>(vkGetPhysicalDeviceProperties2)},
        {"vkGetPhysicalDeviceFeatures2",
         reinterpret_cast<PFN_vkVoidFunction>(vkGetPhysicalDeviceFeatures2)},
    };
    for (const auto& o : overrides) {
        if (std::strcmp(name, o.name) == 0) return o.fn;
    }
    return loader().instance_proc(instance, name);
}

extern "C" VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice device,
                                                                        const char* name) {
    if (!name) return nullptr;
    if (PFN_vkVoidFunction fn = synthesised_proc(name)) return fn;
    if (PFN_vkVoidFunction fn = loader().device_proc(device, name)) return fn;
    // A 1.3 core entry point on a 1.2 driver is usually present under its
    // extension name; the two are aliases with identical signatures.
    std::string promoted = std::string(name);
    promoted += "KHR";
    if (PFN_vkVoidFunction fn = loader().device_proc(device, promoted.c_str())) return fn;
    // Dynamic-state entry points were promoted through EXT, with the same
    // signatures.
    promoted = std::string(name) + "EXT";
    return loader().device_proc(device, promoted.c_str());
}

extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateDeviceExtensionProperties(
    VkPhysicalDevice physical, const char* layer, uint32_t* count, VkExtensionProperties* properties) {
    VkInstance owner = VK_NULL_HANDLE;
    {
        std::lock_guard<std::mutex> guard(g_lock);
        auto it = g_physical_instance.find(physical);
        if (it != g_physical_instance.end()) owner = it->second;
    }
    InstanceState* state = instance_state_of(owner);
    if (!state || !state->real_enumerate_device_extensions) return VK_ERROR_INITIALIZATION_FAILED;

    const uint32_t extra = static_cast<uint32_t>(std::size(kFakeExtensions));
    if (!properties) {
        uint32_t real = 0;
        VkResult result = state->real_enumerate_device_extensions(physical, layer, &real, nullptr);
        if (result != VK_SUCCESS && result != VK_INCOMPLETE) return result;
        *count = real + extra;
        return VK_SUCCESS;
    }
    uint32_t capacity = *count;
    uint32_t real = capacity > extra ? capacity - extra : 0;
    VkResult result = state->real_enumerate_device_extensions(physical, layer, &real, properties);
    if (result != VK_SUCCESS && result != VK_INCOMPLETE) return result;

    uint32_t written = real;
    for (const char* name : kFakeExtensions) {
        bool present = false;
        for (uint32_t i = 0; i < real; ++i) {
            if (std::strcmp(properties[i].extensionName, name) == 0) present = true;
        }
        if (present || written >= capacity) continue;
        std::memset(&properties[written], 0, sizeof(VkExtensionProperties));
        std::strncpy(properties[written].extensionName, name, VK_MAX_EXTENSION_NAME_SIZE - 1);
        properties[written].specVersion = 1;
        ++written;
    }
    *count = written;
    return VK_SUCCESS;
}

extern "C" VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceProperties(
    VkPhysicalDevice physical, VkPhysicalDeviceProperties* properties) {
    VkInstance owner = VK_NULL_HANDLE;
    {
        std::lock_guard<std::mutex> guard(g_lock);
        auto it = g_physical_instance.find(physical);
        if (it != g_physical_instance.end()) owner = it->second;
    }
    InstanceState* state = instance_state_of(owner);
    if (!state || !state->real_physical_properties) return;
    state->real_physical_properties(physical, properties);
    // The lie: vkd3d-proton refuses to start below 1.3.
    properties->apiVersion = VK_API_VERSION_1_3;
    raise_d3d12_limits(properties->limits);
    // D3D12's tiled-resources tier is derived from these; the driver reports
    // the conservative answer, which caps the feature level at 11_0.
    properties->sparseProperties.residencyStandard2DBlockShape = VK_TRUE;
    properties->sparseProperties.residencyStandard3DBlockShape = VK_TRUE;
    properties->sparseProperties.residencyNonResidentStrict = VK_TRUE;
    properties->sparseProperties.residencyAlignedMipSize = VK_FALSE;
}

extern "C" VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceProperties2(
    VkPhysicalDevice physical, VkPhysicalDeviceProperties2* properties) {
    VkInstance owner = VK_NULL_HANDLE;
    {
        std::lock_guard<std::mutex> guard(g_lock);
        auto it = g_physical_instance.find(physical);
        if (it != g_physical_instance.end()) owner = it->second;
    }
    InstanceState* state = instance_state_of(owner);
    if (!state || !state->real_physical_properties2) return;
    state->real_physical_properties2(physical, properties);
    if (std::getenv("VICTORY_SHIM_DEBUG")) std::fprintf(stderr, "shim: Properties2 seen\n");
    properties->properties.apiVersion = VK_API_VERSION_1_3;
    raise_d3d12_limits(properties->properties.limits);
    properties->properties.sparseProperties.residencyStandard2DBlockShape = VK_TRUE;
    properties->properties.sparseProperties.residencyStandard3DBlockShape = VK_TRUE;
    properties->properties.sparseProperties.residencyNonResidentStrict = VK_TRUE;
    properties->properties.sparseProperties.residencyAlignedMipSize = VK_FALSE;

    // The driver ignored the struct it does not know; fill it ourselves.
    if (auto* p13 = find_in_chain<VkPhysicalDeviceVulkan13Properties>(
            properties->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_PROPERTIES)) {
        std::memset(p13, 0, sizeof(*p13));
        p13->sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_PROPERTIES;
        p13->minSubgroupSize = 4;
        p13->maxSubgroupSize = 64;
        p13->maxComputeWorkgroupSubgroups = 64;
        p13->requiredSubgroupSizeStages = VK_SHADER_STAGE_COMPUTE_BIT;
        p13->maxInlineUniformBlockSize = 4096;
        p13->maxPerStageDescriptorInlineUniformBlocks = 4;
        p13->maxPerStageDescriptorUpdateAfterBindInlineUniformBlocks = 4;
        p13->maxDescriptorSetInlineUniformBlocks = 4;
        p13->maxDescriptorSetUpdateAfterBindInlineUniformBlocks = 4;
        p13->maxInlineUniformTotalSize = 4096;
        // vkd3d-proton refuses a device without these (see its device.c).
        p13->storageTexelBufferOffsetAlignmentBytes = 16;
        p13->storageTexelBufferOffsetSingleTexelAlignment = VK_TRUE;
        p13->uniformTexelBufferOffsetAlignmentBytes = 16;
        p13->uniformTexelBufferOffsetSingleTexelAlignment = VK_TRUE;
    }

    // vkd3d-proton refuses a device without single-texel alignment on texel
    // buffers. The driver may already report it (the extension exists on 1.2);
    // only fill what is missing.
    if (auto* texel = find_in_chain<VkPhysicalDeviceTexelBufferAlignmentProperties>(
            properties->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TEXEL_BUFFER_ALIGNMENT_PROPERTIES)) {
        if (texel->storageTexelBufferOffsetAlignmentBytes == 0)
            texel->storageTexelBufferOffsetAlignmentBytes = 16;
        if (texel->uniformTexelBufferOffsetAlignmentBytes == 0)
            texel->uniformTexelBufferOffsetAlignmentBytes = 16;
        texel->storageTexelBufferOffsetSingleTexelAlignment = VK_TRUE;
        texel->uniformTexelBufferOffsetSingleTexelAlignment = VK_TRUE;
    }

    // vkd3d derives the D3D12 shader model from the subgroup properties:
    // it needs QUAD (among others) in compute and fragment stages. AMD's 1.2
    // driver under-reports the set even though the hardware has it.
    if (auto* groups = find_in_chain<VkPhysicalDeviceSubgroupProperties>(
            properties->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES)) {
        if (groups->subgroupSize < 4) groups->subgroupSize = 64;
        groups->supportedStages |= VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_FRAGMENT_BIT |
                                   VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_GEOMETRY_BIT;
        groups->supportedOperations |= VK_SUBGROUP_FEATURE_BASIC_BIT |
                                       VK_SUBGROUP_FEATURE_VOTE_BIT |
                                       VK_SUBGROUP_FEATURE_ARITHMETIC_BIT |
                                       VK_SUBGROUP_FEATURE_BALLOT_BIT |
                                       VK_SUBGROUP_FEATURE_SHUFFLE_BIT |
                                       VK_SUBGROUP_FEATURE_SHUFFLE_RELATIVE_BIT |
                                       VK_SUBGROUP_FEATURE_QUAD_BIT;
        groups->quadOperationsInAllStages = VK_TRUE;
        if (std::getenv("VICTORY_SHIM_DEBUG"))
            std::fprintf(stderr, "shim: subgroup filled size=%u ops=0x%x stages=0x%x\n",
                         groups->subgroupSize, unsigned(groups->supportedOperations),
                         unsigned(groups->supportedStages));
    } else if (std::getenv("VICTORY_SHIM_DEBUG")) {
        std::fprintf(stderr, "shim: no subgroup struct in the chain\n");
    }

    // vkd3d-proton reads the promoted structure: subgroup size and operations
    // live in VkPhysicalDeviceVulkan11Properties on a 1.2 device.
    if (auto* p11 = find_in_chain<VkPhysicalDeviceVulkan11Properties>(
            properties->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES)) {
        if (p11->subgroupSize < 4) p11->subgroupSize = 64;
        p11->subgroupSupportedStages |= VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_FRAGMENT_BIT |
                                        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_GEOMETRY_BIT;
        p11->subgroupSupportedOperations |= VK_SUBGROUP_FEATURE_BASIC_BIT |
                                            VK_SUBGROUP_FEATURE_VOTE_BIT |
                                            VK_SUBGROUP_FEATURE_ARITHMETIC_BIT |
                                            VK_SUBGROUP_FEATURE_BALLOT_BIT |
                                            VK_SUBGROUP_FEATURE_SHUFFLE_BIT |
                                            VK_SUBGROUP_FEATURE_SHUFFLE_RELATIVE_BIT |
                                            VK_SUBGROUP_FEATURE_QUAD_BIT;
        p11->subgroupQuadOperationsInAllStages = VK_TRUE;
        if (std::getenv("VICTORY_SHIM_DEBUG"))
            std::fprintf(stderr, "shim: vulkan11 subgroup size=%u ops=0x%x stages=0x%x\n",
                         p11->subgroupSize, unsigned(p11->subgroupSupportedOperations),
                         unsigned(p11->subgroupSupportedStages));
    }

    // vkd3d prepends this struct to the properties chain; leave it zeroed and
    // it reports "no push descriptors".
    if (auto* push = find_in_chain<VkPhysicalDevicePushDescriptorPropertiesKHR>(
            properties->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PUSH_DESCRIPTOR_PROPERTIES_KHR)) {
        push->maxPushDescriptors = 32;
    }

    // Same story: vkd3d-proton requires transform feedback queries to exist.
    if (auto* xfb = find_in_chain<VkPhysicalDeviceTransformFeedbackPropertiesEXT>(
            properties->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_PROPERTIES_EXT)) {
        xfb->maxTransformFeedbackStreams = 4;
        xfb->maxTransformFeedbackBuffers = 4;
        xfb->maxTransformFeedbackBufferSize = 0x40000000ull;
        xfb->maxTransformFeedbackStreamDataSize = 128;
        xfb->maxTransformFeedbackBufferDataSize = 64;
        xfb->maxTransformFeedbackBufferDataStride = 64;
        xfb->transformFeedbackQueries = VK_TRUE;
        xfb->transformFeedbackStreamsLinesTriangles = VK_TRUE;
        xfb->transformFeedbackRasterizationStreamSelect = VK_FALSE;
        xfb->transformFeedbackDraw = VK_TRUE;
    }
}

extern "C" VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceFeatures2(
    VkPhysicalDevice physical, VkPhysicalDeviceFeatures2* features) {
    VkInstance owner = VK_NULL_HANDLE;
    {
        std::lock_guard<std::mutex> guard(g_lock);
        auto it = g_physical_instance.find(physical);
        if (it != g_physical_instance.end()) owner = it->second;
    }
    InstanceState* state = instance_state_of(owner);
    if (!state || !state->real_physical_features2) return;
    state->real_physical_features2(physical, features);
    if (std::getenv("VICTORY_SHIM_DEBUG"))
        std::fprintf(stderr, "shim: Features2 int16=%d\n", int(features->features.shaderInt16));
    // shaderInt16 is NOT faked: vkd3d would enable it and the driver refuses.

    // Claims backed by this shim, not by the driver.
    if (auto* f13 = find_in_chain<VkPhysicalDeviceVulkan13Features>(
            features->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES)) {
        std::memset(f13, 0, sizeof(*f13));
        f13->sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
        f13->dynamicRendering = VK_TRUE;   // synthesised in renderpass.cpp
        f13->synchronization2 = VK_TRUE;   // real: VK_KHR_synchronization2 is present
        f13->maintenance4 = VK_TRUE;       // approximated in maint4.cpp
        f13->shaderDemoteToHelperInvocation = VK_TRUE;
        f13->shaderZeroInitializeWorkgroupMemory = VK_TRUE;
    }

    // VK_EXT_texel_buffer_alignment carries a feature bit of its own; vkd3d
    // checks it before it reads the alignment properties.
    if (auto* texel = find_in_chain<VkPhysicalDeviceTexelBufferAlignmentFeaturesEXT>(
            features->pNext,
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TEXEL_BUFFER_ALIGNMENT_FEATURES_EXT)) {
        texel->texelBufferAlignment = VK_TRUE;
    }

    if (auto* divisor = find_in_chain<VkPhysicalDeviceVertexAttributeDivisorFeaturesEXT>(
            features->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_FEATURES_EXT)) {
        divisor->vertexAttributeInstanceRateDivisor = VK_TRUE;
        divisor->vertexAttributeInstanceRateZeroDivisor = VK_TRUE;
    }

    if (auto* xfb = find_in_chain<VkPhysicalDeviceTransformFeedbackFeaturesEXT>(
            features->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_FEATURES_EXT)) {
        xfb->transformFeedback = VK_TRUE;
        xfb->geometryStreams = VK_TRUE;
    }

    // Tiled resources (and so feature level 12_0) need the sparse family.
    features->features.sparseBinding = VK_TRUE;
    features->features.sparseResidencyAliased = VK_TRUE;
    features->features.sparseResidencyBuffer = VK_TRUE;
    features->features.sparseResidencyImage2D = VK_TRUE;
    features->features.sparseResidencyImage3D = VK_TRUE;
    features->features.shaderResourceResidency = VK_TRUE;
    features->features.shaderResourceMinLod = VK_TRUE;

    // vkd3d-proton requires both of these outright.
    if (auto* m5 = find_in_chain<VkPhysicalDeviceMaintenance5FeaturesKHR>(
            features->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES_KHR)) {
        m5->maintenance5 = VK_TRUE;
    }
    if (auto* m6 = find_in_chain<VkPhysicalDeviceMaintenance6FeaturesKHR>(
            features->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_6_FEATURES_KHR)) {
        m6->maintenance6 = VK_TRUE;
    }
}
