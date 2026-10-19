#include "../workshop.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <locale>
#include <map>
#include <sstream>
#include <stdexcept>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#endif
namespace fs=std::filesystem;
namespace {
void need(bool ok,const std::string &why) { if (!ok) throw std::runtime_error(why); }
void error(char *out,size_t cap,const std::exception &e) { if (cap) snprintf(out,cap,"%s",e.what()); }
void plain(const fs::path &p) {
    fs::path part;
    for (const auto &c:fs::absolute(p)) {
        part/=c; need(!fs::is_symlink(fs::symlink_status(part)),"Workshop paths cannot contain links.");
#ifdef _WIN32
        auto a=GetFileAttributesW(part.c_str());
        need(a==INVALID_FILE_ATTRIBUTES || !(a&FILE_ATTRIBUTE_REPARSE_POINT),"Workshop paths cannot contain reparse points.");
#endif
    }
}
std::string read(const fs::path &p,uintmax_t max) {
    plain(p); need(fs::is_regular_file(p)&&fs::file_size(p)<=max,"Invalid or oversized workshop file.");
    std::ifstream f(p,std::ios::binary); need(bool(f),"Cannot read workshop file.");
    std::string s((size_t)max+1,'\0'); f.read(s.data(),s.size()); s.resize((size_t)f.gcount());
    need(!f.bad()&&s.size()<=max&&s.find('\0')==std::string::npos,"Invalid workshop text."); return s;
}
void publish(const fs::path &path,const std::string &text,bool replace) {
    plain(path); fs::create_directories(path.parent_path());
    need(replace || !fs::exists(path),"A workshop file with this name already exists.");
    auto tmp=path; tmp+=".writing"; plain(tmp);
    std::ofstream f(tmp,std::ios::binary|std::ios::trunc); f<<text; f.close(); need(bool(f),"Cannot write workshop file.");
#ifdef _WIN32
    need(MoveFileExW(tmp.c_str(),path.c_str(),(replace?MOVEFILE_REPLACE_EXISTING:0)|MOVEFILE_WRITE_THROUGH)!=0,"Cannot publish workshop file.");
#else
    fs::rename(tmp,path);
#endif
}
std::string config_text(const ChallengeConfig &c) {
    std::ostringstream s; s.imbue(std::locale::classic());
    s<<"format=1\nname="<<c.name<<"\ngame_version="<<c.game_version<<"\ngoal_level="<<c.goal_level
     <<"\nseconds="<<c.seconds<<"\ndamage="<<c.damage<<"\nno_hit="<<c.no_hit<<"\nohko="<<c.ohko<<'\n'; return s.str();
}
int integer(const std::string &s) {
    need(!s.empty()&&s.find_first_not_of("0123456789")==std::string::npos&&s.size()<9,"Invalid challenge number."); return std::stoi(s);
}
bool level_ok(const char *s) {
    size_t n=0; for (;n<64 && s[n];++n) if (!((s[n]>='a'&&s[n]<='z')||(s[n]>='A'&&s[n]<='Z')||(s[n]>='0'&&s[n]<='9')||s[n]=='_')) return false;
    return n>0&&n<64;
}
bool sample_ok(const RouteSample &s) {
    return level_ok(s.level)&&std::isfinite(s.x)&&std::isfinite(s.y)&&std::isfinite(s.z)&&
        fabs(s.x)<1e7&&fabs(s.y)<1e7&&fabs(s.z)<1e7&&s.hp>=-32768&&s.hp<=32767&&
        s.lx>=-32767&&s.lx<=32767&&s.ly>=-32767&&s.ly<=32767&&s.rx>=-32767&&s.rx<=32767&&s.ry>=-32767&&s.ry<=32767;
}
}
extern "C" bool workshop_config_read(const char *path,ChallengeConfig *out,char *why,size_t cap) {
    try {
        std::istringstream s(read(fs::u8path(path),4096)); std::map<std::string,std::string> fields; std::string line;
        while (std::getline(s,line)) {
            if (!line.empty()&&line.back()=='\r') line.pop_back(); if (line.empty()) continue;
            auto at=line.find('='); need(at!=std::string::npos&&fields.emplace(line.substr(0,at),line.substr(at+1)).second,"Duplicate or invalid challenge field.");
        }
        need(fields.size()==8&&fields["format"]=="1","Unsupported challenge format."); ChallengeConfig c{};
        auto copy=[&](const char *key,char *to,size_t n) { auto v=fields.at(key); need(v.size()<n,"Challenge field is too long."); memcpy(to,v.c_str(),v.size()+1); };
        copy("name",c.name,sizeof c.name); copy("game_version",c.game_version,sizeof c.game_version); copy("goal_level",c.goal_level,sizeof c.goal_level);
        c.seconds=integer(fields.at("seconds")); c.damage=integer(fields.at("damage"));
        int no_hit=integer(fields.at("no_hit")),ohko=integer(fields.at("ohko")); need(no_hit<=1&&ohko<=1,"Invalid challenge toggle.");
        c.no_hit=no_hit; c.ohko=ohko; need(challenge_config_valid(&c),"Invalid challenge settings."); *out=c; return true;
    } catch (const std::exception &e) { error(why,cap,e); return false; }
}
extern "C" bool workshop_config_write(const char *path,const ChallengeConfig *c,char *why,size_t cap) {
    try { need(challenge_config_valid(c),"Invalid challenge settings."); publish(fs::u8path(path),config_text(*c),false); return true; }
    catch (const std::exception &e) { error(why,cap,e); return false; }
}
extern "C" bool workshop_prepare_run(const char *data,const char *source,const ChallengeConfig *c,char *run,size_t run_cap,char *why,size_t cap) {
    fs::path directory; bool created=false;
    try {
        need(challenge_config_valid(c),"Invalid challenge settings.");
        auto root=fs::absolute(fs::u8path(data))/"workshop"/"runs", src=fs::absolute(fs::u8path(source)); plain(root); plain(src);
        // Collect and bound the snapshot before creating anything. Backups stay with the source.
        std::vector<std::pair<fs::path,fs::path>> files; uintmax_t total=0;
        if (fs::exists(src)) for (const auto &e:fs::directory_iterator(src)) {
            auto name=e.path().filename().u8string();
            if (name.size()!=6 || name.compare(0,5,"save_") || name[5]<'0'||name[5]>'9') continue;
            plain(e.path()); need(e.is_directory(),"Save mounts must be ordinary directories.");
            for (auto it=fs::recursive_directory_iterator(e.path());it!=fs::recursive_directory_iterator();++it) {
                plain(it->path()); need(it.depth()<8,"Save directory nesting exceeds the limit.");
                if (it->is_directory()) continue;
                need(it->is_regular_file(),"Save entries must be ordinary files.");
                auto bytes=it->file_size(); need(bytes<=64*1024*1024,"Save file exceeds the snapshot limit.");
                total+=bytes; need(total<=64*1024*1024 && files.size()<512,"Save snapshot exceeds the limit.");
                files.emplace_back(it->path(),it->path().lexically_relative(src));
            }
        }
        fs::create_directories(root);
        directory=root/("run-"+std::to_string(std::chrono::system_clock::now().time_since_epoch().count()));
        need(directory.generic_u8string().size()+16<run_cap,"Challenge output path is too long.");
        need(fs::create_directory(directory),"Cannot create challenge run directory.");
        created=true;
        fs::create_directory(directory/"save");
        for (const auto &f:files) { auto dest=directory/"save"/f.second; fs::create_directories(dest.parent_path()); fs::copy_file(f.first,dest); }
        publish(directory/"challenge.laschallenge",config_text(*c),false);
        publish(directory/"result.ini","format=1\nstatus=armed\n",false);
        snprintf(run,run_cap,"%s",directory.generic_u8string().c_str()); return true;
    } catch (const std::exception &e) {
        // Only this call's newly-created run is removed; source paths are never modified.
        if (created) { std::error_code ec; fs::remove_all(directory,ec); }
        error(why,cap,e); return false;
    }
}
extern "C" bool workshop_route_write(const char *run,const RouteSample *samples,size_t count,const ChallengeState *state,const char *seed,bool assisted,char *why,size_t cap) {
    try {
        need(count<=40000,"Too many route samples.");
        std::ostringstream csv; csv.imbue(std::locale::classic()); csv<<std::setprecision(9);
        csv<<"elapsed_ns,frame,level,x,y,z,hp,buttons,lx,ly,rx,ry,position\n";
        for (size_t i=0;i<count;++i) {
            const auto &s=samples[i]; need(sample_ok(s)&&s.now_ns>=state->start_ns,"Invalid route sample.");
            csv<<s.now_ns-state->start_ns<<','<<s.frame<<','<<s.level<<','<<s.x<<','<<s.y<<','<<s.z<<','<<s.hp<<','<<s.buttons
               <<','<<s.lx<<','<<s.ly<<','<<s.rx<<','<<s.ry<<','<<s.position<<'\n';
        }
        need(strlen(seed)<128&&strpbrk(seed,"\r\n")==NULL,"Invalid seed label.");
        std::ostringstream result; result.imbue(std::locale::classic());
        result<<"format=1\nstatus="<<state->status<<"\nreason="<<state->reason<<"\nelapsed_seconds="<<state->elapsed
              <<"\nsamples="<<count<<"\nseed="<<seed<<"\nassisted="<<assisted<<"\nreplay=observed_route\n";
        auto root=fs::u8path(run); plain(root); need(fs::is_directory(root),"Challenge run directory is missing.");
        publish(root/"route.csv",csv.str(),true); publish(root/"result.ini",result.str(),true); return true;
    } catch (const std::exception &e) { error(why,cap,e); return false; }
}
extern "C" int workshop_route_read(const char *path,RouteSample *out,size_t max,char *why,size_t cap) {
    try {
        std::istringstream in(read(fs::u8path(path),12*1024*1024)); std::string line;
        need(bool(std::getline(in,line))&&line=="elapsed_ns,frame,level,x,y,z,hp,buttons,lx,ly,rx,ry,position","Unsupported route CSV header.");
        size_t count=0; uint64_t previous=0;
        while (std::getline(in,line)) {
            need(count<max&&count<40000&&line.size()<512,"Route exceeds the size limit.");
            RouteSample s{}; std::vector<std::string> parts; std::istringstream row(line); std::string value;
            while (std::getline(row,value,',')) parts.push_back(value);
            need(parts.size()==13,"Invalid route row.");
            auto parse=[&](size_t i,auto &v) { std::istringstream stream(parts[i]); stream.imbue(std::locale::classic()); need(bool(stream>>v),"Invalid route number."); stream>>std::ws; need(stream.eof(),"Invalid route number."); };
            need(parts[0].find_first_not_of("0123456789")==std::string::npos&&parts[1].find_first_not_of("0123456789")==std::string::npos,"Invalid route time or frame.");
            parse(0,s.now_ns); parse(1,s.frame); need(parts[2].size()<64,"Route level is too long."); memcpy(s.level,parts[2].c_str(),parts[2].size()+1);
            parse(3,s.x); parse(4,s.y); parse(5,s.z); parse(6,s.hp); parse(7,s.buttons);
            parse(8,s.lx); parse(9,s.ly); parse(10,s.rx); parse(11,s.ry); int position; parse(12,position);
            need(position==0||position==1,"Invalid route position flag."); s.position=position; s.playing=true;
            need(sample_ok(s)&&s.now_ns>=previous&&s.now_ns<=3601ull*1000000000ull,"Invalid route bounds or ordering.");
            previous=s.now_ns; out[count++]=s;
        }
        need(count>0,"Route has no samples."); return (int)count;
    } catch (const std::exception &e) { error(why,cap,e); return -1; }
}
