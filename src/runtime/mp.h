/* Local multiplayer (split screen): mp.c. */
#pragma once
#include <windows.h>
#include <stdint.h>
#include "recomp_types.h"

void          mp_init(const char* ini, int cli_players, int (*key_down)(int vk));
int           mp_players(void);
void          mp_apply_table(void);             /* after the image is mapped: seats' robots */
long          mp_layout(void);                  /* changes when the view grid does */
void          mp_grid(int* cols, int* rows);
int           mp_view(void);                    /* the render pass now running, 0 = player 1 */
int           mp_in_views(void);                /* is a render pass running at all? */
recomp_func_t mp_lookup(uint32_t va);           /* for recomp_lookup_manual */
void          mp_menu(HMENU recomp);
void          mp_update_menu(HMENU popup);
int           mp_command(UINT id);

/* Called from the lifted frame draw (patched in by run_lift.py). */
uint32_t hover_cam_obj(uint32_t doc);
void     hover_render_views(void);
