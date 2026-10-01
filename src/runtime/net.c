/*
 * Online play: lockstep over UDP, with players joining and leaving while the
 * game runs. docs/multiplayer.md has the research and the reasoning; the
 * short version:
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
 *              them, the last few ticks repeated in every packet; the host
 *              relays every seat to every client (a star, up to 16 seats).
 *              Seat 0 (the game's human) is fed through the key shims from
 *              the tick's record, robot seats in mp.c. Live keys never reach
 *              the game directly while online.
 *   seats      the host is the one authority on who holds which seat. A seat
 *              nobody holds carries the AI mark (IN_AI) in the record, so
 *              every PC lets the robot's own AI drive it on that tick: empty
 *              seats, players who left (their seats go back to the AI from
 *              the first tick the host has no input for), and players still
 *              catching up.
 *   joining    a PC that joins a running game gets the session's start (its
 *              first level and seed) and replays every input since tick 0 at
 *              full speed, pulling the log from the host; the same hashes
 *              check the replay. Caught up, it says READY, and the host gives
 *              it its seats from a tick far enough ahead that everyone has
 *              the AI's records until then (CLAIM).
 *   desync     every 20 ticks every PC hashes every craft (mp_world_hash);
 *              the host keeps its hashes for the whole session and compares
 *              every one it is sent, and a mismatch stops the game.
 */
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <limits.h>

#include <mmsystem.h>

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
#define G_DOC         0x00460970u
#define KEY_FORWARD   0x004606ECu
#define KEY_REVERSE   0x004606F0u
#define KEY_LEFT      0x004606F4u
#define KEY_RIGHT     0x004606F8u
#define KEY_JUMP      0x004606E8u
#define KEY_WALL      0x004606E4u
#define KEY_CLOAK     0x004606E0u

#define MAGIC    0x52564F48u             /* "HOVR" */
#define VERSION  3
#define DELAY    3                       /* ticks between sampling and running an input */
#define REDUND   8                       /* ticks repeated in every packet */
#define CLAIM_AHEAD 20                   /* a joiner's seats become its own this far ahead */
#define PEER_TIMEOUT 4000                /* ms of silence before a client counts as gone */
#define HOST_TIMEOUT 8000
#define FAKE_TIMER 0x7E57u
#define MAX_PEERS 15
#define IN_AI 0x80                       /* the record's mark: this seat's robot drives itself */
#define TICK_WINDOW 4096                 /* a packet's ticks are at most this far past ours (~3.4 min) */

enum { T_HELLO = 1, T_WELCOME, T_INPUT, T_HASH, T_ACK, T_READY, T_CLAIM, T_BYE, T_LOGREQ, T_FULL };

#pragma pack(push, 1)
typedef struct { uint32_t magic; uint8_t type, version; } hdr_t;
typedef struct { hdr_t h; uint8_t nlocal; } hello_t;
typedef struct { hdr_t h; uint8_t seat_base, nlocal, total, occupied, level, late; uint32_t seed;
                 char teams[MP_MAX_SEATS + 1]; } welcome_t;   /* the host's teams: every PC plays the same sides */
typedef struct { int32_t tick; uint8_t seat; int16_t turn, thrust; uint8_t btn; } entry_t;
typedef struct { hdr_t h; int32_t host_tick; uint16_t n; entry_t e[1]; } input_t;
typedef struct { hdr_t h; int32_t tick; uint32_t hash; } hash_t;
typedef struct { hdr_t h; int32_t tick; } tickmsg_t;      /* READY, CLAIM, LOGREQ */
#pragma pack(pop)

typedef struct { int16_t turn, thrust; uint8_t btn, valid; } rec_t;

static char     g_ini[MAX_PATH];
static HWND     g_frame;
static int      g_headless_net;
static volatile int g_active;            /* a session is running */
static int      g_is_host, g_port = 7795, g_want_clients, g_capacity;
static SOCKET   g_sock = INVALID_SOCKET;
static struct sockaddr_in g_host_addr;
static volatile DWORD g_host_seen;

/* The host's view of the others. */
static struct {
    struct sockaddr_in addr;
    int seat_base, nlocal, late, gone;
    int32_t claim;                       /* its seats are its own from this tick */
    volatile DWORD seen;
} g_peers[MAX_PEERS];
static int      g_npeers;
static int      g_owner[MP_MAX_SEATS];    /* host: -1 nobody, 0 the host, k+1 peer k */

/* The session. */
static int      g_total = 1, g_seat_base, g_nlocal = 1, g_occupied = 1, g_level, g_late;
static uint32_t g_seed;
static volatile int32_t g_claim;         /* a client's own seats count from this tick */
static volatile int32_t g_host_tick;     /* the latest tick the host has reached, as far as we know */

/* Every input since tick 0 (a joiner replays them) and every hash. */
static CRITICAL_SECTION g_lock;
static rec_t*   g_log;
static int32_t  g_log_ticks;
static uint32_t* g_hashes;               /* per 20 ticks: hash+1, 0 = not yet */
static int32_t  g_hash_slots;
static uint32_t g_peer_hash[64][2];      /* hashes others sent before we had ours: tick, hash */

static volatile int32_t g_cur = -1;      /* the tick the game is handling */
static volatile int32_t g_tick;          /* the driver's next tick */
static volatile uint32_t g_cb, g_cb_user;
static volatile int g_timer_on, g_armed;
static HWND     g_view;                  /* where the callback posts WM_USER */
static volatile int g_stopped;
static volatile int g_catching;          /* a joiner replaying: nobody watches the picture */
static DWORD    g_join_t0;
static int      g_desync_test = -1;
static char     g_status[128] = "offline";

