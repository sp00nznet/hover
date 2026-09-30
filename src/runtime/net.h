/* Online play, lockstep over UDP: net.c. */
#pragma once
#include <windows.h>
#include <stdint.h>

void net_init(const char* ini, HWND frame, int headless);
int  net_host(int port, int clients, int nlocal);   /* wait for `clients` PCs, then start */
int  net_join(const char* addr, int nlocal);        /* "host[:port]" */
int  net_active(void);                              /* a session is running */
int  net_is_host(void);
void net_desync_test(int tick);                     /* a deliberate one-unit nudge: the check's test */

/* Inputs for the tick the game is handling. */
void net_seat_input(int seat, double* turn, double* thrust, int* buttons);   /* robot seats (mp.c) */
int  net_key(int vk, int* down);                    /* the key shims: 1 = answered from the record */

/* The game's timer and its tick message (host.c's shims). */
int  net_timer_set(uint32_t cb, uint32_t user, uint32_t* id);   /* 1 = taken over */
int  net_timer_kill(uint32_t id);
void net_saw_post(HWND h, UINT msg);
void net_level_loading(int demo);                  /* levels.c: a level (or the demo) is loading */

void net_menu(HMENU multiplayer);
void net_update_menu(HMENU popup);
int  net_command(UINT id);
