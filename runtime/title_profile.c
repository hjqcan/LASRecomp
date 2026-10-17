#include "title_profile.h"
#include <stdio.h>
#include <string.h>
#include "title_catalog.inc"

static const TitleProfile *selected;
const TitleProfile *title_profile_current(void) { return selected ? selected : &title_profiles[0]; }
bool title_is_las(void) { return !strcmp(title_profile_current()->key, "las"); }

static void hex_id(const unsigned char id[32], char out[65]) {
    static const char hex[] = "0123456789abcdef";
    for (int i=0;i<32;i++) { out[2*i]=hex[id[i]>>4]; out[2*i+1]=hex[id[i]&15]; }
    out[64]=0;
}
bool title_build_id(const char *path, char out[65]) {
    unsigned char header[0x60];
    out[0]=0;
    FILE *f=fopen(path,"rb");
    if (!f) return false;
    size_t n=fread(header,1,sizeof header,f); fclose(f);
    if (n!=sizeof header || memcmp(header,"NSO0",4)) return false;
    hex_id(header+0x40,out);
    return true;
}
bool title_module_matches(const TitleModule *module, const unsigned char id[32]) {
    char value[65]; hex_id(id,value); return !strcmp(value,module->build_id);
}
const TitleProfile *title_profile_find(const char *main_path) {
    char id[65];
    if (!title_build_id(main_path,id)) return NULL;
    for (size_t i=0;i<sizeof title_profiles/sizeof title_profiles[0];i++)
        if (!strcmp(id,title_profiles[i].modules[0].build_id)) return &title_profiles[i];
    return NULL;
}
const TitleProfile *title_profile_validate(const char *root, char *why, size_t size) {
    char path[2048],id[65];
    if (snprintf(path,sizeof path,"%s/exefs/main",root)>=(int)sizeof path) {
        snprintf(why,size,"Game path is too long"); return NULL;
    }
    const TitleProfile *profile=title_profile_find(path);
    if (!profile) { snprintf(why,size,"Unknown or unreadable game executable; this build needs an adapter"); return NULL; }
    for (unsigned i=0;i<profile->module_count;i++) {
        const TitleModule *m=&profile->modules[i];
        if (snprintf(path,sizeof path,"%s/exefs/%s",root,m->name)>=(int)sizeof path ||
            !title_build_id(path,id) || strcmp(id,m->build_id)) {
            snprintf(why,size,"%s %s: missing or mismatched %s module",profile->name,profile->version,m->name);
            return NULL;
        }
    }
    return profile;
}
bool title_profile_select(const char *root, char *why, size_t size) {
    const TitleProfile *p=title_profile_validate(root,why,size);
    if (p) selected=p;
    return p!=NULL;
}
bool title_profile_write_entry(const TitleProfile *p, const char *path, char *why, size_t size) {
    FILE *f=fopen(path,"wb");
    if (!f) { snprintf(why,size,"Cannot create the game module entry"); return false; }
    fprintf(f,"// Generated from the reviewed game adapter.\n#define GAMELIB_GAME_ID \"%s\"\n"
              "#define GAMELIB_GAME_VERSION \"%s\"\n#define GAMELIB_ADAPTER_REVISION %u\n",
              p->key,p->version,p->adapter_revision);
    for (unsigned i=0;i<p->module_count;i++) {
        const char *name=p->modules[i].name;
        fprintf(f,"extern const uint64_t %s_fn_addrs[], %s_fn_count;\nextern const GuestFn %s_fn_ptrs[];\n"
                  "extern const char %s_build_id_full[];\n",name,name,name,name);
    }
    fputs("static const GameLibModule generated_modules[] = {\n",f);
    for (unsigned i=0;i<p->module_count;i++) {
        const TitleModule *m=&p->modules[i];
        fprintf(f,"    {\"%s\", UINT64_C(0x%llx), %s_build_id_full, %s_fn_addrs, %s_fn_ptrs, &%s_fn_count},\n",
                m->name,(unsigned long long)m->base,m->name,m->name,m->name,m->name);
    }
    fputs("};\n",f);
    bool ok=!ferror(f);
    if (fclose(f)) ok=false;
    if (!ok) snprintf(why,size,"Cannot write the game module entry");
    return ok;
}
