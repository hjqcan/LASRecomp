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
static unsigned texture_uploads;
static bool gpu_upload;
bool renderer_upload_nv12(NvnTexture *t, const void *p, uint32_t w, uint32_t h, uint32_t stride, uint32_t uv_off) {
    assert(t==(NvnTexture *)(uintptr_t)0x1234 && p && w==4 && h==2 && stride==256 && uv_off==4096);
    return gpu_upload;
}
void renderer_upload_rgba(NvnTexture *t, const void *p, uint32_t w, uint32_t h, uint32_t s) {
    assert(t==(NvnTexture *)(uintptr_t)0x1234 && p && w==4 && h==2 && s==16);
    texture_uploads++;
}

static void test_decoder_copy(void) {
    // Deliberately padded 4x2 NV12: the two visible rows and chroma must be
    // copied to the original 12-byte guest allocation without padding.
    uint8_t frame[256 * 24]; memset(frame, 0xee, sizeof frame);
    memcpy(frame, "abcd", 4); memcpy(frame + 256, "efgh", 4);
    memcpy(frame + 256 * 16, "ijkl", 4);
    Decoder d = {.video=true, .output_format=4, .width=4, .height=2,
                 .stride=256, .out_cap=sizeof frame};
    d.out_mem[0] = frame; d.out_size[0] = 256 * 17;
    uint64_t object[2] = {0, (uintptr_t)&d};
    uint8_t dst[28]; memset(dst, 0xa5, sizeof dst);
    MBuffer b = {.base=(uintptr_t)(dst+8), .capacity=12, .nvn_device=0xabcdef};
    Cpu c = {.x={(uintptr_t)object, 0, (uintptr_t)&b}};
    dec_get_output_buffer(&c);
    assert(c.x[0]==0 && b.base==(uintptr_t)(dst+8) && b.capacity==12 && b.nvn_device==0xabcdef);
    assert(b.offset==0 && b.size==12 && !memcmp(dst+8,"abcdefghijkl",12));
    for(int i=0;i<8;i++) assert(dst[i]==0xa5 && dst[20+i]==0xa5);
    uint32_t sizes[4]={0xaabbccdd,0,0,0x12345678};
    c=(Cpu){.x={(uintptr_t)object,(uintptr_t)&sizes[1],(uintptr_t)&sizes[2]}};
    dec_get_counts(&c);
    assert(c.x[0]==0 && sizes[1]==12 && sizes[2]==1 && sizes[0]==0xaabbccdd && sizes[3]==0x12345678);
    memset(dst,0xa5,sizeof dst); b.capacity=11;
    c=(Cpu){.x={(uintptr_t)object,0,(uintptr_t)&b}}; dec_get_output_buffer(&c);
    assert(c.x[0]!=0 && b.size==0);
    for(unsigned i=0;i<sizeof dst;i++) assert(dst[i]==0xa5);
    // PCM uses the same copy contract, including a retained base pointer.
    d.video=false; d.out_size[0]=4; b.capacity=12;
    c.x[0]=(uintptr_t)object; dec_get_output_buffer(&c);
    assert(c.x[0]==0 && b.size==4 && !memcmp(dst+8,"abcd",4) && dst[12]==0xa5);
    d.out_size[0]=0; c.x[0]=(uintptr_t)object; dec_get_output_buffer(&c);
    assert(c.x[0]==0 && b.size==0 && b.base==(uintptr_t)(dst+8));
    // Existing NVN texture output must retain the eight-byte handle range.
    NvnTexture *texture=(NvnTexture *)(uintptr_t)0x1234;
    d.video=true; d.output_format=3; d.out_size[0]=256*17;
    b=(MBuffer){.base=(uintptr_t)&texture,.capacity=8,.size=8};
    c.x[0]=(uintptr_t)object; dec_get_output_buffer(&c);
    assert(c.x[0]==0 && b.size==8 && b.base==(uintptr_t)&texture && texture_uploads==1);
    gpu_upload=true; c.x[0]=(uintptr_t)object; dec_get_output_buffer(&c);
    assert(c.x[0]==0 && b.size==8 && b.base==(uintptr_t)&texture && texture_uploads==1);
    sws_freeContext(d.sws); free(d.rgba);
}

int main(int argc, char **argv) {
    test_decoder_copy();
    assert(argc==2); g_romfs_dir=argv[1];
    char path[1024]; snprintf(path,sizeof path,"%s/movie-source-test.wav",g_romfs_dir);
    uint32_t wave[15]={0x46464952,52,0x45564157,0x20746d66,16,0x00010001,48000,96000,
                       0x00100002,0x61746164,16,0x00020001,0x00040003,0x00060005,0x00080007};
    // Match callers that inline these accessors using the guest SDK layout.
    uint8_t raw[80]; memset(raw,0xa5,sizeof raw); Cpu b={0};
    b.x[0]=(uintptr_t)raw; b.x[1]=(uintptr_t)wave; b.x[2]=sizeof wave; buf_ctor_ext(&b);
    uint64_t base,capacity; uint32_t size;
    memcpy(&base,raw+0x28,8); memcpy(&capacity,raw+0x18,8); memcpy(&size,raw+4,4);
    assert(base==(uintptr_t)wave && capacity==sizeof wave && size==sizeof wave);
    b.x[0]=(uintptr_t)raw; b.x[1]=4; b.x[2]=8; buf_set_range(&b);
    assert(*(int32_t *)raw==4 && *(int32_t *)(raw+4)==8);
    b.x[0]=(uintptr_t)raw; buf_base(&b); assert(b.x[0]==base);
    for(unsigned i=64;i<sizeof raw;i++) assert(raw[i]==0xa5);
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
    puts("PASS: inline buffer ABI, bounded PCM/NV12 output copy, int32 size/alignment, file sources and cleanup");
}
