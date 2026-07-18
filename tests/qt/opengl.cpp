#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QGuiApplication>
#include <QImage>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QPixmap>
#include <QScreen>
#include <QSurfaceFormat>
#include <QThread>
#include <QWindow>

#include <cstdio>

static bool
waitForExposure(QWindow &window)
{
    QElapsedTimer timer;

    timer.start();
    while (timer.elapsed() < 5000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        if (window.isExposed())
            return true;
        QThread::msleep(1);
    }
    return false;
}

static bool
compileShader(QOpenGLFunctions *gl, GLuint shader, const char *source)
{
    GLint compiled = GL_FALSE;

    gl->glShaderSource(shader, 1, &source, nullptr);
    gl->glCompileShader(shader);
    gl->glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if (compiled == GL_TRUE)
        return true;

    char log[1024] = { 0 };
    GLsizei size = 0;
    gl->glGetShaderInfoLog(shader, sizeof(log), &size, log);
    std::fprintf(stderr, "Qt %d shader compile failed: %.*s\n",
                 XMIN_QT_MAJOR, static_cast<int>(size), log);
    return false;
}

int
main(int argc, char **argv)
{
    static const char vertexSource[] =
        "#version 110\n"
        "attribute vec2 position;\n"
        "void main() { gl_Position = vec4(position, 0.0, 1.0); }\n";
    static const char fragmentSource[] =
        "#version 110\n"
        "void main() { gl_FragColor = vec4(0.0, 1.0, 0.0, 1.0); }\n";
    static const GLfloat vertices[] = {
        -1.0F, -1.0F,
         1.0F, -1.0F,
         0.0F,  1.0F
    };
    QGuiApplication application(argc, argv);
    QSurfaceFormat format;
    QWindow window;
    QOpenGLContext context;
    GLuint vertex = 0;
    GLuint fragment = 0;
    GLuint program = 0;
    unsigned char pixel[4] = { 0, 0, 0, 0 };
    const char *stage = "checking the OpenGL version";
    int result = 1;

    if (QGuiApplication::platformName() != QStringLiteral("xcb")) {
        std::fprintf(stderr, "Qt %d did not select its xcb platform plugin\n",
                     XMIN_QT_MAJOR);
        return 2;
    }

    format.setRenderableType(QSurfaceFormat::OpenGL);
    format.setVersion(2, 0);
    format.setProfile(QSurfaceFormat::NoProfile);
    format.setSwapBehavior(QSurfaceFormat::DoubleBuffer);
    format.setRedBufferSize(8);
    format.setGreenBufferSize(8);
    format.setBlueBufferSize(8);
    format.setAlphaBufferSize(8);

    window.setSurfaceType(QWindow::OpenGLSurface);
    window.setFormat(format);
    window.resize(64, 64);
    window.create();
    window.show();
    if (!waitForExposure(window))
        return 3;

    context.setFormat(format);
    if (!context.create() || !context.isValid() || context.isOpenGLES() ||
        !context.makeCurrent(&window)) {
        std::fprintf(stderr, "Qt %d could not create the Xmin OpenGL context\n",
                     XMIN_QT_MAJOR);
        return 4;
    }

    QOpenGLFunctions *gl = context.functions();
    gl->initializeOpenGLFunctions();
    const char *version = reinterpret_cast<const char *>(
        gl->glGetString(GL_VERSION));
    int majorVersion = 0;
    int minorVersion = 0;
    if (version == nullptr ||
        std::sscanf(version, "%d.%d", &majorVersion, &minorVersion) != 2 ||
        majorVersion < 2)
        goto cleanup;

    stage = "compiling shaders";
    vertex = gl->glCreateShader(GL_VERTEX_SHADER);
    fragment = gl->glCreateShader(GL_FRAGMENT_SHADER);
    if (vertex == 0 || fragment == 0 ||
        !compileShader(gl, vertex, vertexSource) ||
        !compileShader(gl, fragment, fragmentSource))
        goto cleanup;

    stage = "linking the shader program";
    program = gl->glCreateProgram();
    gl->glAttachShader(program, vertex);
    gl->glAttachShader(program, fragment);
    gl->glBindAttribLocation(program, 0, "position");
    gl->glLinkProgram(program);
    {
        GLint linked = GL_FALSE;
        gl->glGetProgramiv(program, GL_LINK_STATUS, &linked);
        if (linked != GL_TRUE)
            goto cleanup;
    }

    stage = "drawing and reading the OpenGL surface";
    gl->glViewport(0, 0, 64, 64);
    gl->glClearColor(1.0F, 0.0F, 0.0F, 1.0F);
    gl->glClear(GL_COLOR_BUFFER_BIT);
    gl->glUseProgram(program);
    gl->glEnableVertexAttribArray(0);
    gl->glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, vertices);
    gl->glDrawArrays(GL_TRIANGLES, 0, 3);
    gl->glFinish();
    gl->glReadPixels(32, 32, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
    context.swapBuffers(&window);
    if (pixel[1] < 200 || pixel[0] > 40 || pixel[2] > 40)
        goto cleanup;
    stage = "capturing the presented window";
    QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    {
        QScreen *screen = window.screen();
        if (screen == nullptr)
            goto cleanup;
        const QImage capture =
            screen->grabWindow(window.winId(), 32, 32, 1, 1)
                .toImage()
                .convertToFormat(QImage::Format_RGB32);
        if (capture.isNull())
            goto cleanup;
        const QRgb presented = capture.pixel(0, 0);
        if (qGreen(presented) < 200 || qRed(presented) > 40 ||
            qBlue(presented) > 40)
            goto cleanup;
    }
    result = 0;

cleanup:
    if (result != 0) {
        std::fprintf(
            stderr, "Qt %d OpenGL acceptance failed while %s "
                    "(pixel %u,%u,%u,%u)\n",
            XMIN_QT_MAJOR, stage, pixel[0], pixel[1], pixel[2], pixel[3]);
    }
    if (program != 0)
        gl->glDeleteProgram(program);
    if (vertex != 0)
        gl->glDeleteShader(vertex);
    if (fragment != 0)
        gl->glDeleteShader(fragment);
    context.doneCurrent();
    return result;
}
