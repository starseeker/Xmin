#include <X11/Xmd.h>
#include <X11/Xproto.h>
#include <GL/glxproto.h>
#include <X11/extensions/XI2proto.h>
#include <X11/extensions/XKBproto.h>
#include <X11/extensions/bigreqsproto.h>
#include <X11/extensions/compositeproto.h>
#include <X11/extensions/damageproto.h>
#include <X11/extensions/dbeproto.h>
#include <X11/extensions/geproto.h>
#include <X11/extensions/panoramiXproto.h>
#include <X11/extensions/presentproto.h>
#include <X11/extensions/randrproto.h>
#include <X11/extensions/renderproto.h>
#include <X11/extensions/saverproto.h>
#include <X11/extensions/shapeproto.h>
#include <X11/extensions/shmproto.h>
#include <X11/extensions/syncproto.h>
#include <X11/extensions/xcmiscproto.h>
#include <X11/extensions/xfixesproto.h>
#include <X11/extensions/xtestproto.h>

_Static_assert(sizeof(xReq) == 4, "core request header must match the X11 wire ABI");
_Static_assert(sizeof(xEvent) == 32, "core event must match the X11 wire ABI");

int
main(void)
{
    return sz_xReq == 4 ? 0 : 1;
}
