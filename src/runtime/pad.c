/*
 * Xbox controller (XInput) support and the in-game "Recomp" menu that sets
 * it up. docs/controller.md has the design; the short version:
 *
 * Hover! reads its steering by polling GetAsyncKeyState for the keys in its
 * own key table (set by Options > Player Controls > Set Keys), and its menu
 * keys (F2 new game, F3 pause) as WM_KEYDOWN through the accelerator table.
 * So the pad is a keyboard: each action names the key the game has bound to
 * it *right now*, read from that table on every poll, and host.c's key shims
 * report it held. Presses and releases are also posted as WM_KEYDOWN/UP, as
 * a real key would be. Rebinding keys in the game moves the pad with them.
 *
 * The poll runs on its own thread, not in the key shim: while the game is
 * paused nothing polls the keyboard, and Start still has to unpause it.
 *
 * Settings are the [pad] section of hover.ini (beside the exe), written with
 * defaults on first run and rewritten by the menu.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <xinput.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pad.h"
#include "recomp_types.h"

/* The game's key table (Keyboard Settings and Player Controls write it). */
#define KEY_FORWARD 0x004606ECu
#define KEY_REVERSE 0x004606F0u
#define KEY_LEFT    0x004606F4u
#define KEY_RIGHT   0x004606F8u
#define KEY_JUMP    0x004606E8u
#define KEY_WALL    0x004606E4u
#define KEY_CLOAK   0x004606E0u

/* Inputs: XInput's 16 button bits, then the analogue ones as virtual bits. */
enum { IN_LT = 1u << 16, IN_RT = 1u << 17,
       IN_LUP = 1u << 18, IN_LDOWN = 1u << 19, IN_LLEFT = 1u << 20, IN_LRIGHT = 1u << 21,
       IN_RUP = 1u << 22, IN_RDOWN = 1u << 23, IN_RLEFT = 1u << 24, IN_RRIGHT = 1u << 25 };

static const struct { const char* name; uint32_t bit; } g_inputs[] = {
    {"DPAD_UP", XINPUT_GAMEPAD_DPAD_UP}, {"DPAD_DOWN", XINPUT_GAMEPAD_DPAD_DOWN},
    {"DPAD_LEFT", XINPUT_GAMEPAD_DPAD_LEFT}, {"DPAD_RIGHT", XINPUT_GAMEPAD_DPAD_RIGHT},
    {"START", XINPUT_GAMEPAD_START}, {"BACK", XINPUT_GAMEPAD_BACK},
    {"LS", XINPUT_GAMEPAD_LEFT_THUMB}, {"RS", XINPUT_GAMEPAD_RIGHT_THUMB},
    {"LB", XINPUT_GAMEPAD_LEFT_SHOULDER}, {"RB", XINPUT_GAMEPAD_RIGHT_SHOULDER},
    {"A", XINPUT_GAMEPAD_A}, {"B", XINPUT_GAMEPAD_B}, {"X", XINPUT_GAMEPAD_X}, {"Y", XINPUT_GAMEPAD_Y},
    {"LT", IN_LT}, {"RT", IN_RT},
    {"LSTICK_UP", IN_LUP}, {"LSTICK_DOWN", IN_LDOWN}, {"LSTICK_LEFT", IN_LLEFT}, {"LSTICK_RIGHT", IN_LRIGHT},
    {"RSTICK_UP", IN_RUP}, {"RSTICK_DOWN", IN_RDOWN}, {"RSTICK_LEFT", IN_RLEFT}, {"RSTICK_RIGHT", IN_RRIGHT},
};

/* Actions, their default bindings, and the key each one presses: an address
 * in the game's key table, or a fixed menu key. */
static struct {
    const char* name; const char* dflt; uint32_t table; int fixed_vk; uint32_t mask;
} g_act[] = {
    {"forward",  "RT,LSTICK_UP,DPAD_UP",     KEY_FORWARD, 0,      0},
    {"reverse",  "LT,LSTICK_DOWN,DPAD_DOWN", KEY_REVERSE, 0,      0},
    {"left",     "LSTICK_LEFT,DPAD_LEFT",    KEY_LEFT,    0,      0},
    {"right",    "LSTICK_RIGHT,DPAD_RIGHT",  KEY_RIGHT,   0,      0},
    {"jump",     "A",                        KEY_JUMP,    0,      0},
    {"wall",     "X",                        KEY_WALL,    0,      0},
    {"cloak",    "B",                        KEY_CLOAK,   0,      0},
    {"pause",    "START",                    0,           VK_F3,  0},
    {"new_game", "BACK",                     0,           VK_F2,  0},
};
#define NACT ((int)(sizeof g_act / sizeof g_act[0]))

