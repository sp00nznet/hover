/*
 * Hover! - static recompilation host.
 *
 * A 32-bit host on pcrecomp's runtime/native32 (the native bridge, callbacks
 * and the machine lock; see its header). What is here is only what is
 * specific to this game: the guest image, the files it expects beside it, the
 * command line, headless capture and the fault report. docs/host.md has the
 * reasoning.
 *
 * Linked at /BASE:0x60000000 (CMakeLists.txt) so 0x00400000 (HOVER.EXE) is
 * free when main() maps the image.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dwmapi.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "native32.h"
#include "recomp_trace.h"

extern const uint32_t hover_entry_va;     /* recomp_dispatch.c */

#define HOVER_BASE 0x00400000u

static DWORD g_watchdog_s;
static int   g_headless;
static char  g_game[MAX_PATH];           /* the game folder, with a trailing '\' */
static char  g_guest_exe[MAX_PATH], g_guest_cmdline[MAX_PATH + 3];

#define ARG(n) MEM32(g_esp + 4 + 4 * (n))
#define RET(v, nargs) do { g_eax = (uint32_t)(v); g_esp += 4 + 4 * (nargs); } while (0)
static const char* gstr(uint32_t va) { return va ? (const char*)(uintptr_t)va : "(null)"; }

/* ------------------------------------------------------------- the guest */

/* The guest is HOVER.EXE in the game folder, not this host: its hInstance
 * (resources, window class) is GetModuleHandleA(NULL), and the paths it builds
 * from its own file name (Sounds\..., mazes\...) have to land in the game
 * folder. */
static void shim_GetModuleHandleA(void) {
    uint32_t name = ARG(0);
    RET(name ? (uint32_t)(uintptr_t)GetModuleHandleA(gstr(name)) : HOVER_BASE, 1);
}

static void shim_GetModuleFileNameA(void) {
    uint32_t h = ARG(0), size = ARG(2);
    char* out = (char*)(uintptr_t)ARG(1);
    if (h == 0 || h == HOVER_BASE) {
        uint32_t n = (uint32_t)strlen(g_guest_exe);
        if (size) {
            if (n >= size) n = size - 1;
            memcpy(out, g_guest_exe, n);
            out[n] = 0;
        }
        RET(n, 3);
    } else {
        RET(GetModuleFileNameA((HMODULE)(uintptr_t)h, out, size), 3);
    }
}

static void shim_GetCommandLineA(void) { RET((uintptr_t)g_guest_cmdline, 0); }

/* DASHRES.DLL is the dashboard art: a resource DLL whose code is the CRT's
 * DllMain and nothing else. Loaded as a data file, none of its original x86
 * runs, and LoadBitmap/FindResource on the handle work as before. */
static void shim_LoadLibraryA(void) {
    const char* name = gstr(ARG(0));
    const char* base = strrchr(name, '\\');
    base = base ? base + 1 : name;
    if (!_stricmp(base, "dashres.dll")) {
        char path[MAX_PATH];
        _snprintf(path, sizeof path - 1, "%s%s", g_game, base);
        path[sizeof path - 1] = 0;
        HMODULE h = LoadLibraryExA(path, NULL, LOAD_LIBRARY_AS_DATAFILE);
        fprintf(stderr, "[module] LoadLibraryA(\"%s\") -> data file %p\n", name, (void*)h);
        RET((uintptr_t)h, 1);
        return;
    }
    HMODULE h = LoadLibraryA(name);
    fprintf(stderr, "[module] LoadLibraryA(\"%s\") -> %p\n", name, (void*)h);
    RET((uintptr_t)h, 1);
}

/* The startup display check (0x0041CEE3, and 0x0041CF6D, the catalog's entry
 * inside it) asks the screen DC for RC_PALETTE and a 256-entry palette, and
 * on every true-colour display since the late 90s says "We have detected that
 * you are not running a 256 color video driver". The game renders into its
 * own 8-bit DIBs either way, so only that check is told what it wants to
 * hear; the renderer's own GetDeviceCaps calls see the real display. */
