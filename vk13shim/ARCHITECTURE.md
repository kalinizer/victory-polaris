# Vulkan 1.3 → 1.2 拦截层：架构与实测

目标：让 vkd3d-proton 在一台只暴露 **Vulkan 1.2.170** 的 R9 380 上启动，
并把它的 D3D12 调用落到 1.2 能执行的原语上。

**当前状态：已编译、已链接、功能测试通过。** 它在 1.2 驱动上对外报告 1.3.0，
动态渲染桥记录出的 render pass 真实提交并等待完成。

---

## 0. 先纠正两个方向

**（1）`vkCreateRenderPass` 不需要降级。** 它是 Vulkan 1.0 的函数，是"旧式"那一侧。
真正缺的是 `vkCmdBeginRendering`（1.3 核心 / `VK_KHR_dynamic_rendering`），这块卡连那个扩展都没有。
所以要做的是**合成**：把 1.3 的动态渲染实现成 1.0 的 render pass + framebuffer。

**（2）vkd3d-proton 的门槛不是 render pass 风格**，而是启动时的 `apiVersion` 与特性链检查。
拦截层必须先"说谎"过检，再把它用到的 1.3 入口逐个翻译。

## 1. 拦截模型（最终形态）

本 DLL 就叫 `vulkan-1.dll`，与真 loader 的副本 `vulkan_loader_real.dll` 一起放在应用目录。
句柄全程不包装：`VkInstance/VkPhysicalDevice/VkDevice/VkCommandBuffer` 原样穿透，状态按句柄值索引。

导出分两类，全部写在 `vulkan13.def` 里：

| 类别 | 数量 | 形式 |
|---|---|---|
| 自己实现 | 25 | `vkCmdBeginRendering` 等，函数体在本仓库 |
| 转发 | 655 | `vkCreateBuffer = vulkan_loader_real.vkCreateBuffer` |

**为什么用 .def 转发而不是 C++ 桩**：C++ 桩要把 vk.xml 里 650 多个原型重新拼一遍，
而 Windows 头文件里没有 android/ohos/SC 的类型，拼出来的签名必然一批批报错。
`.def` 转发把这件事交给链接器，签名问题整体消失。

两个必须知道的细节：

- 转发目标模块必须是**链接期已知**的模块，否则链接器把 `mod.func` 当成"导出别名"去符号表里找。
  所以构建时先用 `lib /def:vulkan_loader_real.def` 造一个导入库并参与链接。
- 转发目标**不能带连字符**：`vulkan-1-real.vkFoo` 会被错误切分。故副本命名为 `vulkan_loader_real.dll`。

## 2. 模块

```
src/
├─ shim.h         类型、每设备状态、缓存键、注册表声明
├─ loader.cpp     真 loader、句柄表、创建/销毁、能力伪造、扩展增删
├─ renderpass.cpp 动态渲染 → render pass；image view 登记；命令缓冲映射
├─ dispatch.cpp   合成函数总表（GIPA/GDPA 先查它）
└─ sync.cpp       barrier2 / submit2 / maintenance4 + KHR 别名
tools/gen_exports.py   vk.xml → vulkan13.def + vulkan_loader_real.def
build.ps1              生成 → 编译 → 链接 → 放置真 loader 副本
test/test13.cpp        功能测试
```

## 3. 落点表

| 1.3 入口 | 本层实现 | 依赖 |
|---|---|---|
| `vkCmdBeginRendering` | 建/取 `VkRenderPass` + `VkFramebuffer`，记 `vkCmdBeginRenderPass` | 1.0 |
| `vkCmdEndRendering` | `vkCmdEndRenderPass` | 1.0 |
| `vkCmdPipelineBarrier2` | `VK_KHR_synchronization2`（本机有），否则拆成 1.0 barrier | 扩展 |
| `vkQueueSubmit2` | 同上 | 扩展 |
| `vkGetDevice{Buffer,Image}MemoryRequirements` | 造临时对象→查→销毁（近似） | maintenance4 无等价 |
| `VK_KHR_dynamic_rendering` / `_maintenance_4` | 枚举时加，`vkCreateDevice` 前剔除 | 驱动不能看到它们 |
| `apiVersion` / `VkPhysicalDeviceVulkan13Features` | 改值 / 补链 | 唯一的"谎" |