static char  g_ini[MAX_PATH];
static int   g_enabled = 1, g_slot, g_deadzone = 30, g_trigger = 20;  /* slot 0 = first connected */
static HWND  g_wnd;
static volatile LONG g_held[256];       /* keys the pad holds, by VK */
static volatile int  g_connected = -1;  /* XInput user index in use, -1 = none */
static DWORD (WINAPI *p_get)(DWORD, XINPUT_STATE*);

/* ------------------------------------------------------------ settings */

static uint32_t parse_inputs(const char* s) {
    uint32_t m = 0;
    char buf[256], *tok, *ctx = NULL;
    strncpy(buf, s, sizeof buf - 1);
    buf[sizeof buf - 1] = 0;
    for (tok = strtok_s(buf, ", ", &ctx); tok; tok = strtok_s(NULL, ", ", &ctx)) {
        int found = 0;
        for (int i = 0; i < (int)(sizeof g_inputs / sizeof g_inputs[0]); i++)
            if (!_stricmp(tok, g_inputs[i].name)) { m |= g_inputs[i].bit; found = 1; }
        if (!found) fprintf(stderr, "[pad] hover.ini: unknown input \"%s\"\n", tok);
    }
    return m;
}

static void put_int(const char* key, int v) {
    char b[16];
    _snprintf(b, sizeof b, "%d", v);
    WritePrivateProfileStringA("pad", key, b, g_ini);
}

/* Load [pad]; any key that is missing is written with its default, so the
 * file always lists everything there is to change. */
static void load(void) {
    char b[256];
    if (!GetPrivateProfileStringA("pad", "enabled", "", b, sizeof b, g_ini)) {
        WritePrivateProfileStringA("pad", NULL, NULL, g_ini);   /* a fresh, ordered section */
        put_int("enabled", 1);
        put_int("controller", 0);
        put_int("deadzone", 30);
        put_int("trigger", 20);
    }
    g_enabled  = GetPrivateProfileIntA("pad", "enabled", 1, g_ini);
    g_slot     = GetPrivateProfileIntA("pad", "controller", 0, g_ini);
    g_deadzone = GetPrivateProfileIntA("pad", "deadzone", 30, g_ini);
    g_trigger  = GetPrivateProfileIntA("pad", "trigger", 20, g_ini);
    if (g_slot < 0 || g_slot > 4) g_slot = 0;
    if (g_deadzone < 1 || g_deadzone > 95) g_deadzone = 30;
    if (g_trigger < 1 || g_trigger > 95) g_trigger = 20;
    for (int i = 0; i < NACT; i++) {
        if (!GetPrivateProfileStringA("pad", g_act[i].name, "", b, sizeof b, g_ini)) {
            WritePrivateProfileStringA("pad", g_act[i].name, g_act[i].dflt, g_ini);
            strcpy(b, g_act[i].dflt);
        }
        g_act[i].mask = parse_inputs(b);
    }
}

/* ------------------------------------------------------------ polling */

static uint32_t read_inputs(const XINPUT_GAMEPAD* g) {
    uint32_t m = g->wButtons;
    int dz = 32767 * g_deadzone / 100, tr = 255 * g_trigger / 100;
    if (g->bLeftTrigger > tr) m |= IN_LT;
    if (g->bRightTrigger > tr) m |= IN_RT;
    if (g->sThumbLY >  dz) m |= IN_LUP;
    if (g->sThumbLY < -dz) m |= IN_LDOWN;
    if (g->sThumbLX < -dz) m |= IN_LLEFT;
    if (g->sThumbLX >  dz) m |= IN_LRIGHT;
    if (g->sThumbRY >  dz) m |= IN_RUP;
    if (g->sThumbRY < -dz) m |= IN_RDOWN;
    if (g->sThumbRX < -dz) m |= IN_RLEFT;
    if (g->sThumbRX >  dz) m |= IN_RRIGHT;
    return m;
}

static void set_key(int vk, int down) {
    if (vk <= 0 || vk > 255) return;
    if (down) InterlockedIncrement(&g_held[vk]);
    else InterlockedDecrement(&g_held[vk]);
    if (g_wnd) PostMessageA(g_wnd, down ? WM_KEYDOWN : WM_KEYUP, vk, down ? 1 : 0xC0000001u);
}

/* One poll: press and release keys to match the controller. `g` is NULL
 * when no controller is connected, which releases everything. */
static int g_was[NACT], g_vk_was[NACT];

