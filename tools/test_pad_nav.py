"""Run production menu navigation with both confirm conventions and live changes."""
from pathlib import Path
import os
import shlex
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = r'''
#include <cassert>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include "ui/pad_nav.mm"
Settings g_settings = {};
int input_menu_family(uint32_t which) { return which % 3; }
const char *bind_pad_label(int button, int family) {
    static const char *south[] = {"B", "A", "Cross"}, *east[] = {"A", "B", "Circle"};
    return button == SDL_GAMEPAD_BUTTON_SOUTH ? south[family] : east[family];
}
int main() {
    for (int mode : {MENU_CONFIRM_GAME, MENU_CONFIRM_CONTROLLER}) {
        g_settings.menu_confirm = mode;
        for (unsigned family = 0; family < 3; family++) {
            bool south = mode == MENU_CONFIRM_CONTROLLER && family != 0;
            auto confirm = south ? SDL_GAMEPAD_BUTTON_SOUTH : SDL_GAMEPAD_BUTTON_EAST;
            auto back = south ? SDL_GAMEPAD_BUTTON_EAST : SDL_GAMEPAD_BUTTON_SOUTH;
            assert(ui_pad_key(family, confirm, true) == ImGuiKey_GamepadFaceDown);
            assert(!strcmp(ui_pad_confirm_label(), bind_pad_label(confirm, family)));
            assert(!strcmp(ui_pad_back_label(), bind_pad_label(back, family)));
            assert(ui_pad_key(family, confirm, false) == ImGuiKey_GamepadFaceDown);
            assert(ui_pad_key(family, back, true) == ImGuiKey_GamepadFaceRight);
            assert(ui_pad_key(family, back, false) == ImGuiKey_GamepadFaceRight);
        }
    }
    g_settings.menu_confirm = MENU_CONFIRM_GAME;
    assert(ui_pad_key(1, SDL_GAMEPAD_BUTTON_EAST, true) == ImGuiKey_GamepadFaceDown);
    g_settings.menu_confirm = MENU_CONFIRM_CONTROLLER;
    assert(ui_pad_key(1, SDL_GAMEPAD_BUTTON_EAST, false) == ImGuiKey_GamepadFaceDown);
    assert(ui_pad_key(1, SDL_GAMEPAD_BUTTON_EAST, true) == ImGuiKey_GamepadFaceRight);
    assert(ui_pad_key(1, SDL_GAMEPAD_BUTTON_EAST, false) == ImGuiKey_GamepadFaceRight);
    assert(ui_pad_key(1, SDL_GAMEPAD_BUTTON_DPAD_UP, true) == ImGuiKey_GamepadDpadUp);
    assert(ui_pad_key(1, SDL_GAMEPAD_BUTTON_COUNT, true) == ImGuiKey_None);
    puts("PASS: both confirm modes, three controller families, matching labels and release after a setting change");
}
'''
sdl = shlex.split(os.environ.get('SDL_FLAGS') or subprocess.check_output(['pkg-config', '--cflags', 'sdl3'], text=True))
with tempfile.TemporaryDirectory() as directory:
    folder = Path(directory)
    code = folder / 'pad.cpp'; code.write_text(source)
    exe = folder / ('pad.exe' if os.name == 'nt' else 'pad')
    subprocess.run([*shlex.split(os.environ.get('CXX', 'clang++')), '-std=c++17', '-Iruntime', '-Ithird_party/imgui',
                    str(code), *sdl, '-o', str(exe)], cwd=root, check=True)
    subprocess.run([str(exe)], check=True)
