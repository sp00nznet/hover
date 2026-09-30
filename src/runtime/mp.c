/*
 * Multiplayer seats and views: split screen on one PC, and the local half of
 * online play (net.c). docs/multiplayer.md has the research this rests on.
 *
 * Hover! has one human craft (CHumanPlayer, inside the document at
 * [0x00460970]+0x818C) and robots (CRobotPlayer). Humans and robots drive
 * through the same physics: each tick every craft's Think writes a turn
 * (+0x1F0) and a thrust (+0x1F4), 16.16 fixed point in -1.0..+1.0, and
 * CPlayer::Think (0x00409110) integrates them. So:
 *
 *   seats   seat 0 is the game's own human; seats 1..15 each take over a
 *           robot of their own (every level gets one extra flag-runner per
 *           seat, and the seats are the last robots created). The robots'
 *           Think (0x00409300) is only ever reached through a virtual call,
 *           so recomp_lookup_manual hands it to seat_think here, which writes
 *           the seat's controls into turn/thrust and runs the physics.
 *   views   this PC shows its own seats: the frame draw (0x00402250) runs
 *           once per local seat on the render thread, with the camera reads
 *           patched at lift time (run_lift.py) to ask hover_cam_obj() whose
 *           eyes to use; host.c puts each pass's blits in its cell of the
 *           capture buffer.
 *
 * Split screen: every seat is local. Online (net.c): the seats are spread
 * over the PCs, and every seat's controls come from the tick's input record
 * instead of a device, so every PC runs the same world.
 *
 * Players in robot seats play for the robots' team (the flag rules go by
 * class). The HUD, powerups and sounds are still seat 0's: see the roadmap.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <xinput.h>
#include <stdio.h>
#include <stdlib.h>

#include "mp.h"
#include "net.h"
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
#define KEY_FORWARD   0x004606ECu        /* the game's key table (docs/controller.md) */
#define KEY_REVERSE   0x004606F0u
#define KEY_LEFT      0x004606F4u
#define KEY_RIGHT     0x004606F8u
#define KEY_FWD_LEFT  0x004606FCu
#define KEY_FWD_RIGHT 0x00460700u
#define KEY_REV_LEFT  0x00460704u
#define KEY_REV_RIGHT 0x00460708u
#define KEY_JUMP      0x004606E8u
#define KEY_WALL      0x004606E4u
#define KEY_CLOAK     0x004606E0u

static char g_ini[MAX_PATH];
static volatile int g_seats = 1;         /* seats in the game, every PC */
static int g_local[MP_MAX_LOCAL] = { 0 };/* this PC's seats, in view order */
static volatile int g_nlocal = 1;
static volatile LONG g_layout;           /* bumped when the view layout changes */
static volatile int g_view;              /* the render pass now running */
static volatile int g_in_views;          /* inside hover_render_views... */
static volatile DWORD g_views_thread;    /* ...on this thread (the render thread) */
static int (*g_key)(int vk);

/* ------------------------------------------------------------ seats */

/* Every level's table entry gets one more flag-runner per robot seat (level
 * table 0x004C4000, 0x58 a level: +0x28 hunters, +0x2C flag-runners), so the
 * level keeps the AI robots it was designed with. The loader creates
 * hunters, then runners, so the seats are the last robots in the list. The
 * table is read at each level load, so a change applies from the next one. */
#define LEVEL_TABLE   0x004C4000u
#define LEVEL_SIZE    0x58u
#define LEVELS        20                 /* entry 20 is the attract demo: no robots */
static uint32_t g_runners[LEVELS];      /* the shipped counts, before any seats */
static int g_table_saved;

void mp_apply_table(void) {
    for (int l = 0; l < LEVELS; l++) {
        uint32_t at = LEVEL_TABLE + l * LEVEL_SIZE + 0x2C;
        if (!g_table_saved) g_runners[l] = MEM32(at);
        MEM32(at) = g_runners[l] + (uint32_t)(g_seats - 1);
    }
    g_table_saved = 1;
}

