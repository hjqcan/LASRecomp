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
    photo_state_reset(&p);assert(!p.active&&!p.frozen&&near(p.exposure,0)&&near(p.saturation,1));
    photo_state_enter(&p,30);p.aspect=3;
    double rect[4]={0,0,1920,1080};float square[4]={0,0,1,1};
    photo_state_frame(&p,square,rect);
    assert(rect[0]==420&&rect[1]==0&&rect[2]==1080&&rect[3]==1080);
    assert(near(square[0],0.21875f)&&near(square[2],0.5625f)&&near(square[3],1));
    p.pan_x=1;p.zoom=2;double portrait[4]={40,20,1600,900};float edge[4]={.1f,.2f,.8f,.6f};
    p.aspect=4;photo_state_frame(&p,edge,portrait);
    assert(fabs(portrait[2]/portrait[3]-.8)<.0001);
    assert(near(edge[0]+edge[2],.9f)&&edge[1]>=.2f&&edge[1]+edge[3]<=.80001f);
    p.aspect=-1;p.zoom=1;p.pan_x=0;double original[4]={0,0,1920,1080};float full[4]={0,0,1,1};
    photo_state_frame(&p,full,original);assert(near(full[2],1)&&original[2]==1920);
    float color[4];photo_state_color(&p,color);assert(near(color[0],1)&&near(color[1],1)&&near(color[2],0)&&near(color[3],0));
    p.contrast=99;p.vignette=INFINITY;p.exposure=100;photo_state_color(&p,color);
    assert(near(color[0],4)&&near(color[2],1)&&near(color[3],0));
    p.active=false;photo_state_color(&p,color);assert(near(color[0],1)&&near(color[2],0));
    puts("Photo freeze, clean-frame delay, HUD isolation, crop bounds and reset passed");
}
'''
with tempfile.TemporaryDirectory() as temporary:
    root = Path(temporary).resolve(); source=root/'test.c'; source.write_text(HARNESS)
    exe=root/('test.exe' if os.name=='nt' else 'test')
    subprocess.run(shlex.split(os.environ.get('CC','clang'))+['-std=c11','-Wall','-Wextra','-I'+str(ROOT/'runtime'),
                   str(source),str(ROOT/'runtime/photo.c'),'-lm','-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