int net_active(void) { return g_active; }
int net_is_host(void) { return g_is_host; }
int net_playing(void) { return g_active && g_armed; }
int net_catching_up(void) { return g_catching; }
void net_desync_test(int tick) { g_desync_test = tick; }

static void status(const char* fmt, ...) {
    va_list a;
    va_start(a, fmt);
    _vsnprintf(g_status, sizeof g_status - 1, fmt, a);
    va_end(a);
    fprintf(stderr, "[net] %s\n", g_status);
}

static void stop(const char* why) {
    if (g_stopped) return;
    g_stopped = 1;
    status("%s", why);
    if (!g_headless_net) MessageBoxA(NULL, g_status, "Hover! online", MB_OK | MB_ICONWARNING);
}

/* ------------------------------------------------------------ the record */

/* Callers hold g_lock. Ticks come off the wire, so one far past ours is
 * refused before it can size the log. */
static rec_t* slot(int32_t tick, int seat) {
    if (tick < 0 || tick > g_tick + TICK_WINDOW || seat < 0 || seat >= MP_MAX_SEATS) return NULL;
    if (tick >= g_log_ticks) {
        int32_t n = g_log_ticks ? g_log_ticks : 4096;
        while (n <= tick) n *= 2;
        rec_t* p = (rec_t*)realloc(g_log, (size_t)n * MP_MAX_SEATS * sizeof(rec_t));
        if (!p) return NULL;
        memset(p + (size_t)g_log_ticks * MP_MAX_SEATS, 0, (size_t)(n - g_log_ticks) * MP_MAX_SEATS * sizeof(rec_t));
        g_log = p;
        g_log_ticks = n;
    }
    return &g_log[(size_t)tick * MP_MAX_SEATS + seat];
}

/* The first record for a tick and seat wins: the host decides, and a late
 * copy from anyone else cannot change it. */
static void put(int32_t tick, int seat, int16_t turn, int16_t thrust, uint8_t btn) {
    EnterCriticalSection(&g_lock);
    rec_t* r = slot(tick, seat);
    if (r && !r->valid) {
        r->turn = turn;
        r->thrust = thrust;
        r->btn = btn;
        r->valid = 1;
    }
    LeaveCriticalSection(&g_lock);
}

static rec_t rec(int32_t tick, int seat) {
    rec_t r = { 0 };
    EnterCriticalSection(&g_lock);
    if (tick >= 0 && tick < g_log_ticks && seat >= 0 && seat < MP_MAX_SEATS)
        r = g_log[(size_t)tick * MP_MAX_SEATS + seat];
    LeaveCriticalSection(&g_lock);
    return r;
}

static int have(int32_t tick, int seat) { return rec(tick, seat).valid; }

/* mp.c's robot seats: the tick being handled. */
void net_seat_input(int seat, double* turn, double* thrust, int* buttons) {
    rec_t r = rec(g_cur, seat);
    *turn = r.turn / 32767.0;
    *thrust = r.thrust / 32767.0;
    *buttons = r.btn & 0x7F;
}

int net_seat_ai(int seat) { return (rec(g_cur, seat).btn & IN_AI) != 0; }

/* The key shims, online: the game's steering keys answer from seat 0's
 * record for the tick being handled, never from the keyboard. */
int net_key(int vk, int* down) {
    if (!g_active) return 0;
    rec_t r = rec(g_cur, 0);
    vk &= 0xFF;
    if (vk == (int)(MEM32(KEY_FORWARD) & 0xFF)) *down = r.thrust > 16384;
    else if (vk == (int)(MEM32(KEY_REVERSE) & 0xFF)) *down = r.thrust < -16384;
    else if (vk == (int)(MEM32(KEY_LEFT) & 0xFF)) *down = r.turn < -16384;
    else if (vk == (int)(MEM32(KEY_RIGHT) & 0xFF)) *down = r.turn > 16384;
    else *down = 0;                          /* the diagonals, and anything else */
    return 1;
}

static void keep_hash(int32_t tick, uint32_t h) {
    int32_t i = tick / 20;
    EnterCriticalSection(&g_lock);
    if (i >= g_hash_slots) {
        int32_t n = g_hash_slots ? g_hash_slots * 2 : 1024;
        while (n <= i) n *= 2;
        uint32_t* p = (uint32_t*)realloc(g_hashes, (size_t)n * sizeof *p);
        if (p) {
            memset(p + g_hash_slots, 0, (size_t)(n - g_hash_slots) * sizeof *p);
            g_hashes = p;
            g_hash_slots = n;
        }
    }
    if (i < g_hash_slots) g_hashes[i] = h + 1;
    LeaveCriticalSection(&g_lock);
}

static uint32_t own_hash(int32_t tick) {       /* hash + 1, or 0 */
    int32_t i = tick / 20;
    EnterCriticalSection(&g_lock);
    uint32_t v = i >= 0 && i < g_hash_slots ? g_hashes[i] : 0;
    LeaveCriticalSection(&g_lock);
    return v;
}

static void compare(int32_t tick, uint32_t theirs, const char* who) {
    uint32_t mine = own_hash(tick);
    if (!mine) {                                 /* ours comes later: keep theirs */
        g_peer_hash[(tick / 20) & 63][0] = (uint32_t)tick;
        g_peer_hash[(tick / 20) & 63][1] = theirs;
        return;
    }
    if (mine - 1 == theirs) {
        if (tick % 200 == 0) fprintf(stderr, "[net] in sync at tick %d (%08X)\n", tick, theirs);
        return;
    }
    char why[128];
    _snprintf(why, sizeof why - 1, "DESYNC at tick %d: %08X here, %08X on %s", tick, mine - 1, theirs, who);
    why[sizeof why - 1] = 0;
    stop(why);
}

