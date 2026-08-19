/* bo1probe.c -- the .asi that proves we are inside the live BlackOps.exe.
 *
 * Loaded by the repository's own dist/dinput8.dll (asi_load_all), which is in
 * turn dragged in by winmm_shim.c. Everything here runs in the real game
 * process against the real game image.
 *
 * It answers four questions, in this order, and refuses to guess at any of them:
 *
 *   1. Are we in?                 banner + module list + image base.
 *   2. Did anything rewrite the   byte-for-byte comparison of the in-memory
 *      code at runtime?           .text/.rdata against the same sections read
 *                                 back off disk from BlackOps.exe itself. This
 *                                 is the empirical CEG question: if CEG (or
 *                                 Steam, or Wine) decrypted, relocated or
 *                                 hot-patched code, this reports exactly which
 *                                 bytes. The IAT is a built-in positive control
 *                                 -- the loader must have written it, so a run
 *                                 that reports zero differences everywhere
 *                                 would mean the comparison itself is broken.
 *   3. Does the process think it  IsDebuggerPresent, PEB->BeingDebugged,
 *      is being debugged?         PEB->NtGlobalFlag, CheckRemoteDebuggerPresent.
 *   4. Do published addresses      Dvar_FindVar @0x5AE810 called for real, with
 *      work at runtime?           every structure offset it relies on verified
 *                                 rather than assumed.
 *
 * DllMain does nothing but CreateThread (README / Exp. 4: asi_load_all runs
 * under the loader lock).
 *
 * All reads of game memory go through ReadProcessMemory on our own process, so
 * a wrong address is a returned FALSE and a logged line, never a crash in
 * somebody else's game.
 */

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------------- */
/* Addresses. Every one of these was derived from our own disassembly of       */
/* BlackOps.exe (md5 2b179a57416680b60462c5af05552ea2); see RESULTS.md §"the   */
/* address work" for the exact instructions each came from.                    */
/* docs/address-map.md is the wider survey.                                    */
/* ------------------------------------------------------------------------- */

#define A_IMAGEBASE           0x00400000u

/* void *Dvar_FindVar(const char *name)  -- cdecl.  address-map §3.1 (strong)  */
#define A_DVAR_FINDVAR        0x005AE810u
/* unsigned Dvar_Hash(const char *name, int maxlen) -- cdecl.  djb2/33, 0x1505 */
#define A_DVAR_HASH           0x0068B370u
/* char R_IsStereoActive(void) -- "mov al,[0x396346B]; ret". address-map §6.    */
#define A_R_ISSTEREOACTIVE    0x006B8B20u

/* Dvar_Create @0x862B20:  mov [0x261CBD4]=count, [eax*4+0x261CBE8]=ptr,        */
/*                         imul esi,esi,0x70 ; add esi,0x2621BF0                */
#define A_DVAR_COUNT          0x0261CBD4u   /* int                              */
#define A_DVAR_TABLE          0x0261CBE8u   /* dvar_s *[4096]                   */
#define A_DVAR_POOL           0x02621BF0u   /* dvar_s [4096], stride 0x70       */
/* dvar hash-bucket head array; from the lookup at 0x862280:                    */
/*   mov ecx,esi ; and ecx,0x3FF ; mov eax,[ecx*4+0x2620BF0]                    */
#define A_DVAR_BUCKETS        0x02620BF0u
#define DVAR_STRIDE           0x70u
#define DVAR_OFF_NAME         0x00u
#define DVAR_OFF_DESC         0x04u
#define DVAR_OFF_HASH         0x08u   /* proven: cmp [eax+8],esi in the lookup  */
#define DVAR_OFF_FLAGS        0x0Cu
#define DVAR_OFF_TYPE         0x10u   /* proven: mov [esi+0x10],<type> in Create*/
#define DVAR_OFF_MODIFIED     0x14u
#define DVAR_OFF_CURRENT      0x18u   /* proven: mov [esi+0x18/0x28/0x38],eax   */
#define DVAR_OFF_NEXT         0x68u   /* proven: mov eax,[eax+0x68] chain walk  */

/* DxGlobals dx @0x3963440 (address-map §5.4/§6) */
#define A_DX_D3D9             0x03963444u   /* IDirect3D9*        (+0x04)       */
#define A_DX_DEVICE           0x03963448u   /* IDirect3DDevice9*  (+0x08)       */
#define A_DX_NVSTEREOACTIVE   0x0396346Bu   /* bool               (+0x2B)       */

