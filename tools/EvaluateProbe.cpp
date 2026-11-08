// MIT. Synthetic stationary planes, not captured game guides or a VR benchmark.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <wrl/client.h>
#include <nvsdk_ngx_defs.h>
#include <nvsdk_ngx_defs_dlssnr.h>
#include <array>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <vector>

extern "C" void tf2vrnr_parameter_set_int(void*, const char*, int);
extern "C" void tf2vrnr_parameter_set_uint(void*, const char*, unsigned int);
extern "C" void tf2vrnr_parameter_set_float(void*, const char*, float);
extern "C" void tf2vrnr_parameter_set_resource(void*, const char*, ID3D12Resource*);
using Microsoft::WRL::ComPtr;
namespace {
constexpr UINT Width = 1280, Height = 720;
bool Check(FILE* log, HRESULT hr, const char* operation) {
    if (SUCCEEDED(hr)) return true;
    std::fprintf(log, "gpu_failure operation=%s hr=0x%08X\n", operation, static_cast<unsigned>(hr));
    std::fflush(log);
    return false;
}
struct Gpu {
    ID3D12Device* device;
    FILE* log;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    HANDLE event = nullptr;
    UINT64 serial = 0;
    ~Gpu() { if (event) CloseHandle(event); }
    bool Init() {
        D3D12_COMMAND_QUEUE_DESC q{}; q.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        if (!Check(log, device->CreateCommandQueue(&q, IID_PPV_ARGS(queue.GetAddressOf())), "queue") ||
            !Check(log, device->CreateCommandAllocator(q.Type, IID_PPV_ARGS(allocator.GetAddressOf())), "allocator") ||
            !Check(log, device->CreateCommandList(0, q.Type, allocator.Get(), nullptr, IID_PPV_ARGS(list.GetAddressOf())), "list") ||
            !Check(log, list->Close(), "initial_close") ||
            !Check(log, device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(fence.GetAddressOf())), "fence")) return false;
        event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        return event != nullptr;
    }
    bool Begin() {
        return Check(log, allocator->Reset(), "allocator_reset") &&
               Check(log, list->Reset(allocator.Get(), nullptr), "list_reset");
    }
    bool Submit() {
        if (!Check(log, list->Close(), "list_close")) return false;
        ID3D12CommandList* commands[] = {list.Get()};
        queue->ExecuteCommandLists(1, commands);
        if (!Check(log, queue->Signal(fence.Get(), ++serial), "signal") ||
            !Check(log, fence->SetEventOnCompletion(serial, event), "fence_event")) return false;
        if (WaitForSingleObject(event, 30000) != WAIT_OBJECT_0) {
            std::fprintf(log, "gpu_fence_timeout device_removed=0x%08X\n", static_cast<unsigned>(device->GetDeviceRemovedReason()));
            std::fflush(log);
            return false;
        }
        return Check(log, device->GetDeviceRemovedReason(), "device_status");
    }
    void Transition(ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
        D3D12_RESOURCE_BARRIER b{}; b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition = {resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after};
        list->ResourceBarrier(1, &b);
    }
    void Uav(ID3D12Resource* resource) {
        D3D12_RESOURCE_BARRIER b{}; b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV; b.UAV.pResource = resource;
        list->ResourceBarrier(1, &b);
    }
};
bool Buffer(Gpu& g, UINT64 bytes, D3D12_HEAP_TYPE heapType, D3D12_RESOURCE_STATES state, ComPtr<ID3D12Resource>& result) {
    D3D12_HEAP_PROPERTIES heap{}; heap.Type = heapType; heap.CreationNodeMask = heap.VisibleNodeMask = 1;
    D3D12_RESOURCE_DESC d{}; d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; d.Width = bytes;
    d.Height = d.DepthOrArraySize = d.MipLevels = 1; d.SampleDesc.Count = 1; d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    return Check(g.log, g.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &d, state, nullptr,
        IID_PPV_ARGS(result.GetAddressOf())), "buffer");
}
struct Texture {
    ComPtr<ID3D12Resource> resource, upload;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT64 bytes = 0;
};
bool Upload(Gpu& g, Texture& t, DXGI_FORMAT format, UINT bpp, const void* pixels, D3D12_RESOURCE_STATES finalState) {
    D3D12_RESOURCE_DESC d{}; d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = Width; d.Height = Height; d.DepthOrArraySize = d.MipLevels = 1;
    d.Format = format; d.SampleDesc.Count = 1; d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_DEFAULT; heap.CreationNodeMask = heap.VisibleNodeMask = 1;
    if (!Check(g.log, g.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &d,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(t.resource.GetAddressOf())), "texture")) return false;
    g.device->GetCopyableFootprints(&d, 0, 1, 0, &t.footprint, nullptr, nullptr, &t.bytes);
    if (!Buffer(g, t.bytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, t.upload)) return false;
    void* mapped = nullptr; D3D12_RANGE none{};
    if (!Check(g.log, t.upload->Map(0, &none, &mapped), "upload_map")) return false;
    std::memset(mapped, 0, static_cast<size_t>(t.bytes));
    for (UINT y = 0; y < Height; ++y)
        std::memcpy(static_cast<char*>(mapped) + t.footprint.Offset + y * t.footprint.Footprint.RowPitch,
            static_cast<const char*>(pixels) + static_cast<size_t>(y) * Width * bpp, Width * bpp);
    t.upload->Unmap(0, nullptr);
    D3D12_TEXTURE_COPY_LOCATION src{}, dst{};
    src.pResource = t.upload.Get(); src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; src.PlacedFootprint = t.footprint;
    dst.pResource = t.resource.Get(); dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    g.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    g.Transition(t.resource.Get(), D3D12_RESOURCE_STATE_COPY_DEST, finalState);
    return true;
}
uint16_t Half(float value) {
    uint32_t bits; std::memcpy(&bits, &value, 4);
    const int exponent = static_cast<int>((bits >> 23) & 255) - 127 + 15;
    if (exponent <= 0) return 0;
    return static_cast<uint16_t>((exponent << 10) | ((bits & 0x7fffff) >> 13));
}
float Float(uint16_t h) {
    const int exponent = (h >> 10) & 31; const unsigned mantissa = h & 1023;
    if (exponent == 31) return mantissa ? NAN : ((h & 0x8000) ? -INFINITY : INFINITY);
    const float value = exponent ? std::ldexp(1.0f + mantissa / 1024.0f, exponent - 15) : std::ldexp(static_cast<float>(mantissa), -24);
    return (h & 0x8000) ? -value : value;
}
bool Save(const std::filesystem::path& path, const void* data, size_t bytes) {
    FILE* f = _wfopen(path.c_str(), L"wb"); if (!f) return false;
    const bool written = std::fwrite(data, 1, bytes, f) == bytes;
    const bool closed = std::fclose(f) == 0;
    return written && closed;
}
struct Eye {
    Texture color, depth, motion, output;
    NVSDK_NGX_Parameter* params = nullptr;
    NVSDK_NGX_Handle* feature = nullptr;
    std::vector<uint16_t> input;
    ComPtr<ID3D12Resource> readback;
    uint64_t outputHash = 0;
};
void Subrect(void* p, const char* prefix) {
    char key[80];
    for (const auto& pair : {std::pair<const char*, UINT>{"SubrectBaseX", 0}, {"SubrectBaseY", 0}, {"SubrectWidth", Width}, {"SubrectHeight", Height}}) {
        std::snprintf(key, sizeof(key), "DLSSNR.%s%s", prefix, pair.first);
        tf2vrnr_parameter_set_uint(p, key, pair.second);
    }
}
}

