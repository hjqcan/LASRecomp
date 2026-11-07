// Android: the game files folder through the system's own folder picker and storage framework (the activity,
// android/app/src/main/java/com/lasrecomp/LASRecompActivity.java). A picked folder is a content:// URI, not a path.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Opens the picker; done(uri) is called on another thread with the folder, or NULL when nothing was picked.
bool android_pick_folder(void (*done)(const char *uri));
// Copies exefs and romfs from the picked folder (found as launcher.mm's find_game does) to dst/exefs and dst/romfs.
// 0 copied, 1 not found, 2 game packages instead of files, 3 copy failed, -1 cancelled.
int android_import_game(const char *uri, const char *dst, void (*progress)(uint64_t done, uint64_t total),
                        bool (*cancelled)(void));

#ifdef __cplusplus
}
#endif
