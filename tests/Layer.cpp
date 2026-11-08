// Actual layer entry points, simulated downstream runtime. No headset, GPU or
// OpenXR registry is touched. The capture seam checks ownership call ordering.
#include "../src/layer/Layer.cpp"
#include <cstdlib>
using namespace tf2vrnr;
namespace {
unsigned checks = 0, creates = 0, releaseCalls = 0, endCalls = 0, copies = 0, writes = 0, candidateWrites = 0;
bool target = true, captureFirst = true, rejectTransfer = false, requestCapture = false;
bool failRelease = false;
bool overwriteImageHeaders = false;
bool depthArmed = false;
#ifdef TF2VRNR_BACKEND
bool fakeDlaaApplied=false,fakeJitterAccepted=true;
PixelJitter lastJitter;
XrFovf runtimeFovs[2]={{-.85f,.8f,.9f,-.9f},{-.8f,.85f,.9f,-.9f}};
XrFovf submittedFovs[2]{};
XrResult XRAPI_CALL FakeLocate(XrSession,const XrViewLocateInfo*,XrViewState* state,uint32_t capacity,uint32_t* count,XrView* views) {
    *count=2;if(capacity<2)return XR_ERROR_SIZE_INSUFFICIENT;
    state->viewStateFlags=XR_VIEW_STATE_POSITION_VALID_BIT|XR_VIEW_STATE_ORIENTATION_VALID_BIT;
    for(unsigned i=0;i<2;++i)views[i].fov=runtimeFovs[i];
    return XR_SUCCESS;
}
#endif
uint32_t depthWidth = 0, depthHeight = 0;
XrResult waitResult = XR_SUCCESS, frameResult = XR_SUCCESS;
uintptr_t nextInstance = 10, nextSession = 100, nextSwapchain = 1000;
uint32_t acquiredIndex = 0;
XrSwapchainUsageFlags lastUsage = 0;
const XrFrameEndInfo* forwardedFrame = nullptr;
std::vector<uint32_t> capturedSlices;
std::vector<unsigned> writtenCopyIds;
std::vector<std::wstring> writtenPaths;
unsigned readbackNotBeforeEndCalls = 0;
bool deferWrites = false;
std::function<void()> queuedWrite;
void Check(bool ok, const char* label) {
    ++checks;
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", label); std::exit(1); }
}
template<class T> T Handle(uintptr_t n) { return reinterpret_cast<T>(n); }
XrResult XRAPI_CALL FakeDestroyInstance(XrInstance) { return XR_SUCCESS; }
XrResult XRAPI_CALL FakeCreateSession(XrInstance, const XrSessionCreateInfo*, XrSession* s) { *s = Handle<XrSession>(nextSession++); return XR_SUCCESS; }
XrResult XRAPI_CALL FakeDestroySession(XrSession) { return XR_SUCCESS; }
XrResult XRAPI_CALL FakeCreateSwapchain(XrSession, const XrSwapchainCreateInfo* info, XrSwapchain* sc) {
    ++creates; lastUsage = info->usageFlags;
    if (rejectTransfer && (info->usageFlags & XR_SWAPCHAIN_USAGE_TRANSFER_SRC_BIT)) return XR_ERROR_FEATURE_UNSUPPORTED;
    *sc = Handle<XrSwapchain>(nextSwapchain++); return XR_SUCCESS;
}
XrResult XRAPI_CALL FakeDestroySwapchain(XrSwapchain) { return XR_SUCCESS; }
XrResult XRAPI_CALL FakeEnumerate(XrSwapchain, uint32_t capacity, uint32_t* count, XrSwapchainImageBaseHeader* headers) {
    *count = 3;
    if (!capacity) return XR_SUCCESS;
    if (capacity < 3) return XR_ERROR_SIZE_INSUFFICIENT;
    auto images = reinterpret_cast<XrSwapchainImageD3D11KHR*>(headers);
    for (unsigned i = 0; i < 3; ++i) {
        images[i].texture = Handle<ID3D11Texture2D*>(i + 1);
        // Proton's D3D11 bridge copies an internal array whose type/next fields
        // are uninitialized. Reproduce the header bytes observed in the game.
        if (overwriteImageHeaders) {
            images[i].type = static_cast<XrStructureType>(0x616e6946);
            images[i].next = Handle<void*>(0x12345678);
        }
    }
    return XR_SUCCESS;
}
XrResult XRAPI_CALL FakeAcquire(XrSwapchain, const XrSwapchainImageAcquireInfo*, uint32_t* index) { *index = acquiredIndex; return XR_SUCCESS; }
XrResult XRAPI_CALL FakeWait(XrSwapchain, const XrSwapchainImageWaitInfo*) { return waitResult; }
XrResult XRAPI_CALL FakeRelease(XrSwapchain, const XrSwapchainImageReleaseInfo*) { ++releaseCalls; return failRelease ? XR_ERROR_RUNTIME_FAILURE : XR_SUCCESS; }
XrResult XRAPI_CALL FakeEnd(XrSession, const XrFrameEndInfo* info) { ++endCalls; forwardedFrame = info;
#ifdef TF2VRNR_BACKEND
    if(info&&info->layers&&info->layerCount&&info->layers[0]->type==XR_TYPE_COMPOSITION_LAYER_PROJECTION) {
        const auto* p=reinterpret_cast<const XrCompositionLayerProjection*>(info->layers[0]);
        if(p->views&&p->viewCount==2)for(unsigned i=0;i<2;++i)submittedFovs[i]=p->views[i].fov;
    }
#endif
    return frameResult; }
XrResult XRAPI_CALL FakeSync(XrSession, const XrActionsSyncInfo*) { return XR_SESSION_NOT_FOCUSED; }
XrResult XRAPI_CALL FakeProc(XrInstance, const char* name, PFN_xrVoidFunction* fn) {
#define FN(api, f) if (!std::strcmp(name, api)) { *fn = reinterpret_cast<PFN_xrVoidFunction>(&f); return XR_SUCCESS; }
    FN("xrDestroyInstance", FakeDestroyInstance)
    FN("xrCreateSession", FakeCreateSession)
    FN("xrDestroySession", FakeDestroySession)
    FN("xrCreateSwapchain", FakeCreateSwapchain)
    FN("xrDestroySwapchain", FakeDestroySwapchain)
    FN("xrEnumerateSwapchainImages", FakeEnumerate)
    FN("xrAcquireSwapchainImage", FakeAcquire)
    FN("xrWaitSwapchainImage", FakeWait)
    FN("xrReleaseSwapchainImage", FakeRelease)
    FN("xrEndFrame", FakeEnd)
#ifdef TF2VRNR_BACKEND
    FN("xrLocateViews", FakeLocate)
#endif
    FN("xrSyncActions", FakeSync)
#undef FN
    *fn = nullptr; return XR_ERROR_FUNCTION_UNSUPPORTED;
}
XrResult XRAPI_CALL FakeCreateInstance(const XrInstanceCreateInfo*, const XrApiLayerCreateInfo* info, XrInstance* i) {
    Check(info->nextInfo == nullptr, "layer advances chain exactly once");
    *i = Handle<XrInstance>(nextInstance++); return XR_SUCCESS;
}
struct FakeImage : CapturedImage {
    unsigned id;
    bool cpuReady = false;
    explicit FakeImage(unsigned value) : id(value) {}
    bool PrepareForAsyncWrite() override {
        Check(endCalls >= readbackNotBeforeEndCalls, "CPU readback waits for the whole burst");
        cpuReady = true; return true;
    }
    void WriteCandidates(const ViewLayout&, const std::wstring&) override {
        Check(endCalls > 0, "candidate readback occurs after original frame submission");
        ++candidateWrites;
    }
    bool WriteView(const ViewLayout& view, const std::wstring& path) override {
        Check(cpuReady && endCalls > 0 && endCalls >= readbackNotBeforeEndCalls,
            "writer receives only prepared CPU snapshots after frame submissions");
        writtenCopyIds.push_back(id); writtenPaths.push_back(path);
        capturedSlices.push_back(view.slice); ++writes; return true;
    }
};
}
namespace tf2vrnr {
#ifdef TF2VRNR_BACKEND
void ConfigureNeuralFrame(ID3D11Device*,bool,const Config&,const uint64_t (&)[2],bool) {}
void DestroyNeuralBackend(ID3D11Device*) {}
bool ReleaseNeuralView(ID3D11Device*,uint64_t) { return true; }
bool NeuralFrameApplied(ID3D11Device*) { return false; }
bool DlaaFrameApplied(ID3D11Device*) { return fakeDlaaApplied; }
bool SetNeuralJitter(ID3D11Device*,float x,float y) {lastJitter={x,y};return fakeJitterAccepted;}
#endif
void BeginDepthCapture(ID3D11Device*, uint32_t w, uint32_t h, uint64_t, bool, bool) { depthArmed = true; depthWidth = w; depthHeight = h; }
void EndDepthCapture(ID3D11Device*) { depthArmed = false; }
DepthBatch TakeDepthCapture(ID3D11Device*) { return {}; }
std::wstring Directory() { return L"C:\\nonexistent-neural-test"; }
Config ReadConfig() { Config c; c.captureFirst = captureFirst; return c; }
bool IsTarget() { return target; }
void Log(const char*, ...) {}
bool ConsumeCaptureRequest() { bool r = requestCapture; requestCapture = false; return r; }
std::wstring CaptureDirectory() { return L"C:\\nonexistent-neural-test"; }
bool CaptureWriteBusy() { return bool(queuedWrite); }
bool QueueCaptureWrite(std::function<void()> work) {
    if (queuedWrite) return false;
    if (deferWrites) queuedWrite = std::move(work); else work();
    return true;
}
std::shared_ptr<CapturedImage> CaptureImage(ID3D11Device*, ID3D11Texture2D* texture, int64_t, bool, bool) {
    Check(releaseCalls == copies, "GPU copy happens before downstream image release");
    Check(texture == Handle<ID3D11Texture2D*>(3), "captures acquired image index, not assumed index zero");
    ++copies; return std::make_shared<FakeImage>(copies);
}
}
int main() {
    XrNegotiateLoaderInfo loader{}; loader.structType = XR_LOADER_INTERFACE_STRUCT_LOADER_INFO;
    loader.structVersion = XR_LOADER_INFO_STRUCT_VERSION; loader.structSize = sizeof(loader);
    loader.minInterfaceVersion = loader.maxInterfaceVersion = XR_CURRENT_LOADER_API_LAYER_VERSION;
    loader.minApiVersion = XR_MAKE_VERSION(1,0,0); loader.maxApiVersion = XR_CURRENT_API_VERSION;
    XrNegotiateApiLayerRequest req{}; req.structType = XR_LOADER_INTERFACE_STRUCT_API_LAYER_REQUEST;
    req.structVersion = XR_API_LAYER_INFO_STRUCT_VERSION; req.structSize = sizeof(req);
    Check(xrNegotiateLoaderApiLayerInterface(&loader, nullptr, &req) == XR_SUCCESS, "valid loader negotiation");
    loader.structSize = 1;
    Check(xrNegotiateLoaderApiLayerInterface(&loader, nullptr, &req) == XR_ERROR_INITIALIZATION_FAILED, "reject wrong loader ABI");
    loader.structSize = sizeof(loader);
    XrApiLayerNextInfo next{}; next.nextGetInstanceProcAddr = FakeProc; next.nextCreateApiLayerInstance = FakeCreateInstance;
    XrApiLayerCreateInfo layer{}; layer.nextInfo = &next;
    XrInstance instance{};
    Check(req.createApiLayerInstance(nullptr, &layer, &instance) == XR_SUCCESS, "instance creation");
    PFN_xrVoidFunction fn = nullptr;
    Check(req.getInstanceProcAddr(instance, "xrEndFrame", &fn) == XR_SUCCESS && fn == reinterpret_cast<PFN_xrVoidFunction>(&EndFrame), "graphics function intercepted");
    Check(req.getInstanceProcAddr(instance, "xrSyncActions", &fn) == XR_SUCCESS && fn == reinterpret_cast<PFN_xrVoidFunction>(&FakeSync), "controls layer calls pass through untouched");
    Check(req.getInstanceProcAddr(instance, "missing", &fn) == XR_ERROR_FUNCTION_UNSUPPORTED && !fn, "unknown functions retain downstream error");
    XrGraphicsBindingD3D11KHR binding{XR_TYPE_GRAPHICS_BINDING_D3D11_KHR}; binding.device = Handle<ID3D11Device*>(1);
    XrSessionCreateInfo si{XR_TYPE_SESSION_CREATE_INFO}; si.next = &binding;
    XrSession session{}; Check(CreateSession(instance, &si, &session) == XR_SUCCESS, "D3D11 session tracked");
    XrSwapchainCreateInfo ci{XR_TYPE_SWAPCHAIN_CREATE_INFO}; ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
    ci.width = 128; ci.height = 64; ci.arraySize = 2; ci.mipCount = ci.faceCount = ci.sampleCount = 1; ci.format = DXGI_FORMAT_R8G8B8A8_UNORM;
    XrSwapchain sc{}; Check(CreateSwapchain(session, &ci, &sc) == XR_SUCCESS, "swapchain creation");
    Check(lastUsage & XR_SWAPCHAIN_USAGE_TRANSFER_SRC_BIT, "copy permission requested explicitly");
    Check(ci.usageFlags == XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT, "application creation structure not mutated");
    XrSwapchainImageD3D11KHR images[3]{}; for (auto& im : images) im.type = XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR;
    uint32_t count = 0; Check(EnumerateImages(sc, 3, &count, reinterpret_cast<XrSwapchainImageBaseHeader*>(images)) == XR_SUCCESS, "D3D11 images tracked");
    Check(Lookup(swapchains, sc)->images.size() == 3, "normal runtime images retained");
    XrSwapchain protonSc{}; CreateSwapchain(session, &ci, &protonSc);
    overwriteImageHeaders = true;
    Check(EnumerateImages(protonSc, 3, &count, reinterpret_cast<XrSwapchainImageBaseHeader*>(images)) == XR_SUCCESS,
        "Proton-style enumeration succeeds");
    const std::vector<ID3D11Texture2D*> expectedImages = {
        Handle<ID3D11Texture2D*>(1), Handle<ID3D11Texture2D*>(2), Handle<ID3D11Texture2D*>(3)};
    Check(Lookup(swapchains, protonSc)->images == expectedImages,
        "texture pointers retained when Proton overwrites type and next");
    Check(images[0].type == static_cast<XrStructureType>(0x616e6946) && images[0].next == Handle<void*>(0x12345678),
        "layer leaves runtime output untouched and does not traverse overwritten next");
    Check(EnumerateImages(protonSc, 0, &count, nullptr) == XR_SUCCESS && Lookup(swapchains, protonSc)->images == expectedImages,
        "count-only query preserves known textures");
    for (auto& im : images) { im.type = XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR; im.next = nullptr; }
    Check(EnumerateImages(protonSc, 2, &count, reinterpret_cast<XrSwapchainImageBaseHeader*>(images)) == XR_ERROR_SIZE_INSUFFICIENT &&
        Lookup(swapchains, protonSc)->images == expectedImages, "failed enumeration preserves known textures");
    XrSwapchain unknownSc{}; CreateSwapchain(session, &ci, &unknownSc);
    images[0].type = XR_TYPE_UNKNOWN;
    EnumerateImages(unknownSc, 3, &count, reinterpret_cast<XrSwapchainImageBaseHeader*>(images));
    Check(Lookup(swapchains, unknownSc)->images.empty(), "unknown caller image layout is never treated as D3D11");
    overwriteImageHeaders = false;
    DestroySwapchain(unknownSc); DestroySwapchain(protonSc);
    acquiredIndex = 2; uint32_t index = 99;
    AcquireImage(sc, nullptr, &index);
    waitResult = XR_TIMEOUT_EXPIRED;
    Check(WaitImage(sc, nullptr) == XR_TIMEOUT_EXPIRED && !Lookup(swapchains, sc)->queue.Writable(), "timeout is preserved and does not grant ownership");
    waitResult = XR_SUCCESS; WaitImage(sc, nullptr);
    Check(ReleaseImage(sc, nullptr) == XR_SUCCESS && copies == 1, "release copies one owned image");
    XrCompositionLayerDepthInfoKHR depth{XR_TYPE_COMPOSITION_LAYER_DEPTH_INFO_KHR};
    XrCompositionLayerProjectionView views[2]{};
    for (unsigned eye = 0; eye < 2; ++eye) {
        views[eye].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
        views[eye].subImage.swapchain = sc; views[eye].subImage.imageArrayIndex = eye;
        views[eye].subImage.imageRect = {{0,0},{128,64}}; views[eye].pose.orientation.w = 1;
    }
    views[0].next = &depth;
    XrCompositionLayerProjection projection{XR_TYPE_COMPOSITION_LAYER_PROJECTION}; projection.viewCount = 2; projection.views = views;
    const XrCompositionLayerBaseHeader* headers[] = {reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projection)};
    XrFrameEndInfo frame{XR_TYPE_FRAME_END_INFO}; frame.displayTime = 1000000000; frame.layerCount = 1; frame.layers = headers;
    Check(EndFrame(session, &frame) == XR_SUCCESS && forwardedFrame == &frame, "exact original frame forwarded");
    Check(views[0].next == &depth && frame.layers == headers, "extension chains and original views preserved");
    Check(writes == 2 && capturedSlices == std::vector<uint32_t>({0,1}), "both eyes captured from distinct array slices");
    Check(candidateWrites == 2, "both eyes route candidate copies through the capture sink");
    Check(!Lookup(sessions, session)->capturePending, "one complete pair satisfies capture request");
    Check(!depthArmed, "successful capture leaves depth observer disarmed");
    Check(!Lookup(swapchains, sc)->snapshot, "staging copy freed after use");
    Check(Lookup(sessions, session)->histories.size() == 2, "independent eye histories");
    frameResult = XR_ERROR_SESSION_LOST;
    Check(EndFrame(session, &frame) == frameResult, "session-loss result preserved");
    frameResult = XR_SUCCESS; frame.displayTime += 11000000;
    EndFrame(session, &frame);
    Check(Lookup(sessions, session)->histories[0].Generation() == 2, "failed frame resets next history");
    Check(copies == 1, "normal frames do not make GPU copies");
    auto sessionState = Lookup(sessions, session);
    sessionState->capturePending = true; sessionState->captureAttempts = 0;
    XrFrameEndInfo emptyFrame{XR_TYPE_FRAME_END_INFO};
    for (unsigned n = 0; n < 120; ++n) EndFrame(session, &emptyFrame);
    Check(sessionState->capturePending && sessionState->captureAttempts == 0,
        "headset-off empty submissions do not consume capture request");
    // Array captures are deliberately not used to infer an early depth layout.
    Check(depthWidth == 0 && depthHeight == 0, "array swapchain does not supply ambiguous early-depth dimensions");
    for (unsigned n = 0; n < 59; ++n) EndFrame(session, &frame);
    Check(sessionState->capturePending && sessionState->captureAttempts == 59,
        "capture remains pending through 59 unsuccessful stereo submissions");
    EndFrame(session, &frame);
    Check(!sessionState->capturePending, "60 unsuccessful stereo submissions expire request");
    Check(!depthArmed, "expired request disarms early-depth capture");
    sessionState->instance->config.burstFrames = 2;
    auto drawStereo = [&] {
        AcquireImage(sc, nullptr, &index); WaitImage(sc, nullptr); ReleaseImage(sc, nullptr);
        frame.displayTime += 11000000;
        EndFrame(session, &frame);
    };
    sessionState->capturePending = true;
    const auto beforeBurstWrites = writes, beforeBurstCopies = copies;
    readbackNotBeforeEndCalls = endCalls + 2;
    views[0].pose.position.x = 0.125f;
    drawStereo();
    Check(writes == beforeBurstWrites && sessionState->captureFrames.size() == 1,
        "first burst frame is held without any readback");
    auto firstFrameCopy = std::weak_ptr<CapturedImage>(sessionState->captureFrames[0].views[0].snapshot);
    Check(!Lookup(swapchains, sc)->snapshot && !firstFrameCopy.expired(),
        "held frame owns staging copy after swapchain reference is cleared");
    views[0].pose.position.x = 0.75f;
    Check(sessionState->captureFrames[0].views[0].pose.position.x == 0.125f,
        "previous frame metadata is independent of caller memory");
    drawStereo();
    Check(writes == beforeBurstWrites + 4 && !sessionState->capturePending && sessionState->captureFrames.empty(),
        "two consecutive stereo frames complete one burst and release retained frames");
    Check(firstFrameCopy.expired(), "held copy is released after burst readback");
    Check(std::vector<unsigned>(writtenCopyIds.end()-4, writtenCopyIds.end()) ==
        std::vector<unsigned>({beforeBurstCopies+1,beforeBurstCopies+1,beforeBurstCopies+2,beforeBurstCopies+2}),
        "each saved eye comes from its own frame snapshot");
    Check(writtenPaths[writtenPaths.size()-4] != writtenPaths[writtenPaths.size()-2],
        "burst frame filenames cannot overwrite one another");
    const auto afterBurstWrites = writes;
    sessionState->capturePending = true;
    drawStereo();
    EndFrame(session, &emptyFrame);
    Check(sessionState->captureFrames.empty() && writes == afterBurstWrites,
        "empty submission drops incomplete burst instead of joining nonconsecutive frames");
    drawStereo();
    Check(writes == afterBurstWrites && sessionState->captureFrames.size() == 1,
        "first frame after a gap starts a fresh burst");
    frameResult = XR_ERROR_RUNTIME_FAILURE;
    EndFrame(session, &frame); frameResult = XR_SUCCESS;
    Check(sessionState->captureFrames.empty(), "failed runtime submission drops held burst");
    drawStereo();
    views[1].subImage.imageRect.extent.width = 127;
    drawStereo();
    Check(writes == afterBurstWrites && sessionState->captureFrames.size() == 1,
        "layout change restarts burst without mixing eye layouts");
    drawStereo();
    Check(writes == afterBurstWrites + 4 && !sessionState->capturePending,
        "two matching frames after discontinuity complete a fresh burst");
    views[1].subImage.imageRect.extent.width = 128;
    readbackNotBeforeEndCalls = 0;
    deferWrites = true;
    sessionState->capturePending = true;
    const auto beforeAsyncWrites = writes;
    drawStereo(); drawStereo();
    Check(queuedWrite && writes == beforeAsyncWrites && !sessionState->capturePending,
        "graphics submission queues CPU-only IO and returns before files are written");
    Check(sessionState->captureFrames.empty() && !Lookup(swapchains, sc)->snapshot,
        "graphics state releases burst references once worker owns CPU data");
    requestCapture = true;
    const auto beforeBusyCopies = copies;
    for (unsigned n = 0; n < 60; ++n) { frame.displayTime += 11000000; EndFrame(session, &frame); }
    Check(requestCapture && copies == beforeBusyCopies && !sessionState->capturePending,
        "busy IO worker defers new requests without queuing more GPU or CPU payloads");
    rejectTransfer = true; const auto oldCreates = creates;
    XrSwapchain fallback{}; CreateSwapchain(session, &ci, &fallback);
    Check(creates == oldCreates + 2 && lastUsage == ci.usageFlags, "unsupported capture usage retries original swapchain");
    rejectTransfer = false;
    Check(DestroySwapchain(fallback) == XR_SUCCESS && !Lookup(swapchains, fallback), "destroy removes swapchain state");
