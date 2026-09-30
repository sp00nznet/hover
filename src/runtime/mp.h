/* Multiplayer seats and views: mp.c. */
#pragma once
#include <windows.h>
#include <stdint.h>
#include "recomp_types.h"

#define MP_MAX_SEATS 16             /* seats in one game, every PC */
#define MP_MAX_LOCAL 4              /* views on one PC */
enum { MP_JUMP = 1, MP_WALL = 2, MP_CLOAK = 4 };

void          mp_init(const char* ini, int cli_players, int (*key_down)(int vk));
void          mp_set_seats(int total, const int* local, int nlocal);
int           mp_seats(void);
int           mp_local_count(void);
int           mp_local_seat(int li);            /* this PC's li-th seat, or -1 */
void          mp_local_input(int li, double* turn, double* thrust, int* buttons);
uint32_t      mp_world_hash(void);              /* every craft's state, for the desync check */
void          mp_apply_table(void);             /* after the image is mapped: seats' robots */
long          mp_layout(void);                  /* changes when the view grid does */
void          mp_grid(int* cols, int* rows);
int           mp_view(void);                    /* the render pass now running, 0 = first local seat */
int           mp_in_views(void);                /* is a render pass running on this thread? */
recomp_func_t mp_lookup(uint32_t va);           /* for recomp_lookup_manual */
void          mp_menu(HMENU recomp);
void          mp_update_menu(HMENU popup);
int           mp_command(UINT id);

/* Called from the lifted frame draw (patched in by run_lift.py). */
uint32_t hover_cam_obj(uint32_t doc);
void     hover_render_views(void);