/* ------------------------------------------------------------ transport */

static void send_to(const struct sockaddr_in* to, const void* p, int n) {
    sendto(g_sock, (const char*)p, n, 0, (const struct sockaddr*)to, sizeof *to);
}

static void hdr(hdr_t* h, int type) { h->magic = MAGIC; h->type = (uint8_t)type; h->version = VERSION; }

static void send_tick(const struct sockaddr_in* to, int type, int32_t tick) {
    tickmsg_t m;
    hdr(&m.h, type);
    m.tick = tick;
    send_to(to, &m, sizeof m);
}

/* Every record in [from, upto] for seats [s0, s1): as many packets as it
 * takes, at most `max_packets`. */
static void send_inputs(const struct sockaddr_in* to, int32_t from, int32_t upto, int s0, int s1, int max_packets) {
    char buf[1400];
    input_t* m = (input_t*)buf;
    const int cap = (int)((sizeof buf - sizeof(input_t) + sizeof(entry_t)) / sizeof(entry_t));
    int n = 0;
    hdr(&m->h, T_INPUT);
    m->host_tick = g_is_host ? g_tick : -1;
    for (int32_t t = from < 0 ? 0 : from; t <= upto && max_packets > 0; t++)
        for (int s = s0; s < s1; s++) {
            rec_t r = rec(t, s);
            if (!r.valid) continue;
            m->e[n].tick = t;
            m->e[n].seat = (uint8_t)s;
            m->e[n].turn = r.turn;
            m->e[n].thrust = r.thrust;
            m->e[n].btn = r.btn;
            if (++n == cap) {
                m->n = (uint16_t)n;
                send_to(to, buf, (int)(sizeof(input_t) - sizeof(entry_t) + n * sizeof(entry_t)));
                n = 0;
                max_packets--;
            }
        }
    if (n && max_packets > 0) {
        m->n = (uint16_t)n;
        send_to(to, buf, (int)(sizeof(input_t) - sizeof(entry_t) + n * sizeof(entry_t)));
    }
}

/* Our side of the window around tick `t`: a client sends its seats to the
 * host, the host every seat to every client still there. */
static void flush(int32_t t) {
    if (g_is_host) {
        for (int i = 0; i < g_npeers; i++)
            if (!g_peers[i].gone && !(g_peers[i].late && g_peers[i].claim == INT_MAX))
                send_inputs(&g_peers[i].addr, t - REDUND, t + DELAY, 0, g_total, 2);
    } else {
        send_inputs(&g_host_addr, t - REDUND, t + DELAY, g_seat_base, g_seat_base + g_nlocal, 1);
    }
}

static int peer_of(const struct sockaddr_in* a) {
    for (int i = 0; i < g_npeers; i++)
        if (g_peers[i].addr.sin_addr.s_addr == a->sin_addr.s_addr && g_peers[i].addr.sin_port == a->sin_port)
            return i;
    return -1;
}

static void start_session(void);

/* The host: seats for a PC that wants `nlocal` of them, the first free run. */
static int take_seats(int nlocal, int peer) {
    for (int b = 0; b + nlocal <= g_total; b++) {
        int ok = 1;
        for (int s = b; s < b + nlocal && ok; s++) ok = g_owner[s] < 0;
        if (!ok) continue;
        for (int s = b; s < b + nlocal; s++) g_owner[s] = peer + 1;
        return b;
    }
    return -1;
}

static void welcome(int i) {
    welcome_t w;
    hdr(&w.h, T_WELCOME);
    w.seat_base = (uint8_t)g_peers[i].seat_base;
    w.nlocal = (uint8_t)g_peers[i].nlocal;
    w.total = (uint8_t)g_total;
    w.occupied = (uint8_t)g_occupied;
    w.level = (uint8_t)g_level;
    w.late = (uint8_t)g_peers[i].late;
    w.seed = g_seed;
    mp_get_teams(w.teams);
    send_to(&g_peers[i].addr, &w, sizeof w);
}

static void host_hello(const struct sockaddr_in* from, int nlocal) {
    int i = peer_of(from);
    if (i >= 0) { if (g_active) welcome(i); return; }      /* a repeat: our WELCOME was lost */
    if (g_npeers == MAX_PEERS) return;
    nlocal = nlocal < 1 ? 1 : nlocal > MP_MAX_LOCAL ? MP_MAX_LOCAL : nlocal;
    char ip[64] = "?";
    inet_ntop(AF_INET, &from->sin_addr, ip, sizeof ip);
    i = g_npeers;
    if (!g_active) {                              /* the lobby: seats in order of arrival */
        if (g_total + nlocal > MP_MAX_SEATS) return;
        g_peers[i].seat_base = g_total;
        g_total += nlocal;
        g_peers[i].late = 0;
        g_peers[i].claim = 0;
    } else {                                      /* a game on: free seats, or none */
        int b = take_seats(nlocal, i);
        if (b < 0) {
            hdr_t f;
            hdr(&f, T_FULL);
            send_to(from, &f, sizeof f);
            fprintf(stderr, "[net] %s wanted %d seat(s): the game is full\n", ip, nlocal);
            return;
        }
        g_peers[i].seat_base = b;
        g_peers[i].late = 1;
        g_peers[i].claim = INT_MAX;               /* the AI drives its seats until it catches up */
    }
    g_peers[i].addr = *from;
    g_peers[i].nlocal = nlocal;
    g_peers[i].seen = GetTickCount();
    g_peers[i].gone = 0;
    g_npeers++;
    status("%s joined: seat%s %d-%d%s", ip, nlocal > 1 ? "s" : "", g_peers[i].seat_base + 1,
           g_peers[i].seat_base + nlocal, g_active ? " (catching up)" : "");
    if (g_active) welcome(i);
    else if (g_npeers >= g_want_clients) start_session();
}