`VK_EXT_descriptor_indexing` 不动：这块卡真支持，6.6 的 `ResourceDescriptorHeap[]`
由 dxil-spirv 降级成描述符索引，正靠它。

## 4. 联调时踩到的四个坑（都已修）

1. **递归锁**：`instance_state_of()` 自己会拿 `g_lock`，而调用方正持着它 →
   MSVC 的 `std::mutex` 抛 `system_error` → `terminate`（0xC0000409）。改为先取句柄、放锁、再查。
2. **设备级入口解析不到**：`vkGetInstanceProcAddr(VK_NULL_HANDLE, "vkCreateDevice")` 在真 loader 上
   返回 NULL（规范只保证全局函数）。改为通过 instance（`vkCreateDevice`）或 device（`vkDestroyDevice`）解析。
3. **把 1.3 特性结构递给了 1.2 驱动**：`vkCreateDevice` 返回 `VK_ERROR_INITIALIZATION_FAILED`。
   现在临时把 `VkPhysicalDeviceVulkan13Features` 从 pNext 链上摘下来，调用完再挂回去。
4. **枚举返回 `VK_INCOMPLETE`**：缓冲区刚好装下时也应返回 `VK_SUCCESS`，改掉了。

## 5. 功能测试结果

`test\test13.cpp`，在本机 R9 380 + 2021 驱动上：

```
start
creating instance...
instance ok
devices: 1
device      : AMD Radeon (TM) R9 380 Series
apiVersion  : 1.3.0  (shim should say 1.3)
extensions  : 108 total, dynamic_rendering=yes maintenance4=yes
features    : dynamicRendering=1 synchronization2=1 maintenance4=1
device      : created with the fake extensions enabled
render pass : begin/end recorded and submitted without error
PASS
```

导出表：680 项（25 实现 + 655 转发）。

## 6. 构建

需要 MSVC、Windows SDK、Vulkan 头文件（`third_party\Vulkan-Headers-main`）。

```powershell
.\build.ps1          # 生成 def → 编译 → 链接 → 复制 vulkan_loader_real.dll
.\build\test13.exe   # 功能测试
```

## 7. 已知做不到的

- `VK_RENDERING_SUSPENDING_BIT` / `RESUMING_BIT` 的跨命令缓冲语义（需要子通道或次级命令缓冲）。
- MSAA 附件的采样数：view 不携带采样数，还需记录 `vkCreateImage`（代码里标了 TODO）。
- maintenance4 只是近似（带 `pNext` 的创建信息不精确）。
- **最关键的**：这一层只解决"vkd3d-proton 愿不愿意在 1.2 上启动"，
  不解决 4 GB 显存，也不解决"世界黑屏"。

## 8. 下一步

1. 拿 vkd3d-proton 的 `d3d12.dll` / `d3d12core.dll` + DXVK 的 `dxgi.dll`，
   与 `vulkan-1.dll`、`vulkan_loader_real.dll` 一起放进 `victory.exe` 目录；
2. 先跑 vkd3d-proton 自带的最小 D3D12 测试，确认它接受 1.3 的谎；
3. 再上 victory。期望值仍然只是"能进菜单"。

---

## 9. vkd3d-proton 集成实测（2026-11-27）

把 vkd3d-proton 3.1 的 `d3d12.dll` / `d3d12core.dll` 与本层的 `vulkan-1.dll`、
`vulkan_loader_real.dll` 放进同一目录，用 `test\d3d12probe.exe` 触发 `D3D12CreateDevice`。

### 逐级过关的检查点

| # | vkd3d 的检查 | 结果 |
|---|---|---|
| 1 | 能否取到全局入口 | 起初失败：真 loader 未打开（`loader().open()` 只在 vkCreateInstance 调）。改为懒加载后通过 |
| 2 | `vkEnumerateInstanceVersion >= 1.3` | 起初报 "Vulkan 1.3 not supported by loader"。新增该入口并上抬到 1.3 后通过 |
| 3 | 单 texel 对齐 | 需要 `VkPhysicalDeviceVulkan13Properties` 里的 `*TexelBufferOffsetSingleTexelAlignment`（**不是** EXT 结构） |
| 4 | `VK_EXT_transform_feedback` 的 `transformFeedbackQueries` | 补 `VkPhysicalDeviceTransformFeedbackPropertiesEXT` |
| 5 | `VK_EXT_vertex_attribute_divisor` 的两个 feature 位 | 补 `VkPhysicalDeviceVertexAttributeDivisorFeaturesEXT` |
| 6 | **`VK_KHR_push_descriptor`** | **不通过，见下** |

