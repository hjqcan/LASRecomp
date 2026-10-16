#include "research.h"
#include <cassert>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
namespace fs=std::filesystem;
static void write(const fs::path &p,const std::string &s){std::ofstream(p,std::ios::binary)<<s;}
int main(int argc,char **argv){
    assert(argc==2);auto root=fs::u8path(argv[1]);fs::create_directories(root);
    const char *id="ae16f71e002af8cb059a9a74c4d90f34ba984892";char why[256];
    ResearchInput input[2]={{1,{32767,-32767,0,0}},{0,{0,0,123,-456}}},out[4]{};
    auto path=root/"test.lastape";auto name=path.u8string();
    assert(research_input_valid(input));assert(research_tape_write(name.c_str(),id,input,2,why,sizeof why));
    assert(!research_tape_write(name.c_str(),id,input,2,why,sizeof why));
    assert(research_tape_read(name.c_str(),id,out,4,why,sizeof why)==2);
    assert(out[0].buttons==1&&out[0].axes[1]==-32767&&out[1].axes[3]==-456);
    assert(research_tape_read(name.c_str(),id,out,1,why,sizeof why)<0);
    assert(research_tape_read(name.c_str(),"909e904af78ac1b8deefe97ab2ccdb51968f0ec7",out,4,why,sizeof why)<0);
    std::string header=std::string("LASINPUT1\nbuild_id=")+id+"\npoll,buttons,lx,ly,rx,ry\n";
    for(auto row:{"1,0,0,0,0,0\n","0,-1,0,0,0,0\n","0,16777216,0,0,0,0\n","0,0,-32768,0,0,0\n","0,0,0,0,0,0,\n","0,0,0,0,0,0oops\n","0,0,0,0,0\n"}){
        write(path,header+row);out[0].buttons=123;assert(research_tape_read(name.c_str(),id,out,4,why,sizeof why)<0);assert(out[0].buttons==123);
    }
    write(path,header+"0,0,0,0,0,0\n1,0,0,0,0,no\n");assert(research_tape_read(name.c_str(),id,out,4,why,sizeof why)<0&&out[0].buttons==123);
    write(path,header+"0,0,0,0,0,0\r\n");assert(research_tape_read(name.c_str(),id,out,4,why,sizeof why)==1);
    write(path,header+std::string(1,'\0'));assert(research_tape_read(name.c_str(),id,out,4,why,sizeof why)<0);
    write(path,header);assert(research_tape_read(name.c_str(),id,out,4,why,sizeof why)<0);
    write(path,header+"0,0,0,0,0,0\n");std::error_code ec;fs::create_symlink(path,root/"linked.lastape",ec);
    if(!ec)assert(research_tape_read((root/"linked.lastape").u8string().c_str(),id,out,4,why,sizeof why)<0);
    std::cout<<"Input tape identity, bounds, ordering, transactional parsing, links and round-trip passed\n";
}
