/*
 * Local multiplayer: split screen for 2 to 4 players on one PC.
 * docs/multiplayer.md has the research this rests on and the plan beyond it.
 *
 * Hover! has one human craft (CHumanPlayer, inside the document at
 * [0x00460970]+0x818C) and robots (CRobotPlayer). Humans and robots drive
 * through the same physics: each tick every craft's Think writes a turn
 * (+0x1F0) and a thrust (+0x1F4), 16.16 fixed point in -1.0..+1.0, and
 * CPlayer::Think (0x00409110) integrates them. So:
 *
 *   seats   player 1 is the game's own human; players 2-4 each take over a
 *           robot (the first, second, third CRobotPlayer in the level). The
 *           robots' Think (0x00409300) is only ever reached through a virtual
 *           call, so recomp_lookup_manual hands it to seat_think here, which
 *           writes the seat's stick into turn/thrust and runs the physics.
 *   views   the frame draw (0x00402250) runs once per player on the render
 *           thread, with the camera reads patched at lift time (run_lift.py:
 *           0x00402291, 0x004022AA, 0x004022BA) to ask hover_cam_obj() whose
 *           eyes to use. The game already renders twice a frame (the rear-view
 *           mirror is a second camera), so this is the same thing N times.
 *           host.c puts each pass's blits in that player's cell of one
 *           capture buffer; the presenter and --record just see a bigger
 *           picture.
 *
 * Players in robot seats play for the robots' team (the flag rules go by
 * class), so two players is one against the other. Everything else the
 * game only does for "the human" (its HUD, powerups, sounds) is still
 * player 1's: see the roadmap.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <xinput.h>
#include <stdio.h>
#include <stdlib.h>

#include "mp.h"
#include "pad.h"
#include "recomp_types.h"

/* Lifted functions this calls (the generated recomp_funcs.h declares them
 * too; CI compiles this file without the generated tree). */
void sub_00402250(void);   /* the frame draw */
void sub_00409110(void);   /* CPlayer::Think: physics */
void sub_00409300(void);   /* CRobotPlayer::Think: AI, then physics */

#define G_DOC         0x00460970u
#define DOC_HUMAN     0x818Cu
#define DOC_THINKERS  0x83ACu            /* CObList of everything that thinks */
#define VT_ROBOT      0x004BCCE0u
#define G_GAME_STATE  0x00460728u        /* 3 = playing */

static char g_ini[MAX_PATH];
static volatile int g_players = 1;
static volatile LONG g_layout;           /* bumped when the view layout changes */
static volatile int g_view;              /* the render pass now running */
static volatile int g_in_views;          /* inside hover_render_views... */
static volatile DWORD g_views_thread;    /* ...on this thread (the render thread) */
static int (*g_key)(int vk);

/* ------------------------------------------------------------ seats */

/* Seats are robots of their own: every level's table entry gets one more
 * flag-runner per extra player (level table 0x004C4000, 0x58 a level: +0x28
 * hunters, +0x2C flag-runners), so the level keeps the AI robots it was
 * designed with. The loader creates hunters, then runners, so the seats are
 * the last robots in the list. The table is read at each level load, so a
 * change of player count applies from the next level. */
#define LEVEL_TABLE   0x004C4000u
#define LEVEL_SIZE    0x58u
#define LEVELS        20                 /* entry 20 is the attract demo: no robots */
static uint32_t g_runners[LEVELS];      /* the shipped counts, before any seats */
static int g_table_saved;

void mp_apply_table(void) {
    for (int l = 0; l < LEVELS; l++) {
        uint32_t at = LEVEL_TABLE + l * LEVEL_SIZE + 0x2C;
        if (!g_table_saved) g_runners[l] = MEM32(at);
        MEM32(at) = g_runners[l] + (uint32_t)(g_players - 1);
    }
    g_table_saved = 1;
}

