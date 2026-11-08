#include "Registration.h"
#include "Northstar.h"
#include "MenuBridge.h"

#include <windows.h>

#include <cstdint>
#include <cstring>
#include <string>

using namespace tf2vrnr::northstar;

namespace
{
constexpr const wchar_t* kLayerFolder = L"\\layer\\";
constexpr const wchar_t* kManifestName = L"XR_APILAYER_TF2VR_neural_rendering.json";
constexpr const wchar_t* kLayerDllName = L"TF2VRNeuralRenderingLayer.dll";
constexpr int64_t kContextClient = 0x2;

ISys* g_sys = nullptr;

std::string Narrow(const std::wstring& text)
{
    if (text.empty())
        return {};
    int size = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), result.data(), size, nullptr, nullptr);
    return result;
}

void SysLog(LogLevel level, std::string message)
{
    if (g_sys)
        g_sys->Log(0, level, message.data());
}

std::wstring ModuleDirectory(HMODULE module)
{
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(module, path, MAX_PATH);
    std::wstring result = path;
    size_t slash = result.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring() : result.substr(0, slash);
}

class PluginId : public IPluginId
{
public:
    const char* GetString(PluginString prop) override
    {
        switch (prop)
        {
        case PluginString::Name: return "TF2VRNeuralRendering";
        case PluginString::LogName: return "TF2VRNR";
        case PluginString::DependencyName: return "TF2VRNeuralRendering";
        default: return nullptr;
        }
    }

    int64_t GetField(PluginField prop) override
    {
        return prop == PluginField::Context ? kContextClient : 0;
    }
};

class PluginCallbacks : public IPluginCallbacks
{
public:
    void Init(HMODULE northstarModule, const PluginNorthstarData* initData, bool) override
    {
        auto createInterface = reinterpret_cast<CreateInterfaceFn>(GetProcAddress(northstarModule, "CreateInterface"));
        if (createInterface)
            g_sys = static_cast<ISys*>(createInterface("NSSys001", nullptr));

        std::wstring layerDirectory = ModuleDirectory(initData ? initData->pluginHandle : nullptr) + kLayerFolder;
        tf2vrnr::menu::Initialize(layerDirectory);
        std::wstring manifest = layerDirectory + kManifestName;
        if (GetFileAttributesW(manifest.c_str()) == INVALID_FILE_ATTRIBUTES)
        {
            SysLog(LogLevel::Error, "OpenXR layer manifest missing: " + Narrow(manifest));
            return;
        }

        const bool wine = tf2vrnr::IsWine();
        tf2vrnr::RegistrationReport report = tf2vrnr::RegisterForEnvironment(manifest, wine);
        if (report.removedStale > 0)
            SysLog(LogLevel::Info, "removed " + std::to_string(report.removedStale) + " obsolete layer registration(s)");

        bool loaded = GetModuleHandleW((layerDirectory + kLayerDllName).c_str()) != nullptr;
        switch (report.result)
        {
        case tf2vrnr::RegistrationResult::Failed:
            SysLog(LogLevel::Error, wine ? "could not register the OpenXR layer in this Wine/Proton prefix (HKLM)" :
                "could not register the OpenXR layer under HKEY_CURRENT_USER");
            break;
        case tf2vrnr::RegistrationResult::DisabledByUser:
            SysLog(LogLevel::Info, "OpenXR layer is disabled in the registry (value is not 0); leaving it disabled");
            break;
        case tf2vrnr::RegistrationResult::Registered:
            SysLog(LogLevel::Warn, wine ?
                "Neural rendering layer registered in this Wine/Proton prefix (HKLM). Restart once to activate it." :
                "Neural rendering layer registered for this Windows user. Restart once to activate it.");
            break;
        case tf2vrnr::RegistrationResult::AlreadyRegistered:
            if (loaded)
                SysLog(LogLevel::Info, "OpenXR neural rendering layer registered; check its log for per-eye neural_evaluate output_applied=1.");
            else
                SysLog(LogLevel::Warn, wine ?
                    "OpenXR layer is registered in this Wine/Proton prefix (HKLM) but did not load this session. Restart the game; if it persists, check the OpenXR loader diagnostics." :
                    "OpenXR layer is registered but did not load this session. Restart the game; if it persists, make sure the game is not running as administrator.");
            break;
        }
    }

    void Finalize() override {}
    bool Unload() override { return true; }
    void OnSqvmCreated(void*) override {}
    void OnSqvmDestroying(void*) override {}
    void OnLibraryLoaded(HMODULE, const char*) override {}
    void RunFrame() override { tf2vrnr::menu::RunFrame(); }
};

PluginId g_pluginId;
PluginCallbacks g_pluginCallbacks;
}

extern "C" __declspec(dllexport) void* CreateInterface(const char* name, int* status)
{
    void* result = nullptr;
    if (name && std::strcmp(name, "PluginId001") == 0)
        result = static_cast<IPluginId*>(&g_pluginId);
    else if (name && std::strcmp(name, "PluginCallbacks001") == 0)
        result = static_cast<IPluginCallbacks*>(&g_pluginCallbacks);
    if (status)
        *status = result ? 0 : 1;
    return result;
}
