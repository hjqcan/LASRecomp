// Instruction-semantics regression test for the Maxwell shader translator (runtime/nvn/maxwell/translate.c).
//
// Each case is a tiny compute program: MOV32I loads the inputs, then the instruction under test. Its MSL body
// (the GLSL is the same text after glsl_rewrite) is written into a C++ file with stand-ins for the few MSL
// built-ins it uses, compiled and run on the CPU, and the registers / predicates are compared with values worked
// out by hand from the instruction's definition. tools/test_translate_ops.sh drives it.
//
// --glsl <dir>: instead, writes each case's Vulkan GLSL (key.glsl = 1) as a vertex, fragment and compute shader
// (<dir>/<case>_<stage>.{vert,frag,comp}, the case names in <dir>/names.txt) for tools/test_spirv_val.sh, which compiles
// them (glslc, Vulkan 1.3) and runs spirv-val on each. (The shipped shader list holds only hashes of the game's
// shaders - their code is in the player's game files - so CI validates these synthetic programs instead.)
//
// usage: test_translate_ops <out.cpp>
//        test_translate_ops --glsl <dir>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../runtime/nvn/maxwell/shader.h"

#define PT_GUARD (7ull << 16)  // @PT
#define NOP 0x50b0000000070f00ull
#define EXIT 0xe30000000007000full

static uint64_t mov32i(int d, uint32_t v) { return 0x0100000000000000ull | (uint64_t)v << 20 | 0xFull << 12 | PT_GUARD | (uint64_t)d; }

// I2I Rd, Rb (register form). Sizes: 0 8-bit, 1 16-bit, 2 32-bit.
static uint64_t i2i(int d, int b, int dsz, bool dsg, int ssz, bool ssg, bool sat, bool abs_, bool neg) {
    return 0x5ce0000000000000ull | PT_GUARD | (uint64_t)d | (uint64_t)dsz << 8 | (uint64_t)ssz << 10 | (uint64_t)dsg << 12 |
           (uint64_t)ssg << 13 | (uint64_t)b << 20 | (uint64_t)neg << 45 | (uint64_t)abs_ << 49 | (uint64_t)sat << 50;
}

// SHF.{L,R} Rd, Ra (low), shift, Rc (high). width: 0 32-bit, 2 .U64, 3 .S64. shift_reg < 0: immediate `imm`.
static uint64_t shf(bool right, int d, int a, int shift_reg, uint32_t imm, int c, int width, bool wrap) {
    uint64_t w = shift_reg >= 0 ? (right ? 0x5cf8000000000000ull : 0x5bf8000000000000ull) | (uint64_t)shift_reg << 20
                                : (right ? 0x38f8000000000000ull : 0x36f8000000000000ull) | (uint64_t)(imm & 63) << 20;
    return w | PT_GUARD | (uint64_t)d | (uint64_t)a << 8 | (uint64_t)width << 37 | (uint64_t)c << 39 | (uint64_t)wrap << 50;
}

// HSETP2 Pa, Pb, Ra, Rb (register form, both halves), compare `cmp`, combined with predicate (p, pneg) by `bop`
static uint64_t hsetp2(int pa, int pb, int a, int b, int cmp, int p, bool pneg, int bop, bool hand) {
    return 0x5d20000000000000ull | PT_GUARD | (uint64_t)pb | (uint64_t)pa << 3 | (uint64_t)a << 8 | (uint64_t)b << 20 |
           (uint64_t)cmp << 35 | (uint64_t)p << 39 | (uint64_t)pneg << 42 | (uint64_t)bop << 45 | (uint64_t)hand << 49;
}

// LDC Rd, c[idx][Ra + off]; size 4 is 32-bit, 5 is .64 (Rd, Rd+1)
static uint64_t ldc(int d, int a, int idx, uint32_t off, int size) {
    return 0xef90000000000000ull | PT_GUARD | (uint64_t)d | (uint64_t)a << 8 | (uint64_t)(off & 0xFFFF) << 20 |
           (uint64_t)idx << 36 | (uint64_t)size << 48;
}

typedef struct {
    const char *name;
    uint64_t insn[6];  // 0-terminated
    const char *var;   // the register / predicate to check
    uint32_t expect;
} Case;

enum { LT = 1, GT = 4, AND = 0, OR = 1 };
#define LO 0x89ABCDEFu
#define HI 0x01234567u