static void shim_GetDeviceCaps(void) {
    HDC dc = (HDC)(uintptr_t)ARG(0);
    int index = (int)ARG(1);
    int r = GetDeviceCaps(dc, index);
    if (g_cur_func == 0x0041CEE3u || g_cur_func == 0x0041CF6Du) {
        if (index == RASTERCAPS) r |= RC_PALETTE;
        else if (index == SIZEPALETTE) {
            r = 256;
            fprintf(stderr, "[display] the 256-colour check is answered: no warning\n");
        }
    }
    RET((uint32_t)r, 2);
}

/* ------------------------------------------------------------ headless */

/* --headless: nothing reaches the screen (REPO_RULES section 13). The game's
 * own message boxes go to stderr. */
static void shim_MessageBoxA(void) {
    fprintf(stderr, "[messagebox] %s: %s\n", gstr(ARG(2)), gstr(ARG(1)));
    RET(IDOK, 4);
}

static HWND g_main;                      /* the "Hover!" frame window */

/* Headless, the game never hears that it lost activation. It pauses when it
 * does, as the original does in the background: WM_ACTIVATEAPP(FALSE) kills
 * the multimedia timer that paces its frame loop. A headless run's window is
 * never the active one, and whenever the desktop's foreground moved (a person
 * using the machine), a run parked in GetMessage. So its top-level windows
 * are subclassed here, in the host, and the deactivations stop at the host. */
static LRESULT CALLBACK always_active(HWND h, UINT m, WPARAM w, LPARAM l) {
    WNDPROC prev = (WNDPROC)GetPropA(h, "hover.prev");
    if ((m == WM_ACTIVATEAPP && !w) || (m == WM_ACTIVATE && LOWORD(w) == WA_INACTIVE))
        return 0;
    if (m == WM_NCACTIVATE && !w) return DefWindowProcA(h, m, w, l);
    return CallWindowProcA(prev, h, m, w, l);
}

/* Every mode: the host needs the game's frame window (capture, keys).
 * Headless, every top-level window is cloaked (below). */
static void shim_CreateWindowExA(void) {
    int top = !ARG(8) || !(ARG(3) & WS_CHILD);
    uint32_t a[12];
    for (int i = 0; i < 12; i++) a[i] = ARG(i);
    mach_leave();
    DWORD ex = g_headless && top ? (a[0] | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE) & ~WS_EX_APPWINDOW : a[0];
    HWND h = CreateWindowExA(ex, (LPCSTR)(uintptr_t)a[1], (LPCSTR)(uintptr_t)a[2],
                             g_headless && top ? a[3] & ~WS_VISIBLE : a[3], (int)a[4], (int)a[5], (int)a[6],
                             (int)a[7], (HWND)(uintptr_t)a[8], (HMENU)(uintptr_t)a[9],
                             (HINSTANCE)(uintptr_t)a[10], (LPVOID)(uintptr_t)a[11]);
    if (g_headless && top && h) {
        BOOL on = TRUE;
        DwmSetWindowAttribute(h, DWMWA_CLOAK, &on, sizeof on);
        SetPropA(h, "hover.prev", (HANDLE)SetWindowLongPtrA(h, GWLP_WNDPROC, (LONG_PTR)always_active));
        if (a[3] & WS_VISIBLE) ShowWindow(h, SW_SHOWNOACTIVATE);
    }
    mach_enter();
    fprintf(stderr, "[window] CreateWindowExA(\"%s\", %dx%d) -> %shwnd %p\n",
            ARG(2) >> 16 ? gstr(ARG(2)) : "#", (int)ARG(6), (int)ARG(7),
            g_headless && top ? "cloaked " : "", (void*)h);
    if (ARG(2) >> 16 && !strcmp(gstr(ARG(2)), "Hover!")) g_main = h;
    RET((uintptr_t)h, 12);
}

/* Headless, the frame window is cloaked: DWM composes it nowhere, so it
 * never reaches a screen (an RDP session included), but to USER32 it is an
 * ordinary visible window. That matters: Windows makes no WM_PAINT for a
 * hidden window, and the dashboard art is drawn only in WM_PAINT; and MFC's
 * idle loop, which is the game's frame loop, asks IsWindowVisible. Faking
 * paints into a hidden window was tried first and ping-ponged between the
 * frame and its view (docs/host.md). It is shown without activation and
 * kept off the taskbar (and see shim_SetForegroundWindow below).
 * This shim and CreateWindowExA drop the machine lock around the real call,
 * because both run the game's own window procedure inside: its WM_SIZE
 * handler runs inside ShowWindow and waits there for its render thread,
 * which needs the lock to run (a deadlock, before). */