/* The robot in seat `s` (1 = player 2), or 0: none this level, or no level. */
static uint32_t seat_craft(int s) {
    uint32_t doc = MEM32(G_DOC), robots[64];
    int n = 0;
    if (!doc || s < 1) return 0;
    for (uint32_t node = MEM32(doc + DOC_THINKERS); node && n < 64; node = MEM32(node)) {
        uint32_t o = MEM32(node + 8);
        if (MEM32(o) == VT_ROBOT) robots[n++] = o;
    }
    int i = n - (g_players - 1) + (s - 1);
    return i >= 0 && i < n ? robots[i] : 0;
}

/* Seat s's controls: turn and thrust in -1..1. Pad s+1 (XInput index s);
 * seat 1 also answers to I/J/K/L on the keyboard. */
static void seat_input(int s, double* turn, double* thrust) {
    XINPUT_GAMEPAD g;
    double t = 0, f = 0;
    if (pad_read(s, &g)) {
        double dz = 0.25;
        double x = g.sThumbLX / 32767.0, y = g.sThumbLY / 32767.0;
        if (x > dz || x < -dz) t += (x - (x > 0 ? dz : -dz)) / (1 - dz);
        if (y > dz || y < -dz) f += (y - (y > 0 ? dz : -dz)) / (1 - dz);
        f += (g.bRightTrigger - g.bLeftTrigger) / 255.0;
        if (g.wButtons & XINPUT_GAMEPAD_DPAD_LEFT) t -= 1;
        if (g.wButtons & XINPUT_GAMEPAD_DPAD_RIGHT) t += 1;
        if (g.wButtons & XINPUT_GAMEPAD_DPAD_UP) f += 1;
        if (g.wButtons & XINPUT_GAMEPAD_DPAD_DOWN) f -= 1;
    }
    if (s == 1 && g_key) {
        if (g_key('J')) t -= 1;
        if (g_key('L')) t += 1;
        if (g_key('I')) f += 1;
        if (g_key('K')) f -= 1;
    }
    *turn = t < -1 ? -1 : t > 1 ? 1 : t;
    *thrust = f < -1 ? -1 : f > 1 ? 1 : f;
}

/* Replaces CRobotPlayer::Think (vtable slot 7, 0x00409300) for every robot:
 * a robot in a player's seat gets the player's controls, the rest their AI.
 * The spin-out and airborne handling is CRobotPlayer::Think's own
 * (0x00409300: +0x5C, +0x158, +0x15C), then the shared physics. */
static void seat_think(void) {
    uint32_t self = g_ecx;
    int seat = 0;
    for (int s = 1; s < g_players && !seat; s++)
        if (seat_craft(s) == self) seat = s;
    if (!seat) { sub_00409300(); return; }
    if (MEM32(self + 0x5C) == 0) {
        MEM32(self + 0x15C) = MEM32(self + 0x158) ? 0 : 0xFFFFFFFFu;
        if (MEM32(G_GAME_STATE) == 3) {
            double t, f;
            seat_input(seat, &t, &f);
            MEM32(self + 0x1F0) = (uint32_t)(int32_t)(t * 65536);
            MEM32(self + 0x1F4) = (uint32_t)(int32_t)(f * 65536);
        }
    }
    g_ecx = self;
    sub_00409110();          /* CPlayer::Think: the physics; its ret pops our return address */
}

recomp_func_t mp_lookup(uint32_t va) {
    return va == 0x00409300u && g_players > 1 ? seat_think : NULL;
}

/* ------------------------------------------------------------ views */

/* The camera reads in 0x00402250, patched at lift time: `doc` is the
 * document, and the craft returned is the one whose eyes this pass uses. */
uint32_t hover_cam_obj(uint32_t doc) {
    uint32_t craft = g_view ? seat_craft(g_view) : 0;
    return craft ? craft : doc + DOC_HUMAN;
}

/* Replaces the render thread's call to the frame draw (0x0041F012): the draw
 * once per player, secondary views first and player 1 last, so what the game
 * sees afterwards (registers, its frame state) is exactly one normal draw.
 * 0x00402250 is thiscall with no stack arguments and a plain ret, so each
 * pass starts from the same registers and the same return-address slot. */
