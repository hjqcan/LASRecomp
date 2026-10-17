"""Compile and exercise native adapter selection with synthetic NSO headers."""
from pathlib import Path
import os
import shlex
import shutil
import subprocess
import tempfile
from game_catalog import GAMES, inspect_exefs, render_c_catalog, render_entry

ROOT=Path(__file__).resolve().parents[1]
HARNESS=r'''
#include "title_profile.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(int argc,char **argv) {
    assert(argc==4 && title_is_las());
    const char *keys[]={"las","las","acnh"}, *versions[]={"1.0.0","1.1.0","1.0.0"};
    unsigned counts[]={2,2,3};
    char why[256],path[2048],id[65];
    for(int i=0;i<3;i++) {
        const TitleProfile *before=title_profile_current();
        const TitleProfile *p=title_profile_validate(argv[i+1],why,sizeof why);
        assert(p && title_profile_current()==before);
        assert(!strcmp(p->key,keys[i]) && !strcmp(p->version,versions[i]) && p->module_count==counts[i]);
        assert(title_profile_select(argv[i+1],why,sizeof why));
        assert(title_is_las()==(i<2));
        assert(p->init_order[0]==1 && p->init_order[p->module_count-1]==0);
        snprintf(path,sizeof path,"%s/exefs/sdk",argv[i+1]);
        assert(title_build_id(path,id) && strlen(id)==64);
        FILE *f=fopen(path,"r+b"); assert(f); fseek(f,0x5f,SEEK_SET); fputc(1,f); fclose(f);
        assert(!title_profile_validate(argv[i+1],why,sizeof why));
        assert(strstr(why,"sdk") && title_profile_current()==p);
        f=fopen(path,"r+b"); assert(f); fseek(f,0x5f,SEEK_SET); fputc(0,f); fclose(f);
        snprintf(path,sizeof path,"%s/native-entry.inc",argv[i+1]);
        assert(title_profile_write_entry(p,path,why,sizeof why));
    }
    puts("Native title selection, mixed SDK rejection and entry generation: PASS");
    return 0;
}
'''

assert (ROOT/'runtime/title_catalog.inc').read_text()==render_c_catalog(), 'Native catalog is out of date'
with tempfile.TemporaryDirectory() as temp:
    root=Path(temp)
    installs=[]
    for game in GAMES:
        for release in game.releases:
            install=root/(game.id+'-'+release.version)
            exefs=install/'exefs';exefs.mkdir(parents=True)
            for module in release.modules:
                header=bytearray(0x60);header[:4]=b'NSO0';header[0x40:0x60]=bytes.fromhex(module.build_id)
                (exefs/module.name).write_bytes(header)
            installs.append(install)
    harness=root/'test.c';harness.write_text(HARNESS)
    executable=root/('test.exe' if os.name=='nt' else 'test')
    cc=shlex.split(os.environ.get('CC','clang'))
    assert shutil.which(cc[0]),'A C compiler is required'
    subprocess.run(cc+['-std=c11','-Iruntime','runtime/title_profile.c',str(harness),'-o',str(executable)],cwd=ROOT,check=True)
    subprocess.run([str(executable),*map(str,installs)],check=True)
    for install in installs:
        assert (install/'native-entry.inc').read_text()==render_entry(inspect_exefs(install/'exefs'))
