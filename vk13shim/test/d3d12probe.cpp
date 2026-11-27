// Minimal D3D12 probe: which implementation answers, and what shader model
// does it report? Run it with vkd3d-proton's d3d12.dll next to the exe, with
// the vulkan-1 shim and the real loader beside that, to see whether the shim's
// Vulkan 1.3 claim survives contact with a real D3D12 implementation.
#include <windows.h>

#include <d3d12.h>
#include <cstdio>

#pragma comment(lib, "d3d12.lib")

static const char* model_name(D3D_SHADER_MODEL m) {
    switch (m) {
        case D3D_SHADER_MODEL_5_1: return "5.1";
        case D3D_SHADER_MODEL_6_0: return "6.0";
        case D3D_SHADER_MODEL_6_1: return "6.1";
        case D3D_SHADER_MODEL_6_2: return "6.2";
        case D3D_SHADER_MODEL_6_3: return "6.3";
        case D3D_SHADER_MODEL_6_4: return "6.4";
        case D3D_SHADER_MODEL_6_5: return "6.5";
        case D3D_SHADER_MODEL_6_6: return "6.6";
        case D3D_SHADER_MODEL_6_7: return "6.7";
        case D3D_SHADER_MODEL_6_8: return "6.8";
        default: return "other";
    }
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);

    HMODULE d3d12 = GetModuleHandleW(L"d3d12.dll");
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(d3d12, path, MAX_PATH);
    std::printf("d3d12.dll   : %ls\n", path);

    ID3D12Device* device = nullptr;
    HRESULT hr = E_FAIL;
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_12_0,
                                        D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    D3D_FEATURE_LEVEL got = D3D_FEATURE_LEVEL_11_0;
    for (D3D_FEATURE_LEVEL level : levels) {
        hr = D3D12CreateDevice(nullptr, level, __uuidof(ID3D12Device),
                               reinterpret_cast<void**>(&device));
        std::printf("create      : level=0x%04X hr=0x%08lX\n", unsigned(level), hr);
        if (SUCCEEDED(hr) && device) { got = level; break; }
    }
    std::printf("created at  : 0x%04X device=%p\n", unsigned(got), static_cast<void*>(device));
    if (!device) return 1;

    D3D12_FEATURE_DATA_D3D12_OPTIONS opts{};
    if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &opts, sizeof(opts)))) {
        std::printf("caps        : tiled=%d binding=%d typedUAV=%d logicOp=%d\n",
                    int(opts.TiledResourcesTier), int(opts.ResourceBindingTier),
                    int(opts.TypedUAVLoadAdditionalFormats), int(opts.OutputMergerLogicOp));
    }
    D3D12_FEATURE_DATA_D3D12_OPTIONS1 o1{};
    if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS1, &o1, sizeof(o1)))) {
        std::printf("options1    : waveOps=%d int64=%d totalLanes=%u\n", int(o1.WaveOps),
                    int(o1.Int64ShaderOps), o1.TotalLaneCount);
    }

    D3D12_FEATURE_DATA_SHADER_MODEL model{D3D_SHADER_MODEL_6_8};
    hr = device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &model, sizeof(model));
    std::printf("shader model: hr=0x%08lX highest=0x%02X (%s)\n", hr, unsigned(model.HighestShaderModel),
                model_name(static_cast<D3D_SHADER_MODEL>(model.HighestShaderModel)));

    D3D12_FEATURE_DATA_D3D12_OPTIONS7 options7{};
    hr = device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS7, &options7, sizeof(options7));
    std::printf("options7    : hr=0x%08lX mesh_tier=%d sampler_feedback=%d\n", hr,
                int(options7.MeshShaderTier), int(options7.SamplerFeedbackTier));

    D3D12_FEATURE_DATA_ARCHITECTURE arch{};
    arch.NodeIndex = 0;
    hr = device->CheckFeatureSupport(D3D12_FEATURE_ARCHITECTURE, &arch, sizeof(arch));
    std::printf("architecture: hr=0x%08lX uma=%d tile_based=%d\n", hr, int(arch.UMA),
                int(arch.TileBasedRenderer));

    D3D12_FEATURE_DATA_GPU_VIRTUAL_ADDRESS_SUPPORT gva{};
    hr = device->CheckFeatureSupport(D3D12_FEATURE_GPU_VIRTUAL_ADDRESS_SUPPORT, &gva, sizeof(gva));
    std::printf("gpu va      : max_bits=%u max_per_resource=%u\n", gva.MaxGPUVirtualAddressBitsPerProcess,
                gva.MaxGPUVirtualAddressBitsPerResource);

    // A minimal submit: queue, allocator, one empty command list, a fence.
    ID3D12CommandQueue* queue = nullptr;
    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    hr = device->CreateCommandQueue(&qd, __uuidof(ID3D12CommandQueue), reinterpret_cast<void**>(&queue));
    std::printf("queue       : hr=0x%08lX\n", hr);
    if (SUCCEEDED(hr) && queue) {
        ID3D12CommandAllocator* alloc = nullptr;
        device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                       __uuidof(ID3D12CommandAllocator), reinterpret_cast<void**>(&alloc));
        ID3D12GraphicsCommandList* list = nullptr;
        device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, nullptr,
                                  __uuidof(ID3D12GraphicsCommandList), reinterpret_cast<void**>(&list));
        if (list) list->Close();
        ID3D12Fence* fence = nullptr;
        device->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence),
                            reinterpret_cast<void**>(&fence));
        if (list && fence) {
            ID3D12CommandList* lists[] = {list};
            queue->ExecuteCommandLists(1, lists);
            hr = queue->Signal(fence, 1);
            std::printf("execute     : signaled hr=0x%08lX\n", hr);
            HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            fence->SetEventOnCompletion(1, event);
            DWORD waited = WaitForSingleObject(event, 5000);
            std::printf("fence       : waited=%lu completed=%llu\n", waited,
                        unsigned long long(fence->GetCompletedValue()));
            CloseHandle(event);
        }
    }

    device->Release();
    std::printf("done\n");
    return 0;
}
