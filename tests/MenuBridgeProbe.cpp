#include "../src/plugin/MenuBridge.h"
#include "../src/plugin/MenuConfig.h"
#include <windows.h>
#include <cstdlib>
#include <cstdio>
#include <iostream>

#define CHECK(x) do { if (!(x)) { std::cerr << __LINE__ << ": " << #x << " error=" << GetLastError() << "\n"; std::exit(1); } } while (0)
using namespace tf2vrnr::menu;
int wmain(int argc, wchar_t** argv)
{
    std::freopen("menu_bridge_probe.result.txt", "w", stderr);
    CHECK(argc==2);
    // Map the installed engine without running DllMain or loading imports.
    // Only its already-inspected, import-free integer convar methods execute.
    HMODULE engine=LoadLibraryExW(argv[1],nullptr,DONT_RESOLVE_DLL_REFERENCES);
    CHECK(engine);
    auto vtable=reinterpret_cast<unsigned char*>(engine)+0x67fd28;
    HMODULE fixture=LoadLibraryW(L"vstdlib.dll"); CHECK(fixture);
    auto add=reinterpret_cast<void(*)(const char*,void*,int)>(GetProcAddress(fixture,"Add"));
    auto put=reinterpret_cast<void(*)(const char*,int)>(GetProcAddress(fixture,"Put"));
    auto get=reinterpret_cast<int(*)(const char*)>(GetProcAddress(fixture,"Get"));
    CHECK(add && put && get);
    for(size_t i=0;i<kFields.size();++i)
        for(const auto* kind:{"edit_","saved_"})
            add((std::string("tf2vrnr_")+kind+kFields[i]).c_str(),vtable,kDefaults[i]);
    for(const auto* field:{"request","command","ack","base","revision","runtimes","status","ready"})
        add((std::string("tf2vrnr_")+field).c_str(),vtable,0);
    wchar_t temp[MAX_PATH]={};CHECK(GetTempPathW(MAX_PATH,temp));
    auto dir=std::wstring(temp)+L"tf2vrnr-bridge-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64())+L"\\";
    CHECK(CreateDirectoryW(dir.c_str(),nullptr));
    auto path=dir+L"TF2VRNeuralRendering.ini";
    CHECK(WritePrivateProfileStringW(L"neural_rendering",L"intensity_percent",L"73",path.c_str()));
    Initialize(dir);
    RunFrame();
    CHECK(get("tf2vrnr_ready")==1 && get("tf2vrnr_saved_strength")==73);
    put("tf2vrnr_base",get("tf2vrnr_revision"));
    put("tf2vrnr_edit_strength",85);
    put("tf2vrnr_command",2);
    put("tf2vrnr_request",1);
    Sleep(300); RunFrame();
    CHECK(get("tf2vrnr_ack")==1 && get("tf2vrnr_status")==2 && get("tf2vrnr_saved_strength")==85);
    Snapshot snapshot; IniStore store(dir);
    CHECK(store.Load(snapshot) && snapshot.values[5]==85);
    CHECK(DeleteFileW((dir+L"neural.request").c_str()));
    CHECK(DeleteFileW(path.c_str())); CHECK(RemoveDirectoryW(dir.c_str()));
    std::cerr << "PASS: real engine ConVar integer ABI, module/pointer validation, startup INI publish, request/ack, live reload signal\n";
    return 0;
}
