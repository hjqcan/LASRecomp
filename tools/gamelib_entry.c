// Entry of the game code library (runtime/gamelib.h), compiled into it with the generated code: the function
// tables, found by name when the app loads the library. Everything else in it stays hidden.
#include "gamelib.h"

#include "gamelib_modules.inc"

GAMELIB_EXPORT const GameLibTables *lasgame_tables(void) {
    static const GameLibTables t = {GAMELIB_ABI, GAMELIB_ID, GAMELIB_GAME_ID, GAMELIB_GAME_VERSION,
                                    GAMELIB_ADAPTER_REVISION,
                                    sizeof generated_modules / sizeof generated_modules[0], generated_modules, GAMELIB_ADAPTER_ID};
    return &t;
}
