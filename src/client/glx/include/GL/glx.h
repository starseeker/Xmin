#ifndef XMIN_CLIENT_GLX_H
#define XMIN_CLIENT_GLX_H

#include <GL/gl.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GLX_VERSION_1_1 1
#define GLX_VERSION_1_2 1
#define GLX_VERSION_1_3 1
#define GLX_VERSION_1_4 1
#define GLX_EXTENSION_NAME "GLX"

#define GLX_USE_GL 1
#define GLX_BUFFER_SIZE 2
#define GLX_LEVEL 3
#define GLX_RGBA 4
#define GLX_DOUBLEBUFFER 5
#define GLX_STEREO 6
#define GLX_AUX_BUFFERS 7
#define GLX_RED_SIZE 8
#define GLX_GREEN_SIZE 9
#define GLX_BLUE_SIZE 10
#define GLX_ALPHA_SIZE 11
#define GLX_DEPTH_SIZE 12
#define GLX_STENCIL_SIZE 13
#define GLX_ACCUM_RED_SIZE 14
#define GLX_ACCUM_GREEN_SIZE 15
#define GLX_ACCUM_BLUE_SIZE 16
#define GLX_ACCUM_ALPHA_SIZE 17

#define GLX_BAD_SCREEN 1
#define GLX_BAD_ATTRIBUTE 2
#define GLX_NO_EXTENSION 3
#define GLX_BAD_VISUAL 4
#define GLX_BAD_CONTEXT 5
#define GLX_BAD_VALUE 6
#define GLX_BAD_ENUM 7

#define GLX_VENDOR 1
#define GLX_VERSION 2
#define GLX_EXTENSIONS 3

#define GLX_CONFIG_CAVEAT 0x20
#define GLX_DONT_CARE 0xFFFFFFFF
#define GLX_X_VISUAL_TYPE 0x22
#define GLX_TRANSPARENT_TYPE 0x23
#define GLX_WINDOW_BIT 0x00000001
#define GLX_PIXMAP_BIT 0x00000002
#define GLX_PBUFFER_BIT 0x00000004
#define GLX_NONE 0x8000
#define GLX_TRUE_COLOR 0x8002
#define GLX_VISUAL_ID 0x800B
#define GLX_SCREEN 0x800C
#define GLX_DRAWABLE_TYPE 0x8010
#define GLX_RENDER_TYPE 0x8011
#define GLX_X_RENDERABLE 0x8012
#define GLX_FBCONFIG_ID 0x8013
#define GLX_RGBA_TYPE 0x8014
#define GLX_MAX_PBUFFER_WIDTH 0x8016
#define GLX_MAX_PBUFFER_HEIGHT 0x8017
#define GLX_MAX_PBUFFER_PIXELS 0x8018
#define GLX_PRESERVED_CONTENTS 0x801B
#define GLX_LARGEST_PBUFFER 0x801C
#define GLX_WIDTH 0x801D
#define GLX_HEIGHT 0x801E
#define GLX_EVENT_MASK 0x801F
#define GLX_PBUFFER_HEIGHT 0x8040
#define GLX_PBUFFER_WIDTH 0x8041
#define GLX_RGBA_BIT 0x00000001
#define GLX_SAMPLE_BUFFERS 100000
#define GLX_SAMPLES 100001

#define GLX_CONTEXT_MAJOR_VERSION_ARB 0x2091
#define GLX_CONTEXT_MINOR_VERSION_ARB 0x2092
#define GLX_CONTEXT_FLAGS_ARB 0x2094
#define GLX_CONTEXT_DEBUG_BIT_ARB 0x00000001
#define GLX_CONTEXT_FORWARD_COMPATIBLE_BIT_ARB 0x00000002
#define GLX_CONTEXT_PROFILE_MASK_ARB 0x9126
#define GLX_CONTEXT_CORE_PROFILE_BIT_ARB 0x00000001
#define GLX_CONTEXT_COMPATIBILITY_PROFILE_BIT_ARB 0x00000002

typedef struct __GLXcontextRec *GLXContext;
typedef struct __GLXFBConfigRec *GLXFBConfig;
typedef XID GLXPixmap;
typedef XID GLXDrawable;
typedef XID GLXFBConfigID;
typedef XID GLXContextID;
typedef XID GLXWindow;
typedef XID GLXPbuffer;
typedef void (*__GLXextFuncPtr)(void);

XVisualInfo *glXChooseVisual(Display *, int, int *);
GLXContext glXCreateContext(Display *, XVisualInfo *, GLXContext, Bool);
void glXDestroyContext(Display *, GLXContext);
Bool glXMakeCurrent(Display *, GLXDrawable, GLXContext);
void glXCopyContext(Display *, GLXContext, GLXContext, unsigned long);
void glXSwapBuffers(Display *, GLXDrawable);
GLXPixmap glXCreateGLXPixmap(Display *, XVisualInfo *, Pixmap);
void glXDestroyGLXPixmap(Display *, GLXPixmap);
Bool glXQueryExtension(Display *, int *, int *);
Bool glXQueryVersion(Display *, int *, int *);
Bool glXIsDirect(Display *, GLXContext);
int glXGetConfig(Display *, XVisualInfo *, int, int *);
GLXContext glXGetCurrentContext(void);
GLXDrawable glXGetCurrentDrawable(void);
void glXWaitGL(void);
void glXWaitX(void);
void glXUseXFont(Font, int, int, int);
const char *glXQueryExtensionsString(Display *, int);
const char *glXQueryServerString(Display *, int, int);
const char *glXGetClientString(Display *, int);
Display *glXGetCurrentDisplay(void);
GLXFBConfig *glXChooseFBConfig(Display *, int, const int *, int *);
int glXGetFBConfigAttrib(Display *, GLXFBConfig, int, int *);
GLXFBConfig *glXGetFBConfigs(Display *, int, int *);
XVisualInfo *glXGetVisualFromFBConfig(Display *, GLXFBConfig);
GLXWindow glXCreateWindow(Display *, GLXFBConfig, Window, const int *);
void glXDestroyWindow(Display *, GLXWindow);
GLXPixmap glXCreatePixmap(Display *, GLXFBConfig, Pixmap, const int *);
void glXDestroyPixmap(Display *, GLXPixmap);
GLXPbuffer glXCreatePbuffer(Display *, GLXFBConfig, const int *);
void glXDestroyPbuffer(Display *, GLXPbuffer);
void glXQueryDrawable(Display *, GLXDrawable, int, unsigned int *);
GLXContext glXCreateNewContext(Display *, GLXFBConfig, int, GLXContext, Bool);
Bool glXMakeContextCurrent(Display *, GLXDrawable, GLXDrawable, GLXContext);
GLXDrawable glXGetCurrentReadDrawable(void);
int glXQueryContext(Display *, GLXContext, int, int *);
void glXSelectEvent(Display *, GLXDrawable, unsigned long);
void glXGetSelectedEvent(Display *, GLXDrawable, unsigned long *);
GLXContext glXCreateContextAttribsARB(Display *, GLXFBConfig, GLXContext,
                                     Bool, const int *);
__GLXextFuncPtr glXGetProcAddressARB(const GLubyte *);
__GLXextFuncPtr glXGetProcAddress(const GLubyte *);
int glXSwapIntervalMESA(unsigned int);
int glXGetSwapIntervalMESA(void);

#ifdef __cplusplus
}
#endif

#endif
