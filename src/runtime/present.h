/* The presenter: present.c. */
#pragma once
#include <windows.h>
#include <stdint.h>

/* lParam that marks an activation change the presenter really saw, so
 * host.c lets it through to the game (it drops every other deactivation). */
#define PRESENT_REAL_ACTIVATION 0x484F5652

int   present_wanted(const char* ini);          /* load [video]; is presenter=1? */
void  present_load(void);                       /* reload [video] */
void  present_start(HWND frame, HMENU menu, HICON icon);
int   present_running(void);
int   present_focused(void);                    /* is the presenter the foreground window? */
void  present_source(const uint32_t* bgra, int w, int h);   /* the shadow */
void  present_lock(void);                       /* around writes to the shadow */
void  present_unlock(void);                     /* ...and a new picture to show */
void  present_set_title(const char* t);

void  present_menu(HMENU recomp);               /* the Video submenu */
void  present_update_menu(HMENU popup);
int   present_command(UINT id);                 /* 1 if the id was ours */