static void shim_ShowWindow(void) {
    HWND w = (HWND)(uintptr_t)ARG(0);
    int cmd = (int)ARG(1);
    if (w == g_main && cmd != SW_HIDE && cmd != SW_MINIMIZE) cmd = SW_SHOWNOACTIVATE;
    mach_leave();
    BOOL r = ShowWindow(w, cmd);
    mach_enter();
    RET(r, 2);
}

/* Headless, the game's window is never activated: it would take the
 * keyboard from whoever is using the desktop, and the moment they clicked
 * elsewhere the game would see WM_ACTIVATE(WA_INACTIVE) and pause, as the
 * original does on alt-tab (a stalled run, before). WS_EX_NOACTIVATE covers
 * clicks; these cover the game asking for it. Scripted keys are posted, so
 * they need no focus. */
static void shim_SetForegroundWindow(void) { RET(TRUE, 1); }
static void shim_SetActiveWindow(void) { RET((uintptr_t)GetActiveWindow(), 1); }

/* --key NAME@MS[+HOLD]: scripted keyboard, in milliseconds from entry.
 * The game takes its menu keys (F2 start, F3 pause) as WM_KEYDOWN through
 * the accelerator table, and steers by polling the keyboard, so a scripted
 * key is both: posted down and up, and reported held by GetAsyncKeyState /
 * GetKeyState in between. Headless, the real keyboard is not read at all:
 * typing on the machine must not steer a recording. */
#define MAX_KEYS 64
static struct { int vk; DWORD at, hold; } g_keys[MAX_KEYS];
static int g_nkeys;
static volatile LONG g_held[256];

static int vk_of(const char* s) {
    static const struct { const char* n; int vk; } names[] = {
        {"UP", VK_UP}, {"DOWN", VK_DOWN}, {"LEFT", VK_LEFT}, {"RIGHT", VK_RIGHT},
        {"SPACE", VK_SPACE}, {"ENTER", VK_RETURN}, {"ESC", VK_ESCAPE}, {"SHIFT", VK_SHIFT},
        {"CTRL", VK_CONTROL}, {"TAB", VK_TAB}};
    for (int i = 0; i < (int)(sizeof names / sizeof names[0]); i++)
        if (!_stricmp(s, names[i].n)) return names[i].vk;
    if ((s[0] == 'F' || s[0] == 'f') && atoi(s + 1) >= 1 && atoi(s + 1) <= 12) return VK_F1 + atoi(s + 1) - 1;
    if (s[0] && !s[1]) return toupper((unsigned char)s[0]);   /* a letter or digit */
    return (int)strtol(s, NULL, 0);                           /* a raw VK code */
}

static int parse_key(const char* arg) {
    char name[16];
    unsigned at = 0, hold = 100;
    if (g_nkeys >= MAX_KEYS || sscanf(arg, "%15[^@]@%u+%u", name, &at, &hold) < 2) return 0;
    g_keys[g_nkeys].vk = vk_of(name) & 0xFF;
    g_keys[g_nkeys].at = at;
    g_keys[g_nkeys++].hold = hold;
    return 1;
}

static DWORD WINAPI key_thread(LPVOID p) {
    int i = (int)(intptr_t)p;
    DWORD t0 = GetTickCount();
    while (!g_main || GetTickCount() - t0 < g_keys[i].at) Sleep(5);
    int vk = g_keys[i].vk;
    fprintf(stderr, "[input] key 0x%02X down at %lu ms\n", vk, GetTickCount() - t0);
    InterlockedIncrement(&g_held[vk]);
    PostMessageA(g_main, WM_KEYDOWN, vk, 1);
    Sleep(g_keys[i].hold);
    InterlockedDecrement(&g_held[vk]);
    PostMessageA(g_main, WM_KEYUP, vk, 0xC0000001u);
    return 0;
}

static void shim_GetAsyncKeyState(void) {
    int vk = ARG(0) & 0xFF;
    SHORT s = g_held[vk] ? (SHORT)0x8001 : g_headless ? 0 : GetAsyncKeyState(vk);
    RET((uint16_t)s, 1);
}

static void shim_GetKeyState(void) {
    int vk = ARG(0) & 0xFF;
    SHORT s = g_held[vk] ? (SHORT)0x8000 : g_headless ? 0 : GetKeyState(vk);
    RET((uint32_t)(int32_t)s, 1);
}

