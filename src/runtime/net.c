/*
 * Online play: lockstep over UDP. docs/multiplayer.md has the research;
 * the short version:
 *
 * Hover!'s world advances once per 50 ms tick (a timeSetEvent callback,
 * 0x00408AA0, posts WM_USER; its handler 0x00408AF0 polls the keys and runs
 * the tick), with a fixed step and no clock in it. So every PC can run the
 * same world, if every PC runs the same ticks with the same inputs:
 *
 *   the gate   online, the game's timer is ours: a driver thread calls the
 *              game's callback for tick N only when tick N-1 has been handled
 *              (the flag at [0x004C4CE4] is clear again) and every seat's
 *              input for tick N is here. Stall, never skip.
 *   inputs     each PC samples its own players for tick N+DELAY and sends
 *              them, the last few ticks repeated in every packet. The host
 *              relays everyone's to everyone (a star, up to 16 seats). Seat 0
 *              (the game's human) is fed through the key shims from the
 *              tick's record, its jump/wall/cloak posted to the view on the
 *              tick they change; robot seats read the record in mp.c. Live
 *              keys never reach the game directly while online.
 *   session    the host picks the seed, the level and the seats; every PC
 *              starts the same new game (F2 on a pinned seed), and ticks
 *              count from the game's first timer tick.
 *   desync     every 20 ticks each PC hashes every craft (mp_world_hash);
 *              the host compares, and a mismatch stops the game, naming the
 *              tick.
 */
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "net.h"
#include "mp.h"
#include "levels.h"
#include "native32.h"
#include "recomp_types.h"

#define TICK_CB       0x00408AA0u        /* the game's 50 ms timer callback */
#define G_TICK_BUSY   0x004C4CE4u        /* set by the callback, cleared when the tick is handled */
#define G_JOYSTICK    0x0046070Cu        /* Player Controls > Joystick: the handler polls it */
#define G_START_AT    0x0046048Cu
#define G_GAME_STATE  0x00460728u        /* 2 countdown, 3 playing, 4-5 level end */
#define KEY_FORWARD   0x004606ECu
#define KEY_REVERSE   0x004606F0u
#define KEY_LEFT      0x004606F4u
#define KEY_RIGHT     0x004606F8u
#define KEY_JUMP      0x004606E8u
#define KEY_WALL      0x004606E4u
#define KEY_CLOAK     0x004606E0u

#define MAGIC   0x52564F48u              /* "HOVR" */
#define VERSION 1
#define DELAY   3                        /* ticks between sampling and running an input */
#define REDUND  8                        /* ticks repeated in every packet */
#define WIN     512                      /* input ring, in ticks */
#define FAKE_TIMER 0x7E57u
#define MAX_PEERS 15

enum { T_HELLO = 1, T_WELCOME, T_INPUT, T_HASH, T_ACK };

#pragma pack(push, 1)
typedef struct { uint32_t magic; uint8_t type, version; } hdr_t;
typedef struct { hdr_t h; uint8_t nlocal; } hello_t;
typedef struct { hdr_t h; uint8_t seat_base, nlocal, total, level; uint32_t seed; } welcome_t;
typedef struct { int32_t tick; uint8_t seat; int16_t turn, thrust; uint8_t btn; } entry_t;
typedef struct { hdr_t h; uint16_t n; entry_t e[1]; } input_t;
typedef struct { hdr_t h; int32_t tick; uint32_t hash; } hash_t;
#pragma pack(pop)

typedef struct { int32_t tick; int16_t turn, thrust; uint8_t btn, valid; } rec_t;

