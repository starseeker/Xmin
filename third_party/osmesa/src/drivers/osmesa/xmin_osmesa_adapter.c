/*
 * Narrow integration surface between Xmin's public GLX implementation and
 * the pinned OSMesa renderer. Private Mesa types and dispatch tables must not
 * escape this translation unit.
 */
#include "xmin_osmesa_adapter.h"

#include "main/buffers.h"
#include "main/context.h"
#include "glapi/dispatch.h"

static void GLAPIENTRY
xmin_osmesa_draw_buffer(GLenum buffer)
{
    GLcontext *mesa = _mesa_get_current_context();

    if (mesa != NULL && mesa->DrawBuffer->Name == 0 &&
        (buffer == GL_BACK || buffer == GL_BACK_LEFT))
        buffer = GL_FRONT_LEFT;
    _mesa_DrawBuffer(buffer);
}

static void GLAPIENTRY
xmin_osmesa_draw_buffers(GLsizei count, const GLenum *buffers)
{
    GLcontext *mesa = _mesa_get_current_context();

    if (mesa != NULL && mesa->DrawBuffer->Name == 0 && count == 1 &&
        buffers != NULL && buffers[0] == GL_BACK_LEFT) {
        const GLenum front = GL_FRONT_LEFT;

        _mesa_DrawBuffersARB(1, &front);
        return;
    }
    _mesa_DrawBuffersARB(count, buffers);
}

static void GLAPIENTRY
xmin_osmesa_read_buffer(GLenum buffer)
{
    GLcontext *mesa = _mesa_get_current_context();

    if (mesa != NULL && mesa->ReadBuffer->Name == 0 &&
        (buffer == GL_BACK || buffer == GL_BACK_LEFT))
        buffer = GL_FRONT_LEFT;
    _mesa_ReadBuffer(buffer);
}

void
XminOSMesaInstallDispatchHooks(XminOSMesaFlushHook flush_hook)
{
    GLcontext *mesa = _mesa_get_current_context();

    if (mesa != NULL && mesa->Exec != NULL) {
        SET_Flush(mesa->Exec, flush_hook);
        SET_DrawBuffer(mesa->Exec, xmin_osmesa_draw_buffer);
        SET_DrawBuffersARB(mesa->Exec, xmin_osmesa_draw_buffers);
        SET_ReadBuffer(mesa->Exec, xmin_osmesa_read_buffer);
    }
}

void
XminOSMesaFlush(void)
{
    _mesa_Flush();
}

void
XminOSMesaCopyContext(OSMesaContext source, OSMesaContext destination,
                      unsigned long mask)
{
    /* OSMesa derives its private context from GLcontext. Keep that layout
     * fact here, beside the dependency implementation that defines it. */
    _mesa_copy_context((const GLcontext *) source, (GLcontext *) destination,
                       (GLuint) mask);
}