/* A modal dialog would put a window on screen and wait for a person. The
 * first one is Quick Help (115) at every start; OK is what a player clicks.
 * ponytail: every dialog answers IDOK unseen, so the Keyboard Settings or
 * High Score dialogs cannot be driven headless; run the real dialog cloaked
 * and press its buttons if a run ever needs to. */
static void shim_DialogBoxParamA(void) {
    fprintf(stderr, "[headless] dialog %u answered OK unseen\n", ARG(1));
    RET(IDOK, 5);
}

/* ------------------------------------------------------------ capture */

/* The game renders into DIB sections and BitBlts them onto its window: the
 * 3D view (512 wide) and the dashboard pieces, each on its own. A cloaked
 * window cannot be read back, so every blit that lands on the game window is
 * repeated onto a shadow DIB of its client area, and that is what --record
 * pipes to ffmpeg (windowed runs too, so both record the same way). A
 * "frame" is a blit of the 3D view (at least 256x128).
 * ponytail: only BitBlt/StretchBlt are mirrored; GDI text or lines drawn
 * straight onto the window DC are not in the recording. Mirror those too if a
 * screen ever needs them (the menus between levels, say). */
static const char* g_record;
static FILE*   g_ffmpeg;
static HDC     g_shadow_dc;
static uint32_t* g_shadow;               /* top-down BGRA, g_sw x g_sh */
static int     g_sw, g_sh;
static long    g_frames, g_stop_after, g_written;
static DWORD   g_rec_t0;
static long    g_diff_a, g_diff_b;       /* --diff A,B */
static uint32_t* g_diff_shot;

static void finish(void) {
    if (g_ffmpeg) {
        _pclose(g_ffmpeg);
        g_ffmpeg = NULL;
        fprintf(stderr, "[record] %ld frames written to %s\n", g_written, g_record);
    }
}

static int shadow_ready(void) {
    if (g_shadow_dc) return 1;
    RECT rc;
    if (!g_main || !GetClientRect(g_main, &rc) || rc.right < 16 || rc.bottom < 16) return 0;
    g_sw = rc.right & ~1;                /* even: yuv420p */
    g_sh = rc.bottom & ~1;
    BITMAPINFO bi = { { sizeof(BITMAPINFOHEADER), g_sw, -g_sh, 1, 32, BI_RGB } };
    HBITMAP bm = CreateDIBSection(NULL, &bi, DIB_RGB_COLORS, (void**)&g_shadow, NULL, 0);
    g_shadow_dc = CreateCompatibleDC(NULL);
    SelectObject(g_shadow_dc, bm);
    fprintf(stderr, "[capture] window client %ldx%ld\n", rc.right, rc.bottom);
    if (g_record) {
        char cmd[MAX_PATH + 256];
        _snprintf(cmd, sizeof cmd - 1,
                  "ffmpeg -y -loglevel error -f rawvideo -pixel_format bgra -video_size %dx%d "
                  "-framerate 30 -i - -pix_fmt yuv420p \"%s\"", g_sw, g_sh, g_record);
        cmd[sizeof cmd - 1] = 0;
        g_ffmpeg = _popen(cmd, "wb");
        if (!g_ffmpeg) fprintf(stderr, "[record] cannot start ffmpeg (is it on PATH?)\n");
        g_rec_t0 = GetTickCount();
    }
    return 1;
}

/* A fixed 30 fps against the wall clock: a slow stretch holds the last
 * picture, a fast one drops frames, so the video plays at the game's speed. */
static void record_tick(void) {
    if (!g_ffmpeg) return;
    long due = (long)((GetTickCount() - g_rec_t0) * 30 / 1000) + 1;
    while (g_written < due) {
        fwrite(g_shadow, 4, (size_t)g_sw * g_sh, g_ffmpeg);
        g_written++;
    }
}

