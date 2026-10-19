#include "../research.h"
#include <charconv>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <locale>
#include <sstream>
#include <stdexcept>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#endif
namespace fs=std::filesystem;
namespace {
void need(bool v,const char *why) { if (!v) throw std::runtime_error(why); }
void plain(const fs::path &path) {
    fs::path part;
    for (const auto &p:fs::absolute(path)) {
        part/=p; need(!fs::is_symlink(fs::symlink_status(part)),"Input tapes cannot use linked paths.");
#ifdef _WIN32
        auto attr=GetFileAttributesW(part.c_str());
        need(attr==INVALID_FILE_ATTRIBUTES||!(attr&FILE_ATTRIBUTE_REPARSE_POINT),"Input tapes cannot use reparse points.");
#endif
    }
}
bool build_ok(const char *id) { return id&&strlen(id)==40&&strspn(id,"0123456789abcdef")==40; }
template<typename T> void number(const std::string &s,T &v) {
    auto r=std::from_chars(s.data(),s.data()+s.size(),v);
    need(r.ec==std::errc()&&r.ptr==s.data()+s.size(),"Invalid input-tape number.");
}
}
extern "C" bool research_input_valid(const ResearchInput *in) {
    if (!in||(in->buttons&~0xffffffull)) return false;
    for (int a:in->axes) if (a<-32767||a>32767) return false;
    return true;
}
static int tape_read(bool update,const char *path,const char *id,ResearchInput *out,size_t max,char *why,size_t cap) {
    try {
        need(build_ok(id),"Invalid expected Build ID."); auto file=fs::u8path(path); plain(file);
        need(fs::is_regular_file(file)&&fs::file_size(file)<=16*1024*1024,"Invalid or oversized input tape.");
        std::ifstream in(file,std::ios::binary); need(bool(in),"Cannot open input tape.");
        std::string text(16*1024*1024+1,'\0'); in.read(text.data(),text.size()); text.resize((size_t)in.gcount());
        need(!in.bad()&&text.size()<=16*1024*1024&&text.find('\0')==std::string::npos,"Invalid input tape text.");
        std::istringstream data(text); std::string line;
        auto line_read=[&](){ bool ok=bool(std::getline(data,line)); if (!line.empty()&&line.back()=='\r') line.pop_back();return ok; };
        need(line_read()&&line==(update?"LASINPUT2":"LASINPUT1"),"Input tape uses a different observation boundary or format.");
        need(line_read()&&line==std::string("build_id=")+id,"Input tape belongs to a different executable.");
        if(update)need(line_read()&&line=="boundary=las_update_dispatch_v1","Unsupported update boundary.");
        need(line_read()&&line==(update?"update,buttons,lx,ly,rx,ry":"poll,buttons,lx,ly,rx,ry"),"Invalid input tape header.");
        std::vector<ResearchInput> inputs;
        while (line_read()) {
            need(line.size()<160&&inputs.size()<max&&inputs.size()<RESEARCH_MAX_INPUTS,"Input tape exceeds its limits.");
            std::istringstream row(line);std::string part;std::vector<std::string> fields;
            while (std::getline(row,part,',')) fields.push_back(part);
            need(fields.size()==6&&!line.empty()&&line.back()!=',',"Invalid input tape row.");
            uint64_t poll;ResearchInput value{};number(fields[0],poll);number(fields[1],value.buttons);
            for (int i=0;i<4;++i) number(fields[i+2],value.axes[i]);
            need(poll==inputs.size()&&research_input_valid(&value),"Invalid input sequence or bounds.");inputs.push_back(value);
        }
        need(!inputs.empty(),"Input tape is empty.");
        memcpy(out,inputs.data(),inputs.size()*sizeof *out);return (int)inputs.size();
    } catch (const std::exception &e) { if(cap)snprintf(why,cap,"%s",e.what());return -1; }
}
static bool tape_write(bool update,const char *path,const char *id,const ResearchInput *inputs,size_t count,char *why,size_t cap) {
    try {
        need(build_ok(id)&&count>0&&count<=RESEARCH_MAX_INPUTS,"Invalid input tape identity or count.");
        for(size_t i=0;i<count;++i) need(research_input_valid(inputs+i),"Invalid input state.");
        auto file=fs::u8path(path);plain(file);need(!fs::exists(file),"Input tape already exists.");
        auto tmp=file;tmp+=".writing";plain(tmp);need(!fs::exists(tmp),"Input tape staging file already exists.");
        std::ofstream f(tmp,std::ios::binary); f.imbue(std::locale::classic());
        f<<(update?"LASINPUT2":"LASINPUT1")<<"\nbuild_id="<<id<<'\n';
        if(update)f<<"boundary=las_update_dispatch_v1\nupdate,buttons,lx,ly,rx,ry\n";
        else f<<"poll,buttons,lx,ly,rx,ry\n";
        for(size_t i=0;i<count;++i) { const auto &s=inputs[i];f<<i<<','<<s.buttons;for(int a:s.axes)f<<','<<a;f<<'\n'; }
        f.close();need(bool(f),"Cannot write input tape.");fs::rename(tmp,file);return true;
    } catch (const std::exception &e) { if(cap)snprintf(why,cap,"%s",e.what());return false; }
}
extern "C" int research_tape_read(const char *p,const char *id,ResearchInput *out,size_t max,char *why,size_t cap){return tape_read(false,p,id,out,max,why,cap);}
extern "C" bool research_tape_write(const char *p,const char *id,const ResearchInput *in,size_t n,char *why,size_t cap){return tape_write(false,p,id,in,n,why,cap);}
extern "C" int research_update_tape_read(const char *p,const char *id,ResearchInput *out,size_t max,char *why,size_t cap){return tape_read(true,p,id,out,max,why,cap);}
extern "C" bool research_update_tape_write(const char *p,const char *id,const ResearchInput *in,size_t n,char *why,size_t cap){return tape_write(true,p,id,in,n,why,cap);}
