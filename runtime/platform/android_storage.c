// Android: the game files folder (android_storage.h) - JNI glue to the activity's picker and copy.
#include "android_storage.h"

#include <SDL3/SDL.h>
#include <jni.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#include "../platform.h"

static void (*g_done)(const char *);
static void (*g_progress)(uint64_t, uint64_t);
static bool (*g_cancelled)(void);

// (activity, method name, signature): calls an instance method of the activity
static jmethodID activity_method(JNIEnv *env, jobject act, const char *name, const char *sig) {
    jclass c = (*env)->GetObjectClass(env, act);
    jmethodID m = c ? (*env)->GetMethodID(env, c, name, sig) : NULL;
    if (c) (*env)->DeleteLocalRef(env, c);
    if (!m) (*env)->ExceptionClear(env);
    return m;
}

bool android_pick_folder(void (*done)(const char *uri)) {
    JNIEnv *env = (JNIEnv *)SDL_GetAndroidJNIEnv();
    jobject act = (jobject)SDL_GetAndroidActivity();
    if (!env || !act) return false;
    jmethodID m = activity_method(env, act, "pickGameFolder", "()V");
    bool ok = m != NULL;
    if (ok) {
        g_done = done;
        (*env)->CallVoidMethod(env, act, m);
        if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env), ok = false;
    }
    (*env)->DeleteLocalRef(env, act);
    return ok;
}

int android_import_game(const char *uri, const char *dst, void (*progress)(uint64_t, uint64_t), bool (*cancelled)(void)) {
    JNIEnv *env = (JNIEnv *)SDL_GetAndroidJNIEnv();  // (attaches this thread: the setup's own)
    jobject act = (jobject)SDL_GetAndroidActivity();
    if (!env || !act) return 3;
    jmethodID m = activity_method(env, act, "importGameFiles", "(Ljava/lang/String;Ljava/lang/String;)I");
    int r = 3;
    if (m) {
        g_progress = progress, g_cancelled = cancelled;
        jstring ju = (*env)->NewStringUTF(env, uri), jd = (*env)->NewStringUTF(env, dst);
        r = (*env)->CallIntMethod(env, act, m, ju, jd);
        if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env), r = 3;
        (*env)->DeleteLocalRef(env, ju);
        (*env)->DeleteLocalRef(env, jd);
        g_progress = NULL, g_cancelled = NULL;
    }
    (*env)->DeleteLocalRef(env, act);
    return r;
}

bool android_call(const char *method) {
    JNIEnv *env = (JNIEnv *)SDL_GetAndroidJNIEnv();
    jobject act = (jobject)SDL_GetAndroidActivity();
    if (!env || !act) return false;
    jmethodID m = activity_method(env, act, method, "()V");
    bool ok = m != NULL;
    if (ok) {
        (*env)->CallVoidMethod(env, act, m);
        if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env), ok = false;
    }
    (*env)->DeleteLocalRef(env, act);
    return ok;
}

bool android_driver_error_dialog(const char *text) {
    JNIEnv *env = (JNIEnv *)SDL_GetAndroidJNIEnv();
    jobject act = (jobject)SDL_GetAndroidActivity();
    if (!env || !act) return false;
    jclass c = (*env)->GetObjectClass(env, act);
    jmethodID m = c ? (*env)->GetStaticMethodID(env, c, "driverErrorDialog", "(Ljava/lang/String;)Z") : NULL;
    bool shown = false;
    if (m) {
        jstring t = (*env)->NewStringUTF(env, text);
        shown = (*env)->CallStaticBooleanMethod(env, c, m, t);
        if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env), shown = false;
        (*env)->DeleteLocalRef(env, t);
    } else (*env)->ExceptionClear(env);
    if (c) (*env)->DeleteLocalRef(env, c);
    (*env)->DeleteLocalRef(env, act);
    return shown;
}

static void (*g_files_done)(void *, const char *const *, int);
void android_pick_files(void (*done)(void *, const char *const *, int)) {
    g_files_done = done;
    if (!android_call("pickFiles")) done(NULL, NULL, -1);
}

JNIEXPORT void JNICALL Java_com_lasrecomp_LASRecompActivity_nativeFilesPicked(JNIEnv *env, jclass cls, jobjectArray paths) {
    (void)cls;
    void (*done)(void *, const char *const *, int) = g_files_done;
    if (!done) return;
    if (!paths) { done(NULL, NULL, -1); return; }
    jsize n = (*env)->GetArrayLength(env, paths);
    const char **files = calloc((size_t)n + 1, sizeof *files);
    jstring *js = calloc((size_t)n + 1, sizeof *js);
    if (!files || !js) { free(files), free(js); done(NULL, NULL, -1); return; }
    int k = 0;
    for (jsize i = 0; i < n; i++) {
        js[k] = (jstring)(*env)->GetObjectArrayElement(env, paths, i);
        files[k] = js[k] ? (*env)->GetStringUTFChars(env, js[k], NULL) : NULL;
        if (files[k]) k++;
        else if (js[k]) (*env)->DeleteLocalRef(env, js[k]);
    }
    files[k] = NULL;
    done(NULL, files, -1);
    for (int i = 0; i < k; i++) (*env)->ReleaseStringUTFChars(env, js[i], files[i]), (*env)->DeleteLocalRef(env, js[i]);
    free(files), free(js);
}

static void (*g_driver_done)(const char *);
void android_on_driver(void (*done)(const char *)) { g_driver_done = done; }

