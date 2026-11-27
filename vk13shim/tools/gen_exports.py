#!/usr/bin/env python3
"""Write the export table for the vulkan-1 shim.

Two kinds of exports:

  * the entry points this DLL implements (INTERCEPTED), which exist as real
    functions in loader.cpp / renderpass.cpp / sync.cpp;
  * everything else, forwarded by the linker straight to the untouched Khronos
    loader, shipped beside this DLL as vulkan-1-real.dll.

Forwarding in the .def file rather than in C++ avoids restating ~650 prototypes
from vk.xml, and the platform/SC types Windows headers do not have.

    python gen_exports.py --vk-xml <SDK>/share/vulkan/registry/vk.xml
"""
from __future__ import annotations

import argparse
import sys
import xml.etree.ElementTree as ET

INTERCEPTED = {
    # loader.cpp
    "vkCreateInstance", "vkDestroyInstance", "vkCreateDevice", "vkDestroyDevice",
    "vkEnumeratePhysicalDevices", "vkEnumerateDeviceExtensionProperties",
    "vkGetInstanceProcAddr", "vkGetDeviceProcAddr",
    "vkGetPhysicalDeviceProperties", "vkGetPhysicalDeviceProperties2",
    "vkGetPhysicalDeviceQueueFamilyProperties",
    "vkGetPhysicalDeviceFormatProperties",
    "vkGetPhysicalDeviceProperties2KHR",
    "vkGetPhysicalDeviceFeatures2KHR",
    "vkGetPhysicalDeviceFormatProperties2",
    "vkGetPhysicalDeviceFormatProperties2KHR",
    "vkGetPhysicalDeviceQueueFamilyProperties2",
    "vkGetPhysicalDeviceQueueFamilyProperties2KHR",
    "vkGetPhysicalDeviceFeatures2",
    # renderpass.cpp
    "vkAllocateCommandBuffers", "vkFreeCommandBuffers",
    "vkCmdBeginRendering", "vkCmdEndRendering",
    "vkCreateImageView", "vkDestroyImageView",
    # sync.cpp
    "vkCmdPipelineBarrier2", "vkCmdPipelineBarrier2KHR",
    "vkQueueSubmit2", "vkQueueSubmit2KHR",
    "vkGetDeviceBufferMemoryRequirements", "vkGetDeviceBufferMemoryRequirementsKHR",
    "vkGetDeviceImageMemoryRequirements", "vkGetDeviceImageMemoryRequirementsKHR",
    "vkGetDeviceImageSparseMemoryRequirements",
    "vkGetDeviceImageSparseMemoryRequirementsKHR",
    # push.cpp
    "vkCreateDescriptorSetLayout", "vkDestroyDescriptorSetLayout",
    "vkCreatePipelineLayout", "vkDestroyPipelineLayout",
    "vkCreateDescriptorUpdateTemplate", "vkCreateDescriptorUpdateTemplateKHR",
    "vkDestroyDescriptorUpdateTemplate", "vkDestroyDescriptorUpdateTemplateKHR",
    "vkBeginCommandBuffer",
    "vkCmdPushDescriptorSetKHR", "vkCmdPushDescriptorSetWithTemplateKHR",
    # maint5.cpp
    "vkCmdBindIndexBuffer2KHR", "vkGetRenderingAreaGranularityKHR",
    "vkGetImageSubresourceLayout2KHR", "vkGetDeviceImageSubresourceLayoutKHR",
    "vkCmdBindDescriptorSets2KHR", "vkCmdPushDescriptorSet2KHR", "vkCmdPushConstants2KHR",
    "vkCmdBindVertexBuffers2", "vkCmdBindVertexBuffers2EXT",
    "vkCreateShaderModule", "vkCreateShaderModuleKHR",
    "vkCmdSetPatchControlPointsEXT",
    "vkCmdSetRasterizerDiscardEnable",
    "vkCmdSetRasterizerDiscardEnableEXT",
    "vkCmdSetDepthBiasEnable",
    "vkCmdSetDepthBiasEnableEXT",
    "vkCmdSetLogicOp",
    "vkCmdSetLogicOpEXT",
    "vkCmdSetPrimitiveRestartEnable",
    "vkCmdSetPrimitiveRestartEnableEXT",
    "vkCmdBlitImage2",
    "vkCmdBlitImage2KHR",
    "vkCmdCopyImage2",
    "vkCmdCopyImage2KHR",
    "vkCmdCopyBuffer2",
    "vkCmdCopyBuffer2KHR",
    "vkCmdCopyBufferToImage2",
    "vkCmdCopyBufferToImage2KHR",
    "vkCmdCopyImageToBuffer2",
    "vkCmdCopyImageToBuffer2KHR",
    "vkCmdResolveImage2",
    "vkCmdResolveImage2KHR",
}

REAL_MODULE = "vulkan_loader_real"


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--vk-xml", required=True)
    ap.add_argument("--def", dest="def_path", default="vulkan13.def")
    ap.add_argument("--report", default=None)
    ap.add_argument("--real-def", dest="real_def", default=None)
    opts = ap.parse_args()

    root = ET.parse(opts.vk_xml).getroot()

    def names_in(features, api=None):
        out = set()
        for feat in features:
            if api and feat.get("api") != api:
                continue
            for req in feat.findall("require"):
                for cmd in req.findall("command"):
                    out.add(cmd.get("name"))
        return out

    core = names_in(root.findall("./feature"), api="vulkan")

    platform_only = set()
    for ext in root.findall("./extensions/extension"):
        if not ext.get("platform"):
            continue
        for req in ext.findall("require"):
            for cmd in req.findall("command"):
                platform_only.add(cmd.get("name"))
    platform_only -= core

    sc_feats = [f for f in root.findall("./feature") if f.get("api") == "vulkansc"]
    sc_only = names_in(sc_feats) - core

    names = []
    for command in root.findall("./commands/command"):
        if command.get("alias"):
            continue
        proto = command.find("proto")
        if proto is None or proto.find("name") is None:
            continue
        name = proto.find("name").text
        if not name or not name.startswith("vk"):
            continue
        if name in platform_only or name in sc_only:
            continue
        names.append(name)

    names = sorted(set(names))
    forwarded = [n for n in names if n not in INTERCEPTED]

    with open(opts.def_path, "w", encoding="ascii", newline="\r\n") as f:
        f.write("LIBRARY vulkan-1\nEXPORTS\n")
        for name in sorted(INTERCEPTED):
            f.write(f"    {name}\n")
        for name in forwarded:
            f.write(f"    {name} = {REAL_MODULE}.{name}\n")

    if opts.real_def:
        # A forwarder target has to be a module the linker knows, so give it an
        # import library to look at.
        with open(opts.real_def, "w", encoding="ascii", newline="\r\n") as f:
            f.write(f"LIBRARY {REAL_MODULE}\nEXPORTS\n")
            for name in forwarded:
                f.write(f"    {name}\n")

    if opts.report:
        with open(opts.report, "w", encoding="utf-8") as f:
            f.write("\n".join(names))

    print(f"{len(INTERCEPTED)} implemented, {len(forwarded)} forwarded", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
