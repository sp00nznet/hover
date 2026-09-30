/* Xbox controller support and the "Recomp" menu: pad.c. */
#pragma once
#include <windows.h>

void pad_start(const char* ini, HWND frame);   /* load [pad], start polling */
int  pad_held(int vk);                         /* is the pad holding this key? */
void pad_add_menu(HWND frame);                 /* append "Recomp" to the menu bar */
void pad_update_menu(HMENU popup);             /* after MFC's WM_INITMENUPOPUP */
int  pad_command(UINT id);                     /* 1 if the WM_COMMAND id was ours */
int  pad_selftest(void);                       /* --pad-selftest; 0 = pass */
