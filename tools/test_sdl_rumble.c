// Uses a virtual gamepad: never vibrates a physical controller.
#include <SDL3/SDL.h>
#include <stdio.h>
#undef NDEBUG
#include <assert.h>
static Uint16 low, high;
static bool SDLCALL output(void *unused, Uint16 lo, Uint16 hi) {
    (void)unused; low=lo; high=hi; return true;
}
int main(void) {
    assert(SDL_Init(SDL_INIT_GAMEPAD));
    int count=0; SDL_JoystickID *ids=SDL_GetGamepads(&count);
    for (int i=0;i<count;i++) {
        SDL_Gamepad *pad=SDL_OpenGamepad(ids[i]);
        if (pad) {
            printf("Physical device: %s; rumble capability: %s\n", SDL_GetGamepadName(pad),
                SDL_GetBooleanProperty(SDL_GetGamepadProperties(pad), SDL_PROP_GAMEPAD_CAP_RUMBLE_BOOLEAN, false) ? "yes" : "no");
            SDL_CloseGamepad(pad);
        }
    }
    SDL_free(ids);
    SDL_VirtualJoystickDesc desc; SDL_INIT_INTERFACE(&desc);
    desc.type=SDL_JOYSTICK_TYPE_GAMEPAD; desc.naxes=6; desc.nbuttons=15;
    desc.axis_mask=0x3f; desc.button_mask=0x7fff;
    desc.name="LAS rumble regression (virtual)"; desc.Rumble=output;
    SDL_JoystickID id=SDL_AttachVirtualJoystick(&desc); assert(id);
    SDL_Gamepad *pad=SDL_OpenGamepad(id); assert(pad);
    assert(SDL_RumbleGamepad(pad, 32768, 16384, 50));
    assert(low==32768 && high==16384);
    SDL_Delay(70); SDL_UpdateJoysticks(); assert(low==0 && high==0);
    assert(SDL_RumbleGamepad(pad, 65535, 65535, 250));
    assert(SDL_RumbleGamepad(pad, 0, 0, 0)); assert(low==0 && high==0);
    SDL_CloseGamepad(pad); assert(SDL_DetachVirtualJoystick(id)); SDL_Quit();
    puts("PASS: installed SDL3 DLL accepts rumble amplitudes, expires duration, and stops output");
}