bool EvaluateProbe(FILE* log, HMODULE core, ID3D12Device* device, const std::filesystem::path& directory, bool cleanShutdown) {
    using Allocate = NVSDK_NGX_Result(NVSDK_CONV *)(NVSDK_NGX_Parameter**);
    using Create = NVSDK_NGX_Result(NVSDK_CONV *)(ID3D12GraphicsCommandList*, NVSDK_NGX_Feature, NVSDK_NGX_Parameter*, NVSDK_NGX_Handle**);
    using Evaluate = NVSDK_NGX_Result(NVSDK_CONV *)(ID3D12GraphicsCommandList*, const NVSDK_NGX_Handle*, const NVSDK_NGX_Parameter*, PFN_NVSDK_NGX_ProgressCallback);
    using Release = NVSDK_NGX_Result(NVSDK_CONV *)(NVSDK_NGX_Handle*);
    using Destroy = NVSDK_NGX_Result(NVSDK_CONV *)(NVSDK_NGX_Parameter*);
    const auto allocate = reinterpret_cast<Allocate>(GetProcAddress(core, "NVSDK_NGX_D3D12_AllocateParameters"));
    const auto create = reinterpret_cast<Create>(GetProcAddress(core, "NVSDK_NGX_D3D12_CreateFeature"));
    const auto evaluate = reinterpret_cast<Evaluate>(GetProcAddress(core, "NVSDK_NGX_D3D12_EvaluateFeature"));
    const auto release = reinterpret_cast<Release>(GetProcAddress(core, "NVSDK_NGX_D3D12_ReleaseFeature"));
    const auto destroy = reinterpret_cast<Destroy>(GetProcAddress(core, "NVSDK_NGX_D3D12_DestroyParameters"));
    if (!allocate || !create || !evaluate || !release || (cleanShutdown && !destroy)) return false;
    std::fprintf(log, "synthetic_test_begin size=%ux%u eyes=2 frames=4 stationary_planes=true headset_benchmark=false\n", Width, Height);
    std::fflush(log);
    Gpu g{device, log, {}, {}, {}, {}, nullptr, 0};
    if (!g.Init() || !g.Begin()) return false;
    std::array<Eye, 2> eyes;
    std::vector<float> depth(static_cast<size_t>(Width) * Height, 0.5f);
    std::vector<uint32_t> motion(depth.size(), 0);
    std::vector<uint16_t> zero(depth.size() * 4, 0);
    for (size_t eye = 0; eye < eyes.size(); ++eye) {
        auto& e = eyes[eye]; e.input.resize(depth.size() * 4);
        for (UINT y = 0; y < Height; ++y) for (UINT x = 0; x < Width; ++x) {
            const float fx = static_cast<float>(x) / Width, fy = static_cast<float>(y) / Height;
            const float tile = (((x + eye * 24) / 96 + y / 96) & 1) ? 0.16f : 0.0f;
            auto* pixel = e.input.data() + (static_cast<size_t>(y) * Width + x) * 4;
            pixel[0] = Half(0.12f + 0.5f * fx + tile + eye * 0.08f);
            pixel[1] = Half(0.1f + 0.5f * fy + tile);
            pixel[2] = Half(0.15f + 0.3f * (1.0f - fx) + tile);
            pixel[3] = Half(1.0f);
        }
        if (!Save(directory / (L"synthetic-eye" + std::to_wstring(eye) + L"-input.rgba16f"), e.input.data(), e.input.size() * 2) ||
            !Upload(g, e.color, DXGI_FORMAT_R16G16B16A16_FLOAT, 8, e.input.data(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE) ||
            !Upload(g, e.depth, DXGI_FORMAT_R32_FLOAT, 4, depth.data(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE) ||
            !Upload(g, e.motion, DXGI_FORMAT_R16G16_FLOAT, 4, motion.data(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE) ||
            !Upload(g, e.output, DXGI_FORMAT_R16G16B16A16_FLOAT, 8, zero.data(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS)) return false;
    }
    if (!g.Submit()) return false;
    for (size_t eye = 0; eye < eyes.size(); ++eye) {
        auto& e = eyes[eye];
        auto result = allocate(&e.params);
        if (result != NVSDK_NGX_Result_Success || !e.params) return false;
        tf2vrnr_parameter_set_uint(e.params, NVSDK_NGX_Parameter_DLSSNR_Width, Width);
        tf2vrnr_parameter_set_uint(e.params, NVSDK_NGX_Parameter_DLSSNR_Height, Height);
        tf2vrnr_parameter_set_uint(e.params, NVSDK_NGX_Parameter_DLSSNR_Hint_Render_Preset, NVSDK_NGX_DLSSNR_Hint_Render_Preset_B);
        tf2vrnr_parameter_set_float(e.params, NVSDK_NGX_Parameter_DLSSNR_ScalingRatio, 1.0f);
        if (!g.Begin()) return false;
        std::fprintf(log, "feature_create_begin eye=%zu\n", eye); std::fflush(log);
        result = create(g.list.Get(), NVSDK_NGX_Feature_DLSSNR, e.params, &e.feature);
        std::fprintf(log, "feature_create eye=%zu result=0x%08X handle=%p\n", eye, static_cast<unsigned>(result), static_cast<void*>(e.feature)); std::fflush(log);
        if (result != NVSDK_NGX_Result_Success || !e.feature || !g.Submit()) return false;
        tf2vrnr_parameter_set_resource(e.params, NVSDK_NGX_Parameter_DLSSNR_Color, e.color.resource.Get());
        tf2vrnr_parameter_set_resource(e.params, NVSDK_NGX_Parameter_DLSSNR_Depth, e.depth.resource.Get());
        tf2vrnr_parameter_set_resource(e.params, NVSDK_NGX_Parameter_DLSSNR_MVec, e.motion.resource.Get());
        tf2vrnr_parameter_set_resource(e.params, NVSDK_NGX_Parameter_DLSSNR_Output, e.output.resource.Get());
        for (const char* name : {"Color", "Depth", "MVec", "Output"}) Subrect(e.params, name);
        tf2vrnr_parameter_set_int(e.params, NVSDK_NGX_Parameter_DLSSNR_Enabled, 1);
        tf2vrnr_parameter_set_int(e.params, NVSDK_NGX_Parameter_DLSSNR_DepthInverted, 0);
        tf2vrnr_parameter_set_int(e.params, NVSDK_NGX_Parameter_DLSSNR_UICorrection, 0);
        tf2vrnr_parameter_set_int(e.params, NVSDK_NGX_Parameter_DLSSNR_UseAutoMask, 0);
        tf2vrnr_parameter_set_uint(e.params, NVSDK_NGX_Parameter_DLSSNR_Style, 0);
        tf2vrnr_parameter_set_float(e.params, NVSDK_NGX_Parameter_DLSSNR_Intensity, 0.8f);
        tf2vrnr_parameter_set_float(e.params, NVSDK_NGX_Parameter_DLSSNR_LocalToneStrength, 1.0f);
        tf2vrnr_parameter_set_float(e.params, NVSDK_NGX_Parameter_DLSSNR_LocalStructureStrength, 1.0f);
        tf2vrnr_parameter_set_float(e.params, NVSDK_NGX_Parameter_DLSSNR_SkinStructureStrength, 1.0f);
        tf2vrnr_parameter_set_float(e.params, NVSDK_NGX_Parameter_DLSSNR_MVecScaleX, 1.0f);
        tf2vrnr_parameter_set_float(e.params, NVSDK_NGX_Parameter_DLSSNR_MVecScaleY, 1.0f);
    }
    if (eyes[0].feature == eyes[1].feature) return false;
    for (unsigned frame = 0; frame < 4; ++frame) {
        if (!g.Begin()) return false;
        const auto start = std::chrono::steady_clock::now();
        for (size_t n = 0; n < eyes.size(); ++n) {
            const size_t index = (frame & 1) ? 1 - n : n; auto& e = eyes[index];
            tf2vrnr_parameter_set_int(e.params, NVSDK_NGX_Parameter_DLSSNR_Reset, frame == 0 ? 1 : 0);
            g.Uav(e.output.resource.Get());
            std::fprintf(log, "evaluate_begin frame=%u eye=%zu\n", frame, index); std::fflush(log);
            const auto result = evaluate(g.list.Get(), e.feature, e.params, nullptr);
            std::fprintf(log, "evaluate frame=%u eye=%zu result=0x%08X\n", frame, index, static_cast<unsigned>(result)); std::fflush(log);
            if (result != NVSDK_NGX_Result_Success) return false;
            g.Uav(e.output.resource.Get());
        }
        if (!g.Submit()) return false;
        const double elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        std::fprintf(log, "synthetic_frame_complete frame=%u cpu_gpu_wait_ms=%.3f\n", frame, elapsed); std::fflush(log);
    }
    if (!g.Begin()) return false;
    for (auto& e : eyes) {
        if (!Buffer(g, e.output.bytes, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST, e.readback)) return false;
        g.Transition(e.output.resource.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION src{}, dst{};
        src.pResource = e.output.resource.Get(); src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.pResource = e.readback.Get(); dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; dst.PlacedFootprint = e.output.footprint;
        g.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    }
    if (!g.Submit()) return false;
    bool passed = true;
    for (size_t index = 0; index < eyes.size(); ++index) {
        auto& e = eyes[index]; void* mapped = nullptr;
        D3D12_RANGE range{0, static_cast<SIZE_T>(e.output.bytes)};
        if (!Check(log, e.readback->Map(0, &range, &mapped), "readback_map")) return false;
        std::vector<uint16_t> output(e.input.size());
        for (UINT y = 0; y < Height; ++y)
            std::memcpy(output.data() + static_cast<size_t>(y) * Width * 4,
                static_cast<const char*>(mapped) + e.output.footprint.Offset + y * e.output.footprint.Footprint.RowPitch, Width * 8);
        D3D12_RANGE none{}; e.readback->Unmap(0, &none);
        double difference = 0; size_t nonfinite = 0; float minimum = INFINITY, maximum = -INFINITY;
        uint64_t hash = 14695981039346656037ull;
        for (size_t i = 0; i < output.size(); ++i) {
            hash ^= output[i]; hash *= 1099511628211ull;
            if (i % 4 == 3) continue;
            const float value = Float(output[i]);
            if (!std::isfinite(value)) { ++nonfinite; continue; }
            minimum = std::min(minimum, value); maximum = std::max(maximum, value);
            difference += std::abs(value - Float(e.input[i]));
        }
        e.outputHash = hash;
        const bool saved = Save(directory / (L"synthetic-eye" + std::to_wstring(index) + L"-output.rgba16f"), output.data(), output.size() * 2);
        std::fprintf(log, "readback eye=%zu nonfinite_rgb=%zu rgb_min=%.6f rgb_max=%.6f mean_abs_change=%.8f hash=0x%016llX saved=%u\n",
            index, nonfinite, minimum, maximum, difference / (Width * Height * 3.0), static_cast<unsigned long long>(hash), saved);
        passed = passed && saved && nonfinite == 0 && maximum - minimum > 0.02f;
        std::fflush(log);
    }
    passed = passed && eyes[0].outputHash != eyes[1].outputHash;
    for (size_t index = 0; index < eyes.size(); ++index) {
        std::fprintf(log, "feature_release_begin eye=%zu\n", index); std::fflush(log);
        const auto result = release(eyes[index].feature);
        std::fprintf(log, "feature_release eye=%zu result=0x%08X\n", index, static_cast<unsigned>(result)); std::fflush(log);
        passed = passed && result == NVSDK_NGX_Result_Success;
        if (cleanShutdown && result == NVSDK_NGX_Result_Success) {
            std::fprintf(log, "feature_parameters_destroy_begin eye=%zu\n", index); std::fflush(log);
            const auto status = destroy(eyes[index].params);
            std::fprintf(log, "feature_parameters_destroy eye=%zu result=0x%08X\n", index, static_cast<unsigned>(status)); std::fflush(log);
            passed = passed && status == NVSDK_NGX_Result_Success;
        }
    }
    std::fprintf(log, "synthetic_evaluation_passed=%u distinct_feature_handles=1 distinct_outputs=%u vr_guides_validated=0\n", passed, eyes[0].outputHash != eyes[1].outputHash);
    std::fflush(log);
    return passed;
}
