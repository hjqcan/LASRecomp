// time:u / time:a / time:s
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "../hle.h"
#include "../ipc.h"
#include "../research.h"

static const uint8_t clock_uuid[16] = {0x4c, 0x41, 0x52, 0x65, 0x63, 0x6f, 0x6d, 0x70, 1, 2, 3, 4, 5, 6, 7, 8};

static int64_t steady_seconds(void) { return (int64_t)(rt_ticks() / 19200000ull); }
static int64_t unix_now(void) { return (int64_t)time(NULL); }

static void write_system_context(uint8_t *p) {  // SystemClockContext (0x20)
    int64_t steady = steady_seconds();
    *(int64_t *)p = unix_now() - steady;
    *(int64_t *)(p + 8) = steady;
    memcpy(p + 0x10, clock_uuid, 16);
}

static void time_shmem_map(KSharedMem *s) {
    uint8_t *m = (uint8_t *)(uintptr_t)s->guest_addr;
    LOG("[time] shared memory mapped at %llx\n", (unsigned long long)s->guest_addr);
    // 0x00: LockFreeAtomicType<SteadyClockContext{u64 internal_offset, uuid}>
    *(uint32_t *)m = 0;
    *(uint64_t *)(m + 0x08) = 0;
    memcpy(m + 0x10, clock_uuid, 16);
    // 0x38 local system clock, 0x80 network system clock
    *(uint32_t *)(m + 0x38) = 0;
    write_system_context(m + 0x40);
    *(uint32_t *)(m + 0x80) = 0;
    write_system_context(m + 0x88);
    // 0xC8 automatic correction enabled
    *(uint32_t *)(m + 0xC8) = 0;
    m[0xCC] = 0;
}

static void system_clock(Service *s, IpcReq *req, IpcResp *resp) {
    (void)s;
    switch (req->cmd) {
    case 0: OUT(int64_t, 0) = unix_now(); resp_size(resp, 8); break;  // GetCurrentTime
    case 2: write_system_context(resp->data); resp_size(resp, 0x20); break;  // GetSystemClockContext
    default: LOG("[time] ISystemClock cmd %u (ok)\n", req->cmd);
    }
}

static void steady_clock(Service *s, IpcReq *req, IpcResp *resp) {
    (void)s;
    switch (req->cmd) {
    case 0:  // GetCurrentTimePoint
        OUT(int64_t, 0) = steady_seconds();
        memcpy(resp->data + 8, clock_uuid, 16);
        resp_size(resp, 0x18);
        break;
    default: OUT(int64_t, 0) = 0; resp_size(resp, 8); LOG("[time] ISteadyClock cmd %u (ok)\n", req->cmd);
    }
}

// Rules are opaque SDK objects. Our service supports the host's local rules and
// UTC; both conversion directions interpret the same initialized 0x4000 bytes.
// Do not change the process TZ variable here: guest threads convert concurrently.
#define TIME_RULE_SIZE 0x4000
#define TIME_INVALID 0x1c01u
static const char local_location[] = "Host/Local";
static const uint8_t rule_magic[12] = {'L','A','R','T','I','M','E',1,0,0,0,0};

static int rule_mode(IpcReq *req) {
    IpcBuf *b = in_buf(req, 0);
    if (!b || !b->addr || b->size < TIME_RULE_SIZE) return -1;
    const uint8_t *p = (const uint8_t *)(uintptr_t)b->addr;
    if (memcmp(p, rule_magic, sizeof rule_magic) || p[12] > 1) return -1;
    return p[12]; // 0 = local, 1 = UTC
}

static time_t as_utc(struct tm *tm) {
#ifdef _WIN32
    return _mkgmtime(tm);
#else
    return timegm(tm);
#endif
}

static void to_calendar(int64_t t, IpcResp *resp, int utc) {
    time_t tt = (time_t)t;
    struct tm tm;
    if (!(utc ? gmtime_r(&tt, &tm) : localtime_r(&tt, &tm))) { resp->result = TIME_INVALID; return; }
    uint8_t *o = resp->data;
    memset(o, 0, 0x20);
    *(uint16_t *)o = (uint16_t)(tm.tm_year + 1900);
    o[2] = (uint8_t)(tm.tm_mon + 1); o[3] = (uint8_t)tm.tm_mday;
    o[4] = (uint8_t)tm.tm_hour; o[5] = (uint8_t)tm.tm_min; o[6] = (uint8_t)tm.tm_sec;
    *(uint32_t *)(o + 8) = (uint32_t)tm.tm_wday;
    *(uint32_t *)(o + 12) = (uint32_t)tm.tm_yday;
#ifdef _WIN32
    struct tm copy = tm;
    long gmtoff = (long)difftime(as_utc(&copy), tt);
    // Windows timezone display names can exceed the guest's eight-byte field.
    snprintf((char *)o + 16, 8, "%s", utc ? "UTC" : "LCL");
#else
    long gmtoff = tm.tm_gmtoff;
    snprintf((char *)o + 16, 8, "%s", utc ? "UTC" : tm.tm_zone ? tm.tm_zone : "LCL");
#endif
    *(uint32_t *)(o + 24) = (uint32_t)tm.tm_isdst;
    *(int32_t *)(o + 28) = utc ? 0 : (int32_t)gmtoff;
    resp_size(resp, 0x20);
}

static int same_calendar(const struct tm *a, const struct tm *b) {
    return a->tm_year == b->tm_year && a->tm_mon == b->tm_mon && a->tm_mday == b->tm_mday &&
           a->tm_hour == b->tm_hour && a->tm_min == b->tm_min && a->tm_sec == b->tm_sec;
}

