#include "../src/layer/FrameDelivery.h"
#include "../src/backend/FrameCamera.h"
#include <cstdio>
#include <cstdlib>
#include <map>
#include <thread>
#include <future>
using namespace tf2vrnr;
namespace {
unsigned checks=0;
void Check(bool yes,const char* why){++checks;if(!yes){std::fprintf(stderr,"FAIL: %s\n",why);std::exit(1);}}
template<class T>T H(uintptr_t p){return reinterpret_cast<T>(p);}
constexpr XrDuration Period=11111111;
XrTime now=1000000000;unsigned ends=0,begins=0,waits=0,copyCount=0,generatedCopies=0;
bool render=true,failCopy=false,failCreate=false;uintptr_t next=100;
std::map<XrSwapchain,unsigned> ownership;
std::vector<XrTime> submittedTimes;std::vector<float> positions;
XrResult XRAPI_CALL Wait(XrSession,const XrFrameWaitInfo*,XrFrameState* s){++waits;now+=Period;s->predictedDisplayTime=now;s->predictedDisplayPeriod=Period;s->shouldRender=render;return XR_SUCCESS;}
XrResult XRAPI_CALL Begin(XrSession,const XrFrameBeginInfo*){++begins;return XR_SUCCESS;}
XrResult XRAPI_CALL End(XrSession,const XrFrameEndInfo* info){++ends;submittedTimes.push_back(info->displayTime);Check(info->displayTime==now,"submit in runtime display slot");
    if(info->layerCount){auto* p=reinterpret_cast<const XrCompositionLayerProjection*>(info->layers[0]);for(unsigned eye=0;eye<2;++eye){auto it=ownership.find(p->views[eye].subImage.swapchain);if(it!=ownership.end())Check(it->second==0,"submitted image was released");}positions.push_back(p->views[0].pose.position.x);}return XR_SUCCESS;}
XrResult XRAPI_CALL Create(XrSession,const XrSwapchainCreateInfo* info,XrSwapchain* sc){if(failCreate)return XR_ERROR_RUNTIME_FAILURE;Check(info->usageFlags&XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT,"owned chains allow writes");*sc=H<XrSwapchain>(next++);ownership[*sc]=0;return XR_SUCCESS;}
XrResult XRAPI_CALL Destroy(XrSwapchain sc){ownership.erase(sc);return XR_SUCCESS;}
XrResult XRAPI_CALL Enumerate(XrSwapchain sc,uint32_t capacity,uint32_t* count,XrSwapchainImageBaseHeader* p){*count=1;if(capacity){auto* images=reinterpret_cast<XrSwapchainImageD3D11KHR*>(p);images[0].texture=H<ID3D11Texture2D*>(reinterpret_cast<uintptr_t>(sc));}return XR_SUCCESS;}
XrResult XRAPI_CALL Acquire(XrSwapchain sc,const XrSwapchainImageAcquireInfo*,uint32_t* n){Check(ownership.at(sc)==0,"acquire only released image");ownership[sc]=1;*n=0;return XR_SUCCESS;}
XrResult XRAPI_CALL WaitImage(XrSwapchain sc,const XrSwapchainImageWaitInfo*){Check(ownership.at(sc)==1,"wait after acquire");ownership[sc]=2;return XR_SUCCESS;}
XrResult XRAPI_CALL Release(XrSwapchain sc,const XrSwapchainImageReleaseInfo*){Check(ownership.at(sc)==2,"release after wait");ownership[sc]=0;return XR_SUCCESS;}
FrameDeliveryDispatch Api(){return {Wait,Begin,End,Create,Destroy,Enumerate,Acquire,WaitImage,Release};}
}
namespace tf2vrnr {
void Log(const char*,...){}
bool CopyPresentationImage(ID3D11Device*,ID3D11Texture2D* to,ID3D11Texture2D* from){Check(ownership.at(H<XrSwapchain>(reinterpret_cast<uintptr_t>(to)))==2,"copy only while owned and waited");++copyCount;if(reinterpret_cast<uintptr_t>(from)>=12)++generatedCopies;return !failCopy;}
}
int main(){
    float bytes[144]{};bytes[0]=2;bytes[4]=2;bytes[9]=2;bytes[14]=1;bytes[15]=-2;bytes[18]=1;bytes[114]=1000;bytes[115]=1000;bytes[135]=bytes[136]=bytes[137]=1;
    auto camera=DecodeFrameCamera(bytes,1000,1000);Check(camera.valid&&camera.nearPlane==2&&camera.farPlane==1e7f,"infinite world camera decoded");
    Matrix4 inv;Check(Inverse4(camera.worldToClip,inv),"camera invertible");auto identity=Multiply4(camera.worldToClip,inv);for(unsigned k=0;k<16;++k)Check(std::abs(identity[k]-(k%5==0))<1e-5f,"inverse roundtrip");
    bytes[1]=10;auto translated=DecodeFrameCamera(bytes,1000,1000);Check(translated.valid&&translated.worldToClip[3]==-20,"world movement included");
    bytes[135]=.1f;Check(!DecodeFrameCamera(bytes,1000,1000).valid,"weapon depth camera rejected");bytes[135]=1;bytes[19]=1;Check(!DecodeFrameCamera(bytes,1000,1000).valid,"UI camera rejected");bytes[19]=0;bytes[114]=500;Check(!DecodeFrameCamera(bytes,1000,1000).valid,"wrong resolution rejected");
    FrameCameraProjection xr{-.5f,.5f,.5f,-.5f,true};
    bytes[100]=INFINITY;bytes[0]=16;bytes[135]=.2f;
    Check(DecodeFrameCamera(bytes,1000,1000,0,0,&xr).valid,"actual XR projection validates despite stale desktop metadata and unused infinity");
    auto wrong=xr;wrong.right=1;Check(!DecodeFrameCamera(bytes,1000,1000,0,0,&wrong).valid,"different camera projection still rejected");
    bytes[4]=2;bytes[6]=.2f;bytes[9]=2.5f;bytes[10]=-.1f;bytes[15]=-.1f;
    xr={-.6f,.4f,.44f,-.36f,true};
    Check(DecodeFrameCamera(bytes,1000,1000,0,0,&xr).valid,"asymmetric VR eye and small near plane accepted");
    bytes[6]+=2*.25f/1000;bytes[10]-=2*(-.2f)/1000;
    Check(DecodeFrameCamera(bytes,1000,1000,.25f,-.2f,&xr).valid,"jitter removed before XR projection comparison");
    bytes[19]=1;Check(!DecodeFrameCamera(bytes,1000,1000,.25f,-.2f,&xr).valid,"UI projection never accepted through XR validation");
    StereoFrameTextures textures;textures.owner=std::make_shared<int>(1);textures.interpolated=true;
    for(unsigned e=0;e<2;++e){textures.real[e]=H<ID3D11Texture2D*>(10+e);textures.generated[e]=H<ID3D11Texture2D*>(12+e);textures.format[e]=29;}
    XrCompositionLayerProjectionView views[2]{};for(unsigned e=0;e<2;++e){views[e].type=XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;views[e].pose.orientation.w=1;views[e].subImage.swapchain=H<XrSwapchain>(e+1);views[e].subImage.imageRect.extent={1000,1000};}
    XrCompositionLayerProjection projection{XR_TYPE_COMPOSITION_LAYER_PROJECTION};projection.space=H<XrSpace>(5);projection.viewCount=2;projection.views=views;
    const auto* layer=reinterpret_cast<XrCompositionLayerBaseHeader*>(&projection);XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO};end.layerCount=1;end.layers=&layer;end.environmentBlendMode=XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    XrFrameWaitInfo wait{XR_TYPE_FRAME_WAIT_INFO};XrFrameState state{XR_TYPE_FRAME_STATE};
    {
        FrameDelivery d(H<XrSession>(1),H<ID3D11Device*>(1),Api());
        Check(d.Wait(&wait,&state,false)==XR_SUCCESS&&state.predictedDisplayPeriod==Period,"off preserves timing");end.displayTime=state.predictedDisplayTime;Check(d.End(&end,{})==XR_SUCCESS,"off passes submission");d.Complete();Check(ends==1&&waits==1&&copyCount==0,"off has no extra work");
        d.Wait(&wait,&state,true);end.displayTime=state.predictedDisplayTime;d.End(&end,textures);d.Complete();Check(ends==2&&waits==2&&copyCount==0,"warmup is original frame");
        d.Wait(&wait,&state,true);Check(state.predictedDisplayPeriod==2*Period&&state.predictedDisplayTime==now+Period,"paired frame predicts next real slot");end.displayTime=state.predictedDisplayTime;views[0].pose.position.x=2;
        d.End(&end,textures);Check(ends==4&&waits==4&&begins==1,"one app end delivers two runtime frames");Check(generatedCopies==2&&copyCount==4,"both generated eyes then both real eyes copied");
        Check(positions[positions.size()-2]==1&&positions.back()==2,"intermediate pose then real pose");Check(views[0].pose.position.x==2&&views[0].subImage.swapchain==H<XrSwapchain>(1),"caller views unchanged");
        auto pending=std::async(std::launch::async,[&]{XrFrameState nextState{XR_TYPE_FRAME_STATE};auto r=d.Wait(&wait,&nextState,true);return std::make_pair(r,nextState);});
        Check(pending.wait_for(std::chrono::milliseconds(20))==std::future_status::timeout,"next app wait held until guide state rearmed");d.Complete();auto nextState=pending.get().second;
        end.displayTime=nextState.predictedDisplayTime;unsigned before=generatedCopies;d.End(&end,{});d.Complete();Check(generatedCopies==before,"missing guides never present generated texture");
        d.Wait(&wait,&state,true);Check(state.predictedDisplayPeriod==Period,"missing guides restore normal cadence");end.displayTime=state.predictedDisplayTime;d.End(&end,textures);d.Complete();
        render=false;d.Wait(&wait,&state,true);Check(state.predictedDisplayPeriod==Period,"shouldRender false never doubles cadence");end.displayTime=state.predictedDisplayTime;end.layerCount=0;d.End(&end,{});d.Complete();render=true;end.layerCount=1;
    }
    Check(ownership.empty(),"owned swapchains destroyed");
    {
        FrameDelivery d(H<XrSession>(2),H<ID3D11Device*>(1),Api());
        d.Wait(&wait,&state,true);end.displayTime=state.predictedDisplayTime;d.End(&end,textures);d.Complete();
        d.Wait(&wait,&state,true);end.displayTime=state.predictedDisplayTime;failCopy=true;unsigned before=generatedCopies;
        Check(d.End(&end,textures)==XR_SUCCESS,"copy failure still completes valid runtime frames");d.Complete();failCopy=false;
        Check(d.GeneratedPairs()==0,"failed copy never counted as generated submission");
        d.Wait(&wait,&state,true);Check(state.predictedDisplayPeriod==Period,"copy failure disables interpolation");end.displayTime=state.predictedDisplayTime;d.End(&end,textures);d.Complete();
        Check(generatedCopies==before+1,"faulted owned images are never used again");
    }
    {
        FrameDelivery d(H<XrSession>(3),H<ID3D11Device*>(1),Api());failCreate=true;
        d.Wait(&wait,&state,true);end.displayTime=state.predictedDisplayTime;Check(d.End(&end,textures)==XR_SUCCESS,"unsupported internal format preserves app submission");d.Complete();failCreate=false;
        d.Wait(&wait,&state,true);Check(state.predictedDisplayPeriod==Period,"create failure leaves original cadence");end.displayTime=state.predictedDisplayTime;d.End(&end,textures);d.Complete();
    }
    Check(ownership.empty(),"failure paths release owned resources");
    std::printf("Frame delivery and camera: %u checks passed\n",checks);
}