/* The host: a client is gone (said BYE, or went quiet). Its seats go back to
 * the AI from the first tick we have no input for: prepare() fills those. */
static void host_drop(int i, const char* why) {
    if (g_peers[i].gone) return;
    g_peers[i].gone = 1;
    for (int s = g_peers[i].seat_base; s < g_peers[i].seat_base + g_peers[i].nlocal; s++) g_owner[s] = -1;
    status("seat%s %d-%d %s: the AI drives %s now", g_peers[i].nlocal > 1 ? "s" : "", g_peers[i].seat_base + 1,
           g_peers[i].seat_base + g_peers[i].nlocal, why, g_peers[i].nlocal > 1 ? "them" : "it");
}

static DWORD WINAPI recv_thread(LPVOID unused) {
    char buf[2048];
    (void)unused;
    for (;;) {
        struct sockaddr_in from;
        int fl = sizeof from;
        int n = recvfrom(g_sock, buf, sizeof buf, 0, (struct sockaddr*)&from, &fl);
        if (n < (int)sizeof(hdr_t)) { if (n == SOCKET_ERROR) Sleep(10); continue; }
        const hdr_t* h = (const hdr_t*)buf;
        if (h->magic != MAGIC) continue;
        /* A client listens to its host only; anyone else could end or steer
         * its game. The host hears HELLO from anyone, the rest from peers. */
        if (!g_is_host && (from.sin_addr.s_addr != g_host_addr.sin_addr.s_addr ||
                           from.sin_port != g_host_addr.sin_port)) continue;
        if (h->version != VERSION) {
            if (h->type == T_HELLO) fprintf(stderr, "[net] a PC with another version tried to join\n");
            continue;
        }
        int p = g_is_host ? peer_of(&from) : -1;
        if (g_is_host && p < 0 && h->type != T_HELLO) continue;
        if (p >= 0) g_peers[p].seen = GetTickCount();
        if (!g_is_host) g_host_seen = GetTickCount();

        switch (h->type) {
        case T_HELLO:
            if (g_is_host && n >= (int)sizeof(hello_t)) host_hello(&from, ((const hello_t*)buf)->nlocal);
            break;
        case T_WELCOME:
            if (!g_is_host && !g_active && n >= (int)sizeof(welcome_t)) {
                const welcome_t* w = (const welcome_t*)buf;
                if (w->total > MP_MAX_SEATS || w->nlocal < 1 || w->nlocal > g_nlocal ||
                    w->seat_base + w->nlocal > w->total || w->occupied > w->total || w->level > 19) break;
                g_seat_base = w->seat_base;
                g_nlocal = w->nlocal;
                g_total = w->total;
                g_occupied = w->occupied;
                g_level = w->level;
                g_seed = w->seed;
                g_late = w->late;
                g_claim = g_late ? INT_MAX : 0;
                char teams[MP_MAX_SEATS + 1];
                memcpy(teams, w->teams, MP_MAX_SEATS);
                teams[MP_MAX_SEATS] = 0;
                mp_set_teams(teams);
                start_session();
            }
            break;
        case T_FULL:
            if (!g_is_host && !g_active) stop("the game is full");
            break;
        case T_INPUT:
            if (n >= (int)(sizeof(input_t) - sizeof(entry_t))) {
                const input_t* m = (const input_t*)buf;
                int cnt = m->n;
                if ((int)(sizeof(input_t) - sizeof(entry_t) + cnt * sizeof(entry_t)) > n) break;
                if (!g_is_host && m->host_tick > g_host_tick) g_host_tick = m->host_tick;
                for (int i = 0; i < cnt; i++) {
                    const entry_t* e = &m->e[i];
                    /* The host takes a client's inputs only for its own seats, from its claim on. */
                    if (g_is_host && (p < 0 || g_peers[p].gone || e->seat < g_peers[p].seat_base ||
                                      e->seat >= g_peers[p].seat_base + g_peers[p].nlocal ||
                                      e->tick < g_peers[p].claim)) continue;
                    put(e->tick, e->seat, e->turn, e->thrust, e->btn);
                }
            }
            break;
        case T_HASH:
            if (n >= (int)sizeof(hash_t)) {
                const hash_t* m = (const hash_t*)buf;
                compare(m->tick, m->hash, g_is_host ? "a client" : "the host");
                if (g_is_host)                   /* everyone else checks against it too */
                    for (int k = 0; k < g_npeers; k++)
                        if (k != p && !g_peers[k].gone) send_to(&g_peers[k].addr, m, sizeof *m);
            }
            break;
        case T_LOGREQ:                           /* a joiner replaying: the log from its tick on */
            if (g_is_host && p >= 0 && n >= (int)sizeof(tickmsg_t)) {
                int32_t t = ((const tickmsg_t*)buf)->tick;
                send_inputs(&from, t, t + 60, 0, g_total, 8);
            }
            break;
        case T_READY:                            /* a joiner caught up: its seats, from ahead */
            if (g_is_host && p >= 0 && g_peers[p].late) {
                if (g_peers[p].claim == INT_MAX) {
                    g_peers[p].claim = g_tick + DELAY + CLAIM_AHEAD;
                    status("seat%s %d-%d caught up: theirs from tick %d", g_peers[p].nlocal > 1 ? "s" : "",
                           g_peers[p].seat_base + 1, g_peers[p].seat_base + g_peers[p].nlocal, g_peers[p].claim);
                }
                send_tick(&from, T_CLAIM, g_peers[p].claim);
            }
            break;
        case T_CLAIM:
            if (!g_is_host && n >= (int)sizeof(tickmsg_t) && g_claim == INT_MAX) {
                g_claim = ((const tickmsg_t*)buf)->tick;
                status("caught up: %d ticks replayed in %.1f s; our seats are ours from tick %d", g_tick,
                       g_join_t0 ? (GetTickCount() - g_join_t0) / 1000.0 : 0.0, g_claim);
            }
            break;
        case T_BYE:
            if (g_is_host && p >= 0) host_drop(p, "left");
            else if (!g_is_host && g_active) stop("the host ended the game");
            break;
        }
    }
}