/* The CEG check tables (address-map §3.3) and the function journal.lunar.sh
 * documents as a timing-based anti-debug check. Used as byte anchors only. */
#define A_CEG_HWBP_TABLE      0x009A23B0u   /* stride 0x40, 16 entries          */
#define A_CEG_HWBP_DIRECT     0x009A2980u   /* stride 0x30, 3 entries           */
#define A_ANTIDEBUG_4C06E0    0x004C06E0u

/* ------------------------------------------------------------------------- */
/* logging                                                                     */
/* ------------------------------------------------------------------------- */

static HANDLE g_logfile = INVALID_HANDLE_VALUE;
static CRITICAL_SECTION g_loglock;
static DWORD g_t0;

static void P(const char *fmt, ...)
{
    char buf[2048];
    int n;
    va_list ap;

    va_start(ap, fmt);
    n = _vsnprintf(buf, sizeof(buf) - 4, fmt, ap);
    va_end(ap);
    if (n < 0) n = (int)strlen(buf);
    buf[n] = '\n';
    buf[n + 1] = '\0';

    EnterCriticalSection(&g_loglock);
    fputs(buf, stderr);
    fflush(stderr);
    if (g_logfile != INVALID_HANDLE_VALUE) {
        DWORD w;
        WriteFile(g_logfile, buf, (DWORD)strlen(buf), &w, NULL);
        FlushFileBuffers(g_logfile);
    }
    LeaveCriticalSection(&g_loglock);
    OutputDebugStringA(buf);
}

