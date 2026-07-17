#ifndef XMIN_KEYSYMS_H
#define XMIN_KEYSYMS_H

/* Stable X11 keysym values used by the controller. Keeping this deliberately
 * small avoids making a command-line protocol client depend on X11 headers. */
#define XK_BackSpace 0xff08
#define XK_Tab 0xff09
#define XK_Return 0xff0d
#define XK_Escape 0xff1b
#define XK_Home 0xff50
#define XK_Left 0xff51
#define XK_Up 0xff52
#define XK_Right 0xff53
#define XK_Down 0xff54
#define XK_Page_Up 0xff55
#define XK_Page_Down 0xff56
#define XK_End 0xff57
#define XK_Insert 0xff63
#define XK_F1 0xffbe
#define XK_F2 0xffbf
#define XK_F3 0xffc0
#define XK_F4 0xffc1
#define XK_F5 0xffc2
#define XK_F6 0xffc3
#define XK_F7 0xffc4
#define XK_F8 0xffc5
#define XK_F9 0xffc6
#define XK_F10 0xffc7
#define XK_F11 0xffc8
#define XK_F12 0xffc9
#define XK_Delete 0xffff
#define XK_Shift_L 0xffe1
#define XK_Control_L 0xffe3
#define XK_Alt_L 0xffe9
#define XK_Super_L 0xffeb
#define XK_space 0x0020
#define XK_a 0x0061

#endif
