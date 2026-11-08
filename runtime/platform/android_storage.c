// Android: the game files folder (android_storage.h) - JNI glue to the activity's picker and copy.
#include "android_storage.h"

#include <SDL3/SDL.h>
#include <jni.h>
#include <stdio.h>

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
