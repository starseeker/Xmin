#ifndef XMIN_CLIENT_GLXEXT_H
#define XMIN_CLIENT_GLXEXT_H

#include <GL/glx.h>

#define GLX_ARB_get_proc_address 1
#define GLX_ARB_create_context 1
#define GLX_EXT_swap_control 1
#define GLX_MESA_swap_control 1
#define GLX_SGI_swap_control 1

#define GLX_SWAP_INTERVAL_EXT 0x20F1
#define GLX_MAX_SWAP_INTERVAL_EXT 0x20F2

typedef void (*PFNGLXSWAPINTERVALEXTPROC)(Display *, GLXDrawable, int);
typedef int (*PFNGLXSWAPINTERVALMESAPROC)(unsigned int);
typedef int (*PFNGLXGETSWAPINTERVALMESAPROC)(void);
typedef int (*PFNGLXSWAPINTERVALSGIPROC)(int);

#ifdef __cplusplus
extern "C" {
#endif

void glXSwapIntervalEXT(Display *, GLXDrawable, int);
int glXSwapIntervalSGI(int);

#ifdef __cplusplus
}
#endif

#endif
