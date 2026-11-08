#include "../src/plugin/Registration.h"
#include <cstdio>
#include <cstdlib>
using namespace tf2vrnr;
namespace {
unsigned checks = 0;
void Check(bool ok, const char* why) {
    ++checks;
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", why); std::exit(1); }
}
}
int main() {
    // All test writes go to a private key, never Khronos/OpenXR registration.
    auto key = std::wstring(L"Software\\TF2VRNeuralRenderingTests\\") + std::to_wstring(GetCurrentProcessId());
    const std::wstring first = std::wstring(L"C:\\old\\") + kManifestName;
    const std::wstring second = std::wstring(L"C:\\new\\") + kManifestName;
    const std::wstring controls = L"C:\\controls\\XR_APILAYER_TF2VR_seated_controls.json";
    Check(RegisterImplicitLayer(HKEY_CURRENT_USER, key, first).result == RegistrationResult::Registered, "new graphics layer registration");
    Check(RegisterImplicitLayer(HKEY_CURRENT_USER, key, controls).result == RegistrationResult::Registered, "control registration fixture");
    auto report = RegisterImplicitLayer(HKEY_CURRENT_USER, key, second, 1);
    Check(report.removedStale == 1 && report.result == RegistrationResult::DisabledByUser, "upgrade removes only same layer name and preserves disabled choice");
    Check(RegisterImplicitLayer(HKEY_CURRENT_USER, key, second).result == RegistrationResult::DisabledByUser, "automatic registration cannot re-enable disabled layer");
    Check(UnregisterImplicitLayer(HKEY_CURRENT_USER, key, kManifestName) == 1, "uninstall removes graphics registration");
    Check(RegisterImplicitLayer(HKEY_CURRENT_USER, key, controls).result == RegistrationResult::AlreadyRegistered, "seated controls registration survives");
    RegDeleteTreeW(HKEY_CURRENT_USER, key.c_str());
    std::printf("PASS: %u independent layer registration checks\n", checks);
}