/* The robot in seat `s` (s >= 1), or 0: none this level, or no level. */
static uint32_t seat_craft(int s) {
    uint32_t doc = MEM32(G_DOC), robots[64];
    int n = 0;
    if (!doc || s < 1) return 0;
    for (uint32_t node = MEM32(doc + DOC_THINKERS); node && n < 64; node = MEM32(node)) {
        uint32_t o = MEM32(node + 8);
        if (MEM32(o) == VT_ROBOT) robots[n++] = o;
    }
    int i = n - (g_seats - 1) + (s - 1);
    return i >= 0 && i < n ? robots[i] : 0;
}

static double clamp1(double v) { return v < -1 ? -1 : v > 1 ? 1 : v; }

/* This PC's player `li` (0 = the first local player) as turn, thrust and
 * buttons (MP_JUMP/WALL/CLOAK). Local player 1 has the keyboard (the game's
 * own keys: arrows, Home/PgUp/End/PgDn, A/S/D) and controller 1; player 2
 * controller 2 and I/J/K/L; players 3 and 4 controllers 3 and 4. */
void mp_local_input(int li, double* turn, double* thrust, int* buttons) {
    XINPUT_GAMEPAD g;
    double t = 0, f = 0;
    int b = 0;
    if (li > 0 && pad_read(li, &g)) {         /* player 1's pad already presses the game's keys */
        double dz = 0.25;
        double x = g.sThumbLX / 32767.0, y = g.sThumbLY / 32767.0;
        if (x > dz || x < -dz) t += (x - (x > 0 ? dz : -dz)) / (1 - dz);
        if (y > dz || y < -dz) f += (y - (y > 0 ? dz : -dz)) / (1 - dz);
        f += (g.bRightTrigger - g.bLeftTrigger) / 255.0;
        if (g.wButtons & XINPUT_GAMEPAD_DPAD_LEFT) t -= 1;
        if (g.wButtons & XINPUT_GAMEPAD_DPAD_RIGHT) t += 1;
        if (g.wButtons & XINPUT_GAMEPAD_DPAD_UP) f += 1;
        if (g.wButtons & XINPUT_GAMEPAD_DPAD_DOWN) f -= 1;
        if (g.wButtons & XINPUT_GAMEPAD_A) b |= MP_JUMP;
        if (g.wButtons & XINPUT_GAMEPAD_X) b |= MP_WALL;
        if (g.wButtons & XINPUT_GAMEPAD_B) b |= MP_CLOAK;
    }
    if (li == 0 && g_key) {                   /* the game's keys, wherever they are bound */
        int fl = g_key(MEM32(KEY_FWD_LEFT)), fr = g_key(MEM32(KEY_FWD_RIGHT));
        int rl = g_key(MEM32(KEY_REV_LEFT)), rr = g_key(MEM32(KEY_REV_RIGHT));
        if (g_key(MEM32(KEY_LEFT)) || fl || rl) t -= 1;
        if (g_key(MEM32(KEY_RIGHT)) || fr || rr) t += 1;
        if (g_key(MEM32(KEY_FORWARD)) || fl || fr) f += 1;
        if (g_key(MEM32(KEY_REVERSE)) || rl || rr) f -= 1;
        if (g_key(MEM32(KEY_JUMP))) b |= MP_JUMP;
        if (g_key(MEM32(KEY_WALL))) b |= MP_WALL;
        if (g_key(MEM32(KEY_CLOAK))) b |= MP_CLOAK;
    }
    if (li == 1 && g_key) {
        if (g_key('J')) t -= 1;
        if (g_key('L')) t += 1;
        if (g_key('I')) f += 1;
        if (g_key('K')) f -= 1;
    }
    *turn = clamp1(t);
    *thrust = clamp1(f);
    *buttons = b;
}

