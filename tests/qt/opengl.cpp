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

#include <GL/gl.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

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

// Adapted from the classic Mesa osdemo/glxgears scene under the permissive
// terms recorded in LICENSES/osmesa/LICENSE.  It deliberately uses the
// OpenGL 1.x control plane as well as the shader path checked below, giving
// the README image a compact tour of the OSMesa backend.
static void
drawGear(GLfloat innerRadius, GLfloat outerRadius, GLfloat width,
         GLint teeth, GLfloat toothDepth)
{
    constexpr GLfloat pi = 3.14159265358979323846F;
    const GLfloat r0 = innerRadius;
    const GLfloat r1 = outerRadius - toothDepth / 2.0F;
    const GLfloat r2 = outerRadius + toothDepth / 2.0F;
    const GLfloat halfWidth = width / 2.0F;
    const GLfloat step = 2.0F * pi / teeth;
    const GLfloat quarterStep = step / 4.0F;

    glShadeModel(GL_FLAT);
    glNormal3f(0.0F, 0.0F, 1.0F);
    glBegin(GL_QUAD_STRIP);
    for (GLint index = 0; index <= teeth; ++index) {
        const GLfloat angle = index * step;
        glVertex3f(r0 * std::cos(angle), r0 * std::sin(angle), halfWidth);
        glVertex3f(r1 * std::cos(angle), r1 * std::sin(angle), halfWidth);
        if (index < teeth) {
            glVertex3f(r0 * std::cos(angle), r0 * std::sin(angle), halfWidth);
            glVertex3f(r1 * std::cos(angle + 3.0F * quarterStep),
                       r1 * std::sin(angle + 3.0F * quarterStep), halfWidth);
        }
    }
    glEnd();

    glBegin(GL_QUADS);
    for (GLint index = 0; index < teeth; ++index) {
        const GLfloat angle = index * step;
        glVertex3f(r1 * std::cos(angle), r1 * std::sin(angle), halfWidth);
        glVertex3f(r2 * std::cos(angle + quarterStep),
                   r2 * std::sin(angle + quarterStep), halfWidth);
        glVertex3f(r2 * std::cos(angle + 2.0F * quarterStep),
                   r2 * std::sin(angle + 2.0F * quarterStep), halfWidth);
        glVertex3f(r1 * std::cos(angle + 3.0F * quarterStep),
                   r1 * std::sin(angle + 3.0F * quarterStep), halfWidth);
    }
    glEnd();

    glNormal3f(0.0F, 0.0F, -1.0F);
    glBegin(GL_QUAD_STRIP);
    for (GLint index = 0; index <= teeth; ++index) {
        const GLfloat angle = index * step;
        glVertex3f(r1 * std::cos(angle), r1 * std::sin(angle), -halfWidth);
        glVertex3f(r0 * std::cos(angle), r0 * std::sin(angle), -halfWidth);
        if (index < teeth) {
            glVertex3f(r1 * std::cos(angle + 3.0F * quarterStep),
                       r1 * std::sin(angle + 3.0F * quarterStep), -halfWidth);
            glVertex3f(r0 * std::cos(angle), r0 * std::sin(angle), -halfWidth);
        }
    }
    glEnd();

    glBegin(GL_QUADS);
    for (GLint index = 0; index < teeth; ++index) {
        const GLfloat angle = index * step;
        glVertex3f(r1 * std::cos(angle + 3.0F * quarterStep),
                   r1 * std::sin(angle + 3.0F * quarterStep), -halfWidth);
        glVertex3f(r2 * std::cos(angle + 2.0F * quarterStep),
                   r2 * std::sin(angle + 2.0F * quarterStep), -halfWidth);
        glVertex3f(r2 * std::cos(angle + quarterStep),
                   r2 * std::sin(angle + quarterStep), -halfWidth);
        glVertex3f(r1 * std::cos(angle), r1 * std::sin(angle), -halfWidth);
    }
    glEnd();

    glBegin(GL_QUAD_STRIP);
    for (GLint index = 0; index < teeth; ++index) {
        const GLfloat angle = index * step;
        GLfloat u = r2 * std::cos(angle + quarterStep) -
            r1 * std::cos(angle);
        GLfloat v = r2 * std::sin(angle + quarterStep) -
            r1 * std::sin(angle);
        GLfloat length = std::sqrt(u * u + v * v);
        u /= length;
        v /= length;
        glNormal3f(v, -u, 0.0F);
        glVertex3f(r1 * std::cos(angle), r1 * std::sin(angle), halfWidth);
        glVertex3f(r1 * std::cos(angle), r1 * std::sin(angle), -halfWidth);
        glVertex3f(r2 * std::cos(angle + quarterStep),
                   r2 * std::sin(angle + quarterStep), halfWidth);
        glVertex3f(r2 * std::cos(angle + quarterStep),
                   r2 * std::sin(angle + quarterStep), -halfWidth);

        glNormal3f(std::cos(angle), std::sin(angle), 0.0F);
        glVertex3f(r2 * std::cos(angle + 2.0F * quarterStep),
                   r2 * std::sin(angle + 2.0F * quarterStep), halfWidth);
        glVertex3f(r2 * std::cos(angle + 2.0F * quarterStep),
                   r2 * std::sin(angle + 2.0F * quarterStep), -halfWidth);

        u = r1 * std::cos(angle + 3.0F * quarterStep) -
            r2 * std::cos(angle + 2.0F * quarterStep);
        v = r1 * std::sin(angle + 3.0F * quarterStep) -
            r2 * std::sin(angle + 2.0F * quarterStep);
        glNormal3f(v, -u, 0.0F);
        glVertex3f(r1 * std::cos(angle + 3.0F * quarterStep),
                   r1 * std::sin(angle + 3.0F * quarterStep), halfWidth);
        glVertex3f(r1 * std::cos(angle + 3.0F * quarterStep),
                   r1 * std::sin(angle + 3.0F * quarterStep), -halfWidth);
    }
    glEnd();

    glShadeModel(GL_SMOOTH);
    glBegin(GL_QUAD_STRIP);
    for (GLint index = 0; index <= teeth; ++index) {
        const GLfloat angle = index * step;
        glNormal3f(-std::cos(angle), -std::sin(angle), 0.0F);
        glVertex3f(r0 * std::cos(angle), r0 * std::sin(angle), -halfWidth);
        glVertex3f(r0 * std::cos(angle), r0 * std::sin(angle), halfWidth);
    }
    glEnd();
}

