"""Exercise the real SDL input router with two virtual gamepads (no game assets)."""
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parent.parent
HARNESS = r'''
#include "hle.h"
#undef HLE_FUNC
#define HLE_FUNC(sym,fn) static void fn(Cpu *c)
#undef RT_ON_RESET
#define RT_ON_RESET(fn) static void fn(void)
#include "hle/input.c"
#include <assert.h>
static bool active;
static int32_t second_x,second_y;
static bool second_attack;
bool coop_active(void){return active;}
bool coop_requested(void){return active;}
void coop_host_input(int32_t x,int32_t y,bool attack){second_x=x;second_y=y;second_attack=attack;}
int16_t g_bind_key[BIND_KEY_COUNT],g_bind_pad[BIND_PAD_COUNT];
// Unrelated UI/test helpers referenced by exported input.c functions on PE linkers.
void performance_frame(uint64_t f){} void workshop_frame(uint64_t f){} void watch_arm(void){}
int bind_pad_from_name(const char *s){return -1;} int bind_slot_from_name(const char *s){return -1;}
int bind_label_family(void){return input_pad_family();}
bool bind_capture_event(const SDL_Event *e){return false;} void bind_capture_begin(int d,int s){} void bind_reset(int d){}
bool menu_input_captured(void){return false;}
bool menu_is_open(void){return false;}
int game_in_play(void){return 0;}
int cheats_link_health(int *max){(void)max;abort();}
Settings g_settings={.rumble=true,.rumble_strength=100};
bool title_is_las(void){return true;}
uint64_t g_test_buttons,g_test_buttons_until;char g_window_shot_request[256];
static void key(SDL_Scancode sc,bool down){SDL_Event e={0};e.type=down?SDL_EVENT_KEY_DOWN:SDL_EVENT_KEY_UP;e.key.scancode=sc;input_handle_event(&e);}
static SDL_JoystickID virtual_pad(const char *name){
    SDL_VirtualJoystickDesc d;SDL_INIT_INTERFACE(&d);d.type=SDL_JOYSTICK_TYPE_GAMEPAD;
    d.naxes=SDL_GAMEPAD_AXIS_COUNT;d.nbuttons=SDL_GAMEPAD_BUTTON_COUNT;d.name=name;
    d.axis_mask=(1u<<SDL_GAMEPAD_AXIS_COUNT)-1;d.button_mask=(1u<<SDL_GAMEPAD_BUTTON_COUNT)-1;
    SDL_JoystickID id=SDL_AttachVirtualJoystick(&d);assert(id);return id;
}
int main(void){
    assert(SDL_Init(SDL_INIT_GAMEPAD));
    assert(input_family_of_type(SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO)==0);
    assert(input_family_of_type(SDL_GAMEPAD_TYPE_XBOXONE)==1);
    assert(input_family_of_type(SDL_GAMEPAD_TYPE_PS5)==2);
    assert(input_family_of_type(SDL_GAMEPAD_TYPE_STANDARD)==1);
    g_settings.prompts=PROMPTS_NINTENDO;assert(input_menu_family(0)==0);
    g_settings.prompts=PROMPTS_XBOX;assert(input_menu_family(0)==1);
    g_settings.prompts=PROMPTS_PLAYSTATION;assert(input_menu_family(0)==2);
    g_settings.prompts=PROMPTS_AUTO;
    for(int i=0;i<BIND_PAD_COUNT;++i)g_bind_pad[i]=-1;
    g_bind_key[BIND_RS_RIGHT]=SDL_SCANCODE_KP_6;g_bind_key[BIND_LS_UP]=SDL_SCANCODE_W;
    g_bind_pad[BIND_B]=SDL_GAMEPAD_BUTTON_SOUTH;
    key(SDL_SCANCODE_KP_6,true);assert(key_rx==32767&&second_x==0);key(SDL_SCANCODE_KP_6,false);
    active=true;key(SDL_SCANCODE_KP_6,true);key(SDL_SCANCODE_W,true);key(SDL_SCANCODE_KP_0,true);
    assert(key_rx==0&&key_ly==32767&&second_x==32767&&second_attack);
    key(SDL_SCANCODE_KP_6,false);key(SDL_SCANCODE_W,false);key(SDL_SCANCODE_KP_0,false);
    assert(!key_ly&&!second_x&&!second_attack);
    SDL_JoystickID one=virtual_pad("Co-op test P1"),two=virtual_pad("Co-op test P2");
    // Explicitly select the fixture pads; physical controllers cannot contaminate the assertion.
    if(pad)SDL_CloseGamepad(pad);if(pad2)SDL_CloseGamepad(pad2);
    pad=SDL_OpenGamepad(one);pad2=SDL_OpenGamepad(two);assert(pad&&pad2);
    SDL_Joystick *j1=SDL_GetGamepadJoystick(pad),*j2=SDL_GetGamepadJoystick(pad2);
    assert(SDL_SetJoystickVirtualAxis(j2,SDL_GAMEPAD_AXIS_LEFTX,24000));
    assert(SDL_SetJoystickVirtualButton(j2,SDL_GAMEPAD_BUTTON_SOUTH,true));SDL_UpdateGamepads();recompute_pad();
    assert(second_x==24000&&second_attack&&!pad_buttons&&!pad_lx);
    assert(SDL_SetJoystickVirtualAxis(j1,SDL_GAMEPAD_AXIS_LEFTY,-17000));SDL_UpdateGamepads();recompute_pad();
    assert(pad_ly==17000&&second_x==24000);
    SDL_Event e={0};e.type=SDL_EVENT_GAMEPAD_BUTTON_DOWN;e.gbutton.which=two;
    assert(input_second_event(&e));e.gbutton.which=one;assert(!input_second_event(&e));
    e.type=SDL_EVENT_KEY_DOWN;e.key.scancode=SDL_SCANCODE_KP_0;assert(input_second_event(&e));
    e.type=SDL_EVENT_TEXT_INPUT;assert(input_second_event(&e)); // no P2 digit in P1's name field
    e.type=SDL_EVENT_KEY_DOWN;e.key.scancode=SDL_SCANCODE_W;assert(!input_second_event(&e));
    e.type=SDL_EVENT_TEXT_INPUT;assert(!input_second_event(&e));
    e.type=SDL_EVENT_WINDOW_FOCUS_LOST;input_handle_event(&e);recompute_pad();assert(!second_x&&!second_attack);
    e.type=SDL_EVENT_WINDOW_FOCUS_GAINED;input_handle_event(&e);assert(second_x==24000&&second_attack);
    SDL_DetachVirtualJoystick(one);e.type=SDL_EVENT_GAMEPAD_REMOVED;e.gdevice.which=one;input_handle_event(&e);
    assert(!pad&&pad2&&SDL_GetGamepadID(pad2)==two);refresh_pads();assert(!pad||SDL_GetGamepadID(pad)!=two);
    SDL_DetachVirtualJoystick(two);e.gdevice.which=two;input_handle_event(&e);
    assert(!pad2&&!second_x&&!second_attack);
    active=false;key(SDL_SCANCODE_KP_6,true);assert(key_rx==32767&&!second_x);
    if(pad)SDL_CloseGamepad(pad);SDL_Quit();
    puts("Co-op keyboard isolation, two SDL pads, focus and disconnect checks passed");
}
'''
with tempfile.TemporaryDirectory() as temporary:
    folder = Path(temporary)
    source = folder/'input.c'
    source.write_text(HARNESS)
    exe = folder/('input.exe' if os.name == 'nt' else 'input')
    sdl = shlex.split(os.environ.get('SDL_FLAGS') or subprocess.check_output(['pkg-config', '--cflags', '--libs', 'sdl3'], text=True))
    gc = '-Wl,-dead_strip' if sys.platform == 'darwin' else '-Wl,--gc-sections'
    subprocess.run(shlex.split(os.environ.get('CC', 'clang')) + ['-std=gnu11', '-O1', '-ffunction-sections',
                   '-fdata-sections', '-Wno-unused-function', '-I'+str(ROOT/'runtime'), str(source),
                   gc, *sdl, '-lm', '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
