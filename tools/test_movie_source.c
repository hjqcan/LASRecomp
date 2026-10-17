// Build with the runtime's FFmpeg headers/libraries. The fixture is synthetic
// PCM in a WAV container and is created only in the supplied scratch directory.
#include <assert.h>
#include "../runtime/hle.h"
#undef HLE_FUNC
#define HLE_FUNC(sym, fn) static void fn(Cpu *c)
#undef RT_ON_RESET
#define RT_ON_RESET(fn) static void fn(void)
#include "../runtime/hle/movie.c"

const char *g_romfs_dir;
Settings g_settings = {.movie_audio=true};
volatile int g_inject_plus;
uint64_t input_buttons_now(void) { return 0; }
bool rando_romfs_override(const char *p, char *o, size_t n) { (void)p; (void)o; (void)n; return false; }
bool mods_romfs_override(const char *p, char *o, size_t n) { (void)p; (void)o; (void)n; return false; }

int main(int argc, char **argv) {
    assert(argc==2); g_romfs_dir=argv[1];
    char path[1024]; snprintf(path,sizeof path,"%s/movie-source-test.wav",g_romfs_dir);
    uint32_t wave[15]={0x46464952,52,0x45564157,0x20746d66,16,0x00010001,48000,96000,
                       0x00100002,0x61746164,16,0x00020001,0x00040003,0x00060005,0x00080007};
    FILE *file=fopen(path,"wbx"); assert(file); assert(fwrite(wave,1,sizeof wave,file)==sizeof wave); fclose(file);
    Extractor e={0}; uint64_t object[2]={0,(uintptr_t)&e}; Cpu c={0};
    for(int mode=0;mode<2;mode++) {
        c.x[0]=(uintptr_t)object; c.x[1]=(uintptr_t)"content:/movie-source-test.wav";
        if(mode==0) ex_set_path_source(&c);
        else { c.x[2]=c.x[1]; c.x[1]=0; ex_set_data_source(&c); }
        assert(c.x[0]==0 && e.fmt && e.pkt && e.ntracks==1 && e.selected==0 && !e.eof);
        c.x[0]=(uintptr_t)object; c.x[1]=0; ex_select_track(&c); ex_fill(&e);
        assert(e.have_pkt && e.pkt->size==16 && e.pkt->data[0]==1);
    }
    c.x[0]=(uintptr_t)object; c.x[1]=(uintptr_t)"content:/missing-movie-source-test.wav"; ex_set_path_source(&c);
    assert(c.x[0]!=0 && !e.fmt && !e.pkt && e.ntracks==0 && !e.selected);
    ex_fill(&e); assert(e.eof && !e.have_pkt);
    c.x[0]=(uintptr_t)object; c.x[1]=0; ex_set_path_source(&c); assert(c.x[0]!=0);
    assert(remove(path)==0);
    puts("PASS: both file-source overloads, packet read, source replacement, missing/null path and cleanup");
}