static void open_log(void)
{
    char *p = getenv("BO1VR_LOG");
    if (!p || !*p) return;
    g_logfile = CreateFileA(p, GENERIC_WRITE, FILE_SHARE_READ, NULL,
                            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
}

/* ------------------------------------------------------------------------- */
/* safe reads -- ReadProcessMemory on ourselves never faults                    */
/* ------------------------------------------------------------------------- */

static int rd(DWORD addr, void *out, SIZE_T n)
{
    SIZE_T got = 0;
    return ReadProcessMemory(GetCurrentProcess(), (LPCVOID)(UINT_PTR)addr,
                             out, n, &got) && got == n;
}

static DWORD rd32(DWORD addr, int *ok)
{
    DWORD v = 0;
    int good = rd(addr, &v, 4);
    if (ok) *ok = good;
    return good ? v : 0;
}

/* Copy a NUL-terminated string out of the game, without trusting the pointer. */
static int rdstr(DWORD addr, char *out, int cap)
{
    int i;
    if (!addr) { out[0] = '\0'; return 0; }
    for (i = 0; i < cap - 1; i++) {
        char c;
        if (!rd(addr + i, &c, 1)) { out[i] = '\0'; return 0; }
        out[i] = c;
        if (!c) return 1;
    }
    out[cap - 1] = '\0';
    return 1;
}

static void hexdump_bytes(DWORD addr, int n, char *out, int cap)
{
    int i, k = 0;
    for (i = 0; i < n && k < cap - 4; i++) {
        unsigned char b;
        if (!rd(addr + i, &b, 1)) { k += _snprintf(out + k, cap - k, "?? "); continue; }
        k += _snprintf(out + k, cap - k, "%02x ", b);
    }
    out[k > 0 ? k - 1 : 0] = '\0';
}

/* ------------------------------------------------------------------------- */
/* 1. where are we                                                             */
/* ------------------------------------------------------------------------- */

static void report_identity(void)
{
    char exe[MAX_PATH] = {0}, self[MAX_PATH] = {0};
    HMODULE base = GetModuleHandleA(NULL);
    MEMORY_BASIC_INFORMATION mbi;

    GetModuleFileNameA(NULL, exe, MAX_PATH);
    GetModuleFileNameA(GetModuleHandleA("bo1probe.asi"), self, MAX_PATH);

    P("--- 1. identity");
    P("  pid                 = %lu (0x%lx)", GetCurrentProcessId(), GetCurrentProcessId());
    P("  main tid            = %lu", GetCurrentThreadId());
    P("  exe                 = %s", exe);
    P("  GetModuleHandle(0)  = %p   (expected %p, no ASLR)",
      (void *)base, (void *)(UINT_PTR)A_IMAGEBASE);
    P("  this .asi           = %s", self);
    P("  dinput8.dll module  = %p", (void *)GetModuleHandleA("dinput8.dll"));
    P("  winmm.dll module    = %p", (void *)GetModuleHandleA("winmm.dll"));
    P("  d3d9.dll module     = %p", (void *)GetModuleHandleA("d3d9.dll"));
    P("  steam_api.dll       = %p", (void *)GetModuleHandleA("steam_api.dll"));

    if (VirtualQuery((LPCVOID)(UINT_PTR)0x00401000u, &mbi, sizeof(mbi)) == sizeof(mbi))
        P("  .text protection    = 0x%lx (initial 0x%lx), state 0x%lx, size 0x%lx",
          mbi.Protect, mbi.AllocationProtect, mbi.State, (DWORD)mbi.RegionSize);

    if ((UINT_PTR)base != A_IMAGEBASE)
        P("  !! image is NOT at 0x400000 -- every hardcoded address below is wrong");
}

/* ------------------------------------------------------------------------- */
/* 2. did anything rewrite the code at runtime?                                */
/* ------------------------------------------------------------------------- */

struct diffrange { DWORD va; DWORD len; };

static void compare_image_to_disk(void)
{
    char exe[MAX_PATH] = {0};
    HANDLE h;
    DWORD filesize = 0, got = 0;
    unsigned char *file = NULL;
    IMAGE_DOS_HEADER dos;
    IMAGE_NT_HEADERS32 *nt;
    IMAGE_SECTION_HEADER *sh;
    DWORD iat_va = 0, iat_sz = 0;
    int s;

    P("--- 2. in-memory image vs BlackOps.exe on disk");

    GetModuleFileNameA(NULL, exe, MAX_PATH);
    h = CreateFileA(exe, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                    NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        P("  cannot open %s, err=%lu -- comparison skipped", exe, GetLastError());
        return;
    }
    filesize = GetFileSize(h, NULL);
    file = (unsigned char *)malloc(filesize);
    if (!file || !ReadFile(h, file, filesize, &got, NULL) || got != filesize) {
        P("  read failed (%lu/%lu)", got, filesize);
        CloseHandle(h); free(file); return;
    }
    CloseHandle(h);
    P("  read %lu bytes from %s", filesize, exe);

    memcpy(&dos, file, sizeof(dos));
    nt = (IMAGE_NT_HEADERS32 *)(file + dos.e_lfanew);
    P("  file ImageBase=0x%lx  SizeOfImage=0x%lx  sections=%u  DllCharacteristics=0x%x",
      nt->OptionalHeader.ImageBase, nt->OptionalHeader.SizeOfImage,
      nt->FileHeader.NumberOfSections, nt->OptionalHeader.DllCharacteristics);

    iat_va = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IAT].VirtualAddress;
    iat_sz = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IAT].Size;
    P("  IAT (the positive control) = 0x%lx .. 0x%lx",
      A_IMAGEBASE + iat_va, A_IMAGEBASE + iat_va + iat_sz);

    sh = IMAGE_FIRST_SECTION(nt);
    for (s = 0; s < nt->FileHeader.NumberOfSections; s++, sh++) {
        char name[9] = {0};
        DWORD n, i, ndiff = 0, nranges = 0, in_iat = 0;
        DWORD va = A_IMAGEBASE + sh->VirtualAddress;
        struct diffrange rr[8];
        int rc = 0;

        memcpy(name, sh->Name, 8);
        if (strcmp(name, ".text") && strcmp(name, ".rdata"))
            continue;   /* .data is legitimately mutable; comparing it is noise */

        n = sh->SizeOfRawData;
        if (sh->Misc.VirtualSize && sh->Misc.VirtualSize < n) n = sh->Misc.VirtualSize;

        for (i = 0; i < n; i++) {
            unsigned char mem;
            int same;
            if (!rd(va + i, &mem, 1)) { P("  %s: unreadable at 0x%lx", name, va + i); break; }
            same = (mem == file[sh->PointerToRawData + i]);
            if (!same) {
                DWORD rva = sh->VirtualAddress + i;
                ndiff++;
                if (iat_sz && rva >= iat_va && rva < iat_va + iat_sz) in_iat++;
                if (nranges && rr[rc - 1].va + rr[rc - 1].len == va + i)
                    rr[rc - 1].len++;
                else {
                    if (rc < 8) { rr[rc].va = va + i; rr[rc].len = 1; rc++; }
                    nranges++;
                }
            }
        }
        P("  %-7s VA 0x%lx len 0x%lx : %lu differing byte(s) in %lu range(s)"
          "%s", name, va, n, ndiff, nranges,
          in_iat ? "" : "");
        if (in_iat)
            P("            of which %lu are inside the IAT (expected: the loader wrote them)",
              in_iat);
        {
            int j;
            for (j = 0; j < rc; j++) {
                char a[64], b[64];
                DWORD off = rr[j].va - va + sh->PointerToRawData;
                int k, len = (int)(rr[j].len > 12 ? 12 : rr[j].len);
                int p = 0;
                hexdump_bytes(rr[j].va, len, a, sizeof(a));
                for (k = 0, p = 0; k < len; k++)
                    p += _snprintf(b + p, (int)sizeof(b) - p, "%02x ", file[off + k]);
                if (p) b[p - 1] = '\0';
                P("            0x%08lx len %lu  disk[%s]  mem[%s]", rr[j].va, rr[j].len, b, a);
            }
            if (nranges > (DWORD)rc)
                P("            (+%lu more range(s) not shown)", nranges - rc);
        }
    }
    free(file);
}

