// Optional SPIR-V capture: with VICTORY_DUMP_SPV=<dir>, every shader module
// handed to the driver is written out before the call. When a driver refuses a
// module with VK_ERROR_INITIALIZATION_FAILED, the file is the only evidence of
// what it was asked to compile.
#include "shim.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include <unordered_set>
#include <mutex>

namespace {
std::mutex g_lock;
unsigned g_next = 0;
bool g_warned = false;
}  // namespace

extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkCreateShaderModule(
    VkDevice device, const VkShaderModuleCreateInfo* info, const VkAllocationCallbacks* alloc,
    VkShaderModule* out) {
    auto create = reinterpret_cast<PFN_vkCreateShaderModule>(
        shim::loader().device_proc(device, "vkCreateShaderModule"));
    if (!create) return VK_ERROR_INITIALIZATION_FAILED;

    const char* dir = std::getenv("VICTORY_DUMP_SPV");
    if (dir && dir[0] && info && info->pCode && info->codeSize >= 5) {
        std::string path;
        {
            std::lock_guard<std::mutex> guard(g_lock);
            char name[64];
            std::snprintf(name, sizeof(name), "shader_%04u_%s.spv", g_next++,
                          info->pCode[0] == 0x07230203 ? "spv" : "bin");
            path = std::string(dir) + "\\" + name;
        }
        if (FILE* f = std::fopen(path.c_str(), "wb")) {
            std::fwrite(info->pCode, 1, info->codeSize, f);
            std::fclose(f);
        }
    }

    // vkd3d-proton emits SPIR-V 1.6 (Vulkan 1.3). A 1.2 driver refuses the
    // module outright. With VICTORY_SPV_DOWNGRADE=<majorminor>, the header's
    // version word is rewritten (1.5 for Vulkan 1.2) on a copy: the opcodes
    // this driver needs are all valid there.
    std::vector<uint32_t> downgraded;
    const VkShaderModuleCreateInfo* pass = info;
    if (const char* want = std::getenv("VICTORY_SPV_DOWNGRADE"); want && want[0] && info &&
        info->pCode && info->codeSize >= 8 && info->pCode[0] == 0x07230203) {
        const unsigned requested = static_cast<unsigned>(std::strtoul(want, nullptr, 10));
        const unsigned encoded = ((requested / 10) << 16) | ((requested % 10) << 8);
        if (encoded && info->pCode[1] > encoded) {
            downgraded.assign(info->pCode, info->pCode + info->codeSize / 4);
            downgraded[1] = encoded;
            VkShaderModuleCreateInfo patched = *info;
            patched.pCode = downgraded.data();
            pass = &patched;
        }
    }

    VkResult result = create(device, pass, alloc, out);
    if (result != VK_SUCCESS && !g_warned) {
        g_warned = true;
        std::fprintf(stderr, "vulkan-1 shim: vkCreateShaderModule rejected (VkResult %d, %zu bytes)\n",
                     int(result), info ? info->codeSize : 0);
    }
    return result;
}

extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkCreateShaderModuleKHR(
    VkDevice device, const VkShaderModuleCreateInfo* info, const VkAllocationCallbacks* alloc,
    VkShaderModule* out) {
    return vkCreateShaderModule(device, info, alloc, out);
}