static const Case *cases(int *n) {
    static Case c[64];
    int k = 0;
#define CASE(nm, var_, exp_, ...) c[k++] = (Case){nm, {__VA_ARGS__, 0}, var_, exp_}
    // I2I: .SAT compares in the source's signedness and saturates 32-bit destinations too
    CASE("I2I.U8.U32.SAT 0xffffffff", "r0", 0xFFu, mov32i(1, 0xFFFFFFFFu), i2i(0, 1, 0, false, 2, false, true, false, false));
    CASE("I2I.U16.U32.SAT 0x12345", "r0", 0xFFFFu, mov32i(1, 0x12345u), i2i(0, 1, 1, false, 2, false, true, false, false));
    CASE("I2I.S16.U32.SAT 0x80000000", "r0", 0x7FFFu, mov32i(1, 0x80000000u), i2i(0, 1, 1, true, 2, false, true, false, false));
    CASE("I2I.S32.U32.SAT 0x80000000", "r0", 0x7FFFFFFFu, mov32i(1, 0x80000000u), i2i(0, 1, 2, true, 2, false, true, false, false));
    CASE("I2I.U32.S32.SAT -1", "r0", 0u, mov32i(1, 0xFFFFFFFFu), i2i(0, 1, 2, false, 2, true, true, false, false));
    CASE("I2I.U32.S32.SAT 5", "r0", 5u, mov32i(1, 5u), i2i(0, 1, 2, false, 2, true, true, false, false));
    CASE("I2I.S8.S32.SAT -200", "r0", 0xFFFFFF80u, mov32i(1, (uint32_t)-200), i2i(0, 1, 0, true, 2, true, true, false, false));
    CASE("I2I.U16.S32.SAT 70000", "r0", 0xFFFFu, mov32i(1, 70000u), i2i(0, 1, 1, false, 2, true, true, false, false));
    CASE("I2I.S8.S32 0x1ff (zero-extended)", "r0", 0xFFu, mov32i(1, 0x1FFu), i2i(0, 1, 0, true, 2, true, false, false, false));
    CASE("I2I.S32.S32 |-5|", "r0", 5u, mov32i(1, (uint32_t)-5), i2i(0, 1, 2, true, 2, true, false, true, false));
    // SHF: hi:lo funnel; .W wraps the shift, otherwise it's clamped to the width (32, or 64 for .U64 / .S64)
    CASE("SHF.R.W 4", "r0", 0x789ABCDEu, mov32i(1, LO), mov32i(2, HI), mov32i(3, 4), shf(true, 0, 1, 3, 0, 2, 0, true));
    CASE("SHF.R.W 36 (wraps to 4)", "r0", 0x789ABCDEu, mov32i(1, LO), mov32i(2, HI), mov32i(3, 36), shf(true, 0, 1, 3, 0, 2, 0, true));
    CASE("SHF.R 40 (clamps to 32)", "r0", HI, mov32i(1, LO), mov32i(2, HI), mov32i(3, 40), shf(true, 0, 1, 3, 0, 2, 0, false));
    CASE("SHF.L 40 (clamps to 32)", "r0", LO, mov32i(1, LO), mov32i(2, HI), mov32i(3, 40), shf(false, 0, 1, 3, 0, 2, 0, false));
    CASE("SHF.L.W 8", "r0", 0x23456789u, mov32i(1, LO), mov32i(2, HI), mov32i(3, 8), shf(false, 0, 1, 3, 0, 2, 0, true));
    CASE("SHF.R.U64 imm 36", "r0", 0x00123456u, mov32i(1, LO), mov32i(2, HI), shf(true, 0, 1, -1, 36, 2, 2, false));
    CASE("SHF.R.U64 imm 4", "r0", 0x789ABCDEu, mov32i(1, LO), mov32i(2, HI), shf(true, 0, 1, -1, 4, 2, 2, false));
    CASE("SHF.R.U64 70 (clamps to 64)", "r0", 0u, mov32i(1, LO), mov32i(2, HI), mov32i(3, 70), shf(true, 0, 1, 3, 0, 2, 2, false));
    CASE("SHF.R.S64 40", "r0", 0xFFF00000u, mov32i(1, 0), mov32i(2, 0xF0000000u), mov32i(3, 40), shf(true, 0, 1, 3, 0, 2, 3, false));
    CASE("SHF.R.S64 70 (clamps to 64)", "r0", 0xFFFFFFFFu, mov32i(1, 0), mov32i(2, 0xF0000000u), mov32i(3, 70), shf(true, 0, 1, 3, 0, 2, 3, false));
    CASE("SHF.L.U64 40", "r0", 0xABCDEF00u, mov32i(1, LO), mov32i(2, HI), mov32i(3, 40), shf(false, 0, 1, 3, 0, 2, 2, false));
    CASE("SHF.L.U64 64", "r0", 0u, mov32i(1, LO), mov32i(2, HI), mov32i(3, 64), shf(false, 0, 1, 3, 0, 2, 2, false));
    CASE("SHF.L.U64.W 72 (wraps to 8)", "r0", 0x23456789u, mov32i(1, LO), mov32i(2, HI), mov32i(3, 72), shf(false, 0, 1, 3, 0, 2, 2, true));
    CASE("SHF.L.U64 imm 40", "r0", 0xABCDEF00u, mov32i(1, LO), mov32i(2, HI), shf(false, 0, 1, -1, 40, 2, 2, false));
    // HSETP2.H_AND: both halves are combined with the predicate, then ANDed; the second result is the complement
    CASE("HSETP2.GT.H_AND.AND !PT (first)", "p0", 0, mov32i(1, 0x3C003C00u), mov32i(2, 0), hsetp2(0, 1, 1, 2, GT, 7, true, AND, true));
    CASE("HSETP2.GT.H_AND.AND !PT (second)", "p1", 1, mov32i(1, 0x3C003C00u), mov32i(2, 0), hsetp2(0, 1, 1, 2, GT, 7, true, AND, true));
    CASE("HSETP2.GT.H_AND.OR PT (first)", "p0", 1, mov32i(1, 0x00003C00u), mov32i(2, 0), hsetp2(0, 1, 1, 2, GT, 7, false, OR, true));
    CASE("HSETP2.GT.H_AND.OR PT (second)", "p1", 0, mov32i(1, 0x00003C00u), mov32i(2, 0), hsetp2(0, 1, 1, 2, GT, 7, false, OR, true));
    CASE("HSETP2.LT.AND PT (per half)", "p1", 1, mov32i(1, 0xBC000000u), mov32i(2, 0), hsetp2(0, 1, 1, 2, LT, 7, false, AND, false));
    // LDC.64 with the address register among the destinations: both words come from the original address (the
    // test's constant buffer 3 holds 0x1000 + word index at every word)
    CASE("LDC.64 R4, c[3][R4+4] (low)", "r4", 0x1003u, mov32i(4, 8), ldc(4, 4, 3, 4, 5));
    CASE("LDC.64 R4, c[3][R4+4] (high)", "r5", 0x1004u, mov32i(4, 8), ldc(4, 4, 3, 4, 5));
    CASE("LDC.64 R4, c[3][R5+4] (low)", "r4", 0x1003u, mov32i(5, 8), ldc(4, 5, 3, 4, 5));
    CASE("LDC.64 R4, c[3][R5+4] (high)", "r5", 0x1004u, mov32i(5, 8), ldc(4, 5, 3, 4, 5));
    CASE("LDC R4, c[3][R4+4]", "r4", 0x1003u, mov32i(4, 8), ldc(4, 4, 3, 4, 4));
#undef CASE
    *n = k;
    return c;
}