/* ------------------------------------------------------------------------- */
/* 3. debugger visibility                                                      */
/* ------------------------------------------------------------------------- */

static void report_debugger_state(const char *when)
{
    BOOL remote = FALSE;
    DWORD peb = 0;
    unsigned char being = 0;
    DWORD ntglobal = 0;
    int ok1 = 0, ok2 = 0;

    /* PEB from the TIB: fs:[0x30] */
    __asm__ volatile ("movl %%fs:0x30, %0" : "=r"(peb));
    if (peb) {
        rd(peb + 0x02, &being, 1);
        ntglobal = rd32(peb + 0x68, &ok1);
    }
    CheckRemoteDebuggerPresent(GetCurrentProcess(), &remote);
    (void)ok1; (void)ok2;

    P("  [%s] IsDebuggerPresent=%d  PEB@%08lx->BeingDebugged=%u  "
      "NtGlobalFlag=0x%lx  CheckRemoteDebuggerPresent=%d",
      when, (int)IsDebuggerPresent(), peb, being, ntglobal, (int)remote);
}

/* ------------------------------------------------------------------------- */
/* 4. published addresses, at runtime                                          */
/* ------------------------------------------------------------------------- */

typedef void *(__cdecl *pfn_Dvar_FindVar)(const char *name);
typedef unsigned (__cdecl *pfn_Dvar_Hash)(const char *name, int maxlen);
typedef char (__cdecl *pfn_R_IsStereoActive)(void);

/* Our own reimplementation of Dvar_Hash, written from the disassembly of
 * 0x68B370:  h = 0x1505; for each char  h = tolower(c) + h*33.
 * Comparing it against the hash the game itself stored in dvar_s+0x08 is what
 * turns "we read a plausible number" into "we understand the structure". */
static unsigned our_dvar_hash(const char *s)
{
    unsigned h = 0x1505u;
    for (; *s; s++) {
        int c = (int)(signed char)*s;
        if (c >= 'A' && c <= 'Z') c += 0x20;
        h = (unsigned)c + h * 33u;
    }
    return h;
}

static const char *dvar_typename(DWORD t)
{
    static const char *n[] = { "bool", "float", "vec2", "vec3", "vec4", "int",
                               "enum", "string", "color", "int64",
                               "linearColorRGB", "colorXYZ" };
    return t < sizeof(n) / sizeof(n[0]) ? n[t] : "?";
}