static void pad_step(const XINPUT_GAMEPAD* g) {
    uint32_t in = g ? read_inputs(g) : 0;
    for (int i = 0; i < NACT; i++) {
        int on = (in & g_act[i].mask) != 0;
        int vk = g_act[i].table ? (int)(MEM32(g_act[i].table) & 0xFF) : g_act[i].fixed_vk;
        if (on && g_was[i] && vk != g_vk_was[i]) {    /* rebound in game while held */
            set_key(g_vk_was[i], 0);
            g_was[i] = 0;
        }
        if (on != g_was[i]) {
            set_key(on ? vk : g_vk_was[i], on);
            g_vk_was[i] = vk;
            g_was[i] = on;
        }
    }
}

static DWORD WINAPI poll_thread(LPVOID unused) {
    int logged = -2;
    (void)unused;
    for (;;) {
        Sleep(8);
        XINPUT_STATE st;
        int user = -1;
        if (g_enabled && p_get) {
            for (int u = g_slot ? g_slot - 1 : 0; u < (g_slot ? g_slot : 4); u++)
                if (p_get(u, &st) == ERROR_SUCCESS) { user = u; break; }
        }
        if (user != logged) {
            if (user >= 0) fprintf(stderr, "[pad] controller %d connected\n", user + 1);
            else if (logged >= 0) fprintf(stderr, "[pad] controller disconnected\n");
            logged = user;
        }
        g_connected = user;
        pad_step(user >= 0 ? &st.Gamepad : NULL);
    }
}

/* hover.exe --pad-selftest: the mapping, without a controller. Needs the
 * guest image mapped (the key table is guest memory), which main() does
 * first. Uses a scratch ini so the real one is untouched. */
int pad_selftest(void) {
    char tmp[MAX_PATH];
    GetTempPathA(MAX_PATH, tmp);
    strcat(tmp, "hover-pad-selftest.ini");
    DeleteFileA(tmp);
    strcpy(g_ini, tmp);
    load();
    MEM32(KEY_FORWARD) = VK_UP;
    MEM32(KEY_LEFT) = VK_LEFT;
    MEM32(KEY_JUMP) = 'A';
    XINPUT_GAMEPAD g = {0};
    int fails = 0;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "[pad] selftest FAILED: %s\n", #c); fails++; } } while (0)
    g.bRightTrigger = 200;                          /* RT: forward */
    g.sThumbLX = -20000;                            /* left stick, past 30%: left */
    pad_step(&g);
    CHECK(pad_held(VK_UP) && pad_held(VK_LEFT) && !pad_held(VK_RIGHT));
    g.sThumbLX = -5000;                             /* inside the deadzone */
    pad_step(&g);
    CHECK(pad_held(VK_UP) && !pad_held(VK_LEFT));
    MEM32(KEY_FORWARD) = 'W';                       /* rebound in game, still held */
    pad_step(&g);
    CHECK(pad_held('W') && !pad_held(VK_UP));
    g.wButtons = XINPUT_GAMEPAD_A | XINPUT_GAMEPAD_START;
    pad_step(&g);
    CHECK(pad_held('A') && pad_held(VK_F3));
    pad_step(NULL);                                 /* unplugged: all released */
    CHECK(!pad_held('W') && !pad_held('A') && !pad_held(VK_F3));
    CHECK(parse_inputs("rt, DPAD_UP") == (IN_RT | XINPUT_GAMEPAD_DPAD_UP));
    WritePrivateProfileStringA("pad", "jump", "Y,LB", g_ini);
    load();
    CHECK(g_act[4].mask == (XINPUT_GAMEPAD_Y | XINPUT_GAMEPAD_LEFT_SHOULDER));
    CHECK(GetPrivateProfileIntA("pad", "deadzone", 0, g_ini) == 30);   /* defaults written */
#undef CHECK
    DeleteFileA(tmp);
    fprintf(stderr, "[pad] selftest %s\n", fails ? "FAILED" : "OK");
    return fails;
}

int pad_held(int vk) { return g_held[vk & 0xFF] > 0; }

static int g_started;

void pad_start(const char* ini, HWND wnd) {
    strncpy(g_ini, ini, sizeof g_ini - 1);
    g_wnd = wnd;
    load();
    if (g_started++) return;
    HMODULE x = LoadLibraryA("xinput1_4.dll");
    if (!x) x = LoadLibraryA("xinput9_1_0.dll");
    if (x) p_get = (DWORD (WINAPI*)(DWORD, XINPUT_STATE*))GetProcAddress(x, "XInputGetState");
    if (!p_get) fprintf(stderr, "[pad] no XInput on this system: controllers are off\n");
    fprintf(stderr, "[pad] %s, settings in %s\n", g_enabled ? "on" : "off", g_ini);
    CloseHandle(CreateThread(NULL, 0, poll_thread, NULL, 0, NULL));
}