static void to_posix(IpcReq *req, IpcResp *resp, int utc) {
    if (req->data_size < 8) { resp->result = TIME_INVALID; return; }
    const uint8_t *p = req->data;
    uint16_t year; memcpy(&year, p, 2);
    struct tm wanted = {.tm_year=year-1900, .tm_mon=p[2]-1, .tm_mday=p[3],
                        .tm_hour=p[4], .tm_min=p[5], .tm_sec=p[6]};
    IpcBuf *out = out_buf(req, 0);
    if (!out || !out->addr || out->size < 8) { resp->result = TIME_INVALID; return; }
    int64_t times[2]; unsigned count = 0;
    // Try each DST interpretation and round-trip it. A skipped spring hour has
    // no result; an ambiguous autumn hour can have two distinct results.
    for (int dst = 0; dst <= (utc ? 0 : 1); dst++) {
        struct tm trial = wanted, back;
        trial.tm_isdst = dst;
        time_t t = utc ? as_utc(&trial) : mktime(&trial);
        if (!(utc ? gmtime_r(&t, &back) : localtime_r(&t, &back)) || !same_calendar(&wanted, &back)) continue;
        if (!count || times[0] != (int64_t)t) times[count++] = (int64_t)t;
    }
    if (count == 2 && times[0] > times[1]) { int64_t swap=times[0]; times[0]=times[1]; times[1]=swap; }
    if (count > out->size / 8) { resp->result = TIME_INVALID; return; }
    if (count) memcpy((void *)(uintptr_t)out->addr, times, count * 8);
    OUT(uint32_t, 0) = count; resp_size(resp, 4);
}

static void timezone_service(Service *s, IpcReq *req, IpcResp *resp) {
    (void)s;
    switch (req->cmd) {
    case 0:
        memset(resp->data, 0, 0x24); memcpy(resp->data, local_location, sizeof local_location);
        resp_size(resp, 0x24); break;
    case 4: { // LoadTimeZoneRule
        IpcBuf *out = out_buf(req, 0);
        if (req->data_size < 0x24 || !out || !out->addr || out->size < TIME_RULE_SIZE) {
            resp->result = TIME_INVALID; break;
        }
        char name[0x25]; memcpy(name, req->data, 0x24); name[0x24] = 0;
        int utc = !strcmp(name, "UTC") || !strcmp(name, "Etc/UTC");
        if (!utc && strcmp(name, local_location)) { resp->result = TIME_INVALID; break; }
        uint8_t *p = (uint8_t *)(uintptr_t)out->addr;
        memset(p, 0, TIME_RULE_SIZE); memcpy(p, rule_magic, sizeof rule_magic); p[12] = utc;
        break;
    }
    case 100: case 101: {
        int mode = req->cmd == 100 ? rule_mode(req) : 0;
        if (mode < 0 || req->data_size < 8) resp->result = TIME_INVALID;
        else to_calendar(IN(int64_t, 0), resp, mode);
        break;
    }
    case 201: case 202: { // ToPosixTime(rule), ToPosixTimeWithMyRule
        int mode = req->cmd == 201 ? rule_mode(req) : 0;
        if (mode < 0) resp->result = TIME_INVALID;
        else to_posix(req, resp, mode);
        break;
    }
    default: LOG("[time] ITimeZoneService cmd %u (ok)\n", req->cmd); resp_size(resp, 0x10);
    }
}

static uint32_t g_shmem_handle;
RT_ON_RESET(time_reset) { g_shmem_handle = 0; }

static void static_service(Service *s, IpcReq *req, IpcResp *resp) {
    (void)s;
    switch (req->cmd) {
    case 0: case 1: case 4: case 5:
        ipc_return_object(resp, service_new("ISystemClock", system_clock, NULL));
        break;
    case 2: ipc_return_object(resp, service_new("ISteadyClock", steady_clock, NULL)); break;
    case 3: ipc_return_object(resp, service_new("ITimeZoneService", timezone_service, NULL)); break;
    case 20: {  // GetSharedMemoryNativeHandle
        if (!g_shmem_handle) kernel_create_shared_memory(0x1000, time_shmem_map, &g_shmem_handle);
        resp->copy_handles[resp->ncopy++] = g_shmem_handle;
        break;
    }
    case 100: case 101: OUT(uint8_t, 0) = 0; resp_size(resp, 1); break;
    default: LOG("[time] IStaticService cmd %u (ok)\n", req->cmd); resp_size(resp, 0x10);
    }
}

SERVICE("time:u", time_u) { static_service(s, req, resp); }
SERVICE("time:a", time_a) { static_service(s, req, resp); }
SERVICE("time:s", time_s) { static_service(s, req, resp); }

// nn::time::StandardUserSystemClock::GetCurrentTime(PosixTime*): answered directly from the host clock. The SDK's
// shared-memory path aborts (svcBreak via nn::diag::Abort, result 0xcc74) if the local clock's source id ever
// differs from the steady clock's; that happened once in the room sweep on an autosave (entering Eagle's Tower) and
// could not be reproduced, so the date stamp no longer depends on it.
HLE_FUNC("_ZN2nn4time23StandardUserSystemClock14GetCurrentTimeEPNS0_9PosixTimeE", time_std_user_now) {
    static int n;
    if (n++ < 3) LOG("[time] StandardUserSystemClock::GetCurrentTime (host clock)\n");
    if (c->x[0]) {
        int64_t value=unix_now();
        *(int64_t *)(uintptr_t)c->x[0] = value;
        research_external(RESEARCH_EXTERNAL_CLOCK,c->x[30],kernel_current_thread_handle(),0,(uint64_t)value);
    }
    c->x[0] = 0;  // ResultSuccess
}
