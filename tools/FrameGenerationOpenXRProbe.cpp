// A standalone synthetic application. Does not launch Titanfall, EA, or a game prefix.
#include "../src/layer/FrameDelivery.h"
#include "../src/layer/Support.h"
#include "../src/backend/FrameGeneration.h"
#include <openxr_loader_negotiation.h>
#include <wrl/client.h>
#include <cstring>
#include <stdexcept>
#include <vector>
using namespace tf2vrnr;
using Microsoft::WRL::ComPtr;
namespace tf2vrnr {
bool CopyPresentationImage(ID3D11Device* d,ID3D11Texture2D* to,ID3D11Texture2D* from){ComPtr<ID3D11DeviceContext> c;d->GetImmediateContext(&c);c->CopyResource(to,from);c->Flush();return SUCCEEDED(d->GetDeviceRemovedReason());}
}
static void X(XrResult r,const char* name){if(XR_FAILED(r)){Log("fg_xr_failure %s result=%d",name,r);throw std::runtime_error(name);}}
static void D(HRESULT r){if(FAILED(r))throw std::runtime_error("d3d11");}
static uint16_t Half(float v){uint32_t b;std::memcpy(&b,&v,4);return uint16_t(((b>>16)&0x8000)|((((b>>23)&255)-112)<<10)|((b>>13)&1023));}
int wmain(int argc,wchar_t** argv){
    if(argc!=2)return 2;
    try{
        HMODULE module=LoadLibraryW(L"wineopenxr.dll");if(!module)throw std::runtime_error("wineopenxr");
        auto negotiate=reinterpret_cast<PFN_xrNegotiateLoaderRuntimeInterface>(GetProcAddress(module,"xrNegotiateLoaderRuntimeInterface"));if(!negotiate)throw std::runtime_error("negotiate");
        XrNegotiateLoaderInfo loader{XR_LOADER_INTERFACE_STRUCT_LOADER_INFO,XR_LOADER_INFO_STRUCT_VERSION,sizeof(XrNegotiateLoaderInfo),1,1,XR_MAKE_VERSION(1,0,0),XR_MAKE_VERSION(1,0,0)};
        XrNegotiateRuntimeRequest request{XR_LOADER_INTERFACE_STRUCT_RUNTIME_REQUEST,XR_RUNTIME_INFO_STRUCT_VERSION,sizeof(XrNegotiateRuntimeRequest)};
        X(negotiate(&loader,&request),"negotiate");auto get=request.getInstanceProcAddr;XrInstance instance=XR_NULL_HANDLE;
#define PROC(name) PFN_##name name=nullptr;X(get(instance,#name,reinterpret_cast<PFN_xrVoidFunction*>(&name)),#name)
        PROC(xrCreateInstance);
        XrInstanceCreateInfo ci{XR_TYPE_INSTANCE_CREATE_INFO};std::strcpy(ci.applicationInfo.applicationName,"TF2VR FG standalone probe");ci.applicationInfo.apiVersion=XR_MAKE_VERSION(1,0,0);
        const char* extension=XR_KHR_D3D11_ENABLE_EXTENSION_NAME;ci.enabledExtensionCount=1;ci.enabledExtensionNames=&extension;X(xrCreateInstance(&ci,&instance),"create_instance");
        PROC(xrGetSystem);PROC(xrGetSystemProperties);PROC(xrGetD3D11GraphicsRequirementsKHR);PROC(xrCreateSession);PROC(xrDestroySession);PROC(xrDestroyInstance);
        PROC(xrCreateReferenceSpace);PROC(xrDestroySpace);PROC(xrBeginSession);PROC(xrEndSession);PROC(xrRequestExitSession);PROC(xrPollEvent);PROC(xrLocateViews);
        PROC(xrWaitFrame);PROC(xrBeginFrame);PROC(xrEndFrame);PROC(xrCreateSwapchain);PROC(xrDestroySwapchain);PROC(xrEnumerateSwapchainImages);PROC(xrAcquireSwapchainImage);PROC(xrWaitSwapchainImage);PROC(xrReleaseSwapchainImage);
#undef PROC
        XrSystemGetInfo si{XR_TYPE_SYSTEM_GET_INFO};si.formFactor=XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;XrSystemId system;X(xrGetSystem(instance,&si,&system),"system");
        XrSystemProperties props{XR_TYPE_SYSTEM_PROPERTIES};X(xrGetSystemProperties(instance,system,&props),"system_properties");
        if(!std::strstr(props.systemName,"Simulated"))throw std::runtime_error("refusing_non_simulated_runtime");Log("fg_xr_system %s",props.systemName);
        XrGraphicsRequirementsD3D11KHR requirements{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR};X(xrGetD3D11GraphicsRequirementsKHR(instance,system,&requirements),"graphics_requirements");
        ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;D(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context));
        XrGraphicsBindingD3D11KHR binding{XR_TYPE_GRAPHICS_BINDING_D3D11_KHR};binding.device=device.Get();XrSessionCreateInfo sci{XR_TYPE_SESSION_CREATE_INFO};sci.next=&binding;sci.systemId=system;XrSession session;
        X(xrCreateSession(instance,&sci,&session),"session");
        XrReferenceSpaceCreateInfo spaceInfo{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};spaceInfo.referenceSpaceType=XR_REFERENCE_SPACE_TYPE_LOCAL;spaceInfo.poseInReferenceSpace.orientation.w=1;XrSpace space;X(xrCreateReferenceSpace(session,&spaceInfo,&space),"space");
        bool running=false;
        for(unsigned n=0;n<200&&!running;++n){XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};auto r=xrPollEvent(instance,&event);if(r==XR_SUCCESS&&event.type==XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED){auto& changed=reinterpret_cast<XrEventDataSessionStateChanged&>(event);if(changed.state==XR_SESSION_STATE_READY){XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO};begin.primaryViewConfigurationType=XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;X(xrBeginSession(session,&begin),"begin_session");running=true;}}if(!running)Sleep(10);}
        if(!running)throw std::runtime_error("session_not_ready");
        constexpr unsigned w=512,h=512;auto generator=std::make_shared<FrameGeneration>();if(!generator->Init(device.Get(),w,h,argv[1]))throw std::runtime_error("fg_init");
        auto texture=[&](DXGI_FORMAT format){D3D11_TEXTURE2D_DESC d{};d.Width=w;d.Height=h;d.Format=format;d.MipLevels=d.ArraySize=d.SampleDesc.Count=1;d.BindFlags=D3D11_BIND_SHADER_RESOURCE;ComPtr<ID3D11Texture2D> t;D(device->CreateTexture2D(&d,nullptr,&t));return t;};
        auto color=texture(DXGI_FORMAT_R8G8B8A8_UNORM),depth=texture(DXGI_FORMAT_R32_FLOAT),motion=texture(DXGI_FORMAT_R16G16_FLOAT);
        XrSwapchain chains[2]{};std::vector<XrSwapchainImageD3D11KHR> images[2];
        for(unsigned eye=0;eye<2;++eye){XrSwapchainCreateInfo sc{XR_TYPE_SWAPCHAIN_CREATE_INFO};sc.format=DXGI_FORMAT_R8G8B8A8_UNORM;sc.usageFlags=XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT|XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;sc.sampleCount=sc.arraySize=sc.faceCount=sc.mipCount=1;sc.width=w;sc.height=h;X(xrCreateSwapchain(session,&sc,&chains[eye]),"app_chain");uint32_t count;X(xrEnumerateSwapchainImages(chains[eye],0,&count,nullptr),"app_images_count");images[eye].resize(count,{XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});X(xrEnumerateSwapchainImages(chains[eye],count,&count,reinterpret_cast<XrSwapchainImageBaseHeader*>(images[eye].data())),"app_images");}
        float bytes[144]{};bytes[0]=1;bytes[4]=bytes[9]=bytes[14]=bytes[18]=1;bytes[15]=-1;bytes[114]=w;bytes[115]=h;bytes[135]=bytes[136]=bytes[137]=1;auto camera=DecodeFrameCamera(bytes,w,h);
        unsigned paired=0,generatedPairs=0;
        {
            FrameDelivery delivery(session,device.Get(),{xrWaitFrame,xrBeginFrame,xrEndFrame,xrCreateSwapchain,xrDestroySwapchain,xrEnumerateSwapchainImages,xrAcquireSwapchainImage,xrWaitSwapchainImage,xrReleaseSwapchainImage});
            for(unsigned frame=0;frame<48;++frame){
                for(unsigned eventIndex=0;eventIndex<32;++eventIndex){XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};if(xrPollEvent(instance,&event)!=XR_SUCCESS)break;
                    if(event.type==XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED)Log("fg_xr_state state=%d",reinterpret_cast<XrEventDataSessionStateChanged&>(event).state);}
                XrFrameWaitInfo wait{XR_TYPE_FRAME_WAIT_INFO};XrFrameState state{XR_TYPE_FRAME_STATE};bool enabled=frame>=4&&frame<40;
                X(delivery.Wait(&wait,&state,enabled),"app_wait");XrFrameBeginInfo begin{XR_TYPE_FRAME_BEGIN_INFO};X(xrBeginFrame(session,&begin),"app_begin");
                if(state.predictedDisplayPeriod>15000000)++paired;
                XrViewLocateInfo locate{XR_TYPE_VIEW_LOCATE_INFO};locate.viewConfigurationType=XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;locate.displayTime=state.predictedDisplayTime;locate.space=space;
                XrViewState viewState{XR_TYPE_VIEW_STATE};XrView views[2]{{XR_TYPE_VIEW},{XR_TYPE_VIEW}};uint32_t count;X(xrLocateViews(session,&locate,&viewState,2,&count,views),"views");
                StereoFrameTextures textures;textures.owner=generator;textures.interpolated=frame>0;
                XrCompositionLayerProjectionView projectionViews[2]{};
                for(unsigned eye=0;eye<2;++eye){
                    std::vector<unsigned char> pixels(w*h*4);std::vector<float> depths(w*h,.999f);std::vector<uint16_t> motions(w*h*2);
                    unsigned left=64+frame*4+eye*12;
                    for(unsigned y=0;y<h;++y)for(unsigned x=0;x<w;++x){size_t k=size_t(y)*w+x;bool box=x>=left&&x<left+128&&y>=170&&y<340;pixels[k*4]=box?220:25;pixels[k*4+1]=box?60:25;pixels[k*4+2]=box?40:25;pixels[k*4+3]=255;if(box){depths[k]=.98f;motions[k*2]=Half(8);}}
                    context->UpdateSubresource(color.Get(),0,nullptr,pixels.data(),w*4,0);context->UpdateSubresource(depth.Get(),0,nullptr,depths.data(),w*4,0);context->UpdateSubresource(motion.Get(),0,nullptr,motions.data(),w*4,0);
                    if(!generator->Prepare(depth.Get(),motion.Get(),eye,frame,0,0)||!generator->Generate(color.Get(),eye,frame,camera,frame==0))throw std::runtime_error("fg_render");
                    textures.real[eye]=generator->Real(eye);textures.generated[eye]=generator->Generated(eye);textures.format[eye]=28;
                    XrSwapchainImageAcquireInfo a{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};uint32_t index;X(xrAcquireSwapchainImage(chains[eye],&a,&index),"acquire");XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};wi.timeout=XR_INFINITE_DURATION;X(xrWaitSwapchainImage(chains[eye],&wi),"wait_image");
                    CopyPresentationImage(device.Get(),images[eye][index].texture,color.Get());XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};X(xrReleaseSwapchainImage(chains[eye],&ri),"release");
                    auto& view=projectionViews[eye];view.type=XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;view.pose=views[eye].pose;view.fov=views[eye].fov;view.subImage.swapchain=chains[eye];view.subImage.imageRect.extent={w,h};
                }
                XrCompositionLayerProjection p{XR_TYPE_COMPOSITION_LAYER_PROJECTION};p.space=space;p.viewCount=2;p.views=projectionViews;const auto* layer=reinterpret_cast<XrCompositionLayerBaseHeader*>(&p);
                XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO};end.displayTime=state.predictedDisplayTime;end.environmentBlendMode=XR_ENVIRONMENT_BLEND_MODE_OPAQUE;end.layerCount=state.shouldRender?1:0;end.layers=&layer;
                X(delivery.End(&end,textures),"app_end");delivery.Complete();Log("fg_xr_frame frame=%u enabled=%d render=%d app_period_ns=%lld",frame,enabled,state.shouldRender,(long long)state.predictedDisplayPeriod);
            }
            generatedPairs=delivery.GeneratedPairs();
        }
        generator.reset();for(auto sc:chains)X(xrDestroySwapchain(sc),"destroy_chain");X(xrDestroySpace(space),"destroy_space");
        X(xrRequestExitSession(session),"request_exit");
        for(unsigned n=0;n<100;++n){XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};auto r=xrPollEvent(instance,&event);if(r==XR_SUCCESS&&event.type==XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED&&reinterpret_cast<XrEventDataSessionStateChanged&>(event).state==XR_SESSION_STATE_STOPPING){X(xrEndSession(session),"end_session");break;}Sleep(5);}
        X(xrDestroySession(session),"destroy_session");X(xrDestroyInstance(instance),"destroy_instance");FreeLibrary(module);
        bool passed=paired>20&&generatedPairs>20;
        Log("fg_xr_probe_passed=%d paired_app_frames=%u generated_pairs=%u",passed,paired,generatedPairs);return passed?0:4;
    }catch(const std::exception& e){Log("fg_xr_probe_failed %s",e.what());return 1;}
}
