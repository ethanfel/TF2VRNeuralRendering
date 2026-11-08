// MIT. Standalone diagnostic only: no game injection. Optional synthetic GPU test.
// Uses NVIDIA's external SDK declarations; SDK files are not redistributed.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <nvsdk_ngx_defs.h>
#include <nvsdk_ngx_defs_dlssnr.h>
#include <cstdio>
#include <cstring>
#include <filesystem>

using QueryRequirements = NVSDK_NGX_Result(NVSDK_CONV *)(
    IDXGIAdapter*, const NVSDK_NGX_FeatureDiscoveryInfo*, NVSDK_NGX_FeatureRequirement*);
extern "C" unsigned int tf2vrnr_parameter_get_int(void*, const char*, int*);
bool EvaluateProbe(FILE*, HMODULE, ID3D12Device*, const std::filesystem::path&, bool);
static FILE* exceptionLog = nullptr;
static LONG CALLBACK ExceptionReport(EXCEPTION_POINTERS* e) {
    if (!exceptionLog || e->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION) return EXCEPTION_CONTINUE_SEARCH;
    HMODULE module = nullptr; wchar_t name[32768]{};
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(e->ExceptionRecord->ExceptionAddress), &module);
    if (module) GetModuleFileNameW(module, name, 32768);
    std::fprintf(exceptionLog, "access_violation module=%ls offset=0x%llX address=%p operation=%llu target=0x%llX rcx=0x%llX rdx=0x%llX r8=0x%llX r9=0x%llX\n",
        name, static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(e->ExceptionRecord->ExceptionAddress)-reinterpret_cast<uintptr_t>(module)),
        e->ExceptionRecord->ExceptionAddress, static_cast<unsigned long long>(e->ExceptionRecord->ExceptionInformation[0]),
        static_cast<unsigned long long>(e->ExceptionRecord->ExceptionInformation[1]),
        static_cast<unsigned long long>(e->ContextRecord->Rcx), static_cast<unsigned long long>(e->ContextRecord->Rdx),
        static_cast<unsigned long long>(e->ContextRecord->R8), static_cast<unsigned long long>(e->ContextRecord->R9));
    std::fflush(exceptionLog);
    return EXCEPTION_CONTINUE_SEARCH;
}

static bool initialize(FILE* log, HMODULE core, ID3D12Device* device,
                       const NVSDK_NGX_FeatureDiscoveryInfo& discovery, bool evaluate, bool cleanShutdown) {
    // Driver-core ProjectID ABI: API version precedes FeatureCommonInfo.
    // Reference: OptiScaler/proxies/NVNGX_Proxy.h PFN_D3D12_Init_ProjectID.
    using Init = NVSDK_NGX_Result(NVSDK_CONV *)(const char*, NVSDK_NGX_EngineType,
        const char*, const wchar_t*, ID3D12Device*, NVSDK_NGX_Version, const NVSDK_NGX_FeatureCommonInfo*);
    using Capabilities = NVSDK_NGX_Result(NVSDK_CONV *)(NVSDK_NGX_Parameter**);
    const auto init = reinterpret_cast<Init>(GetProcAddress(core, "NVSDK_NGX_D3D12_Init_ProjectID"));
    const auto caps = reinterpret_cast<Capabilities>(GetProcAddress(core, "NVSDK_NGX_D3D12_GetCapabilityParameters"));
    if (!init || !caps) return false;
    const auto& project = discovery.Identifier.v.ProjectDesc;
    std::fprintf(log, "driver_core.init_begin custom_project_id=%s\n", project.ProjectId);
    std::fflush(log);
    auto result = init(project.ProjectId, project.EngineType, project.EngineVersion,
        discovery.ApplicationDataPath, device, NVSDK_NGX_Version_API, discovery.FeatureInfo);
    std::fprintf(log, "driver_core.init_result=0x%08X\n", static_cast<unsigned>(result));
    std::fflush(log);
    if (result != NVSDK_NGX_Result_Success) return false;
    NVSDK_NGX_Parameter* parameters = nullptr;
    result = caps(&parameters);
    std::fprintf(log, "driver_core.capabilities_result=0x%08X\n", static_cast<unsigned>(result));
    bool available = false;
    if (result == NVSDK_NGX_Result_Success && parameters) {
        for (const char* key : {NVSDK_NGX_Parameter_DLSSNR_Available,
                NVSDK_NGX_Parameter_DLSSNR_NeedsUpdatedDriver,
                NVSDK_NGX_Parameter_DLSSNR_MinDriverVersionMajor,
                NVSDK_NGX_Parameter_DLSSNR_MinDriverVersionMinor,
                NVSDK_NGX_Parameter_DLSSNR_FeatureInitResult}) {
            int value = 0;
            const auto status = tf2vrnr_parameter_get_int(parameters, key, &value);
            std::fprintf(log, "capability %s result=0x%08X value=%d hex=0x%08X\n", key, status, value, static_cast<unsigned>(value));
            if (std::strcmp(key, NVSDK_NGX_Parameter_DLSSNR_Available) == 0)
                available = status == NVSDK_NGX_Result_Success && value != 0;
        }
    }
    std::fflush(log);
    bool passed = available;
    if (available && evaluate)
        passed = EvaluateProbe(log, core, device, discovery.ApplicationDataPath, cleanShutdown);
    if (cleanShutdown && passed) {
        using Destroy = NVSDK_NGX_Result(NVSDK_CONV *)(NVSDK_NGX_Parameter*);
        // This is the driver-core export, not the SDK wrapper of the same
        // name. The installed 615.78.08 core requires a second UINT* output:
        // its Shutdown1 entry forwards RDX and writes a 32-bit value through
        // it. Omitting that argument caused the captured null-write fault.
        using Shutdown = NVSDK_NGX_Result(NVSDK_CONV *)(ID3D12Device*, unsigned int*);
        const auto destroy = reinterpret_cast<Destroy>(GetProcAddress(core, "NVSDK_NGX_D3D12_DestroyParameters"));
        const auto shutdown = reinterpret_cast<Shutdown>(GetProcAddress(core, "NVSDK_NGX_D3D12_Shutdown1"));
        if (!destroy || !shutdown) return false;
        std::fprintf(log, "capability_parameters_destroy_begin\n"); std::fflush(log);
        result = destroy(parameters);
        std::fprintf(log, "capability_parameters_destroy_result=0x%08X\n", static_cast<unsigned>(result)); std::fflush(log);
        if (result != NVSDK_NGX_Result_Success) return false;
        std::fprintf(log, "device_shutdown_begin\n"); std::fflush(log);
        unsigned int coreState = ~0u;
        result = shutdown(device, &coreState);
        std::fprintf(log, "device_shutdown_result=0x%08X core_output=%u\n", static_cast<unsigned>(result), coreState); std::fflush(log);
        return result == NVSDK_NGX_Result_Success;
    }
    // This isolated diagnostic leaves NGX resources owned by its process.
    // An earlier mixed-version probe faulted during teardown after all
    // capability values had been obtained. Production teardown is untested.
    return passed;
}

