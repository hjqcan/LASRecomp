import os
from pathlib import Path
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent
HARNESS = r'''
#include "photo.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
static int near(float a,float b){return fabsf(a-b)<0.00001f;}
int main(void) {
    assert(photo_scene_composite(1920,1080,1280,720,true,false,1));
    assert(photo_scene_composite(1920,1080,1920,1080,true,false,1));
    assert(!photo_scene_composite(1920,1080,40,40,true,true,1));
    assert(!photo_scene_composite(1920,1080,1280,720,false,false,1));
    assert(!photo_scene_composite(1920,1080,1280,720,true,true,1));
    assert(!photo_scene_composite(1920,1080,512,512,true,false,1));
    PhotoState p;photo_state_reset(&p);assert(!p.active&&!p.frozen&&p.zoom==1);
    float crop[4]={0.1f,0.2f,0.8f,0.6f};photo_state_crop(&p,crop);assert(near(crop[0],0.1f));
    photo_state_enter(&p,10);photo_state_presented(&p,11);assert(!p.frozen);
    assert(!photo_state_skip_draw(&p,11,true,false));
    assert(!photo_state_skip_draw(&p,11,true,true));
    assert(photo_state_skip_draw(&p,11,true,false));
    assert(!photo_state_skip_draw(&p,11,false,false));
    assert(!photo_state_skip_draw(&p,12,true,false));
    photo_state_presented(&p,12);assert(p.frozen);
    p.zoom=2;p.pan_x=1;p.pan_y=-1;photo_state_crop(&p,crop);
    assert(near(crop[0],0.5f)&&near(crop[1],0.2f)&&near(crop[2],0.4f)&&near(crop[3],0.3f));
    photo_state_refresh(&p,20);assert(!p.frozen);p.hide_hud=false;
    assert(!photo_state_skip_draw(&p,21,true,true)&&!photo_state_skip_draw(&p,21,true,false));
    p.zoom=NAN;p.pan_x=INFINITY;float bounded[4]={0,0,1,1};photo_state_crop(&p,bounded);
    assert(isfinite(bounded[0])&&near(bounded[2],1));
    photo_state_presented(&p,22);assert(p.frozen);
    p.zoom=1;p.pan_x=p.pan_y=0;
    photo_state_pan(&p,1,1);assert(p.pan_x==0&&p.pan_y==0);
    photo_state_zoom(&p,100);assert(p.zoom==4);
    photo_state_pan(&p,100,-100);assert(p.pan_x==-1&&p.pan_y==1);
    photo_state_begin_framing(&p);photo_state_zoom(&p,-100);assert(p.zoom==1);
    photo_state_end_framing(&p,false);assert(p.zoom==4&&p.pan_x==-1&&p.pan_y==1);
    photo_state_begin_framing(&p);photo_state_zoom(&p,-100);photo_state_end_framing(&p,true);assert(p.zoom==1);
    photo_state_reset(&p);assert(!p.active&&!p.frozen&&near(p.exposure,0)&&near(p.saturation,1));
    puts("Photo freeze, clean-frame delay, HUD isolation, crop bounds and reset passed");
}
'''
with tempfile.TemporaryDirectory() as temporary:
    root = Path(temporary).resolve(); source=root/'test.c'; source.write_text(HARNESS)
    exe=root/('test.exe' if os.name=='nt' else 'test')
    subprocess.run(shlex.split(os.environ.get('CC','clang'))+['-std=c11','-Wall','-Wextra','-I'+str(ROOT/'runtime'),
                   str(source),str(ROOT/'runtime/photo.c'),'-lm','-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
    ui_exe = root/('photo-ui.exe' if os.name=='nt' else 'photo-ui')
    imgui = ROOT/'third_party/imgui'
    subprocess.run(shlex.split(os.environ.get('CXX','clang++'))+
                   ['-std=c++17','-x','c++','-I'+str(ROOT/'runtime'),'-I'+str(imgui),
                    str(ROOT/'tools/test_photo_ui.cpp'),str(ROOT/'runtime/photo.c'),
                    *[str(imgui/name) for name in ('imgui.cpp','imgui_draw.cpp','imgui_tables.cpp','imgui_widgets.cpp')],
                    '-o',str(ui_exe)],check=True)
    subprocess.run([str(ui_exe),str(root/'photos with spaces & punctuation')],check=True)