/* ------------------------------------------------------------ the tick gate */

static void post_buttons(int32_t t) {
    static const uint32_t keys[3] = { KEY_JUMP, KEY_WALL, KEY_CLOAK };
    rec_t now = rec(t, 0), was = rec(t - 1, 0);
    if (!g_view || (now.btn & IN_AI)) return;
    for (int i = 0; i < 3; i++)
        if ((now.btn & (1 << i)) && !(was.btn & (1 << i))) {    /* pressed on this tick */
            int vk = (int)(MEM32(keys[i]) & 0xFF);
            PostMessageA(g_view, WM_KEYDOWN, vk, 1);
            PostMessageA(g_view, WM_KEYUP, vk, 0xC0000001u);
        }
}

static void fill_ai(int32_t tick);

/* Between ticks N-1 and N the world is still: hash it, and write our records
 * for tick N+DELAY. */
static void prepare(int32_t tick) {
    if (tick % 20 == 0) {
        uint32_t h = mp_world_hash();
        hash_t m;
        keep_hash(tick, h);
        hdr(&m.h, T_HASH);
        m.tick = tick;
        m.hash = h;
        if (g_is_host) {
            for (int k = 0; k < g_npeers; k++) if (!g_peers[k].gone) send_to(&g_peers[k].addr, &m, sizeof m);
        } else {
            send_to(&g_host_addr, &m, sizeof m);
        }
        uint32_t* ph = g_peer_hash[(tick / 20) & 63];
        if (ph[0] == (uint32_t)tick) compare(tick, ph[1], g_is_host ? "a client" : "the host");
    }
    int32_t at = tick + DELAY;
    if (g_is_host || at >= g_claim)              /* a joiner's seats are the AI's until its claim */
        for (int li = 0; li < g_nlocal; li++) {
            double t, f;
            int b;
            mp_local_input(li, &t, &f, &b);
            put(at, g_seat_base + li, (int16_t)(t * 32767), (int16_t)(f * 32767), (uint8_t)b);
        }
    if (g_is_host) fill_ai(tick);
}

/* The host speaks for every seat no player drives: empty, left, or still
 * catching up. A seat whose player just left may be missing ticks before
 * this one too: those are the AI's as well. Called again while the gate
 * waits, since a player can be found gone after its tick was prepared. */
static void fill_ai(int32_t tick) {
    int32_t at = tick + DELAY;
    for (int s = 0; s < g_total; s++) {
        int ai = g_owner[s] < 0;
        if (g_owner[s] > 0) {
            int k = g_owner[s] - 1;
            ai = g_peers[k].gone || at < g_peers[k].claim;
        }
        if (!ai) continue;
        for (int32_t t = tick; t <= at; t++) put(t, s, 0, 0, IN_AI);
    }
}

