#include "Registration.h"
#include <cstdio>
#include <cstring>

int main(int argc, char** argv)
{
    if (argc != 2 || (std::strcmp(argv[1], "register") && std::strcmp(argv[1], "unregister")))
    {
        std::fprintf(stderr, "Usage: TF2VRNeuralRenderingSetup.exe register|unregister\nRun with the same Wine/Proton prefix as the game.\n");
        return 2;
    }
    const bool wine = tf2vrnr::IsWine();
    if (std::strcmp(argv[1], "unregister") == 0)
    {
        int removed = tf2vrnr::UnregisterImplicitLayer(HKEY_CURRENT_USER, tf2vrnr::kImplicitLayersKey, tf2vrnr::kManifestName);
        if (wine) removed += tf2vrnr::UnregisterImplicitLayer(HKEY_LOCAL_MACHINE, tf2vrnr::kImplicitLayersKey, tf2vrnr::kManifestName);
        std::printf("Removed %d Neural Rendering registration(s).\n", removed);
        return 0;
    }
    wchar_t filename[32768] = {};
    const DWORD length = GetModuleFileNameW(nullptr, filename, 32768);
    if (!length || length >= 32768) return 1;
    std::wstring path(filename, length);
    path = path.substr(0, path.find_last_of(L"\\/")) + L"\\plugins\\layer\\" + tf2vrnr::kManifestName;
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES)
    {
        std::fprintf(stderr, "Layer manifest missing beside this package's setup helper.\n");
        return 1;
    }
    const auto report = tf2vrnr::RegisterForEnvironment(path, wine);
    if (report.result == tf2vrnr::RegistrationResult::Failed)
    {
        std::fprintf(stderr, "Layer registration failed.\n");
        return 1;
    }
    std::printf("Neural Rendering registered in %s; %s; removed %d obsolete registration(s).\n",
        wine ? "Wine/Proton prefix HKLM" : "HKCU", report.result == tf2vrnr::RegistrationResult::DisabledByUser ?
        "disabled state preserved" : "enabled", report.removedStale);
    return 0;
}
