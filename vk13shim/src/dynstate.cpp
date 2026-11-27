// VK_EXT_extended_dynamic_state2 entry points on a driver that only has the
// first generation (or nothing).
//
// vkd3d-proton loads all of these at device creation because they are core in
// 1.3, and fails the device if any is missing. The driver here has only
// VK_EXT_extended_dynamic_state (generation 1), so the generation-2 setters
// have to exist somewhere.
//
// They are recorded as no-ops. That is a real approximation: a title that
// changes depth-bias-enable, rasterizer discard, logic op or primitive restart
// *dynamically* will keep whatever the pipeline state object says. It is
// called out in ARCHITECTURE.md; if the title relies on it, this is where the
// picture would go wrong.
#include "shim.h"

#include <cstdio>

namespace {
bool logged = false;

void note(const char* what) {
    if (logged) return;
    logged = true;
    std::fprintf(stderr,
                 "vulkan-1 shim: %s is a no-op (driver has no "
                 "VK_EXT_extended_dynamic_state2)\n",
                 what);
}
}  // namespace

extern "C" VKAPI_ATTR void VKAPI_CALL vkCmdSetPatchControlPointsEXT(VkCommandBuffer cmd,
                                                                   uint32_t points) {
    (void)cmd;
    (void)points;
    note("vkCmdSetPatchControlPointsEXT");
}

extern "C" VKAPI_ATTR void VKAPI_CALL vkCmdSetRasterizerDiscardEnable(
    VkCommandBuffer cmd, VkBool32 discard) {
    (void)cmd;
    (void)discard;
    note("vkCmdSetRasterizerDiscardEnable");
}

extern "C" VKAPI_ATTR void VKAPI_CALL vkCmdSetRasterizerDiscardEnableEXT(
    VkCommandBuffer cmd, VkBool32 discard) {
    vkCmdSetRasterizerDiscardEnable(cmd, discard);
}

extern "C" VKAPI_ATTR void VKAPI_CALL vkCmdSetDepthBiasEnable(VkCommandBuffer cmd,
                                                              VkBool32 enable) {
    (void)cmd;
    (void)enable;
    note("vkCmdSetDepthBiasEnable");
}

extern "C" VKAPI_ATTR void VKAPI_CALL vkCmdSetDepthBiasEnableEXT(VkCommandBuffer cmd,
                                                                 VkBool32 enable) {
    vkCmdSetDepthBiasEnable(cmd, enable);
}

extern "C" VKAPI_ATTR void VKAPI_CALL vkCmdSetLogicOp(VkCommandBuffer cmd,
                                                      VkLogicOp logic_op) {
    (void)cmd;
    (void)logic_op;
    note("vkCmdSetLogicOp");
}

extern "C" VKAPI_ATTR void VKAPI_CALL vkCmdSetLogicOpEXT(VkCommandBuffer cmd,
                                                         VkLogicOp logic_op) {
    vkCmdSetLogicOp(cmd, logic_op);
}

extern "C" VKAPI_ATTR void VKAPI_CALL vkCmdSetPrimitiveRestartEnable(VkCommandBuffer cmd,
                                                                     VkBool32 enable) {
    (void)cmd;
    (void)enable;
    note("vkCmdSetPrimitiveRestartEnable");
}

extern "C" VKAPI_ATTR void VKAPI_CALL vkCmdSetPrimitiveRestartEnableEXT(VkCommandBuffer cmd,
                                                                        VkBool32 enable) {
    vkCmdSetPrimitiveRestartEnable(cmd, enable);
}
