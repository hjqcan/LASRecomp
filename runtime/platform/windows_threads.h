// Windows guest threads reserve room for recompiled recursion without committing the whole stack. The CRT entry
// point keeps thread-local cleanup; the returned handle remains owned by the caller until the thread is joined.
#pragma once
#ifdef _WIN32
#include <windows.h>
#include <process.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdbool.h>

typedef struct { void *(*entry)(void *); void *argument; } GuestThreadStart;
static unsigned __stdcall guest_thread_trampoline(void *argument) {
    GuestThreadStart start = *(GuestThreadStart *)argument;
    free(argument);
    start.entry(start.argument);
    return 0;
}

// winpthreads passes its requested stack size as a Windows commit size. Guest
// recursion needs a large address reservation, not 256 MB committed per thread.
// The CRT entry point retains thread-local cleanup while Windows grows the stack.
static uintptr_t guest_thread_create(void *(*entry)(void *), void *argument, unsigned reserve) {
    GuestThreadStart *start = malloc(sizeof *start);
    if (!start) return 0;
    *start = (GuestThreadStart){entry, argument};
    uintptr_t handle = _beginthreadex(NULL, reserve, guest_thread_trampoline, start,
                                    STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
    if (!handle) free(start);
    return handle;
}

static bool guest_thread_join(uintptr_t handle) {
    if (WaitForSingleObject((HANDLE)handle, INFINITE) != WAIT_OBJECT_0) return false;
    return CloseHandle((HANDLE)handle) != 0;
}
#endif
