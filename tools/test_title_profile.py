"""Compile and exercise native adapter selection with synthetic NSO headers."""
from pathlib import Path
import os
import json
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
@DEVICE_INTEGER@
@EXPECTED@
int main(int argc,char **argv) {
    assert(argc==(int)title_profile_count()+1 && !title_is_las() && !*title_profile_current()->key);
    assert(device_integer(0)==53);
    char why[256],path[2048],id[65];
    for(int i=0;i<argc-1;i++) {
        const TitleProfile *before=title_profile_current();
        const TitleProfile *p=title_profile_validate(argv[i+1],why,sizeof why);
        assert(p && title_profile_current()==before);
        assert(!strcmp(p->key,keys[i]) && !strcmp(p->version,versions[i]) && p->module_count==counts[i]);
        assert(title_profile_select(argv[i+1],why,sizeof why));
        assert(title_is_las()==las[i]);
        for(unsigned j=0;j<p->module_count;j++) assert(p->init_order[j]==orders[i][j]);
        assert(p->nvn_major==majors[i] && device_integer(0)==majors[i] && device_integer(1)==minors[i]);
        snprintf(path,sizeof path,"%s/exefs/sdk",argv[i+1]);
        assert(title_build_id(path,id) && strlen(id)==64);
        FILE *f=fopen(path,"r+b"); assert(f); fseek(f,0x5f,SEEK_SET); int old=fgetc(f);
        fseek(f,0x5f,SEEK_SET); fputc(old^0x80,f); fclose(f);
        assert(!title_profile_validate(argv[i+1],why,sizeof why));
        assert(strstr(why,"sdk") && title_profile_current()==p);
        f=fopen(path,"r+b"); assert(f); fseek(f,0x5f,SEEK_SET); fputc(old,f); fclose(f);
        snprintf(path,sizeof path,"%s/native-entry.inc",argv[i+1]);
        assert(title_profile_write_entry(p,path,why,sizeof why));
    }
    puts("Native title selection, mixed SDK rejection and entry generation: PASS");
    return 0;
}
'''

assert (ROOT/'runtime/title_catalog.inc').read_text()==render_c_catalog(), 'Native catalog is out of date'
with tempfile.TemporaryDirectory() as temp:
    root=Path(temp).resolve()
    installs=[]
    for game in GAMES:
        for release in game.releases:
            install=root/(game.id+'-'+release.version)
            exefs=install/'exefs';exefs.mkdir(parents=True)
            for module in release.modules:
                header=bytearray(0x60);header[:4]=b'NSO0';header[0x40:0x60]=bytes.fromhex(module.build_id)
                (exefs/module.name).write_bytes(header)
            installs.append(install)
    pairs=[(game,release) for game in GAMES for release in game.releases]
    expected=''
    for name,values,typ in (
        ('keys',[json.dumps(g.id) for g,r in pairs],'const char *'),
        ('versions',[json.dumps(r.version) for g,r in pairs],'const char *'),
        ('counts',[str(len(r.modules)) for g,r in pairs],'unsigned'),
        ('las',[str('las_extensions' in r.runtime['features']).lower() for g,r in pairs],'bool'),
        ('majors',[str(r.runtime.get('nvn_major',53)) for g,r in pairs],'unsigned'),
        ('minors',[str(r.runtime['nvn_minor']) for g,r in pairs],'unsigned')):
        expected+=f'static {typ} {name}[]={{'+','.join(values)+'};\n'
    orders=[','.join(str(next(i for i,m in enumerate(r.modules) if m.name==name)) for name in r.init_order) for g,r in pairs]
    expected+='static unsigned orders[][TITLE_MAX_MODULES]={{'+'},{'.join(orders)+'}};\n'
    objects=(ROOT/'runtime/nvn/objects.c').read_text(encoding='utf-8')
    device_integer=objects[objects.index('static uint32_t device_integer('):objects.index('\nNVN(nvnDeviceBuilderSetDefaults)')]
    harness=root/'test.c';harness.write_text(HARNESS.replace('@EXPECTED@',expected).replace('@DEVICE_INTEGER@',device_integer))
    executable=root/('test.exe' if os.name=='nt' else 'test')
    cc=shlex.split(os.environ.get('CC','clang'))
    assert shutil.which(cc[0]),'A C compiler is required'
    subprocess.run(cc+['-std=c11','-Iruntime','runtime/title_profile.c',str(harness),'-o',str(executable)],cwd=ROOT,check=True)
    subprocess.run([str(executable),*map(str,installs)],check=True)
    for install in installs:
        assert (install/'native-entry.inc').read_text()==render_entry(inspect_exefs(install/'exefs'))