/* ------------------------------------------------------------ menu */

enum { ID_STATUS = 0x6F00, ID_ENABLE, ID_OPEN_INI, ID_RELOAD,
       ID_DZ = 0x6F10,      /* + 0..2 */
       ID_SLOT = 0x6F20 };  /* + 0..4 */
static const int g_dz_presets[] = {15, 30, 45};
static HMENU g_menu;

void pad_add_menu(HWND frame) {
    HMENU bar = GetMenu(frame);
    if (!bar || g_menu) return;
    HMENU dz = CreatePopupMenu(), slot = CreatePopupMenu();
    AppendMenuA(dz, MF_STRING, ID_DZ + 0, "&Small (15%)");
    AppendMenuA(dz, MF_STRING, ID_DZ + 1, "&Medium (30%)");
    AppendMenuA(dz, MF_STRING, ID_DZ + 2, "&Large (45%)");
    AppendMenuA(slot, MF_STRING, ID_SLOT + 0, "&Any (first connected)");
    for (int i = 1; i <= 4; i++) {
        char t[16];
        _snprintf(t, sizeof t, "Controller &%d", i);
        AppendMenuA(slot, MF_STRING, ID_SLOT + i, t);
    }
    g_menu = CreatePopupMenu();
    AppendMenuA(g_menu, MF_STRING | MF_GRAYED, ID_STATUS, "Controller: none");
    AppendMenuA(g_menu, MF_STRING, ID_ENABLE, "&Use Xbox controller");
    AppendMenuA(g_menu, MF_POPUP, (UINT_PTR)dz, "Stick &deadzone");
    AppendMenuA(g_menu, MF_POPUP, (UINT_PTR)slot, "&Which controller");
    AppendMenuA(g_menu, MF_SEPARATOR, 0, NULL);
    AppendMenuA(g_menu, MF_STRING, ID_OPEN_INI, "&Edit settings (hover.ini)...");
    AppendMenuA(g_menu, MF_STRING, ID_RELOAD, "&Reload settings");
    AppendMenuA(bar, MF_POPUP, (UINT_PTR)g_menu, "&Recomp");
    DrawMenuBar(frame);
    fputs("[menu] Recomp menu added\n", stderr);
}

/* MFC greys every item it has no handler for when a popup opens, so the
 * frame's subclass calls this after MFC's WM_INITMENUPOPUP. */
void pad_update_menu(HMENU popup) {
    if (!g_menu) return;
    char t[64];
    if (g_connected >= 0) _snprintf(t, sizeof t, "Controller %d: connected", g_connected + 1);
    else _snprintf(t, sizeof t, !g_started ? "Controller: off (headless run)"
                                : p_get ? "Controller: none connected" : "Controller: no XInput");
    ModifyMenuA(g_menu, ID_STATUS, MF_BYCOMMAND | MF_STRING | MF_GRAYED, ID_STATUS, t);
    for (UINT id = ID_ENABLE; id <= ID_RELOAD; id++) EnableMenuItem(popup, id, MF_BYCOMMAND | MF_ENABLED);
    CheckMenuItem(g_menu, ID_ENABLE, MF_BYCOMMAND | (g_enabled ? MF_CHECKED : MF_UNCHECKED));
    for (int i = 0; i < 3; i++) {
        EnableMenuItem(popup, ID_DZ + i, MF_BYCOMMAND | MF_ENABLED);
        CheckMenuItem(popup, ID_DZ + i, MF_BYCOMMAND | (g_deadzone == g_dz_presets[i] ? MF_CHECKED : MF_UNCHECKED));
    }
    for (int i = 0; i <= 4; i++) {
        EnableMenuItem(popup, ID_SLOT + i, MF_BYCOMMAND | MF_ENABLED);
        CheckMenuItem(popup, ID_SLOT + i, MF_BYCOMMAND | (g_slot == i ? MF_CHECKED : MF_UNCHECKED));
    }
}

int pad_command(UINT id) {
    if (id == ID_ENABLE) put_int("enabled", !g_enabled);
    else if (id >= ID_DZ && id < ID_DZ + 3) put_int("deadzone", g_dz_presets[id - ID_DZ]);
    else if (id >= ID_SLOT && id <= ID_SLOT + 4) put_int("controller", (int)(id - ID_SLOT));
    else if (id == ID_OPEN_INI) { ShellExecuteA(NULL, "open", g_ini, NULL, NULL, SW_SHOWNORMAL); return 1; }
    else if (id != ID_RELOAD) return 0;
    load();
    return 1;
}