static DWORD WINAPI driver(LPVOID unused) {
    int32_t tick = 0, prepared = -1;
    DWORD due = GetTickCount(), waited = GetTickCount(), sent = 0, f2 = GetTickCount(), asked = 0;
    (void)unused;
    for (;;) {
        g_tick = tick;
        if (g_stopped) { Sleep(50); continue; }
        if (g_is_host)                           /* a client gone quiet counts as gone */
            for (int k = 0; k < g_npeers; k++)
                if (!g_peers[k].gone && GetTickCount() - g_peers[k].seen > PEER_TIMEOUT) host_drop(k, "went quiet");
        if (!g_is_host && GetTickCount() - g_host_seen > HOST_TIMEOUT) { stop("the host went quiet: the game is over"); continue; }
        if (!g_timer_on) {
            /* The session's F2 can land while the game is still busy (its
             * Quick Help dialog, say): press it again until a level starts. */
            if (tick == 0 && MEM32(G_GAME_STATE) < 2 && GetTickCount() - f2 > 4000) {
                f2 = GetTickCount();
                levels_session(g_level, g_seed);
            }
            if (!g_is_host && GetTickCount() - sent > 500) {   /* keep the host hearing from us */
                send_tick(&g_host_addr, T_LOGREQ, tick);
                sent = GetTickCount();
            }
            Sleep(5);
            due = GetTickCount();
            continue;
        }
        int catching_up = !g_is_host && g_late && tick < g_host_tick - DELAY - 2;
        if (catching_up && !g_join_t0) g_join_t0 = GetTickCount();
        g_catching = catching_up;
        if (!catching_up && (int)(GetTickCount() - due) < 0) { Sleep(1); continue; }
        if (MEM32(G_TICK_BUSY)) {                /* tick-1 not handled yet */
            Sleep(catching_up ? 0 : 1);
            if (GetTickCount() - waited > 2000) {
                waited = GetTickCount();
                status("the game has not finished tick %d yet (state %u)", tick - 1, MEM32(G_GAME_STATE));
            }
            continue;
        }

        if (prepared != tick) {
            prepared = tick;
            prepare(tick);
            flush(tick);
            sent = GetTickCount();
        }
        if (!g_is_host && g_late && !catching_up && g_claim == INT_MAX && GetTickCount() - asked > 100) {
            asked = GetTickCount();              /* caught up: ask for our seats */
            send_tick(&g_host_addr, T_READY, tick);
        }

        int ready = 1;
        for (int s = 0; s < g_total && ready; s++) ready = have(tick, s);
        if (!ready) {                            /* stall, and keep things moving */
            Sleep(catching_up ? 0 : 2);
            if (g_is_host) fill_ai(tick);
            if (!g_is_host && g_late && g_claim == INT_MAX && GetTickCount() - asked > 20) {
                asked = GetTickCount();          /* a joiner pulls the log it is missing */
                send_tick(&g_host_addr, T_LOGREQ, tick);
            }
            if (GetTickCount() - sent > 30) { flush(tick); sent = GetTickCount(); }
            if (GetTickCount() - waited > 2000) {
                waited = GetTickCount();
                status("waiting for the other players at tick %d", tick);
            }
            continue;
        }
        if (tick == g_desync_test) {             /* the check must catch a one-unit difference */
            uint32_t doc = MEM32(G_DOC);
            if (doc) MEM32(doc + 0x818Cu + 0x44) += 1;
            fprintf(stderr, "[net] desync test: nudged the human craft at tick %d\n", tick);
        }
        MEM32(G_JOYSTICK) = 0;                   /* the handler must not poll a local joystick */
        post_buttons(tick);
        g_cur = tick;
        uint32_t args[5] = { FAKE_TIMER, 0, g_cb_user, 0, 0 };
        native32_call_guest(g_cb, 5, args);      /* posts WM_USER: the game handles tick `tick` */
        if (tick % 200 == 0) fprintf(stderr, "[net] tick %d%s\n", tick, catching_up ? " (catching up)" : "");
        tick++;
        waited = GetTickCount();
        due += 50;
        /* Stalled: no burst to catch up. Replaying: no debt either, or the
         * first real tick would wait out every replayed one's 50 ms. */
        if (catching_up || (int)(GetTickCount() - due) > 250) due = GetTickCount();
    }
}

/* The game's timer, online: ours. 0x00412804 is its only timeSetEvent. */
int net_timer_set(uint32_t cb, uint32_t user, uint32_t* id) {
    if (!g_active || !g_armed || cb != TICK_CB) return 0;    /* the attract demo keeps a real timer */
    g_cb = cb;
    g_cb_user = user;
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
    /* 1 ms waits: at the default 15.6 ms the gate's short sleeps cost ticks
     * (8 ticks a second instead of the game's 20). */
    timeBeginPeriod(1);
    if (g_is_host) {
        g_seed = levels_current_seed();
        g_level = (int)MEM32(G_START_AT) - 1;
        if (g_level < 0 || g_level > 19) g_level = 0;
        g_occupied = g_total;                    /* the seats taken when the game starts */
        if (g_capacity > g_total) g_total = g_capacity;
        for (int s = 0; s < MP_MAX_SEATS; s++) g_owner[s] = -1;
        for (int s = 0; s < g_nlocal; s++) g_owner[s] = 0;
        for (int i = 0; i < g_npeers; i++)
            for (int s = g_peers[i].seat_base; s < g_peers[i].seat_base + g_peers[i].nlocal; s++) g_owner[s] = i + 1;
        for (int i = 0; i < g_npeers; i++) for (int k = 0; k < 3; k++) welcome(i);
    } else {
        hdr_t a;
        hdr(&a, T_ACK);
        send_to(&g_host_addr, &a, sizeof a);
    }
    for (int li = 0; li < g_nlocal; li++) local[li] = g_seat_base + li;
    mp_set_seats(g_total, local, g_nlocal);
    if (!g_late)                                 /* the first ticks: nobody has pressed anything */
        for (int32_t t = 0; t < DELAY; t++)
            for (int s = 0; s < g_total; s++) put(t, s, 0, 0, s < g_occupied ? 0 : IN_AI);
    g_host_seen = GetTickCount();
    g_active = 1;
    status("game on: %d seats (%d open), level %d, seed %u; this PC has seat%s %d-%d%s", g_total,
           g_total - g_occupied, g_level + 1, g_seed, g_nlocal > 1 ? "s" : "", g_seat_base + 1,
           g_seat_base + g_nlocal, g_late ? ", replaying the game so far" : "");
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
        closesocket(g_sock);
        g_sock = INVALID_SOCKET;
        return 0;
    }
    CloseHandle(CreateThread(NULL, 0, recv_thread, NULL, 0, NULL));
    return 1;
}