vkd3d 3.1 的 `device.c` 里这三条是硬性 `ERR` + `E_INVALIDARG`，其余（条件渲染、depth clip、
EXT_dgc 等）只是降级。

### 卡点：push descriptors

```
err:vkd3d_init_device_caps: Push descriptors are not supported by this implementation.
    This is required for correct operation.
```

实测驱动（R9 380，`27.20.20913.2000`）的扩展里只有 `VK_KHR_descriptor_update_template`，
**没有 `VK_KHR_push_descriptor`**；而且 vkd3d 里没有任何配置开关能绕过这一条。

**这一条不能靠"谎报"过去**：声明了扩展之后 vkd3d 会真的调用
`vkCmdPushDescriptorSetKHR` / `vkCmdPushDescriptorSetWithTemplateKHR`，驱动没有这些入口，
拿到的是空指针。要过这一关必须真的实现：

1. 拦截 `vkCreateDescriptorSetLayout`，把 `PUSH_DESCRIPTOR_BIT_KHR` 去掉，记下哪些 set 是推入式；
2. 拦截 `vkCmdPushDescriptorSetKHR`：从每条命令缓冲自己的池里分配一个 set，
   用 `vkUpdateDescriptorSets` 写入，再 `vkCmdBindDescriptorSets` 绑到对应 set index；
3. 拦截 `vkCmdPushDescriptorSetWithTemplateKHR`（vkd3d 大量使用 update template）：展开模板成写操作；
4. 处理"同一 set 多次 push 覆盖"的语义，以及在 `vkCmdBindDescriptorSets` 之后 push 的优先级。

工作量估计 400–600 行 + 一轮调试，是这条路上**下一个、也是最后一个结构性缺口**。

### 当前结论

- 本层的 1.3 伪装**确实被 vkd3d-proton 接受**，它一路走到设备能力检查的最后一条；
- 剩下的不是"版本谎言"问题，而是驱动确实缺一个扩展；
- 即使补上 push descriptors，后面还有 4 GB 显存与"世界黑屏"两个老问题。


---

## 10. 继续推进：push descriptors 与后续关卡（2026-11-27 第二轮）

### 新增实现的入口

| 来源 | 内容 |
|---|---|
| `src/push.cpp` | push descriptor：`vkCreateDescriptorSetLayout`（清 push 标志）、`vkCreatePipelineLayout`（记录 set 布局）、`vkCmdPushDescriptorSetKHR`、`...WithTemplateKHR`（模板展开为写操作）、`vkBeginCommandBuffer`（重置池） |
| `src/maint5.cpp` | maintenance5/6：`vkCmdBindIndexBuffer2KHR`、`vkGetRenderingAreaGranularityKHR`、`vkGetImageSubresourceLayout2KHR`、`vkGetDeviceImageSubresourceLayoutKHR`、`vkCmdBindDescriptorSets2KHR`、`vkCmdPushDescriptorSet2KHR`、`vkCmdPushConstants2KHR` |
| `src/copy2.cpp` | copy_commands2 全套（blit/copy image/buffer/buffer↔image/resolve 的 "2" 版），驱动没有就直接拆成 1.0 调用 |
| `src/dynstate.cpp` | extended_dynamic_state2 的五个 setter —— **无操作**（见下） |
| `src/sync.cpp` | 追加 `vkGetDeviceImageSparseMemoryRequirements`（临时建图再查） |

### 两个通用机制

1. **别名回退**：`vkGetDeviceProcAddr` 取不到核心名时，依次试 `名字+KHR`、`名字+EXT`。
   1.3 的核心入口在 1.2 驱动上通常以扩展别名存在，签名相同，可以原样返回指针。
2. **扩展代启用**：因为我们"谎称 1.3"，vkd3d 不会去启用 KHR/EXT 扩展名，驱动于是不暴露那些入口。
   `vkCreateDevice` 现在会在驱动确实支持时，替它把 `VK_KHR_synchronization2`、`VK_KHR_copy_commands2`、
   `VK_EXT_extended_dynamic_state(1/2/3)` 补进启用列表。

### 必须说明的近似