static char     g_ini[MAX_PATH];
static HWND     g_frame;
static volatile int g_active;            /* a session is running */
static int      g_is_host, g_port = 7795, g_want_clients = 1, g_nlocal_cfg = 1;
static SOCKET   g_sock = INVALID_SOCKET;
static struct sockaddr_in g_host_addr;
static struct { struct sockaddr_in addr; int seat_base, nlocal, acked; DWORD seen; } g_peers[MAX_PEERS];
static int      g_npeers;
static int      g_total = 1, g_seat_base, g_nlocal = 1, g_level;
static uint32_t g_seed;
static CRITICAL_SECTION g_lock;
static rec_t    g_rec[WIN][MP_MAX_SEATS];
static volatile int32_t g_cur = -1;      /* the tick the game is handling */
static volatile uint32_t g_cb, g_cb_user, g_cb_id;   /* the game's timer, while it runs */
static volatile int g_timer_on;
static volatile int g_armed;             /* the session's first level is loading: its timer is ours */
static HWND     g_view;                  /* where the callback posts WM_USER */
static uint32_t g_own_hash[WIN / 20 + 1][2], g_peer_hash[WIN / 20 + 1][2];
static volatile int g_desync;
static int      g_desync_test = -1;      /* --net-desync-test: nudge a craft at this tick */
static char     g_status[96] = "Offline";
static int      g_headless_net;          /* no message boxes: host.c --headless */

int net_active(void) { return g_active; }
int net_is_host(void) { return g_is_host; }

static void status(const char* fmt, ...) {
    va_list a;
    va_start(a, fmt);
    _vsnprintf(g_status, sizeof g_status - 1, fmt, a);
    va_end(a);
    fprintf(stderr, "[net] %s\n", g_status);
}

/* ------------------------------------------------------------ the record */

static void put(int32_t tick, int seat, int16_t turn, int16_t thrust, uint8_t btn) {
    if (seat < 0 || seat >= MP_MAX_SEATS || tick < 0) return;
    rec_t* r = &g_rec[tick & (WIN - 1)][seat];
    EnterCriticalSection(&g_lock);
    if (!(r->valid && r->tick == tick)) {
        r->tick = tick;
        r->turn = turn;
        r->thrust = thrust;
        r->btn = btn;
        r->valid = 1;
    }
    LeaveCriticalSection(&g_lock);
}

static int have(int32_t tick, int seat) {
    const rec_t* r = &g_rec[tick & (WIN - 1)][seat];
    return r->valid && r->tick == tick;
}

static const rec_t* rec(int32_t tick, int seat) {
    static const rec_t none = { 0 };
    return tick >= 0 && have(tick, seat) ? &g_rec[tick & (WIN - 1)][seat] : &none;
}

/* mp.c's robot seats: the tick being handled. */
void net_seat_input(int seat, double* turn, double* thrust, int* buttons) {
    const rec_t* r = rec(g_cur, seat);
    *turn = r->turn / 32767.0;
    *thrust = r->thrust / 32767.0;
    *buttons = r->btn;
}

/* The key shims, online: the game's steering keys answer from seat 0's
 * record for the tick being handled, never from the keyboard. */
int net_key(int vk, int* down) {
    if (!g_active) return 0;
    const rec_t* r = rec(g_cur, 0);
    vk &= 0xFF;
    if (vk == (int)(MEM32(KEY_FORWARD) & 0xFF)) *down = r->thrust > 16384;
    else if (vk == (int)(MEM32(KEY_REVERSE) & 0xFF)) *down = r->thrust < -16384;
    else if (vk == (int)(MEM32(KEY_LEFT) & 0xFF)) *down = r->turn < -16384;
    else if (vk == (int)(MEM32(KEY_RIGHT) & 0xFF)) *down = r->turn > 16384;
    else *down = 0;                          /* the diagonals, and anything else */
    return 1;
}

/* ------------------------------------------------------------ transport */

static void send_to(const struct sockaddr_in* to, const void* p, int n) {
    sendto(g_sock, (const char*)p, n, 0, (const struct sockaddr*)to, sizeof *to);
}

static void hdr(hdr_t* h, int type) { h->magic = MAGIC; h->type = (uint8_t)type; h->version = VERSION; }

