#include "mods.h"
#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
namespace fs = std::filesystem;
static void write(const fs::path &p, const std::string &s) { fs::create_directories(p.parent_path()); std::ofstream(p,std::ios::binary)<<s; }
static std::string read(const fs::path &p) { std::ifstream f(p,std::ios::binary); return {std::istreambuf_iterator<char>(f),{}}; }
static void fixture(const fs::path &p, const char *id, const char *versions="1.0.0,1.1.0") {
    write(p/"mod.ini", std::string("format=1\nid=")+id+"\nname=Fixture\nversion=1\nauthor=Test\ndescription=Synthetic resources\ngame_versions="+versions+"\n");
    write(p/"romfs/region_common/ui/example.bin",std::string(id)+" resource");
}
int main(int argc,char **argv) {
    assert(argc==2);fs::path root=fs::u8path(argv[1]),game=root/"game",data=root/"data";
    fs::create_directories(data);
    write(game/"romfs/region_common/ui/example.bin","original");
    fixture(root/"one","one");fixture(root/"two","two");fixture(root/"base","base","1.0.0");
    auto d=data.u8string(),g=game.u8string();char why[1024]={0},host[2048];ModInfo list[MODS_MAX];
    auto install=[&](const char *name,const char *v="1.1.0") {return mods_import(d.c_str(),g.c_str(),v,(root/name).u8string().c_str(),why,sizeof why);};
    auto enable=[&](const char *id,bool on,int profile=2,const char *v="1.1.0") {return mods_enable(d.c_str(),g.c_str(),profile,v,id,on,why,sizeof why);};
    auto load=[&](int profile=2,const char *v="1.1.0") {return mods_load_active(d.c_str(),g.c_str(),profile,v,why,sizeof why);};
    assert(install("one")); assert(!install("one"));
    assert(read(root/"one/romfs/region_common/ui/example.bin")=="one resource");
    assert(read(game/"romfs/region_common/ui/example.bin")=="original");
    assert(mods_list(d.c_str(),g.c_str(),2,"1.1.0",list,MODS_MAX,why,sizeof why)==1);
    assert(!list[0].enabled&&list[0].compatible&&list[0].files==1);
    assert(enable("one",true));assert(load());
    assert(mods_romfs_override("region_common/ui/example.bin",host,sizeof host));
    assert(read(fs::u8path(host))=="one resource");
    assert(!mods_romfs_override("../outside",host,sizeof host));
    assert(load(1,"1.0.0"));assert(!mods_romfs_override("region_common/ui/example.bin",host,sizeof host));
    assert(!mods_romfs_override("region_common/ui/example.bin",host,2));
    assert(install("two"));assert(!enable("two",true));
    assert(mods_list(d.c_str(),g.c_str(),2,"1.1.0",list,MODS_MAX,why,sizeof why)==2);
    assert(list[1].conflicts==1&&!list[1].enabled);
    assert(read(data/"mods/enabled-1.1.0.txt")=="one\n");
    assert(enable("one",false));assert(enable("two",true));assert(load());
    assert(mods_romfs_override("region_common/ui/example.bin",host,sizeof host));assert(read(fs::u8path(host))=="two resource");
    assert(install("base","1.0.0"));assert(!enable("base",true));assert(enable("base",true,1,"1.0.0"));
    fixture(root/"invalid","../escape");assert(!install("invalid"));assert(!fs::exists(data/"escape"));
    fixture(root/"unknown","unknown");write(root/"unknown/romfs/region_common/ui/not-in-game.bin","unknown");assert(!install("unknown"));
    fixture(root/"duplicate","duplicate");std::ofstream(root/"duplicate/mod.ini",std::ios::app)<<"id=another\n";assert(!install("duplicate"));
    fixture(root/"text","text");auto original=read(root/"text/mod.ini");
    write(root/"text/mod.ini",original+std::string(1,'\0'));assert(!install("text"));
    write(root/"text/mod.ini",original+"description=\xc0\xaf\n");assert(!install("text"));
    write(root/"text/mod.ini",std::string(8193,'a'));assert(!install("text"));
    write(root/"text/mod.ini","\xef\xbb\xbf"+original);assert(install("text"));
    fixture(root/"large","large");fs::resize_file(root/"large/romfs/region_common/ui/example.bin",(1ull<<30)+1);assert(!install("large"));
    fixture(root/"link","link");
    std::error_code ec;fs::create_symlink(game/"romfs/region_common/ui/example.bin",root/"link/romfs/region_common/ui/linked.bin",ec);
    if(!ec)assert(!install("link"));
    // Missing enabled packs remain visible so the UI can disable them and recover.
    fs::rename(data/"mods/two",data/"_removed");
    assert(!load());assert(!mods_romfs_override("region_common/ui/example.bin",host,sizeof host));
    int count=mods_list(d.c_str(),g.c_str(),2,"1.1.0",list,MODS_MAX,why,sizeof why);assert(count==4);
    bool missing=false;for(int i=0;i<count;++i)if(std::string(list[i].id)=="two")missing=list[i].enabled&&*list[i].problem;
    assert(missing&&enable("two",false)&&load());
    mods_clear_active();assert(!mods_romfs_override("region_common/ui/example.bin",host,sizeof host));
    assert(read(game/"romfs/region_common/ui/example.bin")=="original");
    for(const auto &e:fs::directory_iterator(data/"mods"))assert(e.path().filename().u8string().find(".import-")!=0);
    std::cout<<"Mod import, source preservation, version isolation, conflicts, invalid input, links and recovery passed\n";
}