static void view_presented(void) {
    g_frames++;
    if (g_frames == 1 || g_frames == 100 || g_frames % 1000 == 0)
        fprintf(stderr, "[capture] frame %ld presented\n", g_frames);
    if (g_diff_a && (g_frames == g_diff_a || g_frames == g_diff_b)) {
        size_t n = (size_t)g_sw * g_sh;
        if (g_frames == g_diff_a) {
            g_diff_shot = (uint32_t*)malloc(n * 4);
            memcpy(g_diff_shot, g_shadow, n * 4);
        } else if (g_diff_shot) {
            size_t changed = 0;
            for (size_t i = 0; i < n; i++) changed += g_diff_shot[i] != g_shadow[i];
            fprintf(stderr, "[diff] %u%% of the window changed from frame %ld to %ld\n",
                    (unsigned)(changed * 100 / n), g_diff_a, g_diff_b);
        }
    }
    if (g_stop_after && g_frames >= g_stop_after) {
        fprintf(stderr, "[capture] %ld frames: stopping\n", g_frames);
        finish();
        fflush(stderr);
        ExitProcess(0);
    }
}

/* Repeat a blit that landed on the game window onto the shadow. */
static void mirror(HDC dst, int x, int y, int w, int h, HDC src, int sx, int sy, int sw, int sh,
                   DWORD rop) {
    HWND wnd = WindowFromDC(dst);
    if (!wnd || !(wnd == g_main || IsChild(g_main, wnd)) || !shadow_ready()) return;
    POINT p = { x, y };
    MapWindowPoints(wnd, g_main, &p, 1);
    if (sw < 0) BitBlt(g_shadow_dc, p.x, p.y, w, h, src, sx, sy, rop);
    else StretchBlt(g_shadow_dc, p.x, p.y, w, h, src, sx, sy, sw, sh, rop);
    GdiFlush();
    if (w >= 256 && h >= 128) view_presented();
    record_tick();
}

static void shim_BitBlt(void) {
    HDC dst = (HDC)(uintptr_t)ARG(0), src = (HDC)(uintptr_t)ARG(5);
    int x = ARG(1), y = ARG(2), w = ARG(3), h = ARG(4), sx = ARG(6), sy = ARG(7);
    BOOL ok = BitBlt(dst, x, y, w, h, src, sx, sy, ARG(8));
    if (ok) mirror(dst, x, y, w, h, src, sx, sy, -1, -1, ARG(8));
    RET(ok, 9);
}

static void shim_StretchBlt(void) {
    HDC dst = (HDC)(uintptr_t)ARG(0), src = (HDC)(uintptr_t)ARG(5);
    int x = ARG(1), y = ARG(2), w = ARG(3), h = ARG(4), sx = ARG(6), sy = ARG(7);
    BOOL ok = StretchBlt(dst, x, y, w, h, src, sx, sy, ARG(8), ARG(9), ARG(10));
    if (ok) mirror(dst, x, y, w, h, src, sx, sy, ARG(8), ARG(9), ARG(10));
    RET(ok, 11);
}

static void shim_ExitProcess(void) {
    fprintf(stderr, "[exit] ExitProcess(%u) after %ld frames\n", ARG(0), g_frames);
    finish();
    fflush(stderr);
    ExitProcess(ARG(0));
}

#define GUEST_SHIMS \
    { "GetModuleHandleA", shim_GetModuleHandleA }, \
    { "GetModuleFileNameA", shim_GetModuleFileNameA }, \
    { "GetCommandLineA", shim_GetCommandLineA }, \
    { "LoadLibraryA", shim_LoadLibraryA }, \
    { "GetDeviceCaps", shim_GetDeviceCaps }, \
    { "GetAsyncKeyState", shim_GetAsyncKeyState }, \
    { "GetKeyState", shim_GetKeyState }, \
    { "BitBlt", shim_BitBlt }, \
    { "StretchBlt", shim_StretchBlt }, \
    { "CreateWindowExA", shim_CreateWindowExA }, \
    { "ExitProcess", shim_ExitProcess }

static native32_shim_t g_shims[] = { GUEST_SHIMS };
static native32_shim_t g_headless_shims[] = {
    GUEST_SHIMS,
    { "MessageBoxA", shim_MessageBoxA },
    { "ShowWindow", shim_ShowWindow },
    { "DialogBoxParamA", shim_DialogBoxParamA },
    { "SetForegroundWindow", shim_SetForegroundWindow },
    { "SetActiveWindow", shim_SetActiveWindow },
};

/* ------------------------------------------------------------ reports */

recomp_func_t recomp_lookup_manual(uint32_t va) { (void)va; return NULL; }

/* Added after native32's own handler, so callbacks are resolved first and
 * only real faults get here. */