int net_host(int port, int clients, int nlocal, int seats) {
    if (g_sock != INVALID_SOCKET) return 0;
    g_is_host = 1;
    g_want_clients = clients < 0 ? 0 : clients > MAX_PEERS ? MAX_PEERS : clients;
    g_nlocal = nlocal < 1 ? 1 : nlocal > MP_MAX_LOCAL ? MP_MAX_LOCAL : nlocal;
    g_capacity = seats < g_nlocal ? g_nlocal : seats > MP_MAX_SEATS ? MP_MAX_SEATS : seats;
    g_total = g_nlocal;
    g_seat_base = 0;
    g_port = port;
    if (!open_socket(port)) return 0;
    if (g_want_clients) status("hosting on UDP port %d: waiting for %d PC%s", port, g_want_clients,
                               g_want_clients > 1 ? "s" : "");
    else start_session();                        /* an open game: players drop in */
    return 1;
}

/* A client resends HELLO until the host's WELCOME arrives. */
static DWORD WINAPI hello_thread(LPVOID p) {
    hello_t m;
    hdr(&m.h, T_HELLO);
    m.nlocal = (uint8_t)(intptr_t)p;
    while (!g_active && !g_stopped) {
        send_to(&g_host_addr, &m, sizeof m);
        Sleep(250);
    }
    return 0;
}

/* ------------------------------------------------------------ join codes */

/* A join code is the host's IPv4 address and port, in a form that reads out
 * loud and survives a chat window: HOVER-XXXXX-XXXXX (Crockford base 32). */