static void
drawDemo(QOpenGLFunctions *functions, QWindow &window)
{
    const GLfloat lightPosition[] = { 4.0F, 7.0F, 12.0F, 0.0F };
    const GLfloat orange[] = { 0.96F, 0.42F, 0.12F, 1.0F };
    const GLfloat cyan[] = { 0.10F, 0.72F, 0.78F, 1.0F };
    const GLfloat violet[] = { 0.50F, 0.32F, 0.92F, 1.0F };
    const GLdouble aspect = static_cast<GLdouble>(window.width()) /
        std::max(window.height(), 1);

    functions->glUseProgram(0);
    glViewport(0, 0, window.width(), window.height());
    glClearColor(0.025F, 0.045F, 0.07F, 1.0F);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glEnable(GL_CULL_FACE);
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_LIGHTING);
    glEnable(GL_LIGHT0);
    glEnable(GL_NORMALIZE);

    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glFrustum(-aspect, aspect, -1.0, 1.0, 5.0, 80.0);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glLightfv(GL_LIGHT0, GL_POSITION, lightPosition);
    glTranslatef(0.0F, 0.0F, -32.0F);
    glRotatef(22.0F, 1.0F, 0.0F, 0.0F);
    glRotatef(-24.0F, 0.0F, 1.0F, 0.0F);

    glPushMatrix();
    glTranslatef(-3.1F, -2.0F, 0.0F);
    glRotatef(14.0F, 0.0F, 0.0F, 1.0F);
    glMaterialfv(GL_FRONT, GL_AMBIENT_AND_DIFFUSE, orange);
    drawGear(1.0F, 4.0F, 1.2F, 20, 0.8F);
    glPopMatrix();

    glPushMatrix();
    glTranslatef(3.4F, -2.0F, 0.0F);
    glRotatef(-24.0F, 0.0F, 0.0F, 1.0F);
    glMaterialfv(GL_FRONT, GL_AMBIENT_AND_DIFFUSE, cyan);
    drawGear(0.6F, 2.2F, 1.3F, 12, 0.7F);
    glPopMatrix();

    glPushMatrix();
    glTranslatef(-3.1F, 4.3F, 0.0F);
    glRotatef(-31.0F, 0.0F, 0.0F, 1.0F);
    glMaterialfv(GL_FRONT, GL_AMBIENT_AND_DIFFUSE, violet);
    drawGear(1.2F, 2.1F, 1.3F, 12, 0.7F);
    glPopMatrix();

    glDisable(GL_LIGHTING);
    glFinish();
    functions->glFlush();
}

int
main(int argc, char **argv)
{
    static const char vertexSource[] =
        "#version 110\n"
        "attribute vec2 position;\n"
        "void main() { gl_Position = "
        "vec4(position.x, position.y, 0.0, 1.0); }\n";
    static const char fragmentSource[] =
        "#version 110\n"
        "void main() { gl_FragColor = vec4(0.0, 1.0, 0.0, 1.0); }\n";
    static const GLfloat vertices[] = {
        -1.0F, -1.0F,
         1.0F, -1.0F,
         0.0F,  1.0F
    };
    const bool demo = argc == 2 && std::strcmp(argv[1], "--demo") == 0;
    if (demo)
        argc = 1;
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
    format.setDepthBufferSize(24);

    window.setTitle(QStringLiteral("Qt %1 OpenGL on Xmin / OSMesa")
                        .arg(XMIN_QT_MAJOR));
    window.setSurfaceType(QWindow::OpenGLSurface);
    window.setFormat(format);
    window.resize(demo ? QSize(480, 360) : QSize(64, 64));
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
    gl->glViewport(0, 0, window.width(), window.height());
    gl->glClearColor(0.06F, 0.09F, 0.13F, 1.0F);
    gl->glClear(GL_COLOR_BUFFER_BIT);
    gl->glUseProgram(program);
    gl->glEnableVertexAttribArray(0);
    gl->glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, vertices);
    gl->glDrawArrays(GL_TRIANGLES, 0, 3);
    gl->glFinish();
    gl->glReadPixels(window.width() / 2, window.height() / 2,
                     1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
    context.swapBuffers(&window);
    if (pixel[1] < 200 || pixel[0] > 40 || pixel[2] > 40)
        goto cleanup;
    stage = "capturing the presented window";
    QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    if (!demo) {
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
    if (demo) {
        const char *renderer = reinterpret_cast<const char *>(
            gl->glGetString(GL_RENDERER));
        std::printf("Qt %d qxcb + Xmin direct GLX\n"
                    "OSMesa renderer: %s\nOpenGL version: %s\n",
                    XMIN_QT_MAJOR,
                    renderer != nullptr ? renderer : "unknown", version);
        std::fflush(stdout);
        drawDemo(gl, window);
        context.swapBuffers(&window);
    }
    while (demo && window.isVisible()) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 16);
        QThread::msleep(16);
    }

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