static void format_dvar_value(DWORD dv, DWORD type, char *out, int cap)
{
    DWORD cur = dv + DVAR_OFF_CURRENT;
    int ok = 0;
    switch (type) {
    case 0: { unsigned char b = 0; rd(cur, &b, 1);
              _snprintf(out, cap, "%s", b ? "true" : "false"); break; }
    case 1: { float f = 0; rd(cur, &f, 4); _snprintf(out, cap, "%f", f); break; }
    case 2: case 3: case 4: {
              float f[4] = {0,0,0,0}; int k, nn = (int)(type == 2 ? 2 : type == 3 ? 3 : 4), p = 0;
              rd(cur, f, 16);
              p += _snprintf(out + p, cap - p, "(");
              for (k = 0; k < nn; k++) p += _snprintf(out + p, cap - p, "%s%f", k ? ", " : "", f[k]);
              _snprintf(out + p, cap - p, ")"); break; }
    case 5: case 6: { int v = (int)rd32(cur, &ok); _snprintf(out, cap, "%d", v); break; }
    case 7: { DWORD sp = rd32(cur, &ok); char s[256];
              rdstr(sp, s, sizeof(s));
              _snprintf(out, cap, "\"%s\" (char* %08lx)", s, sp); break; }
    case 8: { unsigned char c[4] = {0,0,0,0}; rd(cur, c, 4);
              _snprintf(out, cap, "#%02x%02x%02x%02x", c[0], c[1], c[2], c[3]); break; }
    default: { DWORD v = rd32(cur, &ok); _snprintf(out, cap, "raw 0x%08lx", v); break; }
    }
}

/* Returns 1 if the dvar checked out completely. */
static int probe_one_dvar(pfn_Dvar_FindVar FindVar, const char *name, const char *why)
{
    void *p = FindVar(name);
    DWORD dv = (DWORD)(UINT_PTR)p;
    char nm[128], val[320];
    DWORD hash = 0, type = 0, flags = 0, next = 0;
    unsigned ours;
    int ok = 0, name_ok, hash_ok, pool_ok;

    if (!p) {
        P("    %-18s -> NULL%s%s", name, why ? "   " : "", why ? why : "");
        return 0;
    }
    rdstr(rd32(dv + DVAR_OFF_NAME, &ok), nm, sizeof(nm));
    hash  = rd32(dv + DVAR_OFF_HASH, &ok);
    flags = rd32(dv + DVAR_OFF_FLAGS, &ok);
    type  = rd32(dv + DVAR_OFF_TYPE, &ok);
    next  = rd32(dv + DVAR_OFF_NEXT, &ok);
    ours  = our_dvar_hash(name);
    format_dvar_value(dv, type, val, sizeof(val));

    name_ok = (strcmp(nm, name) == 0);
    hash_ok = (hash == ours);
    pool_ok = (dv >= A_DVAR_POOL && dv < A_DVAR_POOL + 4096u * DVAR_STRIDE
               && ((dv - A_DVAR_POOL) % DVAR_STRIDE) == 0);

    P("    %-18s @%08lx  name=\"%s\"%s  hash=%08lx%s  type=%lu(%-6s) flags=0x%04lx  value=%s",
      name, dv, nm, name_ok ? "" : " <-- MISMATCH",
      hash, hash_ok ? "==ours" : " <-- MISMATCH vs ours", type, dvar_typename(type),
      flags, val);
    if (!pool_ok)
        P("                       !! not on the 0x70 grid of the pool at 0x%08x", A_DVAR_POOL);
    (void)next;
    return name_ok && hash_ok && pool_ok;
}

/* The dvar names probed. com_maxfps is the load-bearing one: run.sh writes
 * com_maxfps "47" into the *mirror's* players/config.cfg, a value that appears
 * nowhere else on this machine, so reading 47 back out of the live process is
 * proof we are reading the game's real dvar and not a plausible-looking
 * coincidence. r_fullscreen and r_mode are set the same way. */
static const char *g_dvar_names[] = {
    "com_maxfps", "r_fullscreen", "r_mode", "r_monitor", "r_displayRefresh",
    "fs_game", "fs_basepath", "sv_cheats", "r_gamma", "version",
    "cg_fov", "name",
};

static int run_dvar_pass(pfn_Dvar_FindVar FindVar, int *tried)
{
    int i, ok = 0, n = 0;
    for (i = 0; i < (int)(sizeof(g_dvar_names) / sizeof(g_dvar_names[0])); i++) {
        n++;
        ok += probe_one_dvar(FindVar, g_dvar_names[i], NULL);
    }
    P("  %d/%d dvars resolved AND fully cross-checked (name, hash, pool grid)", ok, n);
    if (tried) *tried = n;
    return ok;
}

/* ------------------------------------------------------------------------- */
/* the breakpoint target -- deliberately trivial, deliberately noinline        */
/* ------------------------------------------------------------------------- */

volatile int g_bo1probe_heartbeat;
volatile int g_bo1probe_last;