/* Every valid record in [from, to] for the seats in [s0, s1), in one packet. */
static void send_inputs(const struct sockaddr_in* to, int32_t from, int32_t upto, int s0, int s1) {
    char buf[1400];
    input_t* m = (input_t*)buf;
    int max = (int)((sizeof buf - sizeof(input_t) + sizeof(entry_t)) / sizeof(entry_t)), n = 0;
    hdr(&m->h, T_INPUT);
    EnterCriticalSection(&g_lock);
    for (int32_t t = from < 0 ? 0 : from; t <= upto; t++)
        for (int s = s0; s < s1 && n < max; s++)
            if (have(t, s)) {
                const rec_t* r = &g_rec[t & (WIN - 1)][s];
                m->e[n].tick = t;
                m->e[n].seat = (uint8_t)s;
                m->e[n].turn = r->turn;
                m->e[n].thrust = r->thrust;
                m->e[n].btn = r->btn;
                n++;
            }
    LeaveCriticalSection(&g_lock);
    m->n = (uint16_t)n;
    if (n) send_to(to, buf, (int)(sizeof(input_t) - sizeof(entry_t) + n * sizeof(entry_t)));
}

/* Our side of the window around tick `t`: a client sends its seats to the
 * host, the host every seat to every client. */
static void flush(int32_t t) {
    if (g_is_host)
        for (int i = 0; i < g_npeers; i++)
            if (g_peers[i].acked) send_inputs(&g_peers[i].addr, t - REDUND, t + DELAY, 0, g_total);
    if (!g_is_host) send_inputs(&g_host_addr, t - REDUND, t + DELAY, g_seat_base, g_seat_base + g_nlocal);
}

static void check_hash(int32_t tick) {
    int i = (tick / 20) % (WIN / 20 + 1);
    if (g_own_hash[i][0] != (uint32_t)tick || g_peer_hash[i][0] != (uint32_t)tick) return;
    if (g_own_hash[i][1] == g_peer_hash[i][1]) {
        if (tick % 200 == 0) fprintf(stderr, "[net] in sync at tick %d (%08X)\n", tick, g_own_hash[i][1]);
        return;
    }
    if (!g_desync) {
        g_desync = 1;
        status("DESYNC at tick %d: %08X here, %08X on the %s", tick, g_own_hash[i][1],
               g_peer_hash[i][1], g_is_host ? "client" : "host");
    }
}

static void start_session(void);

