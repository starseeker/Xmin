#define GL_GLEXT_PROTOTYPES 1
#include <GL/glx.h>
#include <GL/glext.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int
compile_shader(GLenum type, const char *source, GLuint *result)
{
    GLuint shader = glCreateShader(type);
    GLint compiled = 0;

    if (shader == 0)
        return -1;
    glShaderSource(shader, 1, &source, NULL);
    glCompileShader(shader);
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if (!compiled) {
        char log[1024];
        GLsizei size = 0;

        glGetShaderInfoLog(shader, (GLsizei) sizeof(log), &size, log);
        fprintf(stderr, "shader compilation failed: %.*s\n", (int) size, log);
        glDeleteShader(shader);
        return -1;
    }
    *result = shader;
    return 0;
}

int
main(void)
{
    static const int config_attributes[] = {
        GLX_DRAWABLE_TYPE, GLX_PBUFFER_BIT,
        GLX_RENDER_TYPE, GLX_RGBA_BIT,
        GLX_RED_SIZE, 8,
        GLX_GREEN_SIZE, 8,
        GLX_BLUE_SIZE, 8,
        GLX_ALPHA_SIZE, 8,
        0
    };
    static const int context_attributes[] = {
        GLX_CONTEXT_MAJOR_VERSION_ARB, 2,
        GLX_CONTEXT_MINOR_VERSION_ARB, 0,
        GLX_CONTEXT_PROFILE_MASK_ARB,
            GLX_CONTEXT_COMPATIBILITY_PROFILE_BIT_ARB,
        0
    };
    static const int pbuffer_attributes[] = {
        GLX_PBUFFER_WIDTH, 64,
        GLX_PBUFFER_HEIGHT, 64,
        0
    };
    static const int small_pbuffer_attributes[] = {
        GLX_PBUFFER_WIDTH, 32,
        GLX_PBUFFER_HEIGHT, 48,
        0
    };
    static const char vertex_source[] =
        "#version 110\n"
        "void main() { gl_Position = gl_Vertex; }\n";
    static const char fragment_source[] =
        "#version 110\n"
        "void main() { gl_FragColor = vec4(0.0, 1.0, 0.0, 1.0); }\n";
    GLXFBConfig *configs;
    GLXFBConfig config = NULL;
    GLXContext context = NULL;
    GLXContext shared_context = NULL;
    GLXPbuffer pbuffer = 0;
    GLXPbuffer shared_pbuffer = 0;
    GLuint vertex = 0;
    GLuint fragment = 0;
    GLuint program = 0;
    GLuint texture = 0;
    GLint linked = 0;
    unsigned char pixel[4] = { 0 };
    unsigned char draw_pixel[4] = { 0 };
    unsigned char texture_pixel[4] = { 0 };
    GLfloat copied_clear[4] = { 0.0F, 0.0F, 0.0F, 0.0F };
    unsigned int width = 0;
    unsigned int height = 0;
    const char *version;
    int major = 0;
    int minor = 0;
    int count = 0;
    int result = 1;

    if (!glXQueryVersion(NULL, &major, &minor) || major != 1 || minor != 4 ||
        glXGetProcAddress((const GLubyte *) "glCreateShader") == NULL ||
        glXGetProcAddress((const GLubyte *) "glXCreateContextAttribsARB") == NULL)
        goto cleanup;
    configs = glXChooseFBConfig(NULL, 0, config_attributes, &count);
    if (configs == NULL || count < 1)
        goto cleanup;
    config = configs[0];
    context = glXCreateContextAttribsARB(NULL, config, NULL, True,
                                         context_attributes);
    shared_context = glXCreateContextAttribsARB(NULL, config, context, True,
                                                context_attributes);
    pbuffer = glXCreatePbuffer(NULL, config, pbuffer_attributes);
    shared_pbuffer = glXCreatePbuffer(NULL, config,
                                      small_pbuffer_attributes);
    free(configs);
    if (context == NULL || shared_context == NULL || pbuffer == 0 ||
        shared_pbuffer == 0 ||
        !glXMakeContextCurrent(NULL, pbuffer, pbuffer, context) ||
        !glXIsDirect(NULL, context))
        goto cleanup;
    glXQueryDrawable(NULL, pbuffer, GLX_WIDTH, &width);
    glXQueryDrawable(NULL, pbuffer, GLX_HEIGHT, &height);
    version = (const char *) glGetString(GL_VERSION);
    if (width != 64 || height != 64 || version == NULL ||
        strncmp(version, "2.0", 3) != 0)
        goto cleanup;

    glClearColor(0.25F, 0.5F, 0.75F, 1.0F);
    glXCopyContext(NULL, context, shared_context, GL_COLOR_BUFFER_BIT);

    if (compile_shader(GL_VERTEX_SHADER, vertex_source, &vertex) != 0 ||
        compile_shader(GL_FRAGMENT_SHADER, fragment_source, &fragment) != 0)
        goto cleanup;
    program = glCreateProgram();
    glAttachShader(program, vertex);
    glAttachShader(program, fragment);
    glLinkProgram(program);
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if (!linked) {
        char log[1024];
        GLsizei size = 0;

        glGetProgramInfoLog(program, (GLsizei) sizeof(log), &size, log);
        fprintf(stderr, "program link failed: %.*s\n", (int) size, log);
        goto cleanup;
    }

    glViewport(0, 0, 64, 64);
    glClearColor(1.0F, 0.0F, 0.0F, 1.0F);
    glClear(GL_COLOR_BUFFER_BIT);
    glUseProgram(program);
    glBegin(GL_TRIANGLES);
    glVertex2f(-1.0F, -1.0F);
    glVertex2f(1.0F, -1.0F);
    glVertex2f(0.0F, 1.0F);
    glEnd();
    glFinish();
    glReadPixels(32, 32, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
    if (pixel[1] < 200 || pixel[0] > 40 || pixel[2] > 40)
        goto cleanup;

    if (!glXMakeContextCurrent(NULL, shared_pbuffer, pbuffer, context) ||
        glXGetCurrentDrawable() != shared_pbuffer ||
        glXGetCurrentReadDrawable() != pbuffer)
        goto cleanup;
    glUseProgram(0);
    glClearColor(1.0F, 0.0F, 0.0F, 1.0F);
    glClear(GL_COLOR_BUFFER_BIT);
    glFinish();
    memset(pixel, 0, sizeof(pixel));
    glReadPixels(32, 32, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
    if (pixel[1] < 200 || pixel[0] > 40 || pixel[2] > 40)
        goto cleanup;
    if (!glXMakeContextCurrent(NULL, shared_pbuffer, shared_pbuffer,
                               context))
        goto cleanup;
    glReadPixels(16, 24, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, draw_pixel);
    if (draw_pixel[0] < 200 || draw_pixel[1] > 40 || draw_pixel[2] > 40)
        goto cleanup;

    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, pixel);
    if (texture == 0 ||
        !glXMakeContextCurrent(NULL, shared_pbuffer, shared_pbuffer,
                               shared_context) ||
        !glIsTexture(texture))
        goto cleanup;
    glGetFloatv(GL_COLOR_CLEAR_VALUE, copied_clear);
    if (copied_clear[0] != 0.25F || copied_clear[1] != 0.5F ||
        copied_clear[2] != 0.75F || copied_clear[3] != 1.0F)
        goto cleanup;
    memset(draw_pixel, 0, sizeof(draw_pixel));
    glReadPixels(16, 24, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, draw_pixel);
    if (draw_pixel[0] < 200 || draw_pixel[1] > 40 || draw_pixel[2] > 40)
        goto cleanup;
    glBindTexture(GL_TEXTURE_2D, texture);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, texture_pixel);
    if (texture_pixel[1] < 200 || texture_pixel[0] > 40 ||
        texture_pixel[2] > 40)
        goto cleanup;
    result = 0;

cleanup:
    if (shared_context != NULL && shared_pbuffer != 0)
        glXMakeContextCurrent(NULL, shared_pbuffer, shared_pbuffer,
                              shared_context);
    if (texture != 0)
        glDeleteTextures(1, &texture);
    if (program != 0)
        glDeleteProgram(program);
    if (vertex != 0)
        glDeleteShader(vertex);
    if (fragment != 0)
        glDeleteShader(fragment);
    if (context != NULL || shared_context != NULL)
        glXMakeContextCurrent(NULL, 0, 0, NULL);
    if (shared_pbuffer != 0)
        glXDestroyPbuffer(NULL, shared_pbuffer);
    if (pbuffer != 0)
        glXDestroyPbuffer(NULL, pbuffer);
    if (shared_context != NULL)
        glXDestroyContext(NULL, shared_context);
    if (context != NULL)
        glXDestroyContext(NULL, context);
    return result;
}