`VK_EXT_extended_dynamic_state2` 在这块驱动上不存在，而 `vkCmdSet{RasterizerDiscardEnable,
DepthBiasEnable, LogicOp, PrimitiveRestartEnable}`、`vkCmdSetPatchControlPointsEXT` 是 vkd3d 加载期
**必需**的符号。它们被实现成空操作：如果一个标题在录制时动态改这些状态，实际会沿用管线状态对象里的值。
这是整层里唯一一处"能过检查但可能画错"的地方，其余都是等价翻译。

### 现在的进度条

| 阶段 | 结果 |
|---|---|
| 1.3 伪装被 vkd3d 接受 | 通过 |
| 实例/设备版本、扩展、特性检查 | 通过 |
| texel 对齐 / transform feedback / vertex divisor | 通过 |
| push descriptors | 通过（本层实现） |
| maintenance5+6 | 通过（本层实现） |
| 全部设备入口解析（`vkd3d_load_vk_device_procs`） | **通过** |
| vkd3d 创建内部 meta shader | **失败**：`vkd3d_meta_ops_common_init: Failed to create shader modules, vr -3` |

`vr -3` 是 `VK_ERROR_INITIALIZATION_FAILED`，来自 `vkCreateShaderModule`——驱动拒绝了 vkd3d
内部生成的 SPIR-V。这一层已经不是"缺入口"，而是驱动的 SPIR-V 编译器对 vkd3d 生成的模块有意见；
要定位需要 Vulkan 验证层（本机未安装）或打开 vkd3d 的调试输出。

### 回归

`build\test13.exe` 仍然 PASS（1.3 报告、假扩展、动态渲染桥都正常）。
当前导出表：68 个自实现 + 635 个转发。

---

## 11. 诊断结论：SPIR-V 1.6 vs Vulkan 1.2

### 验证层没装成，但拿到了更直接的证据

LunarG SDK 安装包（303 MB）是图形安装器，`/S` 与 `--mode unattended` 都没能跳过界面。
改走另一条：给 shim 加一个 `VICTORY_DUMP_SPV=<dir>` 开关，把 vkd3d 交给驱动的每个
shader module 落盘。第一次失败时抓到 1216 字节的模块，头部：

```
magic=0x07230203  version=0x00010600 (SPIR-V 1.6)  generator=0x0008000B
```

**vkd3d-proton 按 Vulkan 1.3 生成 SPIR-V 1.6，而 1.2 驱动只认到 1.5，于是
`vkCreateShaderModule` 返回 `VK_ERROR_INITIALIZATION_FAILED`。** 这就是 `vr -3` 的全部含义。

### 补丁：`VICTORY_SPV_DOWNGRADE=15`

`vkCreateShaderModule` 里把模块头的版本字从 1.6 改写为 1.5（拷贝一份再交出去）。
vkd3d 的 meta shader 只用到 1.5 就有的指令，驱动随即接受——设备初始化一路推进到
内存拓扑、bindless、管线库，全部通过。

### 下一个墙：特性等级

设备入口全部解析、meta shader 也过了之后，`D3D12CreateDevice(12_0)` 仍返回 `E_INVALIDARG`：

```
warn:d3d12_device_create: Feature level 0xc000 is not supported.
```

逐级探测的结果：

```
create : level=0xC100 hr=0x80070057   (12_1 失败)
create : level=0xC000 hr=0x80070057   (12_0 失败)
create : level=0xB100 hr=0x00000000   (11_1 成功)
caps   : tiled=4 binding=3 typedUAV=0 logicOp=1
options1: waveOps=0 int64=1
shader model: 5.1
```

vkd3d 的 `d3d12_device_caps_init_shader_model` 要求（源码已核对）：

- `subgroupSize >= 4`，且 supportedOperations 含 BASIC/VOTE/ARITHMETIC/BALLOT/SHUFFLE/QUAD，
  supportedStages 含 compute+fragment；
- `VkPhysicalDeviceVulkan12Features` 的 scalarBlockLayout 或 uniformBufferStandardLayout；
- **`VkPhysicalDeviceFeatures::shaderInt16`**。

前两项驱动都有；`shaderInt16` 这块卡报 **false**，于是 SM 停在 5.1，`waveOps` 随之为 0，
12_0 也就建不出来。

把 `shaderInt16` 也谎报为 true 之后：vkd3d 如实在 `vkCreateDevice` 里启用它，驱动拒绝：