static DWORD WINAPI recv_thread(LPVOID unused) {
    char buf[2048];
    (void)unused;
    for (;;) {
        struct sockaddr_in from;
        int fl = sizeof from;
        int n = recvfrom(g_sock, buf, sizeof buf, 0, (struct sockaddr*)&from, &fl);
        if (n < (int)sizeof(hdr_t)) { if (n == SOCKET_ERROR) Sleep(10); continue; }
        const hdr_t* h = (const hdr_t*)buf;
        if (h->magic != MAGIC || h->version != VERSION) continue;
        if (h->type == T_HELLO && g_is_host && n >= (int)sizeof(hello_t)) {
            int i;
            for (i = 0; i < g_npeers; i++)
                if (!memcmp(&g_peers[i].addr, &from, sizeof from)) break;
            if (i == g_npeers && g_npeers < MAX_PEERS && !g_active) {
                int nl = ((const hello_t*)buf)->nlocal;
                g_peers[i].addr = from;
                g_peers[i].nlocal = nl < 1 ? 1 : nl > MP_MAX_LOCAL ? MP_MAX_LOCAL : nl;
                g_peers[i].seat_base = g_total;
                g_total += g_peers[i].nlocal;
                g_npeers++;
                char ip[64] = "?";
                inet_ntop(AF_INET, &from.sin_addr, ip, sizeof ip);
                status("player%s joined from %s: seats %d-%d (%d of %d PCs)", g_peers[i].nlocal > 1 ? "s" : "",
                       ip, g_peers[i].seat_base + 1,
                       g_peers[i].seat_base + g_peers[i].nlocal, g_npeers, g_want_clients);
                if (g_npeers >= g_want_clients) start_session();
            }
        } else if (h->type == T_WELCOME && !g_is_host && !g_active && n >= (int)sizeof(welcome_t)) {
            const welcome_t* w = (const welcome_t*)buf;
            g_seat_base = w->seat_base;
            g_nlocal = w->nlocal;
            g_total = w->total;
            g_level = w->level;
            g_seed = w->seed;
            start_session();
        } else if (h->type == T_ACK && g_is_host) {
            for (int i = 0; i < g_npeers; i++)
                if (!memcmp(&g_peers[i].addr, &from, sizeof from)) g_peers[i].acked = 1;
        } else if (h->type == T_INPUT && n >= (int)(sizeof(input_t) - sizeof(entry_t))) {
            const input_t* m = (const input_t*)buf;
            int cnt = m->n;
            if ((int)(sizeof(input_t) - sizeof(entry_t) + cnt * sizeof(entry_t)) > n) continue;
            for (int i = 0; i < cnt; i++) put(m->e[i].tick, m->e[i].seat, m->e[i].turn, m->e[i].thrust, m->e[i].btn);
            if (g_is_host)
                for (int i = 0; i < g_npeers; i++)
                    if (!memcmp(&g_peers[i].addr, &from, sizeof from)) g_peers[i].seen = GetTickCount();
        } else if (h->type == T_HASH && n >= (int)sizeof(hash_t)) {
            const hash_t* m = (const hash_t*)buf;
            int i = (m->tick / 20) % (WIN / 20 + 1);
            g_peer_hash[i][0] = (uint32_t)m->tick;
            g_peer_hash[i][1] = m->hash;
            check_hash(m->tick);
            if (g_is_host)       /* every client checks against the host's */
                for (int k = 0; k < g_npeers; k++)
                    if (memcmp(&g_peers[k].addr, &from, sizeof from)) send_to(&g_peers[k].addr, m, sizeof *m);
        }
    }
}

/* ------------------------------------------------------------ the tick gate */

static void post_buttons(int32_t t) {
    static const uint32_t keys[3] = { KEY_JUMP, KEY_WALL, KEY_CLOAK };
    int now = rec(t, 0)->btn, was = rec(t - 1, 0)->btn;
    if (!g_view) return;
    for (int i = 0; i < 3; i++)
        if ((now & (1 << i)) && !(was & (1 << i))) {    /* pressed on this tick */
            int vk = (int)(MEM32(keys[i]) & 0xFF);
            PostMessageA(g_view, WM_KEYDOWN, vk, 1);
            PostMessageA(g_view, WM_KEYUP, vk, 0xC0000001u);
        }
}

