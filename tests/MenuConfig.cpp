#include "../src/plugin/MenuConfig.h"
#include <windows.h>
#include <cstdlib>
#include <cstdio>
#include <iostream>

using namespace tf2vrnr::menu;
#define CHECK(x) do { if (!(x)) { std::cerr << __LINE__ << ": " << #x << " error=" << GetLastError() << "\n"; std::exit(1); } } while (0)
int main()
{
    std::freopen("menu_config_tests.result.txt", "w", stderr);
    wchar_t temp[MAX_PATH] = {};
    CHECK(GetTempPathW(MAX_PATH,temp));
    std::wstring dir=std::wstring(temp)+L"tf2vrnr-menu-test-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64())+L"\\";
    CHECK(CreateDirectoryW(dir.c_str(),nullptr));
    const auto path=dir+L"TF2VRNeuralRendering.ini";
    IniStore store(dir);
    Snapshot snapshot;
    CHECK(!store.Load(snapshot));
    CHECK(WritePrivateProfileStringW(L"capture",L"burst_frames",L"2",path.c_str()));
    CHECK(WritePrivateProfileStringW(L"diagnostics",L"custom_key",L"preserve me",path.c_str()));
    Settings desired={1,1,0,0,100,35,65,85};
    CHECK(store.Save(desired)==2);
    CHECK(store.Load(snapshot) && snapshot.values==desired);
    CHECK(GetFileAttributesW((dir+L"neural.request").c_str())!=INVALID_FILE_ATTRIBUTES);
    CHECK(GetPrivateProfileIntW(L"capture",L"burst_frames",0,path.c_str())==2);
    wchar_t text[32]={};
    GetPrivateProfileStringW(L"diagnostics",L"custom_key",L"",text,32,path.c_str());
    CHECK(std::wstring(text)==L"preserve me");
    auto bad=desired; bad[5]=-1;
    CHECK(store.Save(bad)==-3);
    CHECK(store.Load(snapshot) && snapshot.values==desired);
    // Simulate the CLI replacing the INI; profile reads must see the new file.
    auto next=path+L".cli.tmp";
    CHECK(CopyFileW(path.c_str(),next.c_str(),FALSE));
    CHECK(WritePrivateProfileStringW(L"neural_rendering",L"intensity_percent",L"91",next.c_str()));
    WritePrivateProfileStringW(nullptr,nullptr,nullptr,next.c_str());
    CHECK(MoveFileExW(next.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING));
    CHECK(store.Load(snapshot) && snapshot.values[5]==91);
    // Missing directory/write failure cannot report success.
    IniStore missing(dir+L"missing\\");
    CHECK(missing.Save(desired)==-1);
    CHECK(DeleteFileW((dir+L"neural.request").c_str()));
    CHECK(DeleteFileW(path.c_str()));
    CHECK(RemoveDirectoryW(dir.c_str()));
    std::cerr << "PASS: INI roundtrip, preserved unrelated keys, atomic CLI replacement, reload signal, invalid input, write failure\n";
}