static const char k_b32[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";

static void encode(uint32_t ip, int port, char* out, size_t n) {
    uint64_t v = ((uint64_t)ntohl(ip) << 16) | (uint16_t)port;
    char c[10];
    for (int i = 9; i >= 0; i--) { c[i] = k_b32[v & 31]; v >>= 5; }
    _snprintf(out, n, "HOVER-%.5s-%.5s", c, c + 5);
}

static int decode(const char* s, char* host, size_t n, int* port) {
    uint64_t v = 0;
    int k = 0;
    while (*s == ' ' || *s == '\t') s++;
    if (_strnicmp(s, "HOVER-", 6)) return 0;
    for (s += 6; *s && k < 10; s++) {
        if (*s == '-') continue;
        const char* p = strchr(k_b32, toupper((unsigned char)*s));
        if (!p || !*s) return 0;
        v = (v << 5) | (uint64_t)(p - k_b32);
        k++;
    }
    if (k != 10) return 0;
    uint32_t ip = (uint32_t)(v >> 16);
    *port = (int)(v & 0xFFFF);
    _snprintf(host, n, "%u.%u.%u.%u", ip >> 24, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255);
    return 1;
}

/* The address to give out: a Tailscale one (100.64.0.0/10) if this PC has
 * one, since it works from anywhere, then a private LAN address. */
static uint32_t best_address(void) {
    char name[256];
    struct addrinfo hints = { 0 }, *res = NULL, *a;
    uint32_t best = 0;
    int rank = 0;
    hints.ai_family = AF_INET;
    if (gethostname(name, sizeof name) || getaddrinfo(name, NULL, &hints, &res)) return 0;
    for (a = res; a; a = a->ai_next) {
        uint32_t ip = ((struct sockaddr_in*)a->ai_addr)->sin_addr.s_addr, h = ntohl(ip);
        int r = (h & 0xFFC00000u) == 0x64400000u ? 3                                   /* Tailscale */
              : ((h >> 24) == 192 && ((h >> 16) & 255) == 168) || (h >> 24) == 10 ||
                (h >> 20) == 0xAC1 ? 2 : (h >> 24) != 127 ? 1 : 0;
        if (r > rank) { rank = r; best = ip; }
    }
    freeaddrinfo(res);
    return best;
}

int net_join(const char* addr, int nlocal) {
    char host[128];
    int port = g_port;
    struct addrinfo hints = { 0 }, *res = NULL;
    if (g_sock != INVALID_SOCKET) return 0;
    if (!decode(addr, host, sizeof host, &port)) {
        strncpy(host, addr, sizeof host - 1);
        host[sizeof host - 1] = 0;
        char* colon = strrchr(host, ':');
        if (colon) { *colon = 0; port = atoi(colon + 1); }
    }
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

/* Leaving: say so, so the others need not wait for the silence. */
void net_leave(void) {
    hdr_t b;
    if (g_sock == INVALID_SOCKET) return;
    hdr(&b, T_BYE);
    for (int k = 0; k < 3; k++) {
        if (g_is_host) { for (int i = 0; i < g_npeers; i++) if (!g_peers[i].gone) send_to(&g_peers[i].addr, &b, sizeof b); }
        else send_to(&g_host_addr, &b, sizeof b);
    }
}

void net_init(const char* ini, HWND frame, int headless) {
    strncpy(g_ini, ini, sizeof g_ini - 1);
    g_frame = frame;
    g_headless_net = headless;
    InitializeCriticalSection(&g_lock);
    memset(g_peer_hash, 0xFF, sizeof g_peer_hash);   /* an empty slot is no tick, not tick 0 */
    if (!GetPrivateProfileIntA("net", "port", 0, g_ini)) {
        WritePrivateProfileStringA("net", "port", "7795", g_ini);
        WritePrivateProfileStringA("net", "clients", "0", g_ini);
        WritePrivateProfileStringA("net", "seats", "8", g_ini);
        WritePrivateProfileStringA("net", "join", "", g_ini);
    }
    g_port = GetPrivateProfileIntA("net", "port", 7795, g_ini);
}

/* --net-selftest: the join codes, and the log's bound on wire ticks. */
int net_selftest(void) {
    char code[32], host[64];
    int port = 0, fails = 0;
    if (slot(INT_MAX - 5, 0) || slot(g_tick + TICK_WINDOW + 1, 0) || g_log_ticks) fails++;
    if (!slot(g_tick + 10, 0)) fails++;
    struct in_addr a;
    inet_pton(AF_INET, "100.101.102.103", &a);
    encode(a.s_addr, 7795, code, sizeof code);
    if (!decode(code, host, sizeof host, &port) || strcmp(host, "100.101.102.103") || port != 7795) fails++;
    if (decode("HOVER-12345", host, sizeof host, &port) || decode("100.1.2.3", host, sizeof host, &port)) fails++;
    fprintf(stderr, "[net] selftest %s (%s)\n", fails ? "FAILED" : "OK", code);
    return fails;
}

/* ------------------------------------------------------------ menu */

enum { ID_STATUS = 0x6A00, ID_HOST, ID_JOIN, ID_CODE, ID_LEAVE };

void net_menu(HMENU m) {
    AppendMenuA(m, MF_SEPARATOR, 0, NULL);
    AppendMenuA(m, MF_STRING | MF_GRAYED, ID_STATUS, "Online: offline");
    AppendMenuA(m, MF_STRING, ID_HOST, "&Host an online game");
    AppendMenuA(m, MF_STRING, ID_CODE, "&Copy the join code");
    AppendMenuA(m, MF_STRING, ID_JOIN, "&Join an online game");
    AppendMenuA(m, MF_STRING, ID_LEAVE, "L&eave the online game (quits)");
}

void net_update_menu(HMENU popup) {
    char t[160];
    int seats = GetPrivateProfileIntA("net", "seats", 8, g_ini);
    _snprintf(t, sizeof t, "Online: %s", g_status);
    t[sizeof t - 1] = 0;
    ModifyMenuA(popup, ID_STATUS, MF_BYCOMMAND | MF_STRING | MF_GRAYED, ID_STATUS, t);
    UINT idle = g_sock == INVALID_SOCKET ? MF_ENABLED : MF_GRAYED;
    _snprintf(t, sizeof t, "&Host an online game (%d seats, UDP port %d)", seats, g_port);
    t[sizeof t - 1] = 0;
    ModifyMenuA(popup, ID_HOST, MF_BYCOMMAND | MF_STRING | idle, ID_HOST, t);
    ModifyMenuA(popup, ID_JOIN, MF_BYCOMMAND | MF_STRING | idle, ID_JOIN,
                "&Join an online game (the join code or address on the clipboard)");
    EnableMenuItem(popup, ID_CODE, MF_BYCOMMAND | (g_is_host && g_sock != INVALID_SOCKET ? MF_ENABLED : MF_GRAYED));
    EnableMenuItem(popup, ID_LEAVE, MF_BYCOMMAND | (g_active ? MF_ENABLED : MF_GRAYED));
}

static void clip_put(const char* s) {
    size_t n = strlen(s) + 1;
    HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, n);
    if (!g) return;
    memcpy(GlobalLock(g), s, n);
    GlobalUnlock(g);
    if (OpenClipboard(NULL)) {
        EmptyClipboard();
        if (!SetClipboardData(CF_TEXT, g)) GlobalFree(g);
        CloseClipboard();
    } else GlobalFree(g);
}

int net_command(UINT id) {
    int nlocal = mp_local_count();
    if (id == ID_HOST) {
        net_host(g_port, GetPrivateProfileIntA("net", "clients", 0, g_ini), nlocal,
                 GetPrivateProfileIntA("net", "seats", 8, g_ini));
        if (g_is_host && g_sock != INVALID_SOCKET) net_command(ID_CODE);
    } else if (id == ID_CODE) {
        char code[32], msg[256], ips[64] = "?";
        uint32_t ip = best_address();
        struct in_addr a;
        a.s_addr = ip;
        encode(ip, g_port, code, sizeof code);
        clip_put(code);
        inet_ntop(AF_INET, &a, ips, sizeof ips);
        _snprintf(msg, sizeof msg - 1, "Join code %s (%s:%d) is on the clipboard.\n\n"
                                       "Friends copy it and choose Recomp > Multiplayer > Join.", code, ips, g_port);
        msg[sizeof msg - 1] = 0;
        status("hosting: join code %s", code);
        if (!g_headless_net) MessageBoxA(g_frame, msg, "Hover! online", MB_OK | MB_ICONINFORMATION);
    } else if (id == ID_JOIN) {
        char a[96] = "";
        if (OpenClipboard(NULL)) {           /* a code or address someone pasted wins over the ini */
            HANDLE h = GetClipboardData(CF_TEXT);
            const char* p = h ? (const char*)GlobalLock(h) : NULL;
            if (p && strlen(p) < sizeof a) {
                strcpy(a, p);
                for (char* e = a + strlen(a); e > a && (e[-1] == '\r' || e[-1] == '\n' || e[-1] == ' '); ) *--e = 0;
                if (strchr(a, ' ') || !(strchr(a, '.') || strchr(a, ':') || !_strnicmp(a, "HOVER-", 6))) a[0] = 0;
            }
            if (p) GlobalUnlock(h);
            CloseClipboard();
        }
        if (!a[0]) GetPrivateProfileStringA("net", "join", "", a, sizeof a, g_ini);
        if (!a[0]) {
            MessageBoxA(g_frame, "Copy the host's join code (HOVER-XXXXX-XXXXX) or address and choose Join again.",
                        "Hover! online", MB_OK | MB_ICONINFORMATION);
            return 1;
        }
        WritePrivateProfileStringA("net", "join", a, g_ini);
        net_join(a, nlocal);
    } else if (id == ID_LEAVE) {
        net_leave();
        PostMessageA(g_frame, WM_CLOSE, 0, 0);
    } else return 0;
    return 1;
}