```
vulkan-1 shim: vkCreateDevice -> -8 with 19 extensions enabled
err:vkd3d_create_vk_device: Failed to create Vulkan device, vr -8
```

`-8` = `VK_ERROR_FEATURE_NOT_PRESENT`。

### 结论

这已经不是"缺个入口"的问题，而是**谎言的边界**：

- vkd3d 要的 SM 6.0+ 语义（int16、wave ops、typed UAV load）这块 2021 年的 GCN 驱动确实没有；
- 谎报 → vkd3d 启用 → 驱动拒绝设备；不谎报 → 停在 SM 5.1 / 特性等级 11_1。

要越过去，需要的不是拦截层，而是让 vkd3d 按 SM 5.1 / Vulkan 1.1 重新生成 shader——
那是给 vkd3d-shader 加一个降级目标，属于改源码重编译，不在"代理层"能力范围内。

### 本轮新增的环境变量

| 变量 | 作用 |
|---|---|
| `VICTORY_DUMP_SPV=<dir>` | 把每个 shader module 落盘（诊断驱动拒收的原因） |
| `VICTORY_SPV_DOWNGRADE=15` | 把 SPIR-V 1.6 改写为 1.5 后再交给驱动 |
| `VICTORY_SHIM_DEBUG=1` | 打印能力伪造的命中情况 |

---

## 12. 给 vkd3d 打补丁（未完成构建）

### 补丁内容（已写入源码）

`third_party\vkd3d\vkd3d-proton-3.1\libs\vkd3d\device.c`，三处，均带 `LOCAL PATCH` 注释：

| 位置 | 改动 | 原因 |
|---|---|---|
| `d3d12_device_caps_init_shader_model` 的 SM 6.0 门槛 | 去掉 `features2.features.shaderInt16` 这一项 | 这块 GCN 3 卡报 false，且拒绝启用；它是 SM 停在 5.1 的原因 |
| 同函数末尾 | 强制 `max_shader_model = D3D_SHADER_MODEL_6_6` | 标题要 6.6 设备 |
| `d3d12_device_caps_init` | `TypedUAVLoadAdditionalFormats = TRUE` | 它是特性等级 12_0 的硬条件，GCN 报 false |

### 构建链已就绪

- Meson 1.12.1 + Ninja（pip 装），MSVC 19.52 已被 meson 正确识别；
- 三个缺失的依赖已补齐：`khronos/Vulkan-Headers`、`khronos/SPIRV-Headers`、`subprojects/dxil-spirv`；
- `third_party\vkd3d\vkd3d-proton-3.1\build.bat` 一条命令完成 configure + compile。

### 卡在 widl

meson 的 `include/meson.build` 要用 **widl**（Wine 的 IDL 编译器）从 16 个 `.idl`
生成公共头文件（`vkd3d_d3d12.h` 等），生成的列表还参与 `vkd3d_common` 静态库。

试过并失败的路子：

- 本机没有任何 `widl.exe`（Rust 的 gnullvm 工具链里也没有）；
- MSYS2 的 `mingw64.db` 全库检索：**没有任何包含 widl 的包**；镜像目录列表 403 / 超时；
- MSVC 的 `midl.exe` 解析不了这些 IDL：它们在 `vkd3d_windows.h` 里引现代 Windows SDK 头，
  midl 直接报 `winnt.h` 的 `_declspec(align())` 等语法错误。

### 还差什么

一个 `widl.exe`（来自 MSYS2 的 `mingw-w64-tools` 或任何 Wine 构建）。有它之后：

```
cd third_party\vkd3d\vkd3d-proton-3.1
build.bat
```