__attribute__((noinline))
int bo1probe_breakpoint_target(int tick, void *d3d9_device)
{
    volatile int mixed = tick * 3 + 1;
    g_bo1probe_last = mixed;
    if (d3d9_device) mixed |= 0x40000000;
    return mixed;
}

/* ------------------------------------------------------------------------- */

static int bytes_match(DWORD va, const unsigned char *want, int n)
{
    unsigned char got[32];
    if (n > (int)sizeof(got)) n = (int)sizeof(got);
    if (!rd(va, got, n)) return 0;
    return memcmp(got, want, n) == 0;
}

struct anchor { DWORD va; const char *what; int n; unsigned char b[12]; };

static const struct anchor g_anchors[] = {
    { A_DVAR_FINDVAR, "Dvar_FindVar prologue  mov eax,[esp+4]; test eax,eax; je", 8,
      { 0x8b, 0x44, 0x24, 0x04, 0x85, 0xc0, 0x74, 0x1a } },
    { A_DVAR_HASH,    "Dvar_Hash prologue     push ebx; mov ebx,[esp+8]", 7,
      { 0x53, 0x8b, 0x5c, 0x24, 0x08, 0x85, 0xdb } },
    { A_R_ISSTEREOACTIVE, "R_IsStereoActive       mov al,[0x396346B]; ret", 6,
      { 0xa0, 0x6b, 0x34, 0x96, 0x03, 0xc3 } },
    { A_ANTIDEBUG_4C06E0, "0x4C06E0 (lunar.sh 'timing anti-debug'/CEG helper)", 7,
      { 0x80, 0x7c, 0x24, 0x04, 0x00, 0x75, 0x56 } },
    { A_CEG_HWBP_TABLE,   "CEG stub table 0x9A23B0[0]  push ebp; mov ebp,esp; push 0x9A2900", 8,
      { 0x55, 0x8b, 0xec, 0x68, 0x00, 0x29, 0x9a, 0x00 } },
    { A_CEG_HWBP_DIRECT,  "CEG stub table 0x9A2980[0]  push ebp; mov ebp,esp; push 0x9A2800", 8,
      { 0x55, 0x8b, 0xec, 0x68, 0x00, 0x28, 0x9a, 0x00 } },
};