static LONG CALLBACK crash(EXCEPTION_POINTERS* ep) {
    EXCEPTION_RECORD* r = ep->ExceptionRecord;
    if ((r->ExceptionCode & 0xF0000000u) != 0xC0000000u) return EXCEPTION_CONTINUE_SEARCH;
    fprintf(stderr, "\n=== fault 0x%08lX at 0x%p ===\n", r->ExceptionCode, r->ExceptionAddress);
    if (r->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && r->NumberParameters >= 2) {
        ULONG_PTR op = r->ExceptionInformation[0];
        uint32_t at = (uint32_t)r->ExceptionInformation[1];
        fprintf(stderr, "  %s of 0x%08X%s\n", op == 0 ? "read" : op == 1 ? "write" : "execute", at,
                native32_in_guest(at) ? " (inside the guest image)" : at < 0x10000 ? " (null/low)" : "");
    }
    recomp_trace_flush();
    fprintf(stderr, "  in lifted sub_%08X, last native call %s\n", g_cur_func, g_cur_import);
    fprintf(stderr, "  eax=%08X ecx=%08X edx=%08X ebx=%08X esp=%08X ebp=%08X esi=%08X edi=%08X\n",
            g_eax, g_ecx, g_edx, g_ebx, g_esp, g_ebp, g_esi, g_edi);
    native32_dump_icalls(12);
    recomp_dump_trace("fault");
    fflush(stderr);
    TerminateProcess(GetCurrentProcess(), 3);
    return EXCEPTION_CONTINUE_SEARCH;
}

/* Where every other thread is: its eip and the host return addresses on its
 * stack, newest first. `py -3 tools/addr2line.py` names them. The watchdog
 * prints it, because a stall is a thread waiting on another. */
static void dump_threads(void) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 te = { sizeof te };
    HMODULE self = GetModuleHandleA(NULL);
    uintptr_t lo = (uintptr_t)self, hi = lo + 0x4000000;   /* the host image and its lifted code */
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != GetCurrentProcessId() || te.th32ThreadID == GetCurrentThreadId()) continue;
        HANDLE t = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, te.th32ThreadID);
        CONTEXT c = { 0 };
        c.ContextFlags = CONTEXT_CONTROL;
        if (t && SuspendThread(t) != (DWORD)-1) {
            if (GetThreadContext(t, &c)) {
                fprintf(stderr, "  thread %lu: eip=%08lX esp=%08lX host frames:", te.th32ThreadID, c.Eip, c.Esp);
                MEMORY_BASIC_INFORMATION mbi;
                int n = 0;
                if (VirtualQuery((void*)(uintptr_t)c.Esp, &mbi, sizeof mbi))
                    for (uint32_t* p = (uint32_t*)(uintptr_t)c.Esp;
                         (uint8_t*)p + 4 <= (uint8_t*)mbi.BaseAddress + mbi.RegionSize && n < 16; p++)
                        if (*p >= lo && *p < hi) { fprintf(stderr, " %08X", *p); n++; }
                fprintf(stderr, "\n");
            }
            ResumeThread(t);
        }
        if (t) CloseHandle(t);
    }
    CloseHandle(snap);
}

static DWORD WINAPI watchdog(LPVOID unused) {
    (void)unused;
    Sleep(g_watchdog_s * 1000);
    dump_threads();
    fprintf(stderr, "\n[watchdog] %lu s: in sub_%08X, last native call %s, %u indirect calls\n",
            g_watchdog_s, g_cur_func, g_cur_import, g_icall_count);
    native32_dump_icalls(8);
    fflush(stderr);
    TerminateProcess(GetCurrentProcess(), 4);
    return 0;
}