/* Replaces CRobotPlayer::Think (vtable slot 7, 0x00409300) for every robot:
 * a robot in a seat gets its player's controls, the rest their AI. The
 * spin-out and airborne handling is CRobotPlayer::Think's own (0x00409300:
 * +0x5C, +0x158, +0x15C), then the shared physics. */
static void seat_think(void) {
    uint32_t self = g_ecx;
    int seat = 0;
    for (int s = 1; s < g_seats && !seat; s++)
        if (seat_craft(s) == self) seat = s;
    if (!seat) { sub_00409300(); return; }
    if (MEM32(self + 0x5C) == 0) {
        MEM32(self + 0x15C) = MEM32(self + 0x158) ? 0 : 0xFFFFFFFFu;
        if (MEM32(G_GAME_STATE) == 3) {
            double t = 0, f = 0;
            int b;
            if (net_active()) net_seat_input(seat, &t, &f, &b);
            else
                for (int li = 0; li < g_nlocal; li++)
                    if (g_local[li] == seat) mp_local_input(li, &t, &f, &b);
            MEM32(self + 0x1F0) = (uint32_t)(int32_t)(t * 65536);
            MEM32(self + 0x1F4) = (uint32_t)(int32_t)(f * 65536);
        }
    }
    g_ecx = self;
    sub_00409110();          /* CPlayer::Think: the physics; its ret pops our return address */
}

recomp_func_t mp_lookup(uint32_t va) {
    return va == 0x00409300u && g_seats > 1 ? seat_think : NULL;
}

/* A hash of every craft's state, for net.c's desync check: position,
 * heading and controls of everything that thinks. */
uint32_t mp_world_hash(void) {
    uint32_t doc = MEM32(G_DOC), h = 2166136261u;
    if (!doc) return 0;
    int n = 0;
    for (uint32_t node = MEM32(doc + DOC_THINKERS); node && n < 256; node = MEM32(node), n++) {
        uint32_t o = MEM32(node + 8);
        uint32_t v[6] = { MEM32(o), MEM32(o + 0x44), MEM32(o + 0x48), MEM32(o + 0x19C),
                          MEM32(o + 0x1F0), MEM32(o + 0x1F4) };
        for (int i = 0; i < 6; i++) h = (h ^ v[i]) * 16777619u;
    }
    return h ^ (uint32_t)n;
}

/* ------------------------------------------------------------ views */

/* The camera reads in 0x00402250, patched at lift time: `doc` is the
 * document, and the craft returned is the one whose eyes this pass uses. */
uint32_t hover_cam_obj(uint32_t doc) {
    int seat = g_view < g_nlocal ? g_local[g_view] : 0;
    uint32_t craft = seat ? seat_craft(seat) : 0;
    return craft ? craft : doc + DOC_HUMAN;
}

/* Replaces the render thread's call to the frame draw (0x0041F012): the draw
 * once per local seat, the first local seat last, so what the game sees
 * afterwards (registers, its frame state) is exactly one normal draw.
 * 0x00402250 is thiscall with no stack arguments and a plain ret, so each
 * pass starts from the same registers and the same return-address slot. */
void hover_render_views(void) {
    uint32_t eax = g_eax, ecx = g_ecx, edx = g_edx, ebx = g_ebx, esp = g_esp, ebp = g_ebp,
             esi = g_esi, edi = g_edi;
    int n = g_nlocal;
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
 * WM_PAINT on the UI thread) belong to every view's cell. */
int mp_in_views(void) { return g_in_views && GetCurrentThreadId() == g_views_thread; }

/* ------------------------------------------------------------ layout */

int  mp_seats(void) { return g_seats; }
int  mp_local_count(void) { return g_nlocal; }
int  mp_local_seat(int li) { return li >= 0 && li < g_nlocal ? g_local[li] : -1; }
long mp_layout(void) { return g_layout; }

void mp_grid(int* cols, int* rows) {
    int n = g_nlocal;
    *cols = n >= 2 ? 2 : 1;           /* 2: side by side; 3-4: two by two */
    *rows = n >= 3 ? 2 : 1;
}