void hover_render_views(void) {
    uint32_t eax = g_eax, ecx = g_ecx, edx = g_edx, ebx = g_ebx, esp = g_esp, ebp = g_ebp,
             esi = g_esi, edi = g_edi;
    int n = g_players;
    g_views_thread = GetCurrentThreadId();
    g_in_views = 1;
    for (int v = n - 1; v >= 0; v--) {
        g_eax = eax; g_ecx = ecx; g_edx = edx; g_ebx = ebx;
        g_esp = esp; g_ebp = ebp; g_esi = esi; g_edi = edi;
        g_view = v;
        sub_00402250();
    }
    g_view = 0;
    g_in_views = 0;
}

int mp_view(void) { return g_view; }

/* Blits outside the render passes (the dashboard's background, drawn only in
 * WM_PAINT on the UI thread) belong to every player's cell. */
int mp_in_views(void) { return g_in_views && GetCurrentThreadId() == g_views_thread; }

/* ------------------------------------------------------------ layout */

int  mp_players(void) { return g_players; }
long mp_layout(void) { return g_layout; }

void mp_grid(int* cols, int* rows) {
    int n = g_players;
    *cols = n >= 2 ? 2 : 1;           /* 2: side by side; 3-4: two by two */
    *rows = n >= 3 ? 2 : 1;
}

static void set_players(int n) {
    char b[8];
    if (n < 1) n = 1;
    if (n > 4) n = 4;
    g_players = n;
    InterlockedIncrement(&g_layout);
    mp_apply_table();
    pad_first_only(n > 1);            /* pad 1 is player 1's; pads 2-4 are the seats' */
    _snprintf(b, sizeof b, "%d", n);
    WritePrivateProfileStringA("mp", "players", b, g_ini);
    fprintf(stderr, "[mp] %d player%s\n", n, n > 1 ? "s, split screen" : "");
}

void mp_init(const char* ini, int cli_players, int (*key_down)(int vk)) {
    strncpy(g_ini, ini, sizeof g_ini - 1);
    g_key = key_down;
    if (!GetPrivateProfileIntA("mp", "players", 0, g_ini)) WritePrivateProfileStringA("mp", "players", "1", g_ini);
    int n = cli_players ? cli_players : GetPrivateProfileIntA("mp", "players", 1, g_ini);
    g_players = n < 1 ? 1 : n > 4 ? 4 : n;
    pad_first_only(g_players > 1);
    if (g_players > 1) fprintf(stderr, "[mp] %d players, split screen\n", g_players);
}

/* ------------------------------------------------------------ menu */

enum { ID_PLAYERS = 0x6B00 };   /* + 1..4 */

void mp_menu(HMENU recomp) {
    HMENU m = CreatePopupMenu();
    AppendMenuA(m, MF_STRING, ID_PLAYERS + 1, "&1 player");
    AppendMenuA(m, MF_STRING, ID_PLAYERS + 2, "&2 players (split screen)");
    AppendMenuA(m, MF_STRING, ID_PLAYERS + 3, "&3 players");
    AppendMenuA(m, MF_STRING, ID_PLAYERS + 4, "&4 players");
    AppendMenuA(recomp, MF_POPUP, (UINT_PTR)m, "&Multiplayer");
}

void mp_update_menu(HMENU popup) {
    for (int i = 1; i <= 4; i++) {
        EnableMenuItem(popup, ID_PLAYERS + i, MF_BYCOMMAND | MF_ENABLED);
        CheckMenuItem(popup, ID_PLAYERS + i, MF_BYCOMMAND | (g_players == i ? MF_CHECKED : MF_UNCHECKED));
    }
}

int mp_command(UINT id) {
    if (id < ID_PLAYERS + 1 || id > ID_PLAYERS + 4) return 0;
    set_players((int)(id - ID_PLAYERS));
    return 1;
}