static bool query(FILE* log, const char* label, HMODULE module, IDXGIAdapter* adapter,
                  const NVSDK_NGX_FeatureDiscoveryInfo& discovery) {
    const auto fn = reinterpret_cast<QueryRequirements>(
        GetProcAddress(module, "NVSDK_NGX_D3D12_GetFeatureRequirements"));
    if (!fn) {
        std::fprintf(log, "%s.query_export_missing error=%lu\n", label, GetLastError());
        std::fflush(log);
        return false;
    }
    NVSDK_NGX_FeatureRequirement requirements{};
    std::fprintf(log, "%s.query_begin\n", label);
    std::fflush(log);
    const auto result = fn(adapter, &discovery, &requirements);
    std::fprintf(log, "%s.query_result=0x%08X\n", label, static_cast<unsigned>(result));
    if (result == NVSDK_NGX_Result_Success) {
        const auto flags = static_cast<unsigned>(requirements.FeatureSupported);
        std::fprintf(log, "%s.support_flags=%u driver_unsupported=%u adapter_unsupported=%u min_arch=0x%X min_os=%.254s\n",
            label, flags, (flags & NVSDK_NGX_FeatureSupportResult_DriverVersionUnsupported) != 0,
            (flags & NVSDK_NGX_FeatureSupportResult_AdapterUnsupported) != 0,
            requirements.MinHWArchitecture, requirements.MinOSVersion);
    }
    std::fflush(log);
    return result == NVSDK_NGX_Result_Success &&
           requirements.FeatureSupported == NVSDK_NGX_FeatureSupportResult_Supported;
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv || (argc != 3 && argc != 4)) return 2;
    const bool cleanShutdown = argc == 4 && std::wcscmp(argv[3], L"--evaluate-clean") == 0;
    const bool evaluate = cleanShutdown || (argc == 4 && std::wcscmp(argv[3], L"--evaluate") == 0);
    if (argc == 4 && !evaluate) { LocalFree(argv); return 2; }
    const std::filesystem::path runtime = argv[1];
    const std::filesystem::path logPath = argv[2];
    LocalFree(argv);
    FILE* log = _wfopen(logPath.c_str(), L"w");
    if (!log) return 3;
    exceptionLog = log;
    AddVectoredExceptionHandler(1, ExceptionReport);
    std::fprintf(log, "TF2VR NR probe; synthetic_evaluation_requested=%u\n", evaluate);
    std::fflush(log);
    IDXGIFactory1* factory = nullptr;
    HRESULT hr = CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory));
    std::fprintf(log, "dxgi_factory=0x%08X\n", static_cast<unsigned>(hr));
    std::fflush(log);
    if (FAILED(hr)) { std::fclose(log); return 4; }
    IDXGIAdapter1* adapter = nullptr;
    for (UINT index = 0; ; ++index) {
        IDXGIAdapter1* candidate = nullptr;
        if (factory->EnumAdapters1(index, &candidate) != S_OK) break;
        DXGI_ADAPTER_DESC1 desc{};
        candidate->GetDesc1(&desc);
        if (desc.VendorId == 0x10de && !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
            adapter = candidate;
            std::fprintf(log, "adapter vendor=0x%X device=0x%X dedicated_memory=%llu\n",
                desc.VendorId, desc.DeviceId, static_cast<unsigned long long>(desc.DedicatedVideoMemory));
            break;
        }
        candidate->Release();
    }
    if (!adapter) { factory->Release(); std::fclose(log); return 5; }
    ID3D12Device* device = nullptr;
    hr = D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_12_0, __uuidof(ID3D12Device), reinterpret_cast<void**>(&device));
    std::fprintf(log, "d3d12_device=0x%08X\n", static_cast<unsigned>(hr));
    std::fflush(log);

    const auto folder = runtime.parent_path().wstring();
    const wchar_t* searchPath = folder.c_str();
    const auto dataPath = logPath.parent_path().wstring();
    NVSDK_NGX_FeatureCommonInfo info{};
    info.PathListInfo.Path = &searchPath;
    info.PathListInfo.Length = 1;
    NVSDK_NGX_FeatureDiscoveryInfo discovery{};
    discovery.SDKVersion = NVSDK_NGX_Version_API;
    discovery.FeatureID = NVSDK_NGX_Feature_DLSSNR;
    discovery.Identifier.IdentifierType = NVSDK_NGX_Application_Identifier_Type_Project_Id;
    discovery.Identifier.v.ProjectDesc = {"0ca94998-cffe-473b-bf26-fbd774613980", NVSDK_NGX_ENGINE_TYPE_CUSTOM, "TF2VR-NR-probe-0.1"};
    discovery.ApplicationDataPath = dataPath.c_str();
    discovery.FeatureInfo = &info;
    HMODULE core = LoadLibraryW(L"_nvngx.dll");
    std::fprintf(log, "driver_core_loaded=%u error=%lu\n", core != nullptr, core ? 0 : GetLastError());
    if (core) {
        wchar_t path[32768]{};
        GetModuleFileNameW(core, path, 32768);
        std::fprintf(log, "driver_core_path=%ls\n", path);
    }
    std::fflush(log);
    HMODULE nr = LoadLibraryExW(runtime.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    std::fprintf(log, "nr_loaded=%u error=%lu\n", nr != nullptr, nr ? 0 : GetLastError());
    std::fflush(log);
    // Query using the runtime's declared API version, rather than a newer
    // common-header default that the installed driver may not implement yet.
    using GetApiVersion = unsigned int(NVSDK_CONV *)();
    const auto getApi = nr ? reinterpret_cast<GetApiVersion>(GetProcAddress(nr, "NVSDK_NGX_GetAPIVersion")) : nullptr;
    if (getApi) discovery.SDKVersion = static_cast<NVSDK_NGX_Version>(getApi());
    std::fprintf(log, "discovery_api_version=0x%X header_default_api_version=0x%X\n",
        static_cast<unsigned>(discovery.SDKVersion), static_cast<unsigned>(NVSDK_NGX_Version_API));
    const bool coreSupport = core && query(log, "driver_core", core, adapter, discovery);
    const bool nrSupport = nr && query(log, "nr_runtime", nr, adapter, discovery);
    bool passed = core && device;
    for (unsigned cycle = 0; passed && cycle < (cleanShutdown ? 2u : 1u); ++cycle) {
        std::fprintf(log, "lifecycle_cycle_begin=%u clean_shutdown=%u\n", cycle, cleanShutdown); std::fflush(log);
        passed = initialize(log, core, device, discovery, evaluate, cleanShutdown);
        std::fprintf(log, "lifecycle_cycle_end=%u passed=%u\n", cycle, passed); std::fflush(log);
    }
    std::fprintf(log, "probe_complete=1 driver_core_support=%u nr_direct_query_support=%u test_passed=%u game_neural_rendering_active=0\n", coreSupport, nrSupport, passed);
    const unsigned int exitCode = SUCCEEDED(hr) && passed ? 0 : 1;
    if (cleanShutdown && passed) {
        device->Release(); adapter->Release(); factory->Release();
        if (nr) FreeLibrary(nr);
        if (core) FreeLibrary(core);
        std::fprintf(log, "clean_process_exit=1 cycles=2 device_and_modules_released=1\n");
        exceptionLog = nullptr;
        std::fclose(log);
        return static_cast<int>(exitCode);
    }
    exceptionLog = nullptr; std::fclose(log);
    // End only this helper process. Windows/Wine reclaims the NGX context
    // and modules without executing the unvalidated core teardown path.
    TerminateProcess(GetCurrentProcess(), exitCode);
    return static_cast<int>(exitCode);
}