#ifdef TF2VRNR_BACKEND
    // Exercise the real locate/submit interception with a fake downstream runtime.
    auto live=Lookup(sessions,session);live->instance->config.dlaa=true;
    live->instance->config.capture=false;live->capturePending=false;live->frames=5;
    live->jitterWidth=2476;live->jitterHeight=2594;
    XrViewLocateInfo locate{XR_TYPE_VIEW_LOCATE_INFO};locate.viewConfigurationType=XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    locate.displayTime=frame.displayTime+11000000;
    XrViewState viewState{XR_TYPE_VIEW_STATE};XrView located[2]{};
    for(auto& v:located)v.type=XR_TYPE_VIEW;
    count=0;
    Check(LocateViews(session,&locate,&viewState,2,&count,located)==XR_SUCCESS,"DLAA locate succeeds");
    Check(std::memcmp(&located[0].fov,&runtimeFovs[0],sizeof(XrFovf))!=0,"camera sample changes FOV");
    const auto first=located[0].fov;
    LocateViews(session,&locate,&viewState,2,&count,located);
    Check(!std::memcmp(&first,&located[0].fov,sizeof(first)),"repeated locate does not accumulate jitter");
    for(unsigned eye=0;eye<2;++eye) {
        views[eye].fov=located[eye].fov;views[eye].next=nullptr;
        live->neuralSwapchains[eye]=reinterpret_cast<uint64_t>(views[eye].subImage.swapchain);
        const auto f=located[eye].fov;const auto original=runtimeFovs[eye];
        const float width=std::tan(original.angleRight)-std::tan(original.angleLeft);
        const float displacement=(std::tan(f.angleLeft)-std::tan(original.angleLeft))/width*2476;
        Check(std::abs(displacement+lastJitter.x)<.001f,"eye sample equals requested subpixel displacement");
    }
    frame.displayTime=locate.displayTime;fakeDlaaApplied=true;
    EndFrame(session,&frame);
    Check(!std::memcmp(&submittedFovs[0],&runtimeFovs[0],sizeof(XrFovf))&&
          !std::memcmp(&submittedFovs[1],&runtimeFovs[1],sizeof(XrFovf)),"resolved stereo uses original runtime FOVs");
    Check(!std::memcmp(&views[0].fov,&first,sizeof(first)),"caller projection never mutated");
    Check(!live->neuralFault,"valid projection accepted");
    fakeDlaaApplied=false;live->instance->config.dlaa=false;
    live->jitterWidth=2476;live->jitterHeight=2594;
    LocateViews(session,&locate,&viewState,2,&count,located);
    Check(!std::memcmp(&located[0].fov,&runtimeFovs[0],sizeof(XrFovf)),"DLAA off restores exact runtime FOV");
    live->instance->config.dlaa=true;fakeJitterAccepted=false;
    LocateViews(session,&locate,&viewState,2,&count,located);
    Check(!std::memcmp(&located[0].fov,&runtimeFovs[0],sizeof(XrFovf)),"failed backend cannot jitter camera");
    live->instance->config.dlaa=false;
#endif
    Check(DestroySession(session) == XR_SUCCESS && !Lookup(swapchains, sc), "session teardown clears its swapchains");
    auto finishWrite = std::move(queuedWrite); finishWrite(); finishWrite = {};
    Check(writes == beforeAsyncWrites + 4 && !CaptureWriteBusy(),
        "queued CPU images can finish after XR session teardown");
    target = false;
    XrInstance other{}; req.createApiLayerInstance(nullptr, &layer, &other);
    Check(req.getInstanceProcAddr(other, "xrEndFrame", &fn) == XR_SUCCESS && fn == reinterpret_cast<PFN_xrVoidFunction>(&FakeEnd), "other applications are untouched");
    DestroyInstance(other); DestroyInstance(instance);
    Check(instances.empty() && sessions.empty() && swapchains.empty(), "instance teardown leaves no dispatch state");
    std::printf("PASS: %u OpenXR layer checks\n", checks);
}
