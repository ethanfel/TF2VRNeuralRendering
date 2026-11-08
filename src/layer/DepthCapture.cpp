#include "DepthCapture.h"
#include "Support.h"
#include "Bindings.h"
#include <MinHook.h>
#include <atomic>
#include <map>
#include <mutex>
#include <locale>
#include <sstream>
#include <wrl/client.h>
#include <d3d11_4.h>
#ifdef TF2VRNR_BACKEND
#include "../backend/NeuralBackend.h"
#endif
#ifdef TF2VRNR_FRAMEGEN
#include "../backend/FrameGeneration.h"
#endif
namespace tf2vrnr {
namespace {
using Microsoft::WRL::ComPtr;
using Clear = void (STDMETHODCALLTYPE *)(ID3D11DeviceContext*, ID3D11DepthStencilView*, UINT, FLOAT, UINT8);
using SetTargets = void (STDMETHODCALLTYPE *)(ID3D11DeviceContext*, UINT, ID3D11RenderTargetView* const*, ID3D11DepthStencilView*);
using SetTargetsUav = void (STDMETHODCALLTYPE *)(ID3D11DeviceContext*, UINT, ID3D11RenderTargetView* const*, ID3D11DepthStencilView*, UINT, UINT, ID3D11UnorderedAccessView* const*, const UINT*);
using Draw = void (STDMETHODCALLTYPE *)(ID3D11DeviceContext*, UINT, UINT);
using DrawIndexed = void (STDMETHODCALLTYPE *)(ID3D11DeviceContext*, UINT, UINT, INT);
using DrawInstanced = void (STDMETHODCALLTYPE *)(ID3D11DeviceContext*, UINT, UINT, UINT, UINT);
using DrawIndexedInstanced = void (STDMETHODCALLTYPE *)(ID3D11DeviceContext*, UINT, UINT, UINT, INT, UINT);
Clear originalClear = nullptr;
SetTargets originalTargets = nullptr;
SetTargetsUav originalTargetsUav = nullptr;
Draw originalDraw = nullptr;
DrawIndexed originalDrawIndexed = nullptr;
DrawInstanced originalDrawInstanced = nullptr;
DrawIndexedInstanced originalDrawIndexedInstanced = nullptr;
#ifdef TF2VRNR_FRAMEGEN
using Update = void (STDMETHODCALLTYPE *)(ID3D11DeviceContext*,ID3D11Resource*,UINT,const D3D11_BOX*,const void*,UINT,UINT);
Update originalUpdate=nullptr;
constexpr unsigned HookCount = 8;
struct LiveFrameGeneration {
    std::shared_ptr<FrameGeneration> backend=std::make_shared<FrameGeneration>();
    unsigned width=0,height=0;bool attempted=false,ready=false;
};
std::map<ID3D11Device*,std::shared_ptr<LiveFrameGeneration>> frameGenerators;
std::atomic<bool> frameGenerationRequested{false},cameraNeeded{false};
#else
constexpr unsigned HookCount = 7;
#endif
void* hookedTargets[HookCount]{};
bool installAttempted = false, hooksReady = false;
std::mutex installMutex, statesMutex;
thread_local bool insideObserver = false;
#ifdef TF2VRNR_BACKEND
struct LiveBackend {
    NeuralBackend backend;
    unsigned width=0, height=0;
    bool attempted=false, ready=false;
    NeuralOptions options;
};
std::map<ID3D11Device*, std::shared_ptr<LiveBackend>> neuralBackends;
#endif
struct State {
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11Multithread> protection;
    std::mutex mutex;
    uint32_t width = 0, height = 0;
    uint64_t frame = 0;
    unsigned ordinal = 0, copiesThisFrame = 0;
    bool renderPasses = false;
    bool enginePost = false;
    ComPtr<ID3D11Texture2D> sceneColor, sceneDepth, sceneMotion;
    DXGI_FORMAT sceneDepthFormat = DXGI_FORMAT_UNKNOWN;
    unsigned enginePostOrdinal = 0, enginePostCopies = 0;
    unsigned passOrdinal = 0, passGroupsThisFrame = 0;
    DepthBatch batch;
    bool capture = true;
#ifdef TF2VRNR_BACKEND
    std::shared_ptr<LiveBackend> neural;
    unsigned eye=0;
    unsigned renderedEyes=0;
    bool eyeProcessed=false, resetNeural=true;
    bool jitterSet=false;
    float jitterX=0,jitterY=0;
    uint64_t swapchains[2]{};
#endif
#ifdef TF2VRNR_FRAMEGEN
    std::shared_ptr<LiveFrameGeneration> frameGenerator;
    std::map<ID3D11Resource*,std::array<unsigned char,576>> cameraUploads;
    FrameCamera cameras[2];
    FrameCameraProjection cameraProjections[2];
    bool cameraDiagnostic[2]{};
    bool generated[2]{};
#endif
};
std::map<ID3D11DeviceContext*, std::shared_ptr<State>> states;
std::atomic<bool> armed{false};
// The game and OpenXR bridge may call the same immediate context from different
// threads, including while already inside ID3D11Multithread::Enter. Always take
// that recursive lock before our observer mutex. The opposite order deadlocks
// when its owner re-enters a hooked draw/binding while another observer is
// waiting in an OMGet/PSGet call. Never acquire this lock on an XR-only path.
struct ContextLock {
    ID3D11Multithread* protection;
    explicit ContextLock(ID3D11Multithread* p) : protection(p) {
        if (protection) protection->Enter();
    }
    ~ContextLock() { if (protection) protection->Leave(); }
};
void BeforeClear(ID3D11DeviceContext* context, ID3D11DepthStencilView* view, UINT flags, FLOAT value, UINT8 stencil) {
    if (!view || !(flags & D3D11_CLEAR_DEPTH)) return;
    std::shared_ptr<State> state;
    {
        std::lock_guard lock(statesMutex);
        auto it = states.find(context); if (it == states.end()) return;
        state = it->second;
    }
    ContextLock contextLock(state->protection.Get());
    std::lock_guard lock(state->mutex);
    if (state->renderPasses || state->enginePost) return;
    D3D11_DEPTH_STENCIL_VIEW_DESC vd{}; view->GetDesc(&vd);
    if (vd.ViewDimension != D3D11_DSV_DIMENSION_TEXTURE2D || vd.Texture2D.MipSlice != 0 ||
        (vd.Format != DXGI_FORMAT_D24_UNORM_S8_UINT && vd.Format != DXGI_FORMAT_D32_FLOAT)) return;
    ComPtr<ID3D11Resource> resource; view->GetResource(resource.GetAddressOf());
    ComPtr<ID3D11Texture2D> texture;
    if (!resource || FAILED(resource.As(&texture))) return;
    D3D11_TEXTURE2D_DESC desc{}; texture->GetDesc(&desc);
    if (desc.Width != state->width || desc.Height != state->height || desc.MipLevels != 1 ||
        desc.ArraySize != 1 || desc.SampleDesc.Count != 1) return;
    ++state->batch.observed; ++state->ordinal;
    // Bound the one-frame diagnostic: eight copies total, at most four retained
    // since the previous XR image release. No unbounded stream of GPU readbacks.
    if (state->copiesThisFrame >= 8 || uint64_t(desc.Width) * desc.Height * 4 > 32ull * 1024 * 1024) {
        ++state->batch.skipped; return;
    }
    ComPtr<ID3D11Device> device; context->GetDevice(device.GetAddressOf());
    DepthBeforeClear copy;
    copy.snapshot = CaptureImage(device.Get(), texture.Get(), vd.Format, false, false);
    copy.bindings = InspectBindings(context, nullptr, "before_ClearDepthStencilView");
    copy.resourceId = reinterpret_cast<uintptr_t>(resource.Get());
    copy.frame = state->frame; copy.ordinal = state->ordinal;
    copy.flags = flags; copy.stencil = stencil; copy.clearValue = value;
    copy.sourceFormat = desc.Format; copy.fileFormat = vd.Format;
    ++state->copiesThisFrame;
    if (state->batch.copies.size() >= 4) { state->batch.copies.erase(state->batch.copies.begin()); ++state->batch.skipped; }
    state->batch.copies.push_back(std::move(copy));
}
struct TextureView {
    ComPtr<ID3D11Texture2D> texture;
    D3D11_TEXTURE2D_DESC desc{};
    DXGI_FORMAT viewFormat = DXGI_FORMAT_UNKNOWN;
    bool baseView = false;
    void Resource(ID3D11View* view) {
        if (!view) return;
        ComPtr<ID3D11Resource> r; view->GetResource(r.GetAddressOf());
        if (r && SUCCEEDED(r.As(&texture))) texture->GetDesc(&desc);
    }
    explicit TextureView(ID3D11RenderTargetView* view) {
        Resource(view); if (!view) return;
        D3D11_RENDER_TARGET_VIEW_DESC d{}; view->GetDesc(&d); viewFormat = d.Format;
        baseView = d.ViewDimension == D3D11_RTV_DIMENSION_TEXTURE2D && d.Texture2D.MipSlice == 0;
    }
    explicit TextureView(ID3D11DepthStencilView* view) {
        Resource(view); if (!view) return;
        D3D11_DEPTH_STENCIL_VIEW_DESC d{}; view->GetDesc(&d); viewFormat = d.Format;
        baseView = d.ViewDimension == D3D11_DSV_DIMENSION_TEXTURE2D && d.Texture2D.MipSlice == 0;
    }
    bool Simple() const { return texture && baseView && desc.MipLevels == 1 && desc.ArraySize == 1 && desc.SampleDesc.Count == 1; }
    std::string Json() const {
        if (!texture) return "null";
        std::ostringstream o; o.imbue(std::locale::classic());
        o << "{\"id\":\"0x" << std::hex << reinterpret_cast<uintptr_t>(texture.Get()) << std::dec
          << "\",\"width\":" << desc.Width << ",\"height\":" << desc.Height << ",\"resource_format\":" << desc.Format
          << ",\"view_format\":" << viewFormat << ",\"samples\":" << desc.SampleDesc.Count
          << ",\"mips\":" << desc.MipLevels << ",\"array\":" << desc.ArraySize << '}';
        return o.str();
    }
};
void BeforeTargets(ID3D11DeviceContext* context, UINT count, ID3D11RenderTargetView* const* next, const char* stage) {
    if (count == D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL) return;
    std::shared_ptr<State> state;
    {
        std::lock_guard lock(statesMutex);
        auto it = states.find(context); if (it == states.end()) return;
        state = it->second;
    }
    ContextLock contextLock(state->protection.Get());
    std::lock_guard lock(state->mutex);
    if (!state->renderPasses && !state->enginePost) return;
    // Inspect the pass being left, before the game's binding change executes.
    ID3D11RenderTargetView* raw[3]{};
    ComPtr<ID3D11DepthStencilView> depthView;
    context->OMGetRenderTargets(3, raw, depthView.GetAddressOf());
    ComPtr<ID3D11RenderTargetView> views[3]; for (unsigned n = 0; n < 3; ++n) views[n].Attach(raw[n]);
    TextureView color(views[0].Get()), motion(views[1].Get()), depth(depthView.Get());
    TextureView incoming(count && next ? next[0] : nullptr);
    if (color.texture.Get() == incoming.texture.Get()) return;
    const bool sizeable = (color.desc.Width >= state->width / 2 && color.desc.Height >= state->height / 2) ||
        (depth.desc.Width >= state->width / 2 && depth.desc.Height >= state->height / 2);
    if (!sizeable) return;
    const auto ordinal = ++state->passOrdinal;
    const bool dimensions = color.desc.Width == state->width && color.desc.Height == state->height &&
        depth.desc.Width == state->width && depth.desc.Height == state->height;
    const bool formats = color.viewFormat == DXGI_FORMAT_R16G16B16A16_FLOAT &&
        (depth.viewFormat == DXGI_FORMAT_D24_UNORM_S8_UINT || depth.viewFormat == DXGI_FORMAT_D32_FLOAT);
    const bool simple = color.Simple() && depth.Simple();
    const bool hasMotion = motion.Simple() && motion.viewFormat == DXGI_FORMAT_R16G16_FLOAT &&
        motion.desc.Width == state->width && motion.desc.Height == state->height;
    const bool withinBytes = uint64_t(color.desc.Width) * color.desc.Height * 8 <= 64ull * 1024 * 1024;
    const bool budget = state->batch.passGroups < 4 && state->passGroupsThisFrame < 8;
    const bool eligible = dimensions && formats && simple && withinBytes && hasMotion;
    const char* reason = !dimensions ? "dimensions_differ" : !formats ? "not_hdr_with_supported_depth" :
        !simple ? "multisampled_or_array_or_mips" : !withinBytes ? "byte_limit" : !hasMotion ? "missing_matching_motion_target" :
        state->enginePost ? "tracked_for_engine_post" : !budget ? "group_limit" : "copied";
    if (state->capture && state->batch.passEvents.size() < 64) {
        std::ostringstream event; event.imbue(std::locale::classic());
        event << "{\"ordinal\":" << ordinal << ",\"frame\":" << state->frame << ",\"stage\":\"" << stage
              << "\",\"rt0\":" << color.Json() << ",\"rt1\":" << motion.Json() << ",\"depth\":" << depth.Json()
              << ",\"next_rt0\":" << incoming.Json() << ",\"decision\":\"" << reason << "\"}";
        state->batch.passEvents.push_back(event.str());
    } else if (state->capture) ++state->batch.passEventsDropped;
    if (!eligible) return;
    if (state->enginePost) {
        // Only retain references here. Pixels are copied at the matched draw,
        // after all intervening scene work and before tone mapping consumes them.
        state->sceneColor = color.texture; state->sceneDepth = depth.texture;
        state->sceneMotion = motion.texture; state->sceneDepthFormat = depth.viewFormat;
        return;
    }
    if (!budget) return;
    ++state->batch.passGroups; ++state->passGroupsThisFrame;
    // The observed renderer temporarily leaves HDR for shadow/auxiliary passes
    // before attaching motion. Retain only the latest complete triplet in this
    // release interval, so those earlier passes cannot consume the file budget.
    state->batch.copies.clear();
    ComPtr<ID3D11Device> device; context->GetDevice(device.GetAddressOf());
    const auto bindings = InspectBindings(context, nullptr, stage);
    auto copy = [&](const TextureView& v, const char* role) {
        DepthBeforeClear c;
        c.snapshot = CaptureImage(device.Get(), v.texture.Get(), v.viewFormat, false, false);
        c.bindings = bindings; c.name = "pass-" + std::to_string(ordinal) + "-" + role; c.stage = stage;
        c.frame = state->frame; c.ordinal = ordinal; c.resourceId = reinterpret_cast<uintptr_t>(v.texture.Get());
        c.sourceFormat = v.desc.Format; c.fileFormat = v.viewFormat;
        state->batch.copies.push_back(std::move(c));
    };
    copy(color, "color"); copy(depth, "depth");
    copy(motion, "motion");
}
void BeforeDraw(ID3D11DeviceContext* context, UINT count, UINT instances) {
#ifndef TF2VRNR_FRAMEGEN
    if (instances != 1 || (count != 3 && count != 4 && count != 6)) return;
#else
    if ((instances != 1 || (count != 3 && count != 4 && count != 6))&&!cameraNeeded.load(std::memory_order_relaxed))return;
#endif
    std::shared_ptr<State> state;
    {
        std::lock_guard lock(statesMutex);
        auto it = states.find(context); if (it == states.end()) return;
        state = it->second;
    }
    ContextLock contextLock(state->protection.Get());
    std::lock_guard lock(state->mutex);
#ifdef TF2VRNR_FRAMEGEN
    if(state->frameGenerator&&state->eye<2&&!state->cameras[state->eye].valid&&!state->cameraUploads.empty()) {
        ComPtr<ID3D11Buffer> cb;context->PSGetConstantBuffers(2,1,&cb);
        auto upload=state->cameraUploads.find(cb.Get());
        if(upload==state->cameraUploads.end()){cb.Reset();context->VSGetConstantBuffers(2,1,&cb);upload=state->cameraUploads.find(cb.Get());}
        if(upload!=state->cameraUploads.end()) {
            {
                ComPtr<ID3D11RenderTargetView> rt;context->OMGetRenderTargets(1,&rt,nullptr);TextureView target(rt.Get());
                D3D11_VIEWPORT vp{};UINT n=1;context->RSGetViewports(&n,&vp);
                if(target.Simple()&&target.viewFormat==DXGI_FORMAT_R16G16B16A16_FLOAT&&target.desc.Width==state->width&&target.desc.Height==state->height&&
                   n==1&&vp.MaxDepth>=.999f&&vp.Width==state->width&&vp.Height==state->height) {
                    const auto& expected=state->cameraProjections[state->eye];
                    const auto camera=DecodeFrameCamera(upload->second.data(),state->width,state->height,state->jitterX,state->jitterY,&expected);
                    if(!state->cameraDiagnostic[state->eye]&&(state->frame<10||state->frame%300==0)) {
                        state->cameraDiagnostic[state->eye]=true;
                        float v[144];std::memcpy(v,upload->second.data(),sizeof(v));
                        Log("fg_camera_check frame=%llu eye=%u result=%s xr_projection=%d cb_near=%.6g cb_max_z=%.6g cb_size=%.6g,%.6g cb_scale=%.6g,%.6g expected_tan=%.6g,%.6g,%.6g,%.6g matrix=%.6g,%.6g,%.6g,%.6g;%.6g,%.6g,%.6g,%.6g;%.6g,%.6g,%.6g,%.6g;%.6g,%.6g,%.6g,%.6g",
                            (unsigned long long)state->frame,state->eye,camera.rejection,expected.valid,v[0],v[135],v[114],v[115],v[136],v[137],expected.left,expected.right,expected.up,expected.down,
                            v[4],v[5],v[6],v[7],v[8],v[9],v[10],v[11],v[12],v[13],v[14],v[15],v[16],v[17],v[18],v[19]);
                    }
                    if(camera.valid) {
                    state->cameras[state->eye]=camera;
                    cameraNeeded.store(false,std::memory_order_relaxed);
                    if(state->frame%300==0)Log("fg_camera frame=%llu eye=%u valid=1 near=%.4f far=%.1f fov=%.4f",(unsigned long long)state->frame,state->eye,camera.nearPlane,camera.farPlane,camera.fov);
                    }
                }
            }
        }
    }
    if (instances != 1 || (count != 3 && count != 4 && count != 6)) return;
#endif
    if (!state->enginePost || !state->sceneColor || !state->sceneDepth || !state->sceneMotion) return;
    ComPtr<ID3D11Buffer> constants; context->PSGetConstantBuffers(1, 1, constants.GetAddressOf());
    D3D11_BUFFER_DESC cbd{}; if (constants) constants->GetDesc(&cbd);
    if (cbd.ByteWidth != 304) return;
    ComPtr<ID3D11ShaderResourceView> hdr, exposure;
    context->PSGetShaderResources(1, 1, hdr.GetAddressOf());
    context->PSGetShaderResources(30, 1, exposure.GetAddressOf());
    if (!hdr || !exposure) return;
    D3D11_SHADER_RESOURCE_VIEW_DESC hd{}, ed{}; hdr->GetDesc(&hd); exposure->GetDesc(&ed);
    if (hd.Format != DXGI_FORMAT_R16G16B16A16_FLOAT || ed.Format != DXGI_FORMAT_R16_FLOAT ||
        hd.ViewDimension != D3D11_SRV_DIMENSION_TEXTURE2D || ed.ViewDimension != D3D11_SRV_DIMENSION_TEXTURE2D ||
        hd.Texture2D.MostDetailedMip || ed.Texture2D.MostDetailedMip) return;
    ComPtr<ID3D11Resource> hdrResource, exposureResource;
    hdr->GetResource(hdrResource.GetAddressOf()); exposure->GetResource(exposureResource.GetAddressOf());
    ComPtr<ID3D11Texture2D> hdrTexture, exposureTexture;
    if (FAILED(hdrResource.As(&hdrTexture)) || FAILED(exposureResource.As(&exposureTexture)) ||
        hdrTexture.Get() != state->sceneColor.Get()) return;
    D3D11_TEXTURE2D_DESC ex{}; exposureTexture->GetDesc(&ex);
    if (ex.Width != 1 || ex.Height != 1 || ex.MipLevels != 1 || ex.ArraySize != 1 || ex.SampleDesc.Count != 1) return;
    ComPtr<ID3D11RenderTargetView> output;
    context->OMGetRenderTargets(1, output.GetAddressOf(), nullptr);
    TextureView target(output.Get());
    if (!target.Simple() || target.desc.Width != state->width || target.desc.Height != state->height ||
        (target.viewFormat != DXGI_FORMAT_R8G8B8A8_UNORM && target.viewFormat != DXGI_FORMAT_R8G8B8A8_UNORM_SRGB &&
         target.viewFormat != DXGI_FORMAT_B8G8R8A8_UNORM && target.viewFormat != DXGI_FORMAT_B8G8R8A8_UNORM_SRGB)) return;
    D3D11_VIEWPORT viewport{}; UINT viewportCount = 1; context->RSGetViewports(&viewportCount, &viewport);
    if (viewportCount != 1 || viewport.TopLeftX != 0 || viewport.TopLeftY != 0 ||
        viewport.Width != state->width || viewport.Height != state->height) return;
    ++state->enginePostOrdinal; ++state->batch.enginePostObserved;
    if (state->batch.enginePostObserved > 2 || state->enginePostCopies >= 4) {
        ++state->batch.enginePostSkipped; return;
    }
    ++state->enginePostCopies;
    if (state->capture) {
    const char* stage = "before_engine_post_draw";
    const auto bindings = InspectBindings(context, nullptr, stage);
    ComPtr<ID3D11Device> device; context->GetDevice(device.GetAddressOf());
    state->batch.copies.clear();
    auto copy = [&](ID3D11Texture2D* texture, DXGI_FORMAT format, const char* role, int slot, bool scalar) {
        D3D11_TEXTURE2D_DESC d{}; texture->GetDesc(&d);
        DepthBeforeClear c;
        c.snapshot = CaptureImage(device.Get(), texture, format, false, false);
        c.bindings = bindings; c.name = "engine-post-" + std::to_string(state->enginePostOrdinal) + "-" + role;
        c.stage = stage; c.frame = state->frame; c.ordinal = state->enginePostOrdinal;
        c.resourceId = reinterpret_cast<uintptr_t>(texture); c.sourceFormat = d.Format; c.fileFormat = format; c.slot = slot;
        if (scalar) { c.width = 1; c.height = 1; }
        state->batch.copies.push_back(std::move(c));
    };
    copy(state->sceneColor.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT, "color", 1, false);
    copy(state->sceneDepth.Get(), state->sceneDepthFormat, "depth", -1, false);
    copy(state->sceneMotion.Get(), DXGI_FORMAT_R16G16_FLOAT, "motion", -1, false);
    copy(exposureTexture.Get(), DXGI_FORMAT_R16_FLOAT, "exposure", 30, true);
    Log("engine_post_capture frame=%llu ordinal=%u vertices=%u exposure=1 signature_only=1",
        static_cast<unsigned long long>(state->frame), state->enginePostOrdinal, count);
    }
#ifdef TF2VRNR_BACKEND
    if (state->neural && (state->neural->options.neural||state->neural->options.dlaa) && state->eye < 2 && !state->eyeProcessed) {
        state->eyeProcessed=true;
        auto& live=*state->neural;
        if(live.options.dlaa && !state->jitterSet) return;
        if (!live.attempted) {
            live.attempted=true;
            ComPtr<ID3D11Device> device; context->GetDevice(&device);
            live.ready=live.backend.Init(device.Get(),state->width,state->height,Directory()+L"\\runtime",live.options);
        }
        if (live.ready) {
            if (live.backend.Render(state->sceneColor.Get(),state->sceneDepth.Get(),state->sceneMotion.Get(),
                exposureTexture.Get(),state->eye,state->frame,state->resetNeural,state->jitterX,state->jitterY)) ++state->renderedEyes;
            else live.ready=false;
        }
    }
#endif
#ifdef TF2VRNR_FRAMEGEN
    if(state->frameGenerator&&state->eye<2&&state->cameras[state->eye].valid) {
        auto& live=*state->frameGenerator;
        if(!live.attempted){live.attempted=true;ComPtr<ID3D11Device> device;context->GetDevice(&device);
            live.ready=live.backend->Init(device.Get(),state->width,state->height,Directory()+L"\\runtime");}
        if(live.ready&&!live.backend->Prepare(state->sceneDepth.Get(),state->sceneMotion.Get(),state->eye,state->frame,state->jitterX,state->jitterY))live.ready=false;
    }
#endif
}
void CaptureDraw(ID3D11DeviceContext* context, UINT count, UINT instances) {
    if (!armed.load(std::memory_order_acquire) || insideObserver) return;
    insideObserver=true;
    try { BeforeDraw(context, count, instances); }
    catch (...) { Log("engine_post_capture_failed exception; original draw continues"); }
    insideObserver=false;
}
void STDMETHODCALLTYPE DrawHook(ID3D11DeviceContext* c, UINT n, UINT start) {
    CaptureDraw(c,n,1); originalDraw(c,n,start);
}
void STDMETHODCALLTYPE DrawIndexedHook(ID3D11DeviceContext* c, UINT n, UINT start, INT base) {
    CaptureDraw(c,n,1); originalDrawIndexed(c,n,start,base);
}
void STDMETHODCALLTYPE DrawInstancedHook(ID3D11DeviceContext* c, UINT n, UINT instances, UINT start, UINT first) {
    CaptureDraw(c,n,instances); originalDrawInstanced(c,n,instances,start,first);
}
void STDMETHODCALLTYPE DrawIndexedInstancedHook(ID3D11DeviceContext* c, UINT n, UINT instances, UINT start, INT base, UINT first) {
    CaptureDraw(c,n,instances); originalDrawIndexedInstanced(c,n,instances,start,base,first);
}
void STDMETHODCALLTYPE TargetsHook(ID3D11DeviceContext* c, UINT n, ID3D11RenderTargetView* const* r, ID3D11DepthStencilView* d) {
    if (armed.load(std::memory_order_acquire) && !insideObserver) {
        try { BeforeTargets(c, n, r, "before_OMSetRenderTargets"); }
        catch (...) { Log("render_pass_capture_failed exception; original binding continues"); }
    }
    originalTargets(c, n, r, d);
}
void STDMETHODCALLTYPE TargetsUavHook(ID3D11DeviceContext* c, UINT n, ID3D11RenderTargetView* const* r, ID3D11DepthStencilView* d,
    UINT start, UINT count, ID3D11UnorderedAccessView* const* uavs, const UINT* counts) {
    if (armed.load(std::memory_order_acquire) && !insideObserver) {
        try { BeforeTargets(c, n, r, "before_OMSetRenderTargetsAndUnorderedAccessViews"); }
        catch (...) { Log("render_pass_capture_failed exception; original binding continues"); }
    }
    originalTargetsUav(c, n, r, d, start, count, uavs, counts);
}
void STDMETHODCALLTYPE ClearHook(ID3D11DeviceContext* context, ID3D11DepthStencilView* view, UINT flags, FLOAT value, UINT8 stencil) {
    if (armed.load(std::memory_order_acquire) && !insideObserver) {
        try { BeforeClear(context, view, flags, value, stencil); }
        catch (...) { Log("depth_before_clear_failed exception; original clear continues"); }
    }
    // Always forward the exact original arguments, including stencil-only clears.
    originalClear(context, view, flags, value, stencil);
}
#ifdef TF2VRNR_FRAMEGEN
void STDMETHODCALLTYPE UpdateHook(ID3D11DeviceContext* context,ID3D11Resource* resource,UINT subresource,const D3D11_BOX* box,const void* data,UINT row,UINT slice) {
    if(frameGenerationRequested.load(std::memory_order_relaxed)&&armed.load(std::memory_order_acquire)&&!insideObserver&&resource&&data&&!subresource&&!box) {
        try {
            std::shared_ptr<State> state;
            {std::lock_guard lock(statesMutex);auto it=states.find(context);if(it!=states.end())state=it->second;}
            if(state) {
                ContextLock contextLock(state->protection.Get());std::lock_guard lock(state->mutex);
                if(state->frameGenerator) {
                    D3D11_RESOURCE_DIMENSION type;resource->GetType(&type);
                    if(type==D3D11_RESOURCE_DIMENSION_BUFFER) {
                        D3D11_BUFFER_DESC desc{};static_cast<ID3D11Buffer*>(resource)->GetDesc(&desc);
                        if(desc.ByteWidth==576&&(desc.BindFlags&D3D11_BIND_CONSTANT_BUFFER)&&state->cameraUploads.size()<64)
                            std::memcpy(state->cameraUploads[resource].data(),data,576);
                    }
                }
            }
        }catch(...){Log("fg_camera_upload_failed");}
    }
    originalUpdate(context,resource,subresource,box,data,row,slice);
}
#endif
bool Install(ID3D11DeviceContext* context) {
    std::lock_guard lock(installMutex);
    // ID3D11DeviceContext ABI: IUnknown + ID3D11DeviceChild, then context methods.
    // ClearDepthStencilView is slot 53 (also checked in the synthetic GPU test).
    auto vtable = *reinterpret_cast<void***>(context);
    void* targets[] = {vtable[53], vtable[33], vtable[34], vtable[13], vtable[12], vtable[21], vtable[20]
#ifdef TF2VRNR_FRAMEGEN
        ,vtable[48]
#endif
    };
    if (installAttempted) {
        if (!hooksReady) return false;
        for (unsigned n = 0; n < HookCount; ++n) if (targets[n] != hookedTargets[n]) return false;
        return true;
    }
    installAttempted = true;
    // This forwarding hook may be called after the XR session ends. Pin our
    // module and keep its trampoline until process exit; session teardown only
    // removes capture state. This avoids dangling callbacks if OpenXR unloads.
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
            reinterpret_cast<LPCWSTR>(&ClearHook), &module)) {
        Log("depth_before_clear_pin_failed error=%lu", GetLastError()); return false;
    }
    const auto init = MH_Initialize();
    if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED) {
        Log("depth_before_clear_init_failed status=%d", init); return false;
    }
    void* detours[] = {reinterpret_cast<void*>(&ClearHook), reinterpret_cast<void*>(&TargetsHook), reinterpret_cast<void*>(&TargetsUavHook),
        reinterpret_cast<void*>(&DrawHook), reinterpret_cast<void*>(&DrawIndexedHook), reinterpret_cast<void*>(&DrawInstancedHook), reinterpret_cast<void*>(&DrawIndexedInstancedHook)
#ifdef TF2VRNR_FRAMEGEN
        ,reinterpret_cast<void*>(&UpdateHook)
#endif
    };
    void** originals[] = {reinterpret_cast<void**>(&originalClear), reinterpret_cast<void**>(&originalTargets), reinterpret_cast<void**>(&originalTargetsUav),
        reinterpret_cast<void**>(&originalDraw), reinterpret_cast<void**>(&originalDrawIndexed), reinterpret_cast<void**>(&originalDrawInstanced), reinterpret_cast<void**>(&originalDrawIndexedInstanced)
#ifdef TF2VRNR_FRAMEGEN
        ,reinterpret_cast<void**>(&originalUpdate)
#endif
    };
    for (unsigned n = 0; n < HookCount; ++n) {
        const auto status = MH_CreateHook(targets[n], detours[n], originals[n]);
        if (status != MH_OK) { Log("capture_hook_create_failed index=%u status=%d", n, status); return false; }
    }
    for (unsigned n = 0; n < HookCount; ++n) {
        const auto status = MH_EnableHook(targets[n]);
        // On partial failure, keep existing forwarding trampolines valid and
        // never arm capture state. Do not remove an in-flight trampoline.
        if (status != MH_OK) { Log("capture_hook_enable_failed index=%u status=%d", n, status); return false; }
        hookedTargets[n] = targets[n];
    }
    hooksReady = true;
    Log("graphics_capture_hooks_ready clear_render_targets_draws=1 neural_processing_when_enabled=1 diagnostic_copies_on_request=1");
    return true;
}
}
void BeginDepthCapture(ID3D11Device* device, uint32_t width, uint32_t height, uint64_t frame, bool renderPasses, bool enginePost) {
    if (!device) return;
    if (!width || !height) { EndDepthCapture(device); return; }
    ComPtr<ID3D11DeviceContext> context; device->GetImmediateContext(context.GetAddressOf());
    if (!context || !Install(context.Get())) { EndDepthCapture(device); return; }
    auto state = std::make_shared<State>();
    state->context = context; state->width = width; state->height = height; state->frame = frame;
    context.As(&state->protection);
    state->renderPasses = renderPasses;
    state->enginePost = enginePost;
    std::lock_guard lock(statesMutex);
    states[context.Get()] = std::move(state); armed.store(true, std::memory_order_release);
}
void EndDepthCapture(ID3D11Device* device) {
    if (!device || !armed.load(std::memory_order_acquire)) return;
    ComPtr<ID3D11DeviceContext> context; device->GetImmediateContext(context.GetAddressOf());
    std::lock_guard lock(statesMutex);
    states.erase(context.Get()); armed.store(!states.empty(), std::memory_order_release);
}
DepthBatch TakeDepthCapture(ID3D11Device* device) {
    DepthBatch result;
    if (!device) return result;
    ComPtr<ID3D11DeviceContext> context; device->GetImmediateContext(context.GetAddressOf());
    std::shared_ptr<State> state;
    {
        std::lock_guard lock(statesMutex);
        auto it = states.find(context.Get()); if (it == states.end()) return result;
        state = it->second;
    }
    std::lock_guard lock(state->mutex);
    result = std::move(state->batch); state->batch = {};
    if (state->enginePost) {
        state->sceneColor.Reset(); state->sceneDepth.Reset(); state->sceneMotion.Reset();
    }
    return result;
}
#ifdef TF2VRNR_BACKEND
void ConfigureNeuralFrame(ID3D11Device* device,bool capture,const Config& config,
    const uint64_t (&swapchains)[2],bool reset) {
    if(!device)return;
    ComPtr<ID3D11DeviceContext> context;device->GetImmediateContext(&context);
    std::lock_guard lock(statesMutex);
    auto it=states.find(context.Get());if(it==states.end())return;
    auto& s=*it->second;
    auto& live=neuralBackends[device];
    if(live&&(live->width!=s.width||live->height!=s.height||live->options.resolutionPercent!=config.neuralResolution||
        live->options.intensity!=config.neuralIntensity/100.f||live->options.tone!=config.neuralTone/100.f||
        live->options.detail!=config.neuralDetail/100.f||live->options.preset!=config.neuralPreset||
        live->options.dlaa!=config.dlaa||live->options.neural!=config.neural))live.reset();
    if(!live){live=std::make_shared<LiveBackend>();live->width=s.width;live->height=s.height;
        live->options.resolutionPercent=config.neuralResolution;live->options.intensity=config.neuralIntensity/100.f;
        live->options.tone=config.neuralTone/100.f;live->options.detail=config.neuralDetail/100.f;
        live->options.preset=config.neuralPreset;live->options.dlaa=config.dlaa;live->options.neural=config.neural;}
    s.neural=live;s.capture=capture;s.resetNeural=reset;
    s.swapchains[0]=swapchains[0];s.swapchains[1]=swapchains[1];
#ifdef TF2VRNR_FRAMEGEN
    frameGenerationRequested.store(config.frameGeneration,std::memory_order_relaxed);
    cameraNeeded.store(config.frameGeneration,std::memory_order_relaxed);
    if(config.frameGeneration){
        auto& fg=frameGenerators[device];
        if(fg&&(fg->width!=s.width||fg->height!=s.height))fg.reset();
        if(!fg){fg=std::make_shared<LiveFrameGeneration>();fg->width=s.width;fg->height=s.height;}
        s.frameGenerator=fg;
    }
#endif
}
bool ReleaseNeuralView(ID3D11Device* device,uint64_t swapchain) {
    if(!device)return true;
    ComPtr<ID3D11DeviceContext> context;device->GetImmediateContext(&context);
    std::shared_ptr<State> state;
    {std::lock_guard lock(statesMutex);auto it=states.find(context.Get());if(it==states.end())return true;state=it->second;}
    std::lock_guard lock(state->mutex);
    if(!state->neural)return true;
    const bool matched=state->eye<2 && state->swapchains[state->eye]==swapchain;
    if(!matched){state->neural.reset();Log("neural_disabled unexpected_eye_release=1");return false;}
    ++state->eye;state->eyeProcessed=false;
#ifdef TF2VRNR_FRAMEGEN
    cameraNeeded.store(state->frameGenerator&&state->eye<2,std::memory_order_relaxed);
#endif
    state->sceneColor.Reset();state->sceneDepth.Reset();state->sceneMotion.Reset();state->batch={};
    return true;
}
void DestroyNeuralBackend(ID3D11Device* device) {
    std::lock_guard lock(statesMutex);neuralBackends.erase(device);
#ifdef TF2VRNR_FRAMEGEN
    frameGenerators.erase(device);
#endif
}
bool NeuralFrameApplied(ID3D11Device* device) {
    if(!device)return false;
    ComPtr<ID3D11DeviceContext> context;device->GetImmediateContext(&context);
    std::lock_guard lock(statesMutex);auto it=states.find(context.Get());
    return it!=states.end() && it->second->neural && it->second->neural->options.neural && it->second->renderedEyes==2 && it->second->eye==2;
}
bool DlaaFrameApplied(ID3D11Device* device) {
    if(!device)return false;
    ComPtr<ID3D11DeviceContext> context;device->GetImmediateContext(&context);
    std::lock_guard lock(statesMutex);auto it=states.find(context.Get());
    return it!=states.end() && it->second->neural && it->second->neural->options.dlaa && it->second->renderedEyes==2 && it->second->eye==2;
}
bool SetNeuralJitter(ID3D11Device* device,float x,float y) {
    if(!device)return false;
    ComPtr<ID3D11DeviceContext> context;device->GetImmediateContext(&context);
    std::shared_ptr<State> s;
    {std::lock_guard lock(statesMutex);auto it=states.find(context.Get());if(it==states.end())return false;s=it->second;}
    std::lock_guard lock(s->mutex);
    if(!s->neural||!s->neural->options.dlaa||s->eye||s->eyeProcessed||
       (s->neural->attempted&&!s->neural->ready))return false;
    s->jitterX=x;s->jitterY=y;s->jitterSet=true;return true;
}
#endif
#ifdef TF2VRNR_FRAMEGEN
void SetFrameCameraProjection(ID3D11Device* device,const float (&tangents)[2][4]) {
    if(!device)return;
    ComPtr<ID3D11DeviceContext> context;device->GetImmediateContext(&context);std::shared_ptr<State> s;
    {std::lock_guard lock(statesMutex);auto it=states.find(context.Get());if(it==states.end())return;s=it->second;}
    std::lock_guard lock(s->mutex);
    if(!s->frameGenerator||s->eye||s->eyeProcessed)return;
    for(unsigned eye=0;eye<2;++eye)s->cameraProjections[eye]={tangents[eye][0],tangents[eye][1],tangents[eye][2],tangents[eye][3],true};
}
bool GenerateReleasedView(ID3D11Device* device,uint64_t swapchain,ID3D11Texture2D* color) {
    if(!device)return false;
    ComPtr<ID3D11DeviceContext> context;device->GetImmediateContext(&context);std::shared_ptr<State> s;
    {std::lock_guard lock(statesMutex);auto it=states.find(context.Get());if(it==states.end())return false;s=it->second;}
    ContextLock contextLock(s->protection.Get());std::lock_guard lock(s->mutex);
    if(!s->frameGenerator||s->eye>=2||s->swapchains[s->eye]!=swapchain)return false;
    if(s->neural&&s->neural->options.dlaa&&s->renderedEyes!=s->eye+1)return false;
    if(!s->frameGenerator->ready||!s->cameras[s->eye].valid){
        if(s->frame<10||s->frame%300==0)Log("fg_pending frame=%llu eye=%u camera_valid=%d backend_ready=%d camera_uploads=%zu",(unsigned long long)s->frame,s->eye,s->cameras[s->eye].valid,s->frameGenerator->ready,s->cameraUploads.size());
        return false;
    }
    insideObserver=true;
    bool ok=false;
    try{ok=s->frameGenerator->backend->Generate(color,s->eye,s->frame,s->cameras[s->eye],s->resetNeural);}
    catch(...){Log("fg_release_exception");}
    insideObserver=false;s->generated[s->eye]=ok;return ok;
}
std::shared_ptr<FrameGeneration> GeneratedStereoFrame(ID3D11Device* device) {
    if(!device)return {};
    ComPtr<ID3D11DeviceContext> context;device->GetImmediateContext(&context);
    std::shared_ptr<State> s;
    {std::lock_guard lock(statesMutex);auto it=states.find(context.Get());if(it==states.end())return {};s=it->second;}
    std::lock_guard lock(s->mutex);
    return s->frameGenerator&&s->generated[0]&&s->generated[1]?s->frameGenerator->backend:nullptr;
}
#endif
}