// C++ stand-ins for the MSL built-ins the bodies use (only the ones these instructions emit)
static const char prelude[] =
    "#include <cstdint>\n#include <cstdio>\n#include <cstring>\n#include <cstdlib>\n"
    "typedef uint32_t uint; typedef uint64_t ulong;\n"
    "#define long int64_t\n"
    "template <class T, class U> static T as_type(U u) { static_assert(sizeof(T) == sizeof(U), \"size\"); T t; memcpy(&t, &u, sizeof t); return t; }\n"
    "template <class T> static T min(T a, T b) { return b < a ? b : a; }\n"
    "template <class T> static T max(T a, T b) { return a < b ? b : a; }\n"
    "template <class T> static T clamp(T x, T lo, T hi) { return min(max(x, lo), hi); }\n"
    "struct float2 { float x, y; };\n"
    "static float h2f(uint h) { _Float16 f; uint16_t b = (uint16_t)h; memcpy(&f, &b, 2); return (float)f; }\n"
    "static float2 hx(uint v, uint swz) { if (swz == 1u) { float f = as_type<float>(v); return {f, f}; }"
    " float2 h = {h2f(v & 0xFFFFu), h2f(v >> 16)}; return swz == 2u ? float2{h.x, h.x} : swz == 3u ? float2{h.y, h.y} : h; }\n"
    "static float2 operator-(float2 a) { return {-a.x, -a.y}; }\n"
    "struct cbuf { uint at(uint off) const { return 0x1000u + (off >> 2); } };\n"
    "static const cbuf cb3 = {};\n"
    "static uint cbr(const cbuf &cb, uint off) { return cb.at(off); }\n"
    "static int fails;\n"
    "static void check(const char *name, uint got, uint want) {\n"
    "    if (got != want) { printf(\"FAIL %s: got 0x%08x, want 0x%08x\\n\", name, got, want); fails++; }\n"
    "}\n";

