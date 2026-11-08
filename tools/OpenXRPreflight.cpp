// Native Linux check for the private simulated OpenXR runtime.
#include <openxr/openxr.h>
#include <cstdio>
#include <cstring>
int main() {
    XrInstanceCreateInfo ci{XR_TYPE_INSTANCE_CREATE_INFO};
    std::strcpy(ci.applicationInfo.applicationName, "TF2VR autotest preflight");
    ci.applicationInfo.apiVersion = XR_MAKE_VERSION(1,0,0);
    const char* ext = "XR_KHR_vulkan_enable";
    ci.enabledExtensionCount = 1; ci.enabledExtensionNames = &ext;
    XrInstance instance = XR_NULL_HANDLE;
    auto r = xrCreateInstance(&ci, &instance);
    if (XR_FAILED(r)) { std::printf("xrCreateInstance=%d\n",r); return 1; }
    XrInstanceProperties props{XR_TYPE_INSTANCE_PROPERTIES}; xrGetInstanceProperties(instance,&props);
    XrSystemGetInfo si{XR_TYPE_SYSTEM_GET_INFO}; si.formFactor=XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XrSystemId system=XR_NULL_SYSTEM_ID; r=xrGetSystem(instance,&si,&system);
    if (XR_FAILED(r)) { std::printf("xrGetSystem=%d\n",r);xrDestroyInstance(instance);return 1; }
    XrSystemProperties sp{XR_TYPE_SYSTEM_PROPERTIES}; xrGetSystemProperties(instance,system,&sp);
    XrViewConfigurationView views[2]{{XR_TYPE_VIEW_CONFIGURATION_VIEW},{XR_TYPE_VIEW_CONFIGURATION_VIEW}};
    uint32_t count=0;r=xrEnumerateViewConfigurationViews(instance,system,XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,2,&count,views);
    std::printf("runtime=%s system=%s views=%u size=%ux%u,%ux%u result=%d\n",props.runtimeName,sp.systemName,count,
        views[0].recommendedImageRectWidth,views[0].recommendedImageRectHeight,
        views[1].recommendedImageRectWidth,views[1].recommendedImageRectHeight,r);
    const bool pass=XR_SUCCEEDED(r)&&count==2&&std::strstr(sp.systemName,"Simulated");
    xrDestroyInstance(instance);return pass?0:1;
}