static DWORD WINAPI driver(LPVOID unused) {
    int32_t tick = 0, prepared = -1;
    DWORD due = GetTickCount(), waited = GetTickCount(), sent = 0, f2 = GetTickCount();
    int told = 0;
    (void)unused;
    for (;;) {
        if (g_desync) {
            if (!told++ && !g_headless_net)
                MessageBoxA(NULL, g_status, "Hover! online", MB_OK | MB_ICONWARNING);
            Sleep(50);
            continue;
        }
        if (!g_timer_on) {
            /* The session's F2 can land while the game is still busy (its
             * Quick Help dialog, say): press it again until a level starts. */
            if (tick == 0 && MEM32(G_GAME_STATE) < 2 && GetTickCount() - f2 > 4000) {
                f2 = GetTickCount();
                levels_session(g_level, g_seed);
            }
            Sleep(5);
            due = GetTickCount();
            continue;
        }
        if ((int)(GetTickCount() - due) < 0) { Sleep(1); continue; }
        if (MEM32(G_TICK_BUSY)) { Sleep(1); continue; }     /* tick-1 not handled yet */

        if (prepared != tick) {                  /* between ticks the world is still */
            prepared = tick;
            if (tick % 20 == 0) {
                int i = (tick / 20) % (WIN / 20 + 1);
                hash_t m;
                g_own_hash[i][0] = (uint32_t)tick;
                g_own_hash[i][1] = mp_world_hash();
                hdr(&m.h, T_HASH);
                m.tick = tick;
                m.hash = g_own_hash[i][1];
                if (g_is_host) for (int k = 0; k < g_npeers; k++) send_to(&g_peers[k].addr, &m, sizeof m);
                else send_to(&g_host_addr, &m, sizeof m);
                check_hash(tick);
            }
            for (int li = 0; li < g_nlocal; li++) {
                double t, f;
                int b;
                mp_local_input(li, &t, &f, &b);
                put(tick + DELAY, g_seat_base + li, (int16_t)(t * 32767), (int16_t)(f * 32767), (uint8_t)b);
            }
            flush(tick);
            sent = GetTickCount();
        }

        int ready = 1;
        for (int s = 0; s < g_total && ready; s++) ready = have(tick, s);
        if (!ready) {                            /* stall, and keep the relay moving */
            Sleep(2);
            if (GetTickCount() - sent > 30) { flush(tick); sent = GetTickCount(); }
            if (GetTickCount() - waited > 2000) {
                waited = GetTickCount();
                status("waiting for the other players at tick %d", tick);
            }
            continue;
        }
        if (tick == g_desync_test) {             /* the check must catch a one-unit difference */
            uint32_t doc = MEM32(0x00460970u);
            if (doc) MEM32(doc + 0x818Cu + 0x44) += 1;
            fprintf(stderr, "[net] desync test: nudged the human craft at tick %d\n", tick);
        }
        MEM32(G_JOYSTICK) = 0;                   /* the handler must not poll a local joystick */
        post_buttons(tick);
        g_cur = tick;
        uint32_t args[5] = { g_cb_id, 0, g_cb_user, 0, 0 };
        native32_call_guest(g_cb, 5, args);      /* posts WM_USER: the game handles tick `tick` */
        if (tick % 200 == 0) fprintf(stderr, "[net] tick %d\n", tick);
        tick++;
        waited = GetTickCount();
        due += 50;
        if ((int)(GetTickCount() - due) > 250) due = GetTickCount();   /* stalled: no burst to catch up */
    }
}

/* The game's timer, online: ours. 0x00412804 is its only timeSetEvent. */
int net_timer_set(uint32_t cb, uint32_t user, uint32_t* id) {
    if (!g_active || !g_armed || cb != TICK_CB) return 0;   /* the attract demo keeps a real timer */
    g_cb = cb;
    g_cb_user = user;
    g_cb_id = FAKE_TIMER;
    g_timer_on = 1;
    *id = FAKE_TIMER;
    return 1;
}

int net_timer_kill(uint32_t id) {
    if (id != FAKE_TIMER) return 0;
    g_timer_on = 0;
    return 1;
}

/* levels.c, at every level load: tick 0 is the first tick of the session's
 * first real level, never the attract demo's (which runs on its own timer
 * and may still be up when the session starts). */
void net_level_loading(int demo) {
    if (g_active && !demo && !g_armed) {
        g_armed = 1;
        status("the session's level is loading: ticks are ours");
    }
}

void net_saw_post(HWND h, UINT msg) {
    if (msg == WM_USER && g_active && !g_view) g_view = h;
}

/* ------------------------------------------------------------ session */

