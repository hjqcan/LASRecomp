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

// The system's file picker, several files at once (Import fonts). done(NULL, files, -1) on another thread, as SDL's
// file dialogs call back: local copies of the picked .ttf / .bfttf files (others by name only), an empty list when
// cancelled, NULL when no picker could open.
void android_pick_files(void (*done)(void *userdata, const char *const *files, int filter));

// Calls the activity's method of that name, without arguments (pickDriver, useSystemDriver, restartApp). False: none.
bool android_call(const char *method);
// done(result) on another thread when a driver package was picked (pickDriver) or the system's chosen again
// (useSystemDriver): the package's name, "-" for the system's, "" when the file isn't a driver.
void android_on_driver(void (*done)(const char *result));
// A Vulkan driver error that stops the app: the activity's dialog (another driver package, or the phone's own, then a
// restart). Blocks until it closes; false when it couldn't be shown.
bool android_driver_error_dialog(const char *text);

#ifdef __cplusplus
}
#endif