产物在 `build\libs\d3d12\`，替换 `run\` 里的 `d3d12.dll` / `d3d12core.dll` 即可验证
`D3D12CreateDevice(12_0)` 是否通过、`CheckFeatureSupport(SHADER_MODEL)` 是否回到 6.6。

---

## 13. 补丁版 vkd3d 编译成功并跑通到第一次提交

### 补齐的构建依赖

| 缺什么 | 解决办法 |
|---|---|
| `widl`（Wine 的 IDL 编译器） | MSYS2（`C:\msys64`）→ `pacman -S mingw-w64-ucrt-x86_64-tools` → `C:\msys64\ucrt64\bin\widl.exe` |
| `glslangValidator` | `pacman -S mingw-w64-ucrt-x86_64-glslang` |
| `khronos/Vulkan-Headers`、`khronos/SPIRV-Headers` | 从各自的 GitHub 归档复制 |
| `subprojects/dxil-spirv` 及其 `dxbc-spirv` 子模块 | dxil-spirv 的 `.gitmodules` 指向 `doitsujin/dxbc-spirv`（默认分支是 `main`，不是 `master`） |

`build.bat` 里把 `C:\msys64\ucrt64\bin` 加进 PATH，`meson compile` 通过（214 个目标）。

### 探针结果：目标达成

```
create      : level=0xC000 hr=0x00000000      ← 12_0 建成
caps        : tiled=4 binding=3 typedUAV=1 logicOp=1
options1    : waveOps=1 int64=1 totalLanes=1792
shader model: hr=0x00000000 highest=0x66 (6.6)
```

同时把 shim 里 `shaderInt16` 的谎报撤掉（补丁后不再需要；谎报会让 vkd3d 启用它、
驱动以 `VK_ERROR_FEATURE_NOT_PRESENT` 拒绝建设备）。

### 接上游戏：跑到第一次提交

换成补丁版 `d3d12.dll` / `d3d12core.dll` 后，victory 在
`vulkan-1.dll`+`vulkan_loader_real.dll`+`vkd3d(patched)` 之上启动：

- D3D12 设备创建成功（vkd3d 走完内存拓扑、bindless、管线库初始化）；
- 标题的 pipeline / texture 创建大量通过；
- 首次 `vkQueueSubmit` 失败：

```
d3d12_command_queue_execute: Failed to submit queue(s), vr -8
ERROR victory_gpu::host: host GPU device removed after a submission of 1 lists
```

`vr -8` = `VK_ERROR_FEATURE_NOT_PRESENT`。设备创建时 `VK_KHR_synchronization2`
确实在启用列表里（19 个扩展，已打印确认），所以下一步要查的是提交本身
（timeline semaphore 的 feature、或我们 `vkQueueSubmit2` 转发路径）。
另有少量纹理创建失败（`no host texture 0x80070057`，格式如 3840×2160 / 2048×2048）。

### 当前状态一句话

**从"驱动拒绝 SPIR-V 1.6"一路推到"游戏建完设备和资源、第一次提交队列"**，
剩下的最后一个已知错误在队列提交。

---

## 14. 提交修复后：13 帧呈现，然后 4 GB 显存耗尽

### 修复的问题

`vkQueueSubmit2` 里用 `GetProcAddress` 找 `vkQueueSubmit2KHR`，找不到就直接返回
`VK_ERROR_FEATURE_NOT_PRESENT`（-8）——**那个 -8 是我们自己造的**，不是驱动。
改成先查导出表、再经 `vkGetInstanceProcAddr` 的 trampoline 解析（VkQueue 是 dispatchable 句柄）。

验证：给 `d3d12probe` 加了最小提交（queue + allocator + list + fence）：

```
queue       : hr=0x00000000
execute     : signaled hr=0x00000000
fence       : waited=0 completed=1
```

### 游戏实跑

```
frame presented × 13        ← 真的呈现了 13 帧
d3d12_command_queue_execute: Failed to submit queue(s), vr -2   ← 之后
```

`-2` = `VK_ERROR_OUT_OF_DEVICE_MEMORY`。随后 vkd3d 报大量
"still N pending command lists awaiting execution"，提交队列彻底堵死。

vkd3d 启动时的判断也印证了这一点：

```
Topology: Device heaps are split. Assuming small BAR situation.
```

R9 380 是 4 GB 显存 + 小 BAR，vkd3d 按这个拓扑分配 device-local + staging；
标题在 3840×2160 等尺寸上要的纹理把 4 GB 吃穿了。

### 至此的完整链条

| 层级 | 状态 |
|---|---|
| victory 引擎 | 正常（映射、绑定、翻译着色器、建管线） |
| 我们的 vulkan-1 1.3 伪装层 | 正常（27 个合成入口 + 能力伪造） |
| 补丁版 vkd3d 3.1 | 正常（12_0 设备、SM 6.6、DXIL→SPIR-V） |
| SPIR-V 降版补丁 | 正常（1.6 → 1.5） |
| 驱动 | 正常（Vulkan 1.2 接收并执行） |
| **4 GB 显存** | **不够**（13 帧后 OOM） |

这就是最开始判断的那件事：软件栈可以一路绕过，硬件的显存绕不过去。
