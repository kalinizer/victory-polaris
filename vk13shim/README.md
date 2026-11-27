# vk13shim — Vulkan 1.3 on a 1.2 driver, for GCN3

A drop-in `vulkan-1.dll` that presents a Vulkan 1.2 driver as 1.3, plus a small
patch to vkd3d-proton so its shader-model ladder does not require `shaderInt16`.

It exists because vkd3d-proton requires Vulkan 1.3 and D3D12 feature level 12_0
with shader model 6.6, and an R9 380 (GCN 3, 4 GB, driver 27.20.20913.2000 from
2021) exposes neither: Vulkan 1.2.170, no `VK_KHR_push_descriptor`, no
`VK_KHR_maintenance5/6`, `shaderInt16 = false`, and SPIR-V stops at 1.5.

## What it does

| Problem | Solution |
|---|---|
| `vkEnumerateInstanceVersion` / `apiVersion` below 1.3 | report 1.3 |
| `VK_KHR_dynamic_rendering` missing | `vkCmdBeginRendering` synthesised on `vkCreateRenderPass` + `vkCreateFramebuffer`, cached per device |
| `VK_KHR_push_descriptor` missing | push descriptor sets synthesised on ordinary descriptor sets, including the `...WithTemplate` form |
| `VK_KHR_maintenance5/6` missing | the seven entry points implemented on 1.0/1.1 equivalents |
| `vkCmdBindVertexBuffers2`, `vkCmdBlitImage2`, … missing | forwarded to their KHR/EXT aliases, or unpacked into the 1.0 calls |
| SPIR-V 1.6 emitted for a 1.2 driver | optional header rewrite to 1.5 |
| vkd3d's SM ladder wants `shaderInt16` | patched out (see `patches/`) |

## Measured

On the machine this was written on (R9 380, 4 GB, Windows 10 19045):

```
D3D12CreateDevice(12_0) : succeeds
CheckFeatureSupport(SHADER_MODEL) : 6.6
queue execute + fence    : completes
```

Running the VIctory build of the title on top of it (victory → this shim →
patched vkd3d → driver) reaches **13 presented frames**, then fails with
`VK_ERROR_OUT_OF_DEVICE_MEMORY`: the software stack works, 4 GB does not.

## Layout

```
src/            the shim (loader, render pass bridge, push descriptors,
                maintenance5/6, copy_commands2, dynamic state, SPIR-V dump)
tools/          gen_exports.py — builds the .def from vk.xml
test/           test13 (Vulkan) and d3d12probe (D3D12)
patches/        vkd3d-sm66-gcn.patch against vkd3d-proton v3.1
ARCHITECTURE.md the design, the traps, and the full diagnosis chain
```

## Building

Needs MSVC, the Windows SDK, Vulkan-Headers, and — for the vkd3d patch — MSYS2
with `mingw-w64-ucrt-x86_64-tools` (provides `widl`) and
`mingw-w64-ucrt-x86_64-glslang`, plus the `dxil-spirv` and `dxbc-spirv`
subprojects.

```powershell
.\build.ps1            # generates the export table, compiles, links, and copies
                       # the real loader beside the result as vulkan_loader_real.dll
.\build\test13.exe     # Vulkan-side self test
```

The shim and the real loader must sit next to the application:

```
app\  vulkan-1.dll            (this shim)
      vulkan_loader_real.dll  (renamed copy of the Khronos loader)
      d3d12.dll, d3d12core.dll (vkd3d-proton, with patches/)
```

## Environment switches

| Variable | Effect |
|---|---|
| `VICTORY_SPV_DOWNGRADE=15` | rewrite SPIR-V 1.6 modules to 1.5 before the driver sees them |
| `VICTORY_DUMP_SPV=<dir>` | write every shader module handed to the driver |
| `VICTORY_VK_VALIDATION=1` | add `VK_LAYER_KHRONOS_validation` to the instance |
| `VICTORY_SHIM_DEBUG=1` | trace which capability claims were applied |

## Limitations

- `VK_EXT_extended_dynamic_state2` setters are **no-ops** (the driver has only
  generation 1). A title that changes depth-bias-enable, logic op, rasterizer
  discard or primitive restart dynamically will keep the pipeline state object's
  values.
- MSAA attachment sample counts are assumed to be 1 (a view does not carry the
  image's sample count).
- `VK_RENDERING_SUSPENDING_BIT` / `RESUMING_BIT` across command buffers is not
  modelled.
- maintenance5/6 and sparse-resource claims are approximations.

## Licence

The shim is GPL-3.0-or-later, matching the project it serves. The vkd3d patch
is against code under the same licence.