JNIEXPORT void JNICALL Java_com_lasrecomp_LASRecompActivity_nativeDriverPicked(JNIEnv *env, jclass cls, jstring result) {
    (void)cls;
    void (*done)(const char *) = g_driver_done;
    if (!done) return;
    const char *r = result ? (*env)->GetStringUTFChars(env, result, NULL) : NULL;
    done(r ? r : "");
    if (r) (*env)->ReleaseStringUTFChars(env, result, r);
}

JNIEXPORT void JNICALL Java_com_lasrecomp_LASRecompActivity_nativeFolderPicked(JNIEnv *env, jclass cls, jstring uri) {
    (void)cls;
    void (*done)(const char *) = g_done;
    if (!done) return;
    const char *u = uri ? (*env)->GetStringUTFChars(env, uri, NULL) : NULL;
    done(u);
    if (u) (*env)->ReleaseStringUTFChars(env, uri, u);
}

JNIEXPORT void JNICALL Java_com_lasrecomp_LASRecompActivity_nativeImportProgress(JNIEnv *env, jclass cls, jlong done,
                                                                                 jlong total) {
    (void)env, (void)cls;
    if (g_progress) g_progress((uint64_t)done, (uint64_t)total);
}

JNIEXPORT jboolean JNICALL Java_com_lasrecomp_LASRecompActivity_nativeImportCancelled(JNIEnv *env, jclass cls) {
    (void)env, (void)cls;
    return g_cancelled && g_cancelled() ? JNI_TRUE : JNI_FALSE;
}

// ---- the touch layer (platform.h)
// (asked while the library loads, before JNI is up: the activity sets this first)
bool plat_touch_tablet(void) {
    const char *e = getenv("LARECOMP_ANDROID_TABLET");
    return e && atoi(e) != 0;
}
// (the launcher icon's shortcuts: none on Android yet)
bool plat_touch_play_request(char *seed, size_t n) { (void)seed, (void)n; return false; }
void plat_touch_shortcuts_sync(void) {}

// The system's text prompt: the activity's dialog, its answer polled. Text as UTF-8 bytes (JNI's own strings are
// modified UTF-8: an emoji in a name would not survive).
static pthread_mutex_t g_prompt_lock = PTHREAD_MUTEX_INITIALIZER;
static char g_prompt_text[256];
static int g_prompt_result;  // 1 OK, -1 cancelled (taken once)
static int g_prompt_serial;  // (a prompt cancelled from our side: its late answer is dropped)

static void prompt_answer(int serial, int result, const char *text, size_t len) {
    pthread_mutex_lock(&g_prompt_lock);
    if (serial == g_prompt_serial) {
        g_prompt_result = result;
        snprintf(g_prompt_text, sizeof g_prompt_text, "%.*s", (int)len, text ? text : "");
    }
    pthread_mutex_unlock(&g_prompt_lock);
}

void plat_touch_prompt(const char *title, const char *hint, const char *initial, int max_len, const char *ok, const char *cancel) {
    pthread_mutex_lock(&g_prompt_lock);
    int serial = ++g_prompt_serial;
    g_prompt_result = 0;
    pthread_mutex_unlock(&g_prompt_lock);
    JNIEnv *env = (JNIEnv *)SDL_GetAndroidJNIEnv();
    jobject act = (jobject)SDL_GetAndroidActivity();
    jmethodID m = env && act ? activity_method(env, act, "showPrompt",
        "(ILjava/lang/String;Ljava/lang/String;[BILjava/lang/String;Ljava/lang/String;)V") : NULL;
    if (m) {
        jstring t = (*env)->NewStringUTF(env, title ? title : ""), h = (*env)->NewStringUTF(env, hint ? hint : "");
        jstring o = (*env)->NewStringUTF(env, ok ? ok : "OK"), c = (*env)->NewStringUTF(env, cancel ? cancel : "Cancel");
        jsize n = initial ? (jsize)strlen(initial) : 0;
        jbyteArray v = (*env)->NewByteArray(env, n);
        if (n) (*env)->SetByteArrayRegion(env, v, 0, n, (const jbyte *)initial);
        (*env)->CallVoidMethod(env, act, m, serial, t, h, v, max_len, o, c);
        if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env), m = NULL;
        (*env)->DeleteLocalRef(env, t), (*env)->DeleteLocalRef(env, h), (*env)->DeleteLocalRef(env, v);
        (*env)->DeleteLocalRef(env, o), (*env)->DeleteLocalRef(env, c);
    }
    if (act) (*env)->DeleteLocalRef(env, act);
    if (!m) prompt_answer(serial, -1, NULL, 0);  // (no dialog: the field as it was)
}

int plat_touch_prompt_poll(char *out, size_t n) {
    pthread_mutex_lock(&g_prompt_lock);
    int r = g_prompt_result;
    if (r) { snprintf(out, n, "%s", g_prompt_text); g_prompt_result = 0; }
    pthread_mutex_unlock(&g_prompt_lock);
    return r;
}

void plat_touch_prompt_cancel(void) {
    pthread_mutex_lock(&g_prompt_lock);
    g_prompt_serial++, g_prompt_result = 0;
    pthread_mutex_unlock(&g_prompt_lock);
    android_call("closePrompt");
}

JNIEXPORT void JNICALL Java_com_lasrecomp_LASRecompActivity_nativePromptDone(JNIEnv *env, jclass cls, jint serial, jbyteArray text) {
    (void)cls;
    if (!text) { prompt_answer(serial, -1, NULL, 0); return; }
    jsize n = (*env)->GetArrayLength(env, text);
    jbyte *b = (*env)->GetByteArrayElements(env, text, NULL);
    prompt_answer(serial, 1, (const char *)b, (size_t)n);
    (*env)->ReleaseByteArrayElements(env, text, b, JNI_ABORT);
}
