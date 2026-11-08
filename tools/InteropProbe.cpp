#include "../src/backend/GpuInterop.h"
#include "../src/layer/Graphics.h"
#include "../src/layer/Support.h"
#include <cstdio>
#include <cstdarg>
#include <vector>
namespace tf2vrnr {
void Log(const char* format, ...) {
    va_list args; va_start(args, format); std::vfprintf(stdout, format, args); va_end(args);
    std::fputc('\n', stdout); std::fflush(stdout);
}
}
using Microsoft::WRL::ComPtr;
int main(int argc, char** argv) {
    if (argc != 2) return 2;
    const std::string path(argv[1]); const std::wstring directory(path.begin(), path.end());
    if (!std::freopen((path + "\\interop-probe.log").c_str(), "w", stdout)) return 3;
    ComPtr<ID3D11Device> device; ComPtr<ID3D11DeviceContext> context;
    const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
    auto hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &level, 1,
        D3D11_SDK_VERSION, device.GetAddressOf(), nullptr, context.GetAddressOf());
    if (FAILED(hr)) { tf2vrnr::Log("device_failed hr=%lx", static_cast<unsigned long>(hr)); return 1; }
    tf2vrnr::GpuInterop interop;
    if (!interop.Init(device.Get())) return 1;
    constexpr unsigned width = 257, height = 129;
    struct Saved { std::shared_ptr<tf2vrnr::CapturedImage> image; std::wstring filename; };
    std::vector<Saved> saved;
    for (auto format : {DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R16G16_FLOAT, DXGI_FORMAT_R32_FLOAT}) {
        tf2vrnr::SharedTexture input[2], output[2]; ComPtr<ID3D11RenderTargetView> views[2];
        for (unsigned eye = 0; eye < 2; ++eye) {
            if (!interop.CreateTexture(width, height, format, input[eye]) ||
                !interop.CreateTexture(width, height, format, output[eye]) ||
                FAILED(device->CreateRenderTargetView(input[eye].d11.Get(), nullptr, views[eye].GetAddressOf()))) return 1;
        }
        for (unsigned frame = 0; frame < 6; ++frame) for (unsigned n = 0; n < 2; ++n) {
            const unsigned eye = (frame & 1) ? 1-n : n;
            const float values[] = {.125f*(frame+1)+.03125f*eye, -float(frame+1)-.25f*eye, .5f, 1.f};
            context->ClearRenderTargetView(views[eye].Get(), values);
            auto* list = interop.Begin(); if (!list) return 1;
            interop.Transition(list, input[eye].d12.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
            interop.Transition(list, output[eye].d12.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
            list->CopyResource(output[eye].d12.Get(), input[eye].d12.Get());
            interop.Transition(list, input[eye].d12.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
            interop.Transition(list, output[eye].d12.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
            if (!interop.Submit()) return 1;
            auto image = tf2vrnr::CaptureImage(device.Get(), output[eye].d11.Get(), format, false, false);
            if (!image) return 1;
            saved.push_back({std::move(image), directory + L"\\format" + std::to_wstring(format) +
                L"-frame" + std::to_wstring(frame) + L"-eye" + std::to_wstring(eye) + L".dds"});
            tf2vrnr::Log("gpu_round_trip_queued format=%u frame=%u eye=%u", unsigned(format), frame, eye);
        }
        if (!interop.Drain()) return 1;
        context->Flush();
    }
    const tf2vrnr::ViewLayout layout{0,0,0,0,width,height};
    for (auto& item : saved) if (!item.image->WriteView(layout, item.filename)) return 1;
    if (!interop.Drain()) return 1;
    tf2vrnr::Log("interop_probe_complete=1 files=%zu same_adapter=1 independent_eyes=1 allocator_reuse=1 cpu_transfer_between_apis=0 payload_verification_required=1", saved.size());
    return 0;
}