static void start_session(void) {
    int local[MP_MAX_LOCAL];
    if (g_active) return;
    memset(g_own_hash, 0xFF, sizeof g_own_hash);     /* an empty slot is no tick, not tick 0 */
    memset(g_peer_hash, 0xFF, sizeof g_peer_hash);
    if (g_is_host) {
        g_seed = levels_current_seed();
        g_level = (int)MEM32(G_START_AT) - 1;
        if (g_level < 0 || g_level > 19) g_level = 0;
        for (int i = 0; i < g_npeers; i++) {
            welcome_t w;
            hdr(&w.h, T_WELCOME);
            w.seat_base = (uint8_t)g_peers[i].seat_base;
            w.nlocal = (uint8_t)g_peers[i].nlocal;
            w.total = (uint8_t)g_total;
            w.level = (uint8_t)g_level;
            w.seed = g_seed;
            for (int k = 0; k < 3; k++) send_to(&g_peers[i].addr, &w, sizeof w);
        }
    } else {
        hdr_t a;
        hdr(&a, T_ACK);
        for (int k = 0; k < 3; k++) send_to(&g_host_addr, &a, sizeof a);
    }
    for (int li = 0; li < g_nlocal; li++) local[li] = g_seat_base + li;
    mp_set_seats(g_total, local, g_nlocal);
    for (int32_t t = 0; t < DELAY; t++)          /* the first ticks: nobody has pressed anything */
        for (int s = 0; s < g_total; s++) put(t, s, 0, 0, 0);
    g_active = 1;
    status("game on: %d seats, level %d, seed %u; this PC has seat%s %d-%d", g_total, g_level + 1, g_seed,
           g_nlocal > 1 ? "s" : "", g_seat_base + 1, g_seat_base + g_nlocal);
    levels_session(g_level, g_seed);             /* pin the seed, start the level: F2 */
    CloseHandle(CreateThread(NULL, 0, driver, NULL, 0, NULL));
}

static int open_socket(int port) {
    WSADATA wsa;
    struct sockaddr_in a = { 0 };
    if (WSAStartup(MAKEWORD(2, 2), &wsa)) return 0;
    g_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (g_sock == INVALID_SOCKET) return 0;
    a.sin_family = AF_INET;
    a.sin_port = htons((u_short)port);
    if (bind(g_sock, (struct sockaddr*)&a, sizeof a)) {
        status("cannot use UDP port %d (%d)", port, WSAGetLastError());
        return 0;
    }
    InitializeCriticalSection(&g_lock);
    CloseHandle(CreateThread(NULL, 0, recv_thread, NULL, 0, NULL));
    return 1;
}

int net_host(int port, int clients, int nlocal) {
    if (g_sock != INVALID_SOCKET) return 0;
    g_is_host = 1;
    g_want_clients = clients < 1 ? 1 : clients > MAX_PEERS ? MAX_PEERS : clients;
    g_nlocal = nlocal < 1 ? 1 : nlocal > MP_MAX_LOCAL ? MP_MAX_LOCAL : nlocal;
    g_total = g_nlocal;
    g_seat_base = 0;
    if (!open_socket(port)) return 0;
    status("hosting on UDP port %d: waiting for %d PC%s to join", port, g_want_clients, g_want_clients > 1 ? "s" : "");
    return 1;
}

/* A client resends HELLO until the host's WELCOME arrives. */
static DWORD WINAPI hello_thread(LPVOID p) {
    hello_t m;
    hdr(&m.h, T_HELLO);
    m.nlocal = (uint8_t)(intptr_t)p;
    while (!g_active) {
        send_to(&g_host_addr, &m, sizeof m);
        Sleep(250);
    }
    return 0;
}

int net_join(const char* addr, int nlocal) {
    char host[128];
    int port = g_port;
    struct addrinfo hints = { 0 }, *res = NULL;
    if (g_sock != INVALID_SOCKET) return 0;
    strncpy(host, addr, sizeof host - 1);
    host[sizeof host - 1] = 0;
    char* colon = strrchr(host, ':');
    if (colon) { *colon = 0; port = atoi(colon + 1); }
    g_nlocal = nlocal < 1 ? 1 : nlocal > MP_MAX_LOCAL ? MP_MAX_LOCAL : nlocal;
    if (!open_socket(0)) return 0;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    if (getaddrinfo(host, NULL, &hints, &res) || !res) { status("cannot find %s", host); return 0; }
    g_host_addr = *(struct sockaddr_in*)res->ai_addr;
    g_host_addr.sin_port = htons((u_short)port);
    freeaddrinfo(res);
    status("joining %s:%d", host, port);
    CloseHandle(CreateThread(NULL, 0, hello_thread, (LPVOID)(intptr_t)g_nlocal, 0, NULL));
    return 1;
}

