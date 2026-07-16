#ifndef XMIN_GL_COMPAT_GL_H
#define XMIN_GL_COMPAT_GL_H

/*
 * Xorg's indirect dispatcher includes the conventional GL header names.
 * Route those includes to the embedded, namespaced OSMesa API so no host GL
 * development package or runtime library enters the build graph.
 */
#include <OSMesa/gl.h>

#endif