// The case's program: its instructions with scheduling words, EXIT, padded with NOPs; returns the word count
static int program(const Case *c, uint64_t prog[16]) {
    int m = 0;
    for (const uint64_t *w = c->insn; *w; w++) {
        if (m % 4 == 0) prog[m++] = 0;  // scheduling word
        prog[m++] = *w;
    }
    if (m % 4 == 0) prog[m++] = 0;
    prog[m++] = EXIT;
    while (m % 4) prog[m++] = NOP;
    return m;
}

static int write_glsl(const char *dir) {
    static const struct { int stage; const char *ext; } st[] = {
        {STAGE_VERTEX, "vert"}, {STAGE_FRAGMENT, "frag"}, {STAGE_COMPUTE, "comp"}};
    char path[1024];
    snprintf(path, sizeof path, "%s/names.txt", dir);
    FILE *names = fopen(path, "w");
    if (!names) { perror(path); return 2; }
    int n, bad = 0;
    const Case *cs = cases(&n);
    for (int i = 0; i < n; i++) {
        uint64_t prog[16];
        int m = program(&cs[i], prog);
        for (int j = 0; j < 3; j++) {
            // vertex / fragment: the 0x80-byte program header first (zero, as tools/test_translate.c's; fragment:
            // color target 0 written, output map at 0x30 + 0x48)
            uint8_t buf[0x80 + sizeof prog] = {0};
            size_t hdr = st[j].stage == STAGE_COMPUTE ? 0 : 0x80;
            if (st[j].stage == STAGE_FRAGMENT) buf[0x30 + 0x48] = 0xF;
            memcpy(buf + hdr, prog, (size_t)m * 8);
            ShaderKey key = {0};  // (compute: as the Vulkan backend's key, exec.c translate_compute)
            key.glsl = 1;
            ShaderInfo *s = maxwell_translate(buf, hdr + (size_t)m * 8, st[j].stage, &key);
            fprintf(names, "%02d_%s %s\n", i, st[j].ext, cs[i].name);
            if (!s->msl) {
                printf("FAIL %s (%s): translation: %s\n", cs[i].name, st[j].ext, s->error);
                bad++;
            } else {
                snprintf(path, sizeof path, "%s/%02d_%s.%s", dir, i, st[j].ext, st[j].ext);
                FILE *f = fopen(path, "w");
                if (!f) { perror(path); return 2; }
                fputs(s->msl, f);
                fclose(f);
            }
            maxwell_free(s);
        }
    }
    fclose(names);
    printf("%d cases x 3 stages written to %s, %d translation failures\n", n, dir, bad);
    return bad != 0;
}

int main(int argc, char **argv) {
    if (argc == 3 && !strcmp(argv[1], "--glsl")) return write_glsl(argv[2]);
    if (argc < 2) { fprintf(stderr, "usage: %s <out.cpp> | --glsl <dir>\n", argv[0]); return 2; }
    FILE *o = fopen(argv[1], "w");
    if (!o) { perror(argv[1]); return 2; }
    fputs(prelude, o);
    int n, bad = 0;
    const Case *cs = cases(&n);
    for (int i = 0; i < n; i++) {
        uint64_t prog[16];
        int m = program(&cs[i], prog);
        ShaderKey key = {0};
        ShaderInfo *s = maxwell_translate((const uint8_t *)prog, (size_t)m * 8, STAGE_COMPUTE, &key);
        const char *b = s->msl ? strstr(s->msl, "kernel void main0(") : NULL;
        b = b ? strstr(b, "{\n") : NULL;
        const char *e = b ? strstr(b, "  return;\n}") : NULL;
        if (!e) { printf("FAIL %s: translation: %s\n", cs[i].name, s->msl ? "unexpected layout" : s->error); bad++; continue; }
        fprintf(o, "static void case%d() %.*s  check(\"%s\", (uint)%s, 0x%08xu);\n}\n", i, (int)(e - b), b, cs[i].name, cs[i].var,
                cs[i].expect);
        maxwell_free(s);
    }
    fprintf(o, "int main() {\n");
    for (int i = 0; i < n; i++) fprintf(o, "    case%d();\n", i);
    fprintf(o, "    printf(\"%d cases, %%d failures\\n\", fails);\n    return fails != 0;\n}\n", n);
    fclose(o);
    return bad != 0;
}