void net_desync_test(int tick) { g_desync_test = tick; }

void net_init(const char* ini, HWND frame, int headless) {
    g_headless_net = headless;
    strncpy(g_ini, ini, sizeof g_ini - 1);
    g_frame = frame;
    if (!GetPrivateProfileIntA("net", "port", 0, g_ini)) {
        WritePrivateProfileStringA("net", "port", "7795", g_ini);
        WritePrivateProfileStringA("net", "clients", "1", g_ini);
        WritePrivateProfileStringA("net", "join", "", g_ini);
    }
    g_port = GetPrivateProfileIntA("net", "port", 7795, g_ini);
}

/* ------------------------------------------------------------ menu */

enum { ID_STATUS = 0x6A00, ID_HOST, ID_JOIN };

void net_menu(HMENU m) {
    AppendMenuA(m, MF_SEPARATOR, 0, NULL);
    AppendMenuA(m, MF_STRING | MF_GRAYED, ID_STATUS, "Online: offline");
    AppendMenuA(m, MF_STRING, ID_HOST, "&Host an online game");
    AppendMenuA(m, MF_STRING, ID_JOIN, "&Join an online game...");
}

void net_update_menu(HMENU popup) {
    char t[128], j[96];
    _snprintf(t, sizeof t, "Online: %s", g_status);
    t[sizeof t - 1] = 0;
    ModifyMenuA(popup, ID_STATUS, MF_BYCOMMAND | MF_STRING | MF_GRAYED, ID_STATUS, t);
    GetPrivateProfileStringA("net", "join", "", j, sizeof j, g_ini);
    _snprintf(t, sizeof t, j[0] ? "&Join %s (hover.ini, or an address on the clipboard)"
                                : "&Join the address on the clipboard", j);
    t[sizeof t - 1] = 0;
    UINT on = g_sock == INVALID_SOCKET ? MF_ENABLED : MF_GRAYED;
    ModifyMenuA(popup, ID_JOIN, MF_BYCOMMAND | MF_STRING | on, ID_JOIN, t);
    _snprintf(t, sizeof t, "&Host an online game (UDP port %d, %d other PC%s)", g_port,
              GetPrivateProfileIntA("net", "clients", 1, g_ini), GetPrivateProfileIntA("net", "clients", 1, g_ini) > 1 ? "s" : "");
    ModifyMenuA(popup, ID_HOST, MF_BYCOMMAND | MF_STRING | on, ID_HOST, t);
}

int net_command(UINT id) {
    int nlocal = mp_local_count();
    if (id == ID_HOST) net_host(g_port, GetPrivateProfileIntA("net", "clients", 1, g_ini), nlocal);
    else if (id == ID_JOIN) {
        char a[96] = "";
        if (OpenClipboard(NULL)) {           /* an address someone pasted wins over the ini */
            HANDLE h = GetClipboardData(CF_TEXT);
            const char* p = h ? (const char*)GlobalLock(h) : NULL;
            if (p && strlen(p) < sizeof a && !strchr(p, ' ') && (strchr(p, '.') || strchr(p, ':')))
                strcpy(a, p);
            if (p) GlobalUnlock(h);
            CloseClipboard();
        }
        if (!a[0]) GetPrivateProfileStringA("net", "join", "", a, sizeof a, g_ini);
        if (!a[0]) {
            MessageBoxA(g_frame, "Copy the host's address (for example 100.64.1.2 or 192.168.1.5:7795) "
                                 "and choose Join again, or put it in hover.ini under [net] join=.",
                        "Hover! online", MB_OK | MB_ICONINFORMATION);
            return 1;
        }
        WritePrivateProfileStringA("net", "join", a, g_ini);
        net_join(a, nlocal);
    }
    else return 0;
    return 1;
}
