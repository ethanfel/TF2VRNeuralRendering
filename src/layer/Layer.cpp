#include "Graphics.h"
#include "DepthCapture.h"
#include "Support.h"
#include "Jitter.h"
#ifdef TF2VRNR_FRAMEGEN
#include "FrameDelivery.h"
#include "../backend/FrameGeneration.h"
#endif
#define XR_USE_GRAPHICS_API_D3D11
#define XR_USE_PLATFORM_WIN32
#include <openxr.h>
#include <openxr_platform.h>
#include <openxr_loader_negotiation.h>
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <vector>

namespace tf2vrnr {
namespace {
struct Instance {
    XrInstance handle = XR_NULL_HANDLE;
    Config config;
    bool active = false;
    PFN_xrGetInstanceProcAddr getProc = nullptr;
    PFN_xrDestroyInstance destroyInstance = nullptr;
    PFN_xrCreateSession createSession = nullptr;
    PFN_xrDestroySession destroySession = nullptr;
    PFN_xrCreateSwapchain createSwapchain = nullptr;
    PFN_xrDestroySwapchain destroySwapchain = nullptr;
    PFN_xrEnumerateSwapchainImages enumerateImages = nullptr;
    PFN_xrAcquireSwapchainImage acquireImage = nullptr;
    PFN_xrWaitSwapchainImage waitImage = nullptr;
    PFN_xrReleaseSwapchainImage releaseImage = nullptr;
    PFN_xrEndFrame endFrame = nullptr;
    PFN_xrLocateViews locateViews = nullptr;
#ifdef TF2VRNR_FRAMEGEN
    PFN_xrWaitFrame waitFrame=nullptr;
    PFN_xrBeginFrame beginFrame=nullptr;
#endif
};
struct CapturedView {
    std::shared_ptr<CapturedImage> snapshot;
    ViewLayout layout;
    XrPosef pose{};
    XrFovf fov{};
    uint32_t imageIndex = 0;
    int64_t format = 0;
    uint64_t generation = 0;
    bool depth = false, motion = false, synthesis = false;
};
struct CapturedFrame {
    bool neural = false;
    bool dlaa = false;
    PixelJitter jitter;
    uint64_t frame = 0;
    XrTime displayTime = 0;
    uint32_t layer = 0;
    XrSpace space = XR_NULL_HANDLE;
    std::vector<CapturedView> views;
};
struct Session {
    std::shared_ptr<Instance> instance;
    std::mutex mutex;
    // Borrowed from the session graphics binding; never used after session destruction.
    ID3D11Device* device = nullptr;
    uint64_t frames = 0;
    bool capturePending = false;
    unsigned captureAttempts = 0;
    // At most two stereo frames. Only owned copies and value metadata survive
    // xrEndFrame; no caller-owned structures or runtime textures are retained.
    std::vector<CapturedFrame> captureFrames;
    std::set<XrSwapchain> captureTargets;
    std::map<uint64_t, ViewHistory> histories;
#ifdef TF2VRNR_FRAMEGEN
    std::unique_ptr<FrameDelivery> delivery;
#endif
#ifdef TF2VRNR_BACKEND
    std::vector<uint64_t> releaseOrder;
    uint64_t neuralSwapchains[2]{};
    uint64_t neuralGenerations[2]{};
    XrFovf neuralFovs[2]{};
    XrSpace neuralSpace=XR_NULL_HANDLE;
    bool neuralFault=false;
    unsigned jitterWidth=0,jitterHeight=0;
    XrTime jitterTime=0;
    XrFovf originalFovs[2]{},jitteredFovs[2]{};
#endif
};
struct Swapchain {
    std::shared_ptr<Session> session;
    std::mutex mutex;
    XrSwapchainCreateInfo info{};
    std::vector<ID3D11Texture2D*> images;
    ImageQueue queue;
    std::shared_ptr<CapturedImage> snapshot;
    uint64_t snapshotFrame = 0;
    uint32_t releasedIndex = 0;
    bool hasReleased = false;
    uint64_t acquireCalls = 0, waitCalls = 0, releaseCalls = 0;
    XrResult lastAcquire = XR_SUCCESS, lastWait = XR_SUCCESS;
};
std::mutex registryMutex;
std::map<XrInstance, std::shared_ptr<Instance>> instances;
std::map<XrSession, std::shared_ptr<Session>> sessions;
std::map<XrSwapchain, std::shared_ptr<Swapchain>> swapchains;

template<class Map, class Key> auto Lookup(Map& map, Key key) -> typename Map::mapped_type {
    std::lock_guard lock(registryMutex);
    auto it = map.find(key);
    return it == map.end() ? nullptr : it->second;
}
template<class T> bool Resolve(Instance& i, const char* name, T& fn) {
    PFN_xrVoidFunction result = nullptr;
    if (i.getProc(i.handle, name, &result) != XR_SUCCESS || !result) return false;
    fn = reinterpret_cast<T>(result); return true;
}
const XrBaseInStructure* Find(const void* chain, XrStructureType type) {
    auto p = static_cast<const XrBaseInStructure*>(chain);
    for (unsigned n = 0; p && n < 64; ++n, p = p->next) if (p->type == type) return p;
    return nullptr;
}

XrResult XRAPI_CALL DestroyInstance(XrInstance handle) {
    auto i = Lookup(instances, handle);
    if (!i || !i->destroyInstance) return XR_ERROR_HANDLE_INVALID;
    {
        std::lock_guard lock(registryMutex);
        for (auto& entry : sessions) if (entry.second->instance == i) {
#ifdef TF2VRNR_FRAMEGEN
            entry.second->delivery.reset();
#endif
            EndDepthCapture(entry.second->device);
#ifdef TF2VRNR_BACKEND
            DestroyNeuralBackend(entry.second->device);
#endif
        }
    }
    const auto r = i->destroyInstance(handle);
    if (r == XR_SUCCESS) {
        std::lock_guard lock(registryMutex);
        for (auto it = swapchains.begin(); it != swapchains.end();)
            if (it->second->session->instance == i) it = swapchains.erase(it); else ++it;
        for (auto it = sessions.begin(); it != sessions.end();)
            if (it->second->instance == i) it = sessions.erase(it); else ++it;
        instances.erase(handle);
    }
    return r;
}
XrResult XRAPI_CALL CreateSession(XrInstance instance, const XrSessionCreateInfo* info, XrSession* handle) {
    auto i = Lookup(instances, instance);
    if (!i || !i->createSession) return XR_ERROR_HANDLE_INVALID;
    const auto r = i->createSession(instance, info, handle);
    if (r != XR_SUCCESS || !info || !handle) return r;
    auto s = std::make_shared<Session>(); s->instance = i;
    const auto binding = reinterpret_cast<const XrGraphicsBindingD3D11KHR*>(Find(info->next, XR_TYPE_GRAPHICS_BINDING_D3D11_KHR));
    if (binding) s->device = binding->device;
#ifdef TF2VRNR_FRAMEGEN
    if(s->device&&i->waitFrame&&i->beginFrame)s->delivery=std::make_unique<FrameDelivery>(*handle,s->device,
        FrameDeliveryDispatch{i->waitFrame,i->beginFrame,i->endFrame,i->createSwapchain,i->destroySwapchain,i->enumerateImages,i->acquireImage,i->waitImage,i->releaseImage});
#endif
    s->capturePending = i->config.capture && i->config.captureFirst;
    {
        std::lock_guard lock(registryMutex); sessions[*handle] = s;
    }
    Log("session=%p graphics=%s neural_rendering_requested=%d",
        reinterpret_cast<void*>(*handle), s->device ? "D3D11" : "unsupported_passthrough",i->config.neural);
    return r;
}
XrResult XRAPI_CALL DestroySession(XrSession handle) {
    auto s = Lookup(sessions, handle);
    if (!s) return XR_ERROR_HANDLE_INVALID;
#ifdef TF2VRNR_FRAMEGEN
    s->delivery.reset();
#endif
    EndDepthCapture(s->device);
#ifdef TF2VRNR_BACKEND
    DestroyNeuralBackend(s->device);
#endif
    const auto r = s->instance->destroySession(handle);
    if (r == XR_SUCCESS) {
        std::lock_guard lock(registryMutex);
        for (auto it = swapchains.begin(); it != swapchains.end();)
            if (it->second->session == s) it = swapchains.erase(it); else ++it;
        sessions.erase(handle);
    }
    return r;
}
XrResult XRAPI_CALL CreateSwapchain(XrSession session, const XrSwapchainCreateInfo* info, XrSwapchain* handle) {
    auto s = Lookup(sessions, session);
    if (!s) return XR_ERROR_HANDLE_INVALID;
    XrSwapchainCreateInfo modified{};
    const auto* used = info;
    if (info && s->device && (s->instance->config.capture||s->instance->config.frameGeneration) &&
        (info->usageFlags & XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT) &&
        !(info->createFlags & XR_SWAPCHAIN_CREATE_PROTECTED_CONTENT_BIT) &&
        !(info->usageFlags & XR_SWAPCHAIN_USAGE_TRANSFER_SRC_BIT)) {
        modified = *info; modified.usageFlags |= XR_SWAPCHAIN_USAGE_TRANSFER_SRC_BIT; used = &modified;
    }
    auto r = s->instance->createSwapchain(session, used, handle);
    if (used != info && (r == XR_ERROR_FEATURE_UNSUPPORTED || r == XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED || r == XR_ERROR_VALIDATION_FAILURE)) {
        Log("capture_transfer_src_unavailable result=%d; retrying original swapchain flags", r);
        used = info; r = s->instance->createSwapchain(session, info, handle);
    }
    if (r != XR_SUCCESS || !used || !handle) return r;
    auto sc = std::make_shared<Swapchain>(); sc->session = s; sc->info = *used; sc->info.next = nullptr;
    {
        std::lock_guard lock(registryMutex); swapchains[*handle] = sc;
    }
    Log("swapchain=%p size=%ux%u array=%u samples=%u format=%lld usage=0x%llx capture_copy_allowed=%d",
        reinterpret_cast<void*>(*handle), used->width, used->height, used->arraySize, used->sampleCount,
        static_cast<long long>(used->format), static_cast<unsigned long long>(used->usageFlags),
        !!(used->usageFlags & XR_SWAPCHAIN_USAGE_TRANSFER_SRC_BIT));
    return r;
}
XrResult XRAPI_CALL DestroySwapchain(XrSwapchain handle) {
    auto sc = Lookup(swapchains, handle);
    if (!sc) return XR_ERROR_HANDLE_INVALID;
    const auto r = sc->session->instance->destroySwapchain(handle);
    if (r == XR_SUCCESS) { std::lock_guard lock(registryMutex); swapchains.erase(handle); }
    return r;
}
XrResult XRAPI_CALL EnumerateImages(XrSwapchain handle, uint32_t capacity, uint32_t* count, XrSwapchainImageBaseHeader* images) {
    auto sc = Lookup(swapchains, handle);
    if (!sc) return XR_ERROR_HANDLE_INVALID;
    const auto inputType = capacity && images ? images[0].type : XR_TYPE_UNKNOWN;
    const auto r = sc->session->instance->enumerateImages(handle, capacity, count, images);
    // The caller chooses the image structure before enumeration. Proton 11's
    // wineopenxr D3D11 bridge memcpy()s its internal image array, including
    // uninitialized type/next fields, over the caller's initialized headers.
    // Use the saved input type and read only texture; never follow output next.
    // ValveSoftware/Proton wineopenxr/openxr_loader.c, xrEnumerateSwapchainImages.
    if (r == XR_SUCCESS && sc->session->device && capacity && count && *count <= capacity && images &&
        inputType == XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR) {
        const auto d3d = reinterpret_cast<const XrSwapchainImageD3D11KHR*>(images);
        std::lock_guard lock(sc->mutex);
        sc->images.clear();
        for (uint32_t n = 0; n < *count; ++n) sc->images.push_back(d3d[n].texture);
    }
    {
        std::lock_guard lock(sc->mutex);
        Log("enumerate_images swapchain=%p result=%d capacity=%u count=%u input_type=%d output_type=%d tracked=%zu first_texture=%p",
            reinterpret_cast<void*>(handle), r, capacity, count && XR_SUCCEEDED(r) ? *count : 0,
            inputType, capacity && images ? images[0].type : XR_TYPE_UNKNOWN, sc->images.size(),
            sc->images.empty() ? nullptr : static_cast<void*>(sc->images[0]));
    }
    return r;
}
XrResult XRAPI_CALL AcquireImage(XrSwapchain handle, const XrSwapchainImageAcquireInfo* info, uint32_t* index) {
    auto sc = Lookup(swapchains, handle);
    if (!sc) return XR_ERROR_HANDLE_INVALID;
    const auto r = sc->session->instance->acquireImage(handle, info, index);
    {
        std::lock_guard lock(sc->mutex);
        ++sc->acquireCalls; sc->lastAcquire = r;
        if (r == XR_SUCCESS && index) sc->queue.Acquired(*index);
    }
    return r;
}
XrResult XRAPI_CALL WaitImage(XrSwapchain handle, const XrSwapchainImageWaitInfo* info) {
    auto sc = Lookup(swapchains, handle);
    if (!sc) return XR_ERROR_HANDLE_INVALID;
    const auto r = sc->session->instance->waitImage(handle, info);
    {
        std::lock_guard lock(sc->mutex);
        ++sc->waitCalls; sc->lastWait = r;
        if (r == XR_SUCCESS) sc->queue.Waited();
    }
    return r;
}
XrResult XRAPI_CALL ReleaseImage(XrSwapchain handle, const XrSwapchainImageReleaseInfo* info) {
    auto sc = Lookup(swapchains, handle);
    if (!sc) return XR_ERROR_HANDLE_INVALID;
    auto s = sc->session;
    bool capture = false, diagnose = false; uint64_t frame = 0;
    {
        std::lock_guard lock(s->mutex);
        capture = s->capturePending && (s->captureTargets.empty() || s->captureTargets.count(handle));
        diagnose = s->capturePending && s->captureAttempts < 2;
        frame = s->frames;
    }
    std::shared_ptr<CapturedImage> snapshot;
    std::optional<uint32_t> index;
    {
        std::lock_guard lock(sc->mutex);
        ++sc->releaseCalls;
        index = sc->queue.Writable();
#ifdef TF2VRNR_FRAMEGEN
        if(index&&*index<sc->images.size()&&(sc->info.usageFlags&XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT)&&
           (sc->info.usageFlags&XR_SWAPCHAIN_USAGE_TRANSFER_SRC_BIT)&&!(sc->info.createFlags&XR_SWAPCHAIN_CREATE_PROTECTED_CONTENT_BIT))
            GenerateReleasedView(s->device,reinterpret_cast<uint64_t>(handle),sc->images[*index]);
#endif
        if (capture && index && *index < sc->images.size() &&
            (sc->info.usageFlags & XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT) &&
            (sc->info.usageFlags & XR_SWAPCHAIN_USAGE_TRANSFER_SRC_BIT) &&
            !(sc->info.createFlags & XR_SWAPCHAIN_CREATE_PROTECTED_CONTENT_BIT)) {
            try { snapshot = CaptureImage(s->device, sc->images[*index], sc->info.format,
                s->instance->config.inspectBindings, s->instance->config.captureCandidates); }
            catch (...) { Log("capture_copy_failed exception; original frame continues"); }
        }
        if (diagnose)
            Log("capture_release frame=%llu swapchain=%p selected=%d writable=%d index=%u images=%zu acquires=%llu waits=%llu releases=%llu last_acquire=%d last_wait=%d copied=%d",
                static_cast<unsigned long long>(frame), reinterpret_cast<void*>(handle), capture,
                index.has_value(), index.value_or(0), sc->images.size(),
                static_cast<unsigned long long>(sc->acquireCalls), static_cast<unsigned long long>(sc->waitCalls),
                static_cast<unsigned long long>(sc->releaseCalls), sc->lastAcquire, sc->lastWait, !!snapshot);
    }
#ifdef TF2VRNR_BACKEND
    if ((sc->info.usageFlags & XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT) && index) {
        std::lock_guard lock(s->mutex);
        const auto id=static_cast<uint64_t>(reinterpret_cast<uintptr_t>(handle));
        s->releaseOrder.push_back(id);
        if (!ReleaseNeuralView(s->device,id))s->neuralFault=true;
    }
#endif
    // Never inspect/copy a runtime texture below this point.
    const auto r = s->instance->releaseImage(handle, info);
    if (diagnose) Log("capture_release_result swapchain=%p result=%d", reinterpret_cast<void*>(handle), r);
    if (r == XR_SUCCESS) {
        std::lock_guard lock(sc->mutex);
        sc->snapshot = std::move(snapshot); sc->snapshotFrame = frame;
        sc->hasReleased = index.has_value();
        if (index) sc->releasedIndex = *index;
        sc->queue.Released();
    }
    return r;
}

ViewLayout Layout(const XrSwapchainSubImage& image) {
    return {static_cast<uint64_t>(reinterpret_cast<uintptr_t>(image.swapchain)), image.imageArrayIndex,
        image.imageRect.offset.x, image.imageRect.offset.y, image.imageRect.extent.width, image.imageRect.extent.height};
}
bool WriteCapturedFrame(const CapturedFrame& frame, const std::wstring& burstBase, unsigned burstIndex, unsigned burstCount) {
    const auto base = burstBase + L"-frame" + std::to_wstring(frame.frame) + L"-layer" + std::to_wstring(frame.layer);
    unsigned captured = 0;
    for (unsigned eye = 0; eye < frame.views.size(); ++eye) {
        const auto& view = frame.views[eye];
        const auto& layout = view.layout;
        const auto name = base + L"-eye" + std::to_wstring(eye);
        if (!view.snapshot->WriteView(layout, name + L".dds")) continue;
        ++captured;
        try { view.snapshot->WriteCandidates(layout, name); }
        catch (...) { Log("capture_candidates_write_failed exception; color saved"); }
        const auto& bindings = view.snapshot->BindingsJson();
        if (!bindings.empty()) {
            FILE* file = _wfopen((name + L"-bindings.json").c_str(), L"wb");
            bool saved = false;
            if (file) {
                saved = std::fwrite(bindings.data(), 1, bindings.size(), file) == bindings.size();
                if (std::fclose(file)) saved = false;
            }
            Log("capture_bindings_saved frame=%llu eye=%u ok=%d validated_neural_guides=false",
                static_cast<unsigned long long>(frame.frame), eye, saved);
        }
        FILE* meta = _wfopen((name + L".json").c_str(), L"wb");
        if (meta) {
            std::fprintf(meta,
                "{\n  \"mode\": \"%s\", \"neural_rendering\": %s,\n"
                "  \"dlaa\": %s, \"jitter_pixels\": [%g, %g],\n"
                "  \"frame\": %llu, \"display_time\": %lld, \"layer\": %u, \"eye\": %u,\n"
                "  \"burst_index\": %u, \"burst_frames\": %u, \"readback_after_burst\": true, \"file_write_async\": true,\n"
                "  \"image_index\": %u, \"array_slice\": %u, \"format\": %lld,\n"
                "  \"rect\": [%d, %d, %d, %d],\n"
                "  \"position\": [%g, %g, %g], \"orientation\": [%g, %g, %g, %g],\n"
                "  \"fov\": [%g, %g, %g, %g],\n"
                "  \"depth_submitted\": %s, \"motion_submitted\": %s, \"synthesis_submitted\": %s\n}\n",
                frame.neural ? "neural_rendering" : frame.dlaa ? "dlaa" : "capture_only", frame.neural ? "true" : "false",
                frame.dlaa ? "true" : "false",frame.jitter.x,frame.jitter.y,
                static_cast<unsigned long long>(frame.frame), static_cast<long long>(frame.displayTime), frame.layer, eye,
                burstIndex, burstCount, view.imageIndex, layout.slice, static_cast<long long>(view.format),
                layout.x, layout.y, layout.width, layout.height,
                view.pose.position.x, view.pose.position.y, view.pose.position.z,
                view.pose.orientation.x, view.pose.orientation.y, view.pose.orientation.z, view.pose.orientation.w,
                view.fov.angleLeft, view.fov.angleRight, view.fov.angleUp, view.fov.angleDown,
                view.depth ? "true" : "false", view.motion ? "true" : "false", view.synthesis ? "true" : "false");
            std::fclose(meta);
        }
    }
    if (captured == 2)
        Log("stereo_capture_saved frame=%llu layer=%u eyes=2 burst_index=%u burst_frames=%u neural_rendering=%d",
            static_cast<unsigned long long>(frame.frame), frame.layer, burstIndex, burstCount, frame.neural);
    return captured == 2;
}
bool Consecutive(const CapturedFrame& previous, const CapturedFrame& next) {
    if (next.frame != previous.frame + 1 || next.layer != previous.layer || next.space != previous.space ||
        next.displayTime <= previous.displayTime || next.displayTime - previous.displayTime > 250000000 ||
        previous.views.size() != 2 || next.views.size() != 2) return false;
    for (unsigned eye = 0; eye < 2; ++eye) {
        const auto& a = previous.views[eye]; const auto& b = next.views[eye];
        if (!(a.layout == b.layout) || a.format != b.format || a.generation != b.generation ||
            a.fov.angleLeft != b.fov.angleLeft || a.fov.angleRight != b.fov.angleRight ||
            a.fov.angleUp != b.fov.angleUp || a.fov.angleDown != b.fov.angleDown) return false;
    }
    return true;
}
void InspectFrame(Session& s, const XrFrameEndInfo* info) {
    if (!info || (info->layerCount && !info->layers)) { s.captureFrames.clear(); return; }
    bool selectedTargets = false;
    CapturedFrame capturedFrame;
#ifdef TF2VRNR_BACKEND
    capturedFrame.neural=NeuralFrameApplied(s.device);
    capturedFrame.dlaa=DlaaFrameApplied(s.device);
    if(capturedFrame.dlaa) {const auto j=DlaaJitter(s.frames);capturedFrame.jitter={-j.x,-j.y};}
#endif
    capturedFrame.frame = s.frames; capturedFrame.displayTime = info->displayTime;
    std::map<uint64_t, bool> seen;
    for (uint32_t layerIndex = 0; layerIndex < std::min(info->layerCount, 64u); ++layerIndex) {
        auto header = info->layers[layerIndex];
        if (!header || header->type != XR_TYPE_COMPOSITION_LAYER_PROJECTION) continue;
        const auto* p = reinterpret_cast<const XrCompositionLayerProjection*>(header);
        if (!p->views || p->viewCount > 16) continue;
        const bool selected = !selectedTargets && p->viewCount == 2;
        if (selected) {
            selectedTargets = true;
            s.captureTargets = {p->views[0].subImage.swapchain, p->views[1].subImage.swapchain};
            capturedFrame.layer = layerIndex; capturedFrame.space = p->space;
        }
        for (uint32_t eye = 0; eye < p->viewCount; ++eye) {
            const auto& view = p->views[eye];
            const auto layout = Layout(view.subImage);
            const uint64_t key = (static_cast<uint64_t>(layerIndex) << 32) | eye;
            seen[key] = true;
            auto& history = s.histories[key];
            const bool reset = history.Observe(layout, info->displayTime);
            const bool depth = Find(view.next, XR_TYPE_COMPOSITION_LAYER_DEPTH_INFO_KHR) != nullptr;
            const bool motion = Find(view.next, XR_TYPE_COMPOSITION_LAYER_SPACE_WARP_INFO_FB) != nullptr;
            const bool synthesis = Find(view.next, XR_TYPE_FRAME_SYNTHESIS_INFO_EXT) != nullptr;
            if (reset || s.frames % s.instance->config.logEvery == 0)
                Log("frame=%llu layer=%u eye=%u swapchain=%p slice=%u rect=%d,%d,%d,%d depth_submitted=%d motion_submitted=%d synthesis_submitted=%d history_generation=%llu",
                    static_cast<unsigned long long>(s.frames), layerIndex, eye, reinterpret_cast<void*>(view.subImage.swapchain),
                    layout.slice, layout.x, layout.y, layout.width, layout.height, depth, motion, synthesis,
                    static_cast<unsigned long long>(history.Generation()));
            if (!s.capturePending || !selected) continue;
            auto sc = Lookup(swapchains, view.subImage.swapchain);
            if (!sc || sc->session.get() != &s) continue;
            std::lock_guard lock(sc->mutex);
            if (s.captureAttempts < 2)
                Log("capture_view frame=%llu eye=%u snapshot=%d snapshot_frame=%llu bounds_valid=%d releases=%llu images=%zu",
                    static_cast<unsigned long long>(s.frames), eye, !!sc->snapshot,
                    static_cast<unsigned long long>(sc->snapshotFrame),
                    Fits(sc->info.width, sc->info.height, sc->info.arraySize, layout),
                    static_cast<unsigned long long>(sc->releaseCalls), sc->images.size());
            if (!sc->snapshot || sc->snapshotFrame != s.frames || !Fits(sc->info.width, sc->info.height, sc->info.arraySize, layout)) continue;
            CapturedView copy;
            copy.snapshot = sc->snapshot; copy.layout = layout; copy.pose = view.pose; copy.fov = view.fov;
            copy.imageIndex = sc->releasedIndex; copy.format = sc->info.format; copy.generation = history.Generation();
            copy.depth = depth; copy.motion = motion; copy.synthesis = synthesis;
            capturedFrame.views.push_back(std::move(copy));
        }
    }
    for (auto& [key, history] : s.histories) if (!seen.count(key)) history.Invalidate();
    if (!s.capturePending) return;
    bool saved = false, unsuccessful = false;
    if (capturedFrame.views.size() == 2) {
        if (!s.captureFrames.empty() && !Consecutive(s.captureFrames.back(), capturedFrame)) {
            s.captureFrames.clear(); unsuccessful = true;
            Log("capture_burst_reset reason=frame_layout_or_time_discontinuity");
        }
        s.captureFrames.push_back(std::move(capturedFrame));
        const unsigned goal = std::clamp(s.instance->config.burstFrames, 1u, 2u);
        if (s.captureFrames.size() == goal) {
            // Read back on the graphics thread, then release all D3D references
            // before sending CPU-only data to the single bounded IO worker.
            const auto readbackStart = GetTickCount64();
            bool ready = true;
            for (auto& frame : s.captureFrames) for (auto& view : frame.views)
                if (!view.snapshot->PrepareForAsyncWrite()) ready = false;
            Log("capture_cpu_readback frames=%u ready=%d elapsed_ms=%llu", goal, ready,
                static_cast<unsigned long long>(GetTickCount64() - readbackStart));
            const auto dir = CaptureDirectory();
            if (ready && !dir.empty()) {
                const auto base = dir + L"\\capture-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());
                saved = QueueCaptureWrite([frames = std::move(s.captureFrames), base, goal]() {
                    bool complete = true;
                    for (unsigned n = 0; n < goal; ++n)
                        if (!WriteCapturedFrame(frames[n], base, n, goal)) complete = false;
                    const bool neural=std::all_of(frames.begin(),frames.end(),[](const auto& frame){return frame.neural;});
                    Log("capture_burst_saved frames=%u complete=%d async=1 neural_rendering=%d", goal, complete, neural);
                });
                Log("capture_async_queued frames=%u accepted=%d", goal, saved);
            }
            s.captureFrames.clear();
            unsuccessful = !saved;
        }
    } else {
        s.captureFrames.clear();
        unsuccessful = selectedTargets;
    }
    if (unsuccessful) ++s.captureAttempts;
    if (saved || s.captureAttempts >= 60) {
        if (!saved) Log("capture_failed: no complete stereo burst after 60 unsuccessful stereo submissions");
        s.capturePending = false; s.captureAttempts = 0; s.captureFrames.clear();
    }
}
#ifdef TF2VRNR_BACKEND
XrResult XRAPI_CALL LocateViews(XrSession handle,const XrViewLocateInfo* info,
    XrViewState* state,uint32_t capacity,uint32_t* count,XrView* views) {
    auto s=Lookup(sessions,handle);if(!s)return XR_ERROR_HANDLE_INVALID;
    const auto r=s->instance->locateViews(handle,info,state,capacity,count,views);
    if(r!=XR_SUCCESS||!info||!state||!count||*count!=2||capacity<2||!views||
       info->viewConfigurationType!=XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO||
       !(state->viewStateFlags&XR_VIEW_STATE_ORIENTATION_VALID_BIT)||
       !(state->viewStateFlags&XR_VIEW_STATE_POSITION_VALID_BIT))return r;
    std::lock_guard lock(s->mutex);
#ifdef TF2VRNR_FRAMEGEN
    if(s->instance->config.frameGeneration&&!s->neuralFault) {
        float tangents[2][4];
        for(unsigned eye=0;eye<2;++eye)tangents[eye][0]=std::tan(views[eye].fov.angleLeft),tangents[eye][1]=std::tan(views[eye].fov.angleRight),tangents[eye][2]=std::tan(views[eye].fov.angleUp),tangents[eye][3]=std::tan(views[eye].fov.angleDown);
        SetFrameCameraProjection(s->device,tangents);
    }
#endif
    if(!s->instance->config.dlaa||s->neuralFault||!s->jitterWidth||!s->jitterHeight)return r;
    const auto jitter=DlaaJitter(s->frames);
    // NGX wants the image displacement, opposite the sample location. Both
    // axes/signs were checked against a supersampled synthetic reference.
    if(!SetNeuralJitter(s->device,-jitter.x,-jitter.y))return r;
    s->jitterTime=info->displayTime;
    for(unsigned eye=0;eye<2;++eye) {
        s->originalFovs[eye]=views[eye].fov;
        views[eye].fov=JitterFov(views[eye].fov,s->jitterWidth,s->jitterHeight,jitter);
        s->jitteredFovs[eye]=views[eye].fov;
    }
    return r;
}
#endif
#ifdef TF2VRNR_FRAMEGEN
XrResult XRAPI_CALL WaitFrame(XrSession handle,const XrFrameWaitInfo* info,XrFrameState* state) {
    auto s=Lookup(sessions,handle);if(!s)return XR_ERROR_HANDLE_INVALID;
    bool enabled=false;{std::lock_guard lock(s->mutex);enabled=s->instance->config.frameGeneration&&!s->neuralFault;}
    return s->delivery?s->delivery->Wait(info,state,enabled):s->instance->waitFrame(handle,info,state);
}
#endif
XrResult XRAPI_CALL EndFrame(XrSession handle, const XrFrameEndInfo* info) {
    auto s = Lookup(sessions, handle);
    if (!s) return XR_ERROR_HANDLE_INVALID;
    // DLAA resolves the subpixel camera sample back to the normal eye frustum.
    // Only correct a projection whose submitted FOV matches our current sample.
    // All caller-owned structures and extension chains remain untouched.
#ifdef TF2VRNR_BACKEND
    XrFrameEndInfo unjittered{};
    std::vector<const XrCompositionLayerBaseHeader*> layerPointers;
    std::vector<XrCompositionLayerProjection> projections;
    std::vector<std::array<XrCompositionLayerProjectionView,2>> projectionViews;
    XrTime jitterTime=0;
    XrFovf originalFovs[2]{},jitteredFovs[2]{};
    uint64_t eyeSwapchains[2]{};
    {std::lock_guard lock(s->mutex);jitterTime=s->jitterTime;
     for(unsigned eye=0;eye<2;++eye) {originalFovs[eye]=s->originalFovs[eye];jitteredFovs[eye]=s->jitteredFovs[eye];eyeSwapchains[eye]=s->neuralSwapchains[eye];}}
    if(info&&info->layers&&info->layerCount<=64&&jitterTime==info->displayTime&&DlaaFrameApplied(s->device)) {
        unjittered=*info;
        layerPointers.assign(info->layers,info->layers+info->layerCount);
        projections.resize(info->layerCount);projectionViews.resize(info->layerCount);
        bool corrected=false;
        for(unsigned l=0;l<info->layerCount;++l) {
            const auto* h=info->layers[l];if(!h||h->type!=XR_TYPE_COMPOSITION_LAYER_PROJECTION)continue;
            const auto* p=reinterpret_cast<const XrCompositionLayerProjection*>(h);
            if(!p->views||p->viewCount!=2)continue;
            bool match=true;
            for(unsigned eye=0;eye<2;++eye)
                match=match&&!std::memcmp(&p->views[eye].fov,&jitteredFovs[eye],sizeof(XrFovf))&&
                      reinterpret_cast<uint64_t>(p->views[eye].subImage.swapchain)==eyeSwapchains[eye];
            if(!match)continue;
            projections[l]=*p;
            for(unsigned eye=0;eye<2;++eye) {projectionViews[l][eye]=p->views[eye];projectionViews[l][eye].fov=originalFovs[eye];}
            projections[l].views=projectionViews[l].data();layerPointers[l]=reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projections[l]);
            corrected=true;
        }
        if(corrected) {unjittered.layers=layerPointers.data();info=&unjittered;}
        else {std::lock_guard lock(s->mutex);s->neuralFault=true;Log("dlaa_disabled submitted_projection_does_not_match_sample=1");}
    }
#endif
    // Preserve exact submission when DLAA is off.
    // File IO only touches owned copies and happens after the original submission.
#ifdef TF2VRNR_FRAMEGEN
    StereoFrameTextures textures;
    if(auto generated=GeneratedStereoFrame(s->device)) {
        textures.owner=generated;textures.interpolated=generated->Interpolated(0)&&generated->Interpolated(1);
        for(unsigned eye=0;eye<2;++eye){textures.real[eye]=generated->Real(eye);textures.generated[eye]=generated->Generated(eye);
            auto sc=Lookup(swapchains,reinterpret_cast<XrSwapchain>(s->neuralSwapchains[eye]));if(sc)textures.format[eye]=sc->info.format;}
    }
    const auto r=s->delivery?s->delivery->End(info,textures):s->instance->endFrame(handle,info);
#else
    const auto r = s->instance->endFrame(handle, info);
#endif
    std::lock_guard lock(s->mutex);
    if (r == XR_SUCCESS) {
        try { InspectFrame(*s, info); }
        catch (...) { Log("frame_inspection_failed exception; original frame already submitted"); s->capturePending = false; s->captureFrames.clear(); }
    } else {
        s->captureFrames.clear();
        for (auto& entry : s->histories) entry.second.Invalidate();
        Log("end_frame_result=%d; temporal history invalidated", r);
    }
    ++s->frames;
#ifdef TF2VRNR_BACKEND
    if(s->frames%30==0 && DeleteFileW((Directory()+L"\\neural.request").c_str())) {
        const auto config=ReadConfig();
        s->instance->config.neural=config.neural;
        s->instance->config.neuralResolution=config.neuralResolution;
        s->instance->config.neuralIntensity=config.neuralIntensity;
        s->instance->config.neuralTone=config.neuralTone;
        s->instance->config.neuralDetail=config.neuralDetail;
        s->instance->config.neuralPreset=config.neuralPreset;
        s->instance->config.dlaa=config.dlaa;
        s->instance->config.frameGeneration=config.frameGeneration;
        Log("fg_settings_reloaded enabled=%d",config.frameGeneration);
        Log("neural_settings_reloaded enabled=%d resolution_percent=%u intensity_percent=%u tone_percent=%u detail_percent=%u preset=%c dlaa=%d",
            config.neural,config.neuralResolution,config.neuralIntensity,config.neuralTone,config.neuralDetail,'A'+config.neuralPreset-1,config.dlaa);
    }
#endif
    if (s->instance->config.capture && !s->capturePending && !CaptureWriteBusy() && s->frames % 30 == 0 && ConsumeCaptureRequest()) {
        s->capturePending = true; s->captureAttempts = 0; s->captureFrames.clear();
        Log("capture_requested burst_frames=%u; copying before release and saving after burst", s->instance->config.burstFrames);
    }
    // Clear swapchain references; an unfinished burst retains only owned copies.
    uint32_t depthWidth = 0, depthHeight = 0;
    {
        std::lock_guard registryLock(registryMutex);
        for (auto& entry : swapchains) if (entry.second->session == s) {
            std::lock_guard imageLock(entry.second->mutex); entry.second->snapshot.reset();
            const auto& sc = *entry.second;
            if (!depthWidth && sc.info.arraySize == 1 && sc.info.sampleCount == 1 &&
                (sc.info.usageFlags & XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT) &&
                (sc.info.usageFlags & XR_SWAPCHAIN_USAGE_TRANSFER_SRC_BIT) &&
                !(sc.info.createFlags & XR_SWAPCHAIN_CREATE_PROTECTED_CONTENT_BIT) &&
                (s->captureTargets.empty() || s->captureTargets.count(entry.first))) {
                depthWidth = sc.info.width; depthHeight = sc.info.height;
            }
        }
    }
    try {
#ifdef TF2VRNR_BACKEND
        bool neuralReady=false, neuralReset=true;
        uint64_t ids[2]{};
        // This renderer uses two full-size, distinct color swapchains and
        // renders/releases the left eye before the right. Learn and validate
        // that order from actual OpenXR submissions before enabling the hook.
        if (r==XR_SUCCESS && info && info->layers && (s->instance->config.neural||s->instance->config.dlaa||s->instance->config.frameGeneration) && !s->neuralFault) {
            for(uint32_t l=0;l<std::min(info->layerCount,64u);++l) {
                const auto* header=info->layers[l];
                if(!header||header->type!=XR_TYPE_COMPOSITION_LAYER_PROJECTION)continue;
                const auto* projection=reinterpret_cast<const XrCompositionLayerProjection*>(header);
                if(!projection->views||projection->viewCount!=2)break;
                bool valid=true;neuralReset=s->neuralSpace!=projection->space;
                s->neuralSpace=projection->space;
                for(unsigned eye=0;eye<2;++eye) {
                    const auto& view=projection->views[eye];const auto layout=Layout(view.subImage);
                    if(s->instance->config.dlaa&&view.next)valid=false;
                    ids[eye]=layout.swapchain;auto sc=Lookup(swapchains,view.subImage.swapchain);
                    if(!sc||sc->session!=s||sc->info.arraySize!=1||sc->info.sampleCount!=1||layout.slice||layout.x||layout.y||
                        layout.width!=static_cast<int32_t>(depthWidth)||layout.height!=static_cast<int32_t>(depthHeight))valid=false;
                    const uint64_t generation=s->histories[(static_cast<uint64_t>(l)<<32)|eye].Generation();
                    neuralReset=neuralReset||s->neuralSwapchains[eye]!=ids[eye]||s->neuralGenerations[eye]!=generation;
                    neuralReset=neuralReset||std::memcmp(&s->neuralFovs[eye],&view.fov,sizeof(view.fov))!=0;
                    s->neuralFovs[eye]=view.fov;
                    s->neuralSwapchains[eye]=ids[eye];s->neuralGenerations[eye]=generation;
                }
                neuralReady=valid && ids[0]!=ids[1] && s->releaseOrder==std::vector<uint64_t>{ids[0],ids[1]};
                break;
            }
        }
        s->releaseOrder.clear();
        s->jitterTime=0;s->jitterWidth=neuralReady?depthWidth:0;s->jitterHeight=neuralReady?depthHeight:0;
        if(neuralReady) {
            BeginDepthCapture(s->device,depthWidth,depthHeight,s->frames,true,true);
            ConfigureNeuralFrame(s->device,s->capturePending&&s->instance->config.captureCandidates,
                s->instance->config,ids,neuralReset);
        } else
#endif
        if (s->capturePending && s->instance->config.captureCandidates &&
            (s->instance->config.depthBeforeClear || s->instance->config.renderPasses || s->instance->config.enginePost))
            BeginDepthCapture(s->device, depthWidth, depthHeight, s->frames,
                s->instance->config.renderPasses, s->instance->config.enginePost);
        else EndDepthCapture(s->device);
    } catch (...) { Log("depth_before_clear_arm_failed exception"); EndDepthCapture(s->device); }
#ifdef TF2VRNR_FRAMEGEN
    if(s->delivery)s->delivery->Complete();
#endif
    return r;
}

XrResult XRAPI_CALL GetProc(XrInstance instance, const char* name, PFN_xrVoidFunction* fn) {
    if (!name || !fn) return XR_ERROR_VALIDATION_FAILURE;
    *fn = nullptr;
    auto i = Lookup(instances, instance);
    if (!i) return XR_ERROR_HANDLE_INVALID;
#define HOOK(api, function) if (!std::strcmp(name, api)) { *fn = reinterpret_cast<PFN_xrVoidFunction>(&function); return XR_SUCCESS; }
    HOOK("xrGetInstanceProcAddr", GetProc)
    if (i->destroyInstance) { HOOK("xrDestroyInstance", DestroyInstance) }
    if (i->active) {
        HOOK("xrCreateSession", CreateSession)
        HOOK("xrDestroySession", DestroySession)
        HOOK("xrCreateSwapchain", CreateSwapchain)
        HOOK("xrDestroySwapchain", DestroySwapchain)
        HOOK("xrEnumerateSwapchainImages", EnumerateImages)
        HOOK("xrAcquireSwapchainImage", AcquireImage)
        HOOK("xrWaitSwapchainImage", WaitImage)
        HOOK("xrReleaseSwapchainImage", ReleaseImage)
        HOOK("xrEndFrame", EndFrame)
#ifdef TF2VRNR_FRAMEGEN
        if(i->waitFrame){HOOK("xrWaitFrame",WaitFrame)}
#endif
#ifdef TF2VRNR_BACKEND
        if(i->locateViews) { HOOK("xrLocateViews", LocateViews) }
#endif
    }
#undef HOOK
    return i->getProc(instance, name, fn);
}
XrResult XRAPI_CALL CreateInstance(const XrInstanceCreateInfo* info, const XrApiLayerCreateInfo* layer, XrInstance* instance) {
    if (!layer || !layer->nextInfo || !layer->nextInfo->nextCreateApiLayerInstance ||
        !layer->nextInfo->nextGetInstanceProcAddr || !instance) return XR_ERROR_INITIALIZATION_FAILED;
    auto next = *layer; next.nextInfo = layer->nextInfo->next;
    const auto r = layer->nextInfo->nextCreateApiLayerInstance(info, &next, instance);
    if (r != XR_SUCCESS) return r;
    auto i = std::make_shared<Instance>(); i->handle = *instance; i->getProc = layer->nextInfo->nextGetInstanceProcAddr;
    Resolve(*i, "xrDestroyInstance", i->destroyInstance);
    if (IsTarget()) {
        i->config = ReadConfig();
        i->active = i->config.enabled &&
            Resolve(*i, "xrCreateSession", i->createSession) && Resolve(*i, "xrDestroySession", i->destroySession) &&
            Resolve(*i, "xrCreateSwapchain", i->createSwapchain) && Resolve(*i, "xrDestroySwapchain", i->destroySwapchain) &&
            Resolve(*i, "xrEnumerateSwapchainImages", i->enumerateImages) && Resolve(*i, "xrAcquireSwapchainImage", i->acquireImage) &&
            Resolve(*i, "xrWaitSwapchainImage", i->waitImage) && Resolve(*i, "xrReleaseSwapchainImage", i->releaseImage) &&
            Resolve(*i, "xrEndFrame", i->endFrame);
        Resolve(*i,"xrLocateViews",i->locateViews);
#ifdef TF2VRNR_FRAMEGEN
        Resolve(*i,"xrWaitFrame",i->waitFrame);Resolve(*i,"xrBeginFrame",i->beginFrame);
#endif
        Log("TF2VRNeuralRendering 0.1.0 build=frame-generation-camera-2 active=%d mode=graphics capture=%d runtime_selection=unchanged",
            i->active, i->config.capture);
        PFN_xrGetInstanceProperties properties = nullptr;
        if (Resolve(*i, "xrGetInstanceProperties", properties)) {
            XrInstanceProperties p{XR_TYPE_INSTANCE_PROPERTIES};
            if (properties(*instance, &p) == XR_SUCCESS) Log("runtime=%s", p.runtimeName);
        }
    }
    { std::lock_guard lock(registryMutex); instances[*instance] = i; }
    return r;
}
}
}
extern "C" __declspec(dllexport) XrResult XRAPI_CALL xrNegotiateLoaderApiLayerInterface(
    const XrNegotiateLoaderInfo* loader, const char*, XrNegotiateApiLayerRequest* request) {
    if (!loader || !request || loader->structType != XR_LOADER_INTERFACE_STRUCT_LOADER_INFO ||
        loader->structVersion != XR_LOADER_INFO_STRUCT_VERSION || loader->structSize != sizeof(*loader) ||
        request->structType != XR_LOADER_INTERFACE_STRUCT_API_LAYER_REQUEST ||
        request->structVersion != XR_API_LAYER_INFO_STRUCT_VERSION || request->structSize != sizeof(*request) ||
        loader->minInterfaceVersion > XR_CURRENT_LOADER_API_LAYER_VERSION ||
        loader->maxInterfaceVersion < XR_CURRENT_LOADER_API_LAYER_VERSION) return XR_ERROR_INITIALIZATION_FAILED;
    const auto version = std::min<XrVersion>(XR_CURRENT_API_VERSION, loader->maxApiVersion);
    if (version < loader->minApiVersion) return XR_ERROR_INITIALIZATION_FAILED;
    request->layerInterfaceVersion = XR_CURRENT_LOADER_API_LAYER_VERSION;
    request->layerApiVersion = version;
    request->getInstanceProcAddr = tf2vrnr::GetProc;
    request->createApiLayerInstance = tf2vrnr::CreateInstance;
    return XR_SUCCESS;
}