static DWORD WINAPI worker(LPVOID arg)
{
    pfn_Dvar_FindVar FindVar = (pfn_Dvar_FindVar)(UINT_PTR)A_DVAR_FINDVAR;
    pfn_Dvar_Hash    GameHash = (pfn_Dvar_Hash)(UINT_PTR)A_DVAR_HASH;
    pfn_R_IsStereoActive IsStereo = (pfn_R_IsStereoActive)(UINT_PTR)A_R_ISSTEREOACTIVE;
    int i, anchors_ok = 0, waited_ms = 0, budget_ms;
    int dvars_ok = 0, dvars_tried = 0;
    DWORD count = 0, dev = 0;
    char *e;
    int quit_after = 0;

    (void)arg;
    g_t0 = GetTickCount();

    e = getenv("BO1VR_WAIT_MS");  budget_ms = e ? atoi(e) : 300000;
    e = getenv("BO1VR_QUIT_AFTER_S"); quit_after = e ? atoi(e) : 0;

    P("=== EXPERIMENT 7: bo1probe.asi inside the live BlackOps.exe ===");
    P("built " __DATE__ " " __TIME__ " with GCC " __VERSION__ ", pointer size %u",
      (unsigned)sizeof(void *));

    report_identity();

    P("--- 1b. published-address anchors (in-memory bytes vs our disassembly)");
    for (i = 0; i < (int)(sizeof(g_anchors) / sizeof(g_anchors[0])); i++) {
        char got[64];
        int ok = bytes_match(g_anchors[i].va, g_anchors[i].b, g_anchors[i].n);
        hexdump_bytes(g_anchors[i].va, g_anchors[i].n, got, sizeof(got));
        P("    0x%08lx %-4s [%s]  %s", g_anchors[i].va, ok ? "OK" : "FAIL",
          got, g_anchors[i].what);
        anchors_ok += ok;
    }
    P("  %d/%d anchors match", anchors_ok,
      (int)(sizeof(g_anchors) / sizeof(g_anchors[0])));

    compare_image_to_disk();

    P("--- 3. debugger visibility");
    report_debugger_state("asi startup");

    /* "count > 0" fires early -- measured, sometimes when only TWO dvars exist,
     * seconds before the renderer registers its own -- so the EARLY pass can
     * legitimately resolve only 1/12. That is deliberate: getting in early is
     * what proves the loader is up before the game is, and the LATE pass in the
     * heartbeat loop (§9) is the one whose numbers count. Waiting for the count
     * to stop growing instead was tried and is worse: under Wine this process
     * does not live long enough to reach a stable count (see RESULTS.md §8). */
    P("--- 4. waiting for the game's dvar system (poll [0x%08x], budget %d ms)",
      A_DVAR_COUNT, budget_ms);
    for (;;) {
        int ok = 0;
        count = rd32(A_DVAR_COUNT, &ok);
        if (ok && count > 0 && count < 4096) break;
        if (waited_ms >= budget_ms) {
            P("  TIMEOUT after %d ms, dvar count still %lu -- probe cannot continue",
              waited_ms, count);
            goto heartbeat;
        }
        Sleep(100);
        waited_ms += 100;
    }
    P("  dvar system up after ~%d ms, %lu dvars registered", waited_ms, count);

    if (anchors_ok == 0) {
        P("  refusing to CALL into the image: not one anchor matched");
        goto heartbeat;
    }

    P("--- 5. calling Dvar_FindVar @0x%08x for real (early pass)", A_DVAR_FINDVAR);
    dvars_ok = run_dvar_pass(FindVar, &dvars_tried);

    P("--- 6. closing the loop: table -> name -> Dvar_FindVar -> same pointer?");
    {
        int checked = 0, agreed = 0;
        int step = (int)(count / 12) + 1;
        for (i = 0; i < (int)count; i += step) {
            int ok = 0;
            DWORD dv = rd32(A_DVAR_TABLE + (DWORD)i * 4, &ok);
            char nm[128];
            void *back;
            if (!ok || !dv) continue;
            if (!rdstr(rd32(dv + DVAR_OFF_NAME, &ok), nm, sizeof(nm)) || !nm[0]) continue;
            back = FindVar(nm);
            checked++;
            if ((DWORD)(UINT_PTR)back == dv) agreed++;
            else P("    table[%d] \"%s\" @%08lx but Dvar_FindVar gave %p", i, nm, dv, back);
            if (checked >= 12) break;
        }
        P("  %d/%d table entries round-tripped through Dvar_FindVar by name", agreed, checked);
    }

    P("--- 7. our Dvar_Hash reimplementation vs the game's own @0x%08x", A_DVAR_HASH);
    {
        static const char *t[] = { "fs_game", "R_CUSTOMWIDTH", "cg_fov", "" };
        int agreed = 0, n = 0;
        for (i = 0; t[i][0] || i < 3; i++) {
            unsigned a, b;
            if (i >= 3) break;
            a = GameHash(t[i], 0);
            b = our_dvar_hash(t[i]);
            n++;
            if (a == b) agreed++;
            P("    Dvar_Hash(\"%s\") game=%08x ours=%08x %s", t[i], a, b,
              a == b ? "==" : "<-- MISMATCH");
        }
        P("  %d/%d hashes agree", agreed, n);
    }

    P("--- 8. renderer globals (DxGlobals @0x3963440)");
    {
        int ok = 0;
        DWORD d3d9 = rd32(A_DX_D3D9, &ok);
        unsigned char nvstereo = 0;
        dev = rd32(A_DX_DEVICE, &ok);
        rd(A_DX_NVSTEREOACTIVE, &nvstereo, 1);
        P("    dx->d3d9   (+0x04) = %08lx", d3d9);
        P("    dx->device (+0x08) = %08lx  %s", dev,
          dev ? "<-- live IDirect3DDevice9, BAC-281's hook target" : "(not created yet)");
        P("    dx->nvStereoActivated (+0x2B) = %u", nvstereo);
        P("    R_IsStereoActive() called @0x%08x -> %d",
          A_R_ISSTEREOACTIVE, (int)IsStereo());
        if (dev) {
            DWORD vtbl = rd32(dev, &ok);
            P("    device vtable      = %08lx (%s)", vtbl,
              (vtbl > 0x10000) ? "plausible" : "implausible");
        }
    }

    report_debugger_state("after probing");

    P("=== EXPERIMENT 7 SUMMARY: anchors %d/%d, dvars %d/%d, dvar count %lu, device %s ===",
      anchors_ok, (int)(sizeof(g_anchors) / sizeof(g_anchors[0])),
      dvars_ok, dvars_tried, count, dev ? "yes" : "no");
    if (anchors_ok == (int)(sizeof(g_anchors) / sizeof(g_anchors[0]))
        && dvars_tried && dvars_ok >= dvars_tried - 2)
        P("=== EXPERIMENT 7 END: PASS ===");
    else
        P("=== EXPERIMENT 7 END: PARTIAL ===");

heartbeat:
    /* Stay alive and keep calling one small, named function of our own so that
     * a debugger attaching later has something to break on. */
    P("--- heartbeat: bo1probe_breakpoint_target @%p, call it once a second",
      (void *)&bo1probe_breakpoint_target);
    for (i = 0; ; i++) {
        int ok = 0;
        g_bo1probe_heartbeat = i;
        bo1probe_breakpoint_target(i, (void *)(UINT_PTR)rd32(A_DX_DEVICE, &ok));
        /* The early pass above may run before players/config.cfg is exec'd.
         * Re-run it once the renderer has a device: by then the config has
         * certainly been applied, so com_maxfps must read back the value
         * run.sh wrote. */
        if (!dev && anchors_ok && (dev = rd32(A_DX_DEVICE, &ok)) != 0) {
            P("--- 9. dx->device became %08lx at t+%lus: re-probing dvars",
              dev, (GetTickCount() - g_t0) / 1000);
            dvars_ok = run_dvar_pass(FindVar, &dvars_tried);
            P("=== EXPERIMENT 7 LATE PASS: dvars %d/%d, device %08lx, dvar count %lu ===",
              dvars_ok, dvars_tried, dev, rd32(A_DVAR_COUNT, &ok));
            /* This, not the early pass, is the verdict: by now the renderer has
             * a device, so players/config.cfg has certainly been applied and
             * every dvar we ask for actually exists. */
            if (anchors_ok == (int)(sizeof(g_anchors) / sizeof(g_anchors[0]))
                && dvars_tried && dvars_ok >= dvars_tried - 1)
                P("=== EXPERIMENT 7 FINAL: PASS ===");
            else
                P("=== EXPERIMENT 7 FINAL: PARTIAL ===");
        }
        /* The decisive anti-debug observation. Report the exact moment the
         * process starts seeing a debugger, and carry on. If BO1's CEG checks
         * or the 0x4C06E0 check reacted to a debugger under Wine, the process
         * would die around here rather than keep logging heartbeats. */
        {
            static int last_dbg = -1;
            int now_dbg = (int)IsDebuggerPresent();
            if (now_dbg != last_dbg) {
                DWORD peb = 0; unsigned char being = 0;
                __asm__ volatile ("movl %%fs:0x30, %0" : "=r"(peb));
                if (peb) rd(peb + 0x02, &being, 1);
                P("  *** debugger state CHANGED at heartbeat %d (t+%lus): "
                  "IsDebuggerPresent %d -> %d, PEB->BeingDebugged=%u",
                  i, (GetTickCount() - g_t0) / 1000, last_dbg, now_dbg, being);
                last_dbg = now_dbg;
            }
        }
        if ((i % 150) == 0) {
            P("  heartbeat %d, t+%lus, dvars=%lu, device=%08lx, IsDebuggerPresent=%d",
              i, (GetTickCount() - g_t0) / 1000, rd32(A_DVAR_COUNT, &ok),
              rd32(A_DX_DEVICE, &ok), (int)IsDebuggerPresent());
        }
        if (quit_after && (int)((GetTickCount() - g_t0) / 1000) >= quit_after) {
            P("  BO1VR_QUIT_AFTER_S=%d reached; ExitProcess(0)", quit_after);
            Sleep(200);
            ExitProcess(0);
        }
        Sleep(100);   /* 100 ms, not 1 s: the game only lives ~10 s under Wine (RESULTS §8), so a debugger that attaches and continues must be able to hit this within a fraction of a second, not on the next whole second. */
    }
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    (void)inst; (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        HANDLE t;
        InitializeCriticalSection(&g_loglock);
        open_log();
        /* Under the loader lock: CreateThread and get out (README / Exp. 4). */
        t = CreateThread(NULL, 0, worker, NULL, 0, NULL);
        if (t) CloseHandle(t);
        else fprintf(stderr, "[bo1probe] CreateThread failed %lu\n", GetLastError());
    }
    return TRUE;
}
