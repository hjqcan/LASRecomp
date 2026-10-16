// Builds the game code library (runtime/gamelib.h) on the player's machine, from their own game files: the generator
// (tools/gen.py; shipped frozen as lasgen/) turns exefs/main and exefs/sdk into C, the bundled compiler (zig/) builds
// that into <game_dir>/lasgame.*. The compiled pieces are kept in <game_dir>/.gamebuild: an update compiles only the
// pieces whose generated C changed.
#pragma once
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    // now and then, from the calling thread: how far the whole build is (0..1) and what it's doing
    void (*progress)(double fraction, const char *step);
    int jobs;  // compiler processes at once (>= 1)
    bool (*cancelled)(void);  // optional, polled on this thread; completed objects stay reusable
} GameBuildOpts;

// Builds it (a few minutes). false: why says what went wrong (details in the log).
bool gamebuild_run(const char *game_dir, const GameBuildOpts *opts, char *why, size_t why_size);
// The generator and the compiler are there (shipped builds; a source checkout with tools/venv and clang).
bool gamebuild_available(void);
// Import a base NSP/NSZ/XCI/XCZ and optional update using player-selected local keys (never persisted).
bool gameimport_run(const char *base, const char *update, const char *keys, const char *title_keys, const char *game_dir,
                    const char *expected_version,
                    void (*progress)(const char *), char *why, size_t why_size);
typedef struct { char format[8], version[16], metadata[48]; } GameArchiveInfo;
bool gameimport_inspect(const char *base, const char *update, const char *keys, const char *title_keys,
                        const char *game_dir, GameArchiveInfo info[2], char *why, size_t why_size);

#ifdef __cplusplus
}
#endif
