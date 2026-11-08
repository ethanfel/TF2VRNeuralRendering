// Standalone D3D11 validation of the real capture path. Use an isolated prefix.
// Known color/depth/HDR values are copied, then originals are overwritten before
// readback. The caller verifies the DDS payloads and JSON reports independently.
#include "../src/layer/Graphics.h"
#include "../src/layer/Support.h"
#include "../src/layer/DepthCapture.h"
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <atomic>
#include <future>
#include <vector>
#include <d3dcompiler.h>
#include <wrl/client.h>
using Microsoft::WRL::ComPtr;
namespace tf2vrnr {
void Log(const char* format, ...) {
    va_list args; va_start(args, format); std::vfprintf(stdout, format, args); va_end(args);
    std::fputc('\n', stdout); std::fflush(stdout);
}
}
namespace {
bool Ok(HRESULT hr, const char* name) {
    if (FAILED(hr)) std::printf("FAILED %s hr=0x%08lx\n", name, static_cast<unsigned long>(hr));
    return SUCCEEDED(hr);
}
bool WriteAsync(const std::shared_ptr<tf2vrnr::CapturedImage>& snapshot,
    const tf2vrnr::ViewLayout& layout, const std::wstring& base, bool candidates) {
    if (!snapshot->PrepareForAsyncWrite()) return false;
    std::promise<void> release;
    auto gate = release.get_future().share();
    auto result = std::make_shared<std::atomic<bool>>(false);
    const auto graphicsThread = GetCurrentThreadId();
    if (!tf2vrnr::QueueCaptureWrite([snapshot, layout, base, candidates, gate, result, graphicsThread] {
        gate.wait(); // proves the graphics thread can continue while IO is blocked
        bool ok = GetCurrentThreadId() != graphicsThread && snapshot->WriteView(layout, base + L".dds");
        if (candidates) snapshot->WriteCandidates(layout, base);
        result->store(ok);
    })) return false;
    const bool bounded = tf2vrnr::CaptureWriteBusy() && !tf2vrnr::QueueCaptureWrite([] {});
    release.set_value();
    const auto start = GetTickCount64();
    while (tf2vrnr::CaptureWriteBusy() && GetTickCount64() - start < 10000) Sleep(1);
    const bool passed = bounded && !tf2vrnr::CaptureWriteBusy() && result->load();
    std::printf("cpu_async_write_passed=%d separate_thread=1 bounded_queue=1\n", passed);
    return passed;
}
bool Run(ID3D11Device* device, ID3D11DeviceContext* context, const std::wstring& base, bool floatDepth) {
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = 128; desc.Height = 64; desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; desc.BindFlags = D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> color, hdr, depth;
    ComPtr<ID3D11RenderTargetView> colorView, hdrView;
    if (!Ok(device->CreateTexture2D(&desc, nullptr, color.GetAddressOf()), "color texture") ||
        !Ok(device->CreateRenderTargetView(color.Get(), nullptr, colorView.GetAddressOf()), "color RTV")) return false;
    desc.Format = DXGI_FORMAT_R16G16B16A16_TYPELESS;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (!Ok(device->CreateTexture2D(&desc, nullptr, hdr.GetAddressOf()), "HDR texture")) return false;
    D3D11_RENDER_TARGET_VIEW_DESC rt{}; rt.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    rt.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    if (!Ok(device->CreateRenderTargetView(hdr.Get(), &rt, hdrView.GetAddressOf()), "HDR RTV")) return false;
    D3D11_SHADER_RESOURCE_VIEW_DESC sr{}; sr.Format = rt.Format;
    sr.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; sr.Texture2D.MipLevels = 1;
    ComPtr<ID3D11ShaderResourceView> hdrSrv;
    if (!Ok(device->CreateShaderResourceView(hdr.Get(), &sr, hdrSrv.GetAddressOf()), "HDR SRV")) return false;
    desc.Format = floatDepth ? DXGI_FORMAT_R32_TYPELESS : DXGI_FORMAT_R24G8_TYPELESS;
    desc.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
    if (!Ok(device->CreateTexture2D(&desc, nullptr, depth.GetAddressOf()), "depth texture")) return false;
    D3D11_DEPTH_STENCIL_VIEW_DESC ds{};
    ds.Format = floatDepth ? DXGI_FORMAT_D32_FLOAT : DXGI_FORMAT_D24_UNORM_S8_UINT;
    ds.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
    ComPtr<ID3D11DepthStencilView> depthView;
    if (!Ok(device->CreateDepthStencilView(depth.Get(), &ds, depthView.GetAddressOf()), "depth DSV")) return false;
    const float colorValue[4] = {.25f, .5f, .75f, 1.f}, hdrValue[4] = {.5f, 1.f, 2.f, 1.f}, zeros[4]{};
    const UINT depthFlags = D3D11_CLEAR_DEPTH | (floatDepth ? 0 : D3D11_CLEAR_STENCIL);
    context->ClearRenderTargetView(colorView.Get(), colorValue);
    context->ClearRenderTargetView(hdrView.Get(), hdrValue);
    context->ClearDepthStencilView(depthView.Get(), depthFlags, .25f, 0x5a);
    ID3D11RenderTargetView* colorBinding = colorView.Get();
    ID3D11ShaderResourceView* hdrBinding = hdrSrv.Get();
    context->OMSetRenderTargets(1, &colorBinding, depthView.Get());
    context->PSSetShaderResources(7, 1, &hdrBinding);
    tf2vrnr::BeginDepthCapture(device, 128, 64, 7);
    // Mimic the game: meaningful depth is cleared before XR color capture.
    context->ClearDepthStencilView(depthView.Get(), depthFlags, .75f, 0x12);
    auto snapshot = tf2vrnr::CaptureImage(device, color.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, true, true);
    tf2vrnr::EndDepthCapture(device);
    if (!snapshot) return false;
    // Ensure inspection did not alter the game's bindings.
    ComPtr<ID3D11DepthStencilView> boundDepth;
    ComPtr<ID3D11ShaderResourceView> boundHdr;
    context->OMGetRenderTargets(0, nullptr, boundDepth.GetAddressOf());
    context->PSGetShaderResources(7, 1, boundHdr.GetAddressOf());
    if (boundDepth.Get() != depthView.Get() || boundHdr.Get() != hdrSrv.Get()) return false;
    hdrBinding = nullptr; context->PSSetShaderResources(7, 1, &hdrBinding);
    context->OMSetRenderTargets(0, nullptr, nullptr);
    context->ClearRenderTargetView(colorView.Get(), zeros);
    context->ClearRenderTargetView(hdrView.Get(), zeros);
    context->ClearDepthStencilView(depthView.Get(), depthFlags, .875f, 0);
    tf2vrnr::ViewLayout layout{0, 0, 0, 0, 128, 64};
    if (!WriteAsync(snapshot, layout, base, true)) return false;
    std::printf("capture_probe_saved depth_format=%u bindings_unchanged=1 originals_overwritten=1\n", ds.Format);
    return true;
}
bool CheckBudget(ID3D11Device* device, ID3D11DeviceContext* context) {
    D3D11_TEXTURE2D_DESC d{};
    d.Width = d.Height = 32; d.MipLevels = d.ArraySize = d.SampleDesc.Count = 1;
    d.Format = DXGI_FORMAT_D32_FLOAT; d.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    ComPtr<ID3D11Texture2D> texture; ComPtr<ID3D11DepthStencilView> view;
    if (!Ok(device->CreateTexture2D(&d, nullptr, texture.GetAddressOf()), "budget texture") ||
        !Ok(device->CreateDepthStencilView(texture.Get(), nullptr, view.GetAddressOf()), "budget DSV")) return false;
    tf2vrnr::BeginDepthCapture(device, 32, 32, 8);
    for (unsigned n = 0; n < 10; ++n) context->ClearDepthStencilView(view.Get(), D3D11_CLEAR_DEPTH, n / 10.f, 0);
    auto batch = tf2vrnr::TakeDepthCapture(device);
    if (batch.observed != 10 || batch.skipped != 6 || batch.copies.size() != 4 ||
        batch.copies.front().ordinal != 5 || batch.copies.back().ordinal != 8) return false;
    if (!tf2vrnr::TakeDepthCapture(device).copies.empty()) return false;
    context->ClearDepthStencilView(view.Get(), D3D11_CLEAR_DEPTH, 1.f, 0);
    if (!tf2vrnr::TakeDepthCapture(device).copies.empty()) return false; // frame budget remains exhausted
    tf2vrnr::EndDepthCapture(device);
    context->ClearDepthStencilView(view.Get(), D3D11_CLEAR_DEPTH, 0.f, 0);
    if (!tf2vrnr::TakeDepthCapture(device).copies.empty()) return false;
    std::puts("capture_budget_passed=1 max_frame_copies=8 max_retained_per_release=4 inactive_passthrough=1");
    return true;
}
bool RunRenderPass(ID3D11Device* device, ID3D11DeviceContext* context, const std::wstring& base, bool uavApi) {
    struct Target { ComPtr<ID3D11Texture2D> texture; ComPtr<ID3D11RenderTargetView> view; } hdr, motion, color;
    auto make = [&](Target& t, DXGI_FORMAT resourceFormat, DXGI_FORMAT viewFormat) {
        D3D11_TEXTURE2D_DESC d{}; d.Width = 128; d.Height = 64;
        d.MipLevels = d.ArraySize = d.SampleDesc.Count = 1; d.Format = resourceFormat; d.BindFlags = D3D11_BIND_RENDER_TARGET;
        if (!Ok(device->CreateTexture2D(&d, nullptr, t.texture.GetAddressOf()), "pass texture")) return false;
        D3D11_RENDER_TARGET_VIEW_DESC v{}; v.Format = viewFormat; v.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        return Ok(device->CreateRenderTargetView(t.texture.Get(), &v, t.view.GetAddressOf()), "pass RTV");
    };
    if (!make(hdr, DXGI_FORMAT_R16G16B16A16_TYPELESS, DXGI_FORMAT_R16G16B16A16_FLOAT) ||
        !make(motion, DXGI_FORMAT_R16G16_TYPELESS, DXGI_FORMAT_R16G16_FLOAT) ||
        !make(color, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM)) return false;
    D3D11_TEXTURE2D_DESC d{}; d.Width = 128; d.Height = 64; d.MipLevels = d.ArraySize = d.SampleDesc.Count = 1;
    d.Format = DXGI_FORMAT_R24G8_TYPELESS; d.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    ComPtr<ID3D11Texture2D> depth; ComPtr<ID3D11DepthStencilView> depthView;
    if (!Ok(device->CreateTexture2D(&d, nullptr, depth.GetAddressOf()), "pass depth")) return false;
    D3D11_DEPTH_STENCIL_VIEW_DESC vd{}; vd.Format = DXGI_FORMAT_D24_UNORM_S8_UINT; vd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
    if (!Ok(device->CreateDepthStencilView(depth.Get(), &vd, depthView.GetAddressOf()), "pass DSV")) return false;
    const float hdrValue[4] = {.5f, 1.f, 2.f, 1.f}, motionValue[4] = {2.f, -4.f, 0, 0};
    const float colorValue[4] = {.25f, .5f, .75f, 1.f}, zero[4]{};
    context->ClearRenderTargetView(hdr.view.Get(), hdrValue); context->ClearRenderTargetView(motion.view.Get(), motionValue);
    context->ClearRenderTargetView(color.view.Get(), colorValue);
    context->ClearDepthStencilView(depthView.Get(), D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, .25f, 0x5a);
    ID3D11RenderTargetView* main[] = {hdr.view.Get(), motion.view.Get()};
    ID3D11RenderTargetView* final = color.view.Get();
    auto bind = [&](UINT count, ID3D11RenderTargetView* const* targets, ID3D11DepthStencilView* z) {
        if (uavApi) context->OMSetRenderTargetsAndUnorderedAccessViews(count, targets, z, count, 0, nullptr, nullptr);
        else context->OMSetRenderTargets(count, targets, z);
    };
    // Regression for the live trace: HDR/depth-only transitions precede motion
    // attachment. They must not exhaust the budget for complete scene inputs.
    tf2vrnr::BeginDepthCapture(device, 128, 64, 41, true);
    ID3D11RenderTargetView* hdrOnly = hdr.view.Get();
    for (unsigned n = 0; n < 6; ++n) { bind(1, &hdrOnly, depthView.Get()); bind(1, &final, nullptr); }
    auto preliminary = tf2vrnr::TakeDepthCapture(device);
    if (!preliminary.copies.empty() || preliminary.passGroups != 0) return false;
    tf2vrnr::EndDepthCapture(device);
    bind(2, main, depthView.Get());
    tf2vrnr::BeginDepthCapture(device, 128, 64, 42, true);
    bind(2, main, depthView.Get()); // unchanged color target must not produce a stale capture
    context->OMSetRenderTargetsAndUnorderedAccessViews(D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL,
        nullptr, nullptr, 2, 0, nullptr, nullptr);
    auto unchanged = tf2vrnr::TakeDepthCapture(device);
    if (!unchanged.copies.empty() || !unchanged.passEvents.empty()) return false;
    bind(1, &final, nullptr);
    ComPtr<ID3D11RenderTargetView> bound; ComPtr<ID3D11DepthStencilView> boundDepth;
    context->OMGetRenderTargets(1, bound.GetAddressOf(), boundDepth.GetAddressOf());
    if (bound.Get() != final || boundDepth) return false;
    context->ClearRenderTargetView(hdr.view.Get(), zero); context->ClearRenderTargetView(motion.view.Get(), zero);
    context->ClearDepthStencilView(depthView.Get(), D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, .875f, 0);
    auto snapshot = tf2vrnr::CaptureImage(device, color.texture.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, false, true);
    if (!snapshot) return false;
    const tf2vrnr::ViewLayout layout{0,0,0,0,128,64};
    if (!WriteAsync(snapshot, layout, base, true)) return false;
    // Per-release and per-frame limits remain in force after a snapshot drains
    // the pending copies. Rebinding and clearing still execute normally.
    for (unsigned n = 0; n < 5; ++n) {
        bind(2, main, depthView.Get());
        const float h[4] = {float(n + 1), .5f * (n + 1), 0, 1};
        const float m[4] = {float(n), -float(n), 0, 0};
        context->ClearRenderTargetView(hdr.view.Get(), h); context->ClearRenderTargetView(motion.view.Get(), m);
        context->ClearDepthStencilView(depthView.Get(), D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, .25f + n * .125f, 0x5a);
        bind(1, &final, nullptr);
    }
    auto budget = tf2vrnr::TakeDepthCapture(device);
    if (budget.passGroups != 4 || budget.copies.size() != 3) return false;
    for (const auto& copy : budget.copies) {
        if (copy.ordinal != budget.copies.front().ordinal || !copy.snapshot) return false;
        const std::string role = copy.name.substr(copy.name.rfind('-') + 1);
        if (!WriteAsync(copy.snapshot, layout, base + L"-latest-" + std::wstring(role.begin(), role.end()), false)) return false;
    }
    for (unsigned n = 0; n < 5; ++n) { bind(2, main, depthView.Get()); bind(1, &final, nullptr); }
    budget = tf2vrnr::TakeDepthCapture(device);
    if (budget.passGroups != 3 || budget.copies.size() != 3) return false;
    tf2vrnr::EndDepthCapture(device);
    bind(2, main, depthView.Get()); bind(1, &final, nullptr);
    if (!tf2vrnr::TakeDepthCapture(device).copies.empty()) return false;
    std::printf("render_pass_probe_passed=1 uav_api=%d original_bindings_forwarded=1 group_budget_passed=1 inactive_passthrough=1 preliminary_passes_skipped=1 latest_complete_group_retained=1\n", uavApi);
    return true;
}
bool RunLargeWrite(ID3D11Device* device, ID3D11DeviceContext* context, const std::wstring& dir) {
    // Match the live burst's ~0.8 GB payload. Four array snapshots exercise
    // independent slices while the original texture keeps receiving GPU work.
    constexpr UINT width = 2476, height = 2594, slices = 4, copies = 4;
    D3D11_TEXTURE2D_DESC d{}; d.Width = width; d.Height = height;
    d.MipLevels = d.SampleDesc.Count = 1; d.ArraySize = slices;
    d.Format = DXGI_FORMAT_R16G16B16A16_FLOAT; d.BindFlags = D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> texture;
    if (!Ok(device->CreateTexture2D(&d, nullptr, texture.GetAddressOf()), "large texture")) return false;
    ComPtr<ID3D11RenderTargetView> views[slices];
    for (UINT n = 0; n < slices; ++n) {
        D3D11_RENDER_TARGET_VIEW_DESC v{}; v.Format = d.Format; v.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
        v.Texture2DArray.FirstArraySlice = n; v.Texture2DArray.ArraySize = 1;
        if (!Ok(device->CreateRenderTargetView(texture.Get(), &v, views[n].GetAddressOf()), "large RTV")) return false;
    }
    std::vector<std::shared_ptr<tf2vrnr::CapturedImage>> snapshots;
    for (UINT copy = 0; copy < copies; ++copy) {
        for (UINT slice = 0; slice < slices; ++slice) {
            const float values[] = {float(copy+1), float(slice+1), .25f, 1.f};
            context->ClearRenderTargetView(views[slice].Get(), values);
        }
        auto snapshot = tf2vrnr::CaptureImage(device, texture.Get(), d.Format, false, false);
        if (!snapshot) return false;
        snapshots.push_back(std::move(snapshot));
    }
    const auto start = GetTickCount64();
    for (auto& snapshot : snapshots) if (!snapshot->PrepareForAsyncWrite()) return false;
    const auto readbackMs = GetTickCount64() - start;
    auto result = std::make_shared<std::atomic<bool>>(false);
    if (!tf2vrnr::QueueCaptureWrite([snapshots, dir, result] {
        bool ok = true;
        for (UINT copy = 0; copy < copies; ++copy) for (UINT slice = 0; slice < slices; ++slice) {
            const tf2vrnr::ViewLayout layout{0,slice,0,0,int(width),int(height)};
            const auto path = dir + L"\\stress-copy" + std::to_wstring(copy) + L"-slice" + std::to_wstring(slice) + L".dds";
            if (!snapshots[copy]->WriteView(layout, path)) ok = false;
        }
        result->store(ok);
    })) return false;
    unsigned submissions = 0;
    const auto writeStart = GetTickCount64();
    while (tf2vrnr::CaptureWriteBusy() && GetTickCount64() - writeStart < 60000) {
        const float changed[] = {0,0,0,0};
        context->ClearRenderTargetView(views[submissions % slices].Get(), changed);
        context->Flush(); ++submissions; Sleep(10);
    }
    const bool passed = !tf2vrnr::CaptureWriteBusy() && result->load() && submissions > 0;
    std::printf("large_async_write_passed=%d payload_bytes=%llu readback_ms=%llu io_ms=%llu gpu_submissions_during_io=%u\n",
        passed, static_cast<unsigned long long>(width) * height * slices * copies * 8,
        static_cast<unsigned long long>(readbackMs), static_cast<unsigned long long>(GetTickCount64() - writeStart), submissions);
    return passed;
}
bool RunEnginePost(ID3D11Device* device, ID3D11DeviceContext* context, const std::wstring& base, unsigned api) {
    context->ClearState();
    struct Target {
        ComPtr<ID3D11Texture2D> texture;
        ComPtr<ID3D11RenderTargetView> rtv;
        ComPtr<ID3D11ShaderResourceView> srv;
    } hdr, motion, exposure, output;
    auto make = [&](Target& t, DXGI_FORMAT format, UINT width, UINT height) {
        D3D11_TEXTURE2D_DESC d{}; d.Width = width; d.Height = height;
        d.MipLevels = d.ArraySize = d.SampleDesc.Count = 1; d.Format = format;
        d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        return Ok(device->CreateTexture2D(&d, nullptr, t.texture.GetAddressOf()), "epost texture") &&
            Ok(device->CreateRenderTargetView(t.texture.Get(), nullptr, t.rtv.GetAddressOf()), "epost RTV") &&
            Ok(device->CreateShaderResourceView(t.texture.Get(), nullptr, t.srv.GetAddressOf()), "epost SRV");
    };
    if (!make(hdr, DXGI_FORMAT_R16G16B16A16_FLOAT, 128, 64) || !make(motion, DXGI_FORMAT_R16G16_FLOAT, 128, 64) ||
        !make(exposure, DXGI_FORMAT_R16_FLOAT, 1, 1) || !make(output, DXGI_FORMAT_R8G8B8A8_UNORM, 128, 64)) return false;
    D3D11_TEXTURE2D_DESC d{}; d.Width = 128; d.Height = 64; d.MipLevels = d.ArraySize = d.SampleDesc.Count = 1;
    d.Format = DXGI_FORMAT_D32_FLOAT; d.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    ComPtr<ID3D11Texture2D> depth; ComPtr<ID3D11DepthStencilView> dsv;
    if (!Ok(device->CreateTexture2D(&d, nullptr, depth.GetAddressOf()), "epost depth") ||
        !Ok(device->CreateDepthStencilView(depth.Get(), nullptr, dsv.GetAddressOf()), "epost DSV")) return false;
    const char* vsSource = "float4 main(uint id:SV_VertexID):SV_Position {float2 p=float2((id<<1)&2,id&2);return float4(p*float2(2,-2)+float2(-1,1),0,1);}";
    const char* psSource = "cbuffer CBufEnginePost:register(b1){float4 params[19];}; Texture2D<float4> FBTexture:register(t1); Texture2D<float> Exposure:register(t30); float4 main(float4 p:SV_Position):SV_Target {return FBTexture.Load(int3(p.xy,0))*Exposure.Load(int3(0,0,0))*params[0].x;}";
    ComPtr<ID3DBlob> vsCode, psCode, errors;
    if (!Ok(D3DCompile(vsSource, std::strlen(vsSource), nullptr, nullptr, nullptr, "main", "vs_5_0", 0, 0, vsCode.GetAddressOf(), errors.GetAddressOf()), "epost VS compile") ||
        !Ok(D3DCompile(psSource, std::strlen(psSource), nullptr, nullptr, nullptr, "main", "ps_5_0", 0, 0, psCode.GetAddressOf(), errors.ReleaseAndGetAddressOf()), "epost PS compile")) return false;
    ComPtr<ID3D11VertexShader> vs; ComPtr<ID3D11PixelShader> ps;
    if (!Ok(device->CreateVertexShader(vsCode->GetBufferPointer(), vsCode->GetBufferSize(), nullptr, vs.GetAddressOf()), "epost VS") ||
        !Ok(device->CreatePixelShader(psCode->GetBufferPointer(), psCode->GetBufferSize(), nullptr, ps.GetAddressOf()), "epost PS")) return false;
    float parameters[76]{}; parameters[0] = 1;
    D3D11_BUFFER_DESC bd{}; bd.ByteWidth = sizeof(parameters); bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    D3D11_SUBRESOURCE_DATA initial{parameters,0,0}; ComPtr<ID3D11Buffer> constants, indices;
    if (!Ok(device->CreateBuffer(&bd, &initial, constants.GetAddressOf()), "epost CB")) return false;
    const UINT indexValues[] = {0,1,2}; bd.ByteWidth = sizeof(indexValues); bd.BindFlags = D3D11_BIND_INDEX_BUFFER;
    initial.pSysMem = indexValues;
    if (!Ok(device->CreateBuffer(&bd, &initial, indices.GetAddressOf()), "epost IB")) return false;
    const float h[] = {.5f,1.f,2.f,1.f}, m[] = {2.f,-4.f,0,0}, e[] = {2.f,0,0,0}, zero[] = {0,0,0,0};
    context->ClearRenderTargetView(hdr.rtv.Get(), h); context->ClearRenderTargetView(motion.rtv.Get(), m);
    context->ClearRenderTargetView(exposure.rtv.Get(), e); context->ClearRenderTargetView(output.rtv.Get(), zero);
    context->ClearDepthStencilView(dsv.Get(), D3D11_CLEAR_DEPTH, .25f, 0);
    ID3D11RenderTargetView* scene[] = {hdr.rtv.Get(), motion.rtv.Get()};
    context->OMSetRenderTargets(2, scene, dsv.Get());
    tf2vrnr::BeginDepthCapture(device,128,64,80+api,true,true);
    ID3D11RenderTargetView* out = output.rtv.Get(); context->OMSetRenderTargets(1,&out,nullptr);
    context->VSSetShader(vs.Get(),nullptr,0); context->PSSetShader(ps.Get(),nullptr,0);
    ID3D11ShaderResourceView* hdrView=hdr.srv.Get(); ID3D11ShaderResourceView* expoView=exposure.srv.Get();
    context->PSSetShaderResources(1,1,&hdrView); context->PSSetShaderResources(30,1,&expoView);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->IASetIndexBuffer(indices.Get(),DXGI_FORMAT_R32_UINT,0);
    D3D11_VIEWPORT viewport{0,0,128,64,0,1}; context->RSSetViewports(1,&viewport);
    // Wrong/missing constants must not satisfy the signature.
    context->Draw(3,0);
    auto rejected=tf2vrnr::TakeDepthCapture(device);
    if (rejected.enginePostObserved || !rejected.copies.empty()) return false;
    // Taking a release batch invalidates its guide references; track afresh.
    ID3D11ShaderResourceView* nullView=nullptr;context->PSSetShaderResources(1,1,&nullView);
    context->OMSetRenderTargets(2,scene,dsv.Get());context->OMSetRenderTargets(1,&out,nullptr);
    context->PSSetShaderResources(1,1,&hdrView);
    ID3D11Buffer* cb=constants.Get();context->PSSetConstantBuffers(1,1,&cb);
    if (api==0) context->Draw(3,0);
    else if (api==1) context->DrawIndexed(3,0,0);
    else if (api==2) context->DrawInstanced(3,1,0,0);
    else context->DrawIndexedInstanced(3,1,0,0,0);
    auto snapshot=tf2vrnr::CaptureImage(device,output.texture.Get(),DXGI_FORMAT_R8G8B8A8_UNORM,true,true);
    tf2vrnr::EndDepthCapture(device);if (!snapshot) return false;
    context->ClearState();
    context->ClearRenderTargetView(hdr.rtv.Get(),zero);context->ClearRenderTargetView(motion.rtv.Get(),zero);
    context->ClearRenderTargetView(exposure.rtv.Get(),zero);context->ClearDepthStencilView(dsv.Get(),D3D11_CLEAR_DEPTH,1.f,0);
    const tf2vrnr::ViewLayout layout{0,0,0,0,128,64};
    if (!WriteAsync(snapshot,layout,base,true)) return false;
    std::printf("engine_post_probe_passed=1 draw_api=%u rejected_missing_cb=1 source_changes_after_capture=1\n",api);
    return true;
}
}
int main(int argc, char** argv) {
    if (argc != 2 && argc != 3) { std::puts("Usage: CaptureProbe.exe <existing-output-directory> [--stress]"); return 2; }
    if (argc == 3 && std::string(argv[2]) != "--stress") return 2;
    const std::string path = argv[1]; const std::wstring dir(path.begin(), path.end());
    if (!std::freopen((path + "\\capture-probe.log").c_str(), "w", stdout)) return 3;
    ComPtr<ID3D11Device> device; ComPtr<ID3D11DeviceContext> context;
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0};
    if (!Ok(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 1,
        D3D11_SDK_VERSION, device.GetAddressOf(), nullptr, context.GetAddressOf()), "D3D11 device")) return 1;
    const bool passed = Run(device.Get(), context.Get(), dir + L"\\probe-d24", false) &&
        Run(device.Get(), context.Get(), dir + L"\\probe-d32", true) && CheckBudget(device.Get(), context.Get()) &&
        RunRenderPass(device.Get(), context.Get(), dir + L"\\pass-om", false) &&
        RunRenderPass(device.Get(), context.Get(), dir + L"\\pass-uav", true) &&
        RunEnginePost(device.Get(), context.Get(), dir + L"\\engine-post-0", 0) &&
        RunEnginePost(device.Get(), context.Get(), dir + L"\\engine-post-1", 1) &&
        RunEnginePost(device.Get(), context.Get(), dir + L"\\engine-post-2", 2) &&
        RunEnginePost(device.Get(), context.Get(), dir + L"\\engine-post-3", 3) &&
        (argc != 3 || RunLargeWrite(device.Get(), context.Get(), dir));
    context->ClearState(); context->Flush();
    std::printf("capture_probe_complete=%d payload_verification_required=1\n", passed);
    return passed ? 0 : 1;
}