/* Seats and which of them are shown here. Split screen: seats 0..n-1, all
 * local. Online (net.c): the session's total, and this PC's block. */
void mp_set_seats(int total, const int* local, int nlocal) {
    if (total < 1) total = 1;
    if (total > MP_MAX_SEATS) total = MP_MAX_SEATS;
    if (nlocal < 1) nlocal = 1;
    if (nlocal > MP_MAX_LOCAL) nlocal = MP_MAX_LOCAL;
    g_seats = total;
    for (int i = 0; i < nlocal; i++) g_local[i] = local[i];
    g_nlocal = nlocal;
    InterlockedIncrement(&g_layout);
    mp_apply_table();
    pad_first_only(nlocal > 1);       /* controller 1 is local player 1's; 2-4 the others' */
}

static void set_players(int n) {
    int local[MP_MAX_LOCAL];
    char b[8];
    if (n < 1) n = 1;
    if (n > MP_MAX_LOCAL) n = MP_MAX_LOCAL;
    for (int i = 0; i < n; i++) local[i] = i;
    mp_set_seats(n, local, n);
    _snprintf(b, sizeof b, "%d", n);
    WritePrivateProfileStringA("mp", "players", b, g_ini);
    fprintf(stderr, "[mp] %d player%s\n", n, n > 1 ? "s, split screen" : "");
}

void mp_init(const char* ini, int cli_players, int (*key_down)(int vk)) {
    int local[MP_MAX_LOCAL];
    strncpy(g_ini, ini, sizeof g_ini - 1);
    g_key = key_down;
    if (!GetPrivateProfileIntA("mp", "players", 0, g_ini)) WritePrivateProfileStringA("mp", "players", "1", g_ini);
    int n = cli_players ? cli_players : GetPrivateProfileIntA("mp", "players", 1, g_ini);
    n = n < 1 ? 1 : n > MP_MAX_LOCAL ? MP_MAX_LOCAL : n;
    for (int i = 0; i < n; i++) local[i] = i;
    g_seats = n;
    for (int i = 0; i < n; i++) g_local[i] = local[i];
    g_nlocal = n;
    pad_first_only(n > 1);
    if (n > 1) fprintf(stderr, "[mp] %d players, split screen\n", n);
}

/* ------------------------------------------------------------ menu */

enum { ID_PLAYERS = 0x6B00 };   /* + 1..4 */

void mp_menu(HMENU recomp) {
    HMENU m = CreatePopupMenu();
    AppendMenuA(m, MF_STRING, ID_PLAYERS + 1, "&1 player");
    AppendMenuA(m, MF_STRING, ID_PLAYERS + 2, "&2 players (split screen)");
    AppendMenuA(m, MF_STRING, ID_PLAYERS + 3, "&3 players");
    AppendMenuA(m, MF_STRING, ID_PLAYERS + 4, "&4 players");
    net_menu(m);
    AppendMenuA(recomp, MF_POPUP, (UINT_PTR)m, "&Multiplayer");
}

void mp_update_menu(HMENU popup) {
    UINT on = net_active() ? MF_GRAYED : MF_ENABLED;   /* the session decides, online */
    for (int i = 1; i <= MP_MAX_LOCAL; i++) {
        EnableMenuItem(popup, ID_PLAYERS + i, MF_BYCOMMAND | on);
        CheckMenuItem(popup, ID_PLAYERS + i,
                      MF_BYCOMMAND | (!net_active() && g_nlocal == i ? MF_CHECKED : MF_UNCHECKED));
    }
    net_update_menu(popup);
}

int mp_command(UINT id) {
    if (net_command(id)) return 1;
    if (id < ID_PLAYERS + 1 || id > ID_PLAYERS + MP_MAX_LOCAL || net_active()) return 0;
    set_players((int)(id - ID_PLAYERS));
    return 1;
}