int main(int argc, char** argv) {
    const char* game = "game\\hover";
    int run = 0;
    for (int i = 1; i < argc; i++) {
        int n = recomp_trace_arg(argc, argv, i);
        if (n) { i += n - 1; continue; }
        if (!strcmp(argv[i], "--run")) run = 1;
        else if (!strcmp(argv[i], "--headless")) g_headless = 1;
        else if (!strcmp(argv[i], "--key") && i + 1 < argc) {
            if (!parse_key(argv[++i])) { fprintf(stderr, "bad --key %s\n", argv[i]); return 1; }
        }
        else if (!strcmp(argv[i], "--record") && i + 1 < argc) {
            /* Absolute now: --run moves into the game folder before ffmpeg starts. */
            static char rec[MAX_PATH];
            GetFullPathNameA(argv[++i], MAX_PATH, rec, NULL);
            g_record = rec;
        }
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) g_stop_after = atol(argv[++i]);
        else if (!strcmp(argv[i], "--diff") && i + 1 < argc) {
            if (sscanf(argv[++i], "%ld,%ld", &g_diff_a, &g_diff_b) != 2 || g_diff_a < 1 || g_diff_b <= g_diff_a) {
                fprintf(stderr, "bad --diff %s (want A,B with 0 < A < B)\n", argv[i]);
                return 1;
            }
        }
        else if (!strcmp(argv[i], "--game") && i + 1 < argc) game = argv[++i];
        else if (!strcmp(argv[i], "--watchdog") && i + 1 < argc) g_watchdog_s = strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--native-trace")) native32_trace_native = 1;
        else if (!strcmp(argv[i], "--callbacks")) native32_trace_callbacks = 1;
        else {
            printf("usage: hover [--run] [--headless] [--record out.mp4] [--frames N] [--diff A,B]\n"
                   "             [--key NAME@MS[+HOLD]] [--game game\\hover] [--watchdog S]\n"
                   "             [--native-trace] [--callbacks]\n");
            recomp_trace_help();
            return argv[i][1] == 'h' || argv[i][2] == 'h' ? 0 : 1;
        }
    }
    GetFullPathNameA(game, MAX_PATH - 1, g_game, NULL);
    if (g_game[strlen(g_game) - 1] != '\\') strcat(g_game, "\\");
    _snprintf(g_guest_exe, sizeof g_guest_exe - 1, "%sHOVER.EXE", g_game);
    _snprintf(g_guest_cmdline, sizeof g_guest_cmdline - 1, "\"%s\"", g_guest_exe);

    /* Headless runs start from a fresh profile: the game keeps its settings
     * and high scores under HKCU\Software\Microsoft\Hover!, and a recording
     * must not depend on (or change) what a player chose. HKCU is pointed at
     * a scratch key that is emptied every start. */
    if (g_headless) {
        HKEY k;
        RegDeleteTreeA(HKEY_CURRENT_USER, "Software\\hover-recomp\\headless");
        if (RegCreateKeyA(HKEY_CURRENT_USER, "Software\\hover-recomp\\headless", &k) == ERROR_SUCCESS)
            RegOverridePredefKey(HKEY_CURRENT_USER, k);
    }
    native32_init();
    AddVectoredExceptionHandler(0, crash);
    printf("Hover! recomp host\n  lifted functions in dispatch: %u\n", recomp_dispatch_count);

    native32_shim_t* shims = g_headless ? g_headless_shims : g_shims;
    int nshims = g_headless ? (int)(sizeof g_headless_shims / sizeof g_headless_shims[0])
                            : (int)(sizeof g_shims / sizeof g_shims[0]);
    uint32_t span = native32_map(g_guest_exe, HOVER_BASE);
    if (!span) { fprintf(stderr, "cannot map %s at 0x%08X\n", g_guest_exe, HOVER_BASE); return 1; }
    printf("  mapped HOVER.EXE: 0x%08X-0x%08X\n", HOVER_BASE, HOVER_BASE + span);
    if (native32_bind(HOVER_BASE, shims, nshims) != 0) return 1;

    if (!run) {
        printf("\n(dry run: image mapped and bound; --run enters 0x%08X)\n", hover_entry_va);
        return 0;
    }
    /* The game opens mazes\ and Sounds\ relative to the working directory. */
    if (!SetCurrentDirectoryA(g_game)) { fprintf(stderr, "cannot enter %s\n", g_game); return 1; }
    if (g_watchdog_s) CloseHandle(CreateThread(NULL, 0, watchdog, NULL, 0, NULL));
    for (int i = 0; i < g_nkeys; i++)
        CloseHandle(CreateThread(NULL, 0, key_thread, (LPVOID)(intptr_t)i, 0, NULL));
    printf("  entering 0x%08X\n\n", hover_entry_va);
    fflush(stdout);
    native32_call_guest(hover_entry_va, 0, NULL);
    printf("\nentry returned eax=%08X\n", g_eax);
    return (int)g_eax;
}
