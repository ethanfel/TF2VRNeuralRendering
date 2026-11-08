#include "../src/plugin/MenuSettings.h"
#include <cstdlib>
#include <iostream>
#include <map>

using namespace tf2vrnr::menu;
#define CHECK(x) do { if (!(x)) { std::cerr << __LINE__ << ": " << #x << "\n"; std::exit(1); } } while (0)
struct MemoryVars : Variables
{
    std::map<std::string, int> values;
    bool Read(const std::string& name, int& value) override { value = values[name]; return true; }
    bool Write(const std::string& name, int value) override { values[name] = value; return true; }
    int& operator[](const char* name) { return values[name]; }
    void Edit(const Settings& s)
    {
        for (size_t i=0; i<s.size(); ++i) values[std::string("tf2vrnr_edit_")+kFields[i]] = s[i];
        values["tf2vrnr_base"] = values["tf2vrnr_revision"];
        values["tf2vrnr_command"] = 2;
        ++values["tf2vrnr_request"];
    }
};
struct MemoryStore : Store
{
    Snapshot snapshot{{1, 1, 0, 1, 100, 100, 100, 100}, 7, true};
    int saves=0, result=2;
    bool readable=true;
    bool Load(Snapshot& out) override { out=snapshot; return readable; }
    int Save(const Settings& values) override { ++saves; if(result==2 || result==-6) snapshot.values=values; return result; }
};
int main()
{
    MemoryVars vars;
    MemoryStore disk;
    Controller controller;
    CHECK(controller.Tick(vars,disk));
    CHECK(vars["tf2vrnr_saved_nr"]==1 && vars["tf2vrnr_saved_dlaa"]==1 && vars["tf2vrnr_saved_strength"]==100);
    CHECK(disk.saves==0); // mod defaults must never overwrite saved preferences
    vars["tf2vrnr_edit_nr"]=0;
    CHECK(controller.Tick(vars,disk) && disk.saves==0); // draft alone cannot apply

    auto settings=disk.snapshot.values;
    settings[3]=0; settings[5]=45; settings[6]=65; settings[7]=80;
    vars.Edit(settings);
    CHECK(controller.Tick(vars,disk));
    CHECK(disk.snapshot.values==settings && disk.saves==1 && vars["tf2vrnr_status"]==2);
    CHECK(controller.Tick(vars,disk) && disk.saves==1); // each request exactly once

    vars.Edit(settings);
    disk.snapshot.values[5]=73; // CLI edit after menu opened; do not clobber it
    CHECK(controller.Tick(vars,disk));
    CHECK(vars["tf2vrnr_status"]==-5 && disk.saves==1 && vars["tf2vrnr_saved_strength"]==73);

    settings=disk.snapshot.values;
    settings[2]=1; disk.snapshot.runtimes=3;
    vars.Edit(settings); CHECK(controller.Tick(vars,disk));
    CHECK(vars["tf2vrnr_status"]==-2 && disk.saves==1);
    settings[2]=0; settings[5]=101;
    vars.Edit(settings); CHECK(controller.Tick(vars,disk));
    CHECK(vars["tf2vrnr_status"]==-3 && disk.saves==1);
    settings[5]=50; settings[4]=0;
    vars.Edit(settings); CHECK(controller.Tick(vars,disk));
    CHECK(vars["tf2vrnr_status"]==-3 && disk.saves==1);
    settings[4]=100; disk.snapshot.layerEnabled=false;
    vars.Edit(settings); CHECK(controller.Tick(vars,disk));
    CHECK(vars["tf2vrnr_status"]==-4 && disk.saves==1);

    settings[0]=settings[1]=settings[2]=0; // all off works even with runtimes missing/layer disabled
    disk.snapshot.runtimes=0;
    vars.Edit(settings); CHECK(controller.Tick(vars,disk));
    CHECK(vars["tf2vrnr_status"]==2 && disk.snapshot.values==settings);
    settings[5]=80; disk.result=-1;
    vars.Edit(settings); CHECK(controller.Tick(vars,disk));
    CHECK(vars["tf2vrnr_status"]==-1 && disk.snapshot.values[5]==50 && vars["tf2vrnr_saved_strength"]==50);
    disk.result=-6;
    vars.Edit(settings); CHECK(controller.Tick(vars,disk));
    CHECK(vars["tf2vrnr_status"]==-6 && disk.snapshot.values[5]==80);

    Controller restarted;
    vars["tf2vrnr_command"]=0; vars["tf2vrnr_request"]=0;
    CHECK(restarted.Tick(vars,disk));
    CHECK(vars["tf2vrnr_saved_strength"]==80 && vars["tf2vrnr_saved_nr"]==0);
    disk.readable=false; ++vars["tf2vrnr_request"];
    CHECK(!restarted.Tick(vars,disk) && vars["tf2vrnr_status"]==-1);
    std::cout << "PASS: startup, explicit apply, once-only requests, CLI conflicts, missing runtimes, validation, all-off, persistence errors, restart\n";
}
