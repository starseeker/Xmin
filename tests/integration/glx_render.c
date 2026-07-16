#include <X11/X.h>
#include <X11/Xproto.h>
#include <GL/glxproto.h>
#include <GL/glxtokens.h>

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

enum {
    X_PROTOCOL_MAJOR = 11,
    X_QUERY_EXTENSION = 98,
    X_REPLY = 1,
    SETUP_FIXED_SIZE = 32,
    REPLY_SIZE = 32,
    GLX_VISUAL_PROPERTIES = 40,
    TEST_WIDTH = 16,
    TEST_HEIGHT = 16,
    SECOND_WIDTH = 12,
    SECOND_HEIGHT = 10,
    PBUFFER_WIDTH = 9,
    PBUFFER_HEIGHT = 7,
    PIXMAP_WIDTH = 11,
    PIXMAP_HEIGHT = 8,
    GL_COLOR_BUFFER_BIT_VALUE = 0x00004000,
    GL_TRIANGLES_VALUE = 0x0004,
    GL_RGBA_VALUE = 0x1908,
    GL_UNSIGNED_BYTE_VALUE = 0x1401
};

static int littleEndian;

static void
put16(unsigned char *out, uint16_t value)
{
    if (littleEndian) {
        out[0] = (unsigned char) value;
        out[1] = (unsigned char) (value >> 8);
    }
    else {
        out[0] = (unsigned char) (value >> 8);
        out[1] = (unsigned char) value;
    }
}

static uint16_t
get16(const unsigned char *in)
{
    if (littleEndian)
        return (uint16_t) (in[0] | ((uint16_t) in[1] << 8));
    return (uint16_t) (((uint16_t) in[0] << 8) | in[1]);
}

static uint32_t
get32(const unsigned char *in)
{
    if (littleEndian) {
        return (uint32_t) in[0] | ((uint32_t) in[1] << 8) |
            ((uint32_t) in[2] << 16) | ((uint32_t) in[3] << 24);
    }
    return ((uint32_t) in[0] << 24) | ((uint32_t) in[1] << 16) |
        ((uint32_t) in[2] << 8) | (uint32_t) in[3];
}

static void
put32(unsigned char *out, uint32_t value)
{
    if (littleEndian) {
        out[0] = (unsigned char) value;
        out[1] = (unsigned char) (value >> 8);
        out[2] = (unsigned char) (value >> 16);
        out[3] = (unsigned char) (value >> 24);
    }
    else {
        out[0] = (unsigned char) (value >> 24);
        out[1] = (unsigned char) (value >> 16);
        out[2] = (unsigned char) (value >> 8);
        out[3] = (unsigned char) value;
    }
}

static uint32_t
findSingleWindowConfig(const unsigned char *payload, uint32_t configCount,
                       uint32_t attributeCount)
{
    size_t recordSize = (size_t) attributeCount * 8;
    uint32_t config;

    for (config = 0; config < configCount; ++config) {
        const unsigned char *record = payload + (size_t) config * recordSize;
        uint32_t fbconfig = 0;
        uint32_t drawableType = 0;
        uint32_t renderType = 0;
        uint32_t doubleBuffered = 1;
        uint32_t attribute;

        for (attribute = 0; attribute < attributeCount; ++attribute) {
            uint32_t name = get32(record + (size_t) attribute * 8);
            uint32_t value = get32(record + (size_t) attribute * 8 + 4);

            if (name == GLX_FBCONFIG_ID)
                fbconfig = value;
            else if (name == GLX_DRAWABLE_TYPE)
                drawableType = value;
            else if (name == GLX_RENDER_TYPE)
                renderType = value;
            else if (name == GLX_DOUBLEBUFFER)
                doubleBuffered = value;
        }
        if (fbconfig != 0 && !doubleBuffered &&
            (drawableType & GLX_WINDOW_BIT) != 0 &&
            (renderType & GLX_RGBA_BIT) != 0)
            return fbconfig;
    }
    return 0;
}

static int
writeAll(int fd, const void *data, size_t size)
{
    const unsigned char *cursor = data;

    while (size != 0) {
        ssize_t count = write(fd, cursor, size);
        if (count < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (count == 0)
            return -1;
        cursor += (size_t) count;
        size -= (size_t) count;
    }
    return 0;
}

static int
readAll(int fd, void *data, size_t size)
{
    unsigned char *cursor = data;

    while (size != 0) {
        ssize_t count = read(fd, cursor, size);
        if (count == 0)
            return -1;
        if (count < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        cursor += (size_t) count;
        size -= (size_t) count;
    }
    return 0;
}

static int
readDisplayNumber(int fd, char *display, size_t capacity)
{
    struct pollfd ready = { .fd = fd, .events = POLLIN | POLLHUP };
    size_t used = 0;

    if (poll(&ready, 1, 15000) <= 0)
        return -1;
    while (used + 1 < capacity) {
        char byte;
        ssize_t count = read(fd, &byte, 1);
        if (count < 0 && errno == EINTR)
            continue;
        if (count != 1)
            return -1;
        if (byte == '\n') {
            display[used] = '\0';
            return used == 0 ? -1 : 0;
        }
        if (byte < '0' || byte > '9')
            return -1;
        display[used++] = byte;
    }
    return -1;
}

static int
stopServer(pid_t child)
{
    const struct timespec delay = { .tv_sec = 0, .tv_nsec = 10000000 };
    int status;
    int i;

    if (kill(child, SIGTERM) != 0 && errno != ESRCH)
        return -1;
    for (i = 0; i < 500; ++i) {
        pid_t result = waitpid(child, &status, WNOHANG);
        if (result == child)
            return WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : -1;
        if (result < 0 && errno != EINTR)
            return errno == ECHILD ? 0 : -1;
        nanosleep(&delay, NULL);
    }
    kill(child, SIGKILL);
    waitpid(child, &status, 0);
    return -1;
}

static int
queryGlxOpcode(int fd, uint8_t *opcode)
{
    static const char name[] = "GLX";
    unsigned char request[12] = { 0 };
    unsigned char reply[REPLY_SIZE];

    request[0] = X_QUERY_EXTENSION;
    put16(request + 2, 3);
    put16(request + 4, 3);
    memcpy(request + 8, name, 3);
    if (writeAll(fd, request, sizeof(request)) != 0 ||
        readAll(fd, reply, sizeof(reply)) != 0 || reply[0] != X_REPLY ||
        get16(reply + 2) != 1 || reply[8] == 0) {
        return -1;
    }
    *opcode = reply[9];
    return 0;
}

static int
readReply(int fd, uint16_t sequence, unsigned char reply[REPLY_SIZE])
{
    if (readAll(fd, reply, REPLY_SIZE) != 0)
        return -1;
    if (reply[0] != X_REPLY || get16(reply + 2) != sequence) {
        fprintf(stderr,
                "expected reply for sequence %u, got type %u sequence %u\n",
                sequence, reply[0], get16(reply + 2));
        return -1;
    }
    return 0;
}

static int
checkOppositeEndianGlx(const char *socketPath)
{
    unsigned char clientPrefix[12] = { 0 };
    unsigned char serverPrefix[8];
    unsigned char queryVersion[12] = { 0 };
    unsigned char getConfigs[8] = { 0 };
    unsigned char badRequest[4] = { 0 };
    unsigned char reply[REPLY_SIZE];
    struct sockaddr_un address;
    unsigned char *setup = NULL;
    unsigned char *payload = NULL;
    uint32_t payloadWords;
    size_t setupSize;
    int previousEndian = littleEndian;
    int fd = -1;
    int result = -1;
    uint8_t opcode;

    littleEndian = !previousEndian;
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    memcpy(address.sun_path, socketPath, strlen(socketPath) + 1);
    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0 || connect(fd, (struct sockaddr *) &address,
                          sizeof(address)) != 0)
        goto cleanup;

    clientPrefix[0] = littleEndian ? 'l' : 'B';
    put16(clientPrefix + 2, X_PROTOCOL_MAJOR);
    if (writeAll(fd, clientPrefix, sizeof(clientPrefix)) != 0 ||
        readAll(fd, serverPrefix, sizeof(serverPrefix)) != 0 ||
        serverPrefix[0] != X_REPLY)
        goto cleanup;
    setupSize = (size_t) get16(serverPrefix + 6) * 4;
    if (setupSize < SETUP_FIXED_SIZE)
        goto cleanup;
    setup = malloc(setupSize);
    if (setup == NULL || readAll(fd, setup, setupSize) != 0 ||
        queryGlxOpcode(fd, &opcode) != 0)
        goto cleanup;

    queryVersion[0] = opcode;
    queryVersion[1] = X_GLXQueryVersion;
    put16(queryVersion + 2, sizeof(queryVersion) / 4);
    put32(queryVersion + 4, 1);
    put32(queryVersion + 8, 4);
    if (writeAll(fd, queryVersion, sizeof(queryVersion)) != 0 ||
        readReply(fd, 2, reply) != 0 || get32(reply + 8) != 1 ||
        get32(reply + 12) < 4)
        goto cleanup;

    getConfigs[0] = opcode;
    getConfigs[1] = X_GLXGetFBConfigs;
    put16(getConfigs + 2, sizeof(getConfigs) / 4);
    put32(getConfigs + 4, 0);
    if (writeAll(fd, getConfigs, sizeof(getConfigs)) != 0 ||
        readReply(fd, 3, reply) != 0)
        goto cleanup;
    payloadWords = get32(reply + 4);
    if (get32(reply + 8) == 0 || get32(reply + 12) == 0 ||
        (uint64_t) get32(reply + 8) * get32(reply + 12) * 2 != payloadWords)
        goto cleanup;
#if SIZE_MAX < UINT32_MAX
    if (payloadWords > SIZE_MAX / 4)
        goto cleanup;
#endif
    payload = malloc((size_t) payloadWords * 4);
    if (payload == NULL ||
        readAll(fd, payload, (size_t) payloadWords * 4) != 0)
        goto cleanup;

    badRequest[0] = opcode;
    badRequest[1] = X_GLXQueryVersion;
    put16(badRequest + 2, 1);
    if (writeAll(fd, badRequest, sizeof(badRequest)) != 0 ||
        readAll(fd, reply, sizeof(reply)) != 0 || reply[0] != 0 ||
        reply[1] != BadLength || get16(reply + 2) != 4)
        goto cleanup;
    result = 0;

cleanup:
    free(payload);
    free(setup);
    if (fd >= 0)
        close(fd);
    littleEndian = previousEndian;
    return result;
}

int
main(int argc, char **argv)
{
    unsigned char clientPrefix[12] = { 0 };
    unsigned char serverPrefix[8];
    unsigned char reply[REPLY_SIZE];
    unsigned char render[100] = { 0 };
    unsigned char clearRender[36] = { 0 };
    unsigned char clearOnlyRender[16] = { 0 };
    unsigned char createPbuffer[36] = { 0 };
    unsigned char readPixels[36] = { 0 };
    unsigned char readPixel[4] = { 0 };
    unsigned char *fbconfigs = NULL;
    unsigned char *setup = NULL;
    uint32_t *visuals = NULL;
    char display[8];
    char displayFd[16];
    char socketPath[sizeof(((struct sockaddr_un *) 0)->sun_path)] = { 0 };
    struct sockaddr_un address;
    xGLXQueryVersionReq queryVersion = { 0 };
    xGLXGetVisualConfigsReq getVisuals = { 0 };
    xGLXGetFBConfigsReq getFBConfigs = { 0 };
    xCreateWindowReq createWindow = { 0 };
    xCreatePixmapReq createPixmap = { 0 };
    xResourceReq mapWindow = { 0 };
    xResourceReq freePixmap = { 0 };
    xGLXCreateContextReq createContext = { 0 };
    xGLXMakeCurrentReq makeCurrent = { 0 };
    xGLXMakeContextCurrentReq makeContextCurrent = { 0 };
    xGLXWaitGLReq waitGL = { 0 };
    xGLXCreateWindowReq createGlxWindow = { 0 };
    xGLXCreatePixmapReq createGlxPixmap = { 0 };
    xGLXDestroyPixmapReq destroyGlxPixmap = { 0 };
    xGLXCreateNewContextReq createNewContext = { 0 };
    xGLXCopyContextReq copyContext = { 0 };
    xGLXDestroyPbufferReq destroyPbuffer = { 0 };
    xGLXSingleReq flush = { 0 };
    xGLXSwapBuffersReq swapBuffers = { 0 };
    xGetImageReq getImage = { 0 };
    uint16_t endianProbe = 1;
    uint8_t glxOpcode;
    uint8_t formatCount;
    uint32_t resourceBase;
    uint32_t resourceMask;
    uint32_t rootWindow;
    uint32_t rootVisual;
    uint32_t windowId;
    uint32_t secondWindowId;
    uint32_t singleWindowId;
    uint32_t singleGlxWindowId;
    uint32_t pbufferId;
    uint32_t pixmapId;
    uint32_t glxPixmapId;
    uint32_t contextId;
    uint32_t singleContextId;
    uint32_t contextTag;
    uint32_t singleFbconfig;
    uint32_t payloadWords;
    uint32_t pixel;
    size_t setupSize;
    size_t rootOffset;
    size_t i;
    int readyPipe[2] = { -1, -1 };
    int client = -1;
    int result = 1;
    int rootHasGlx = 0;
    pid_t child = -1;
    const char *stage = "starting Xmin";

    if (argc != 2) {
        fprintf(stderr, "usage: %s /path/to/Xmin\n", argv[0]);
        return 2;
    }
    littleEndian = *(unsigned char *) &endianProbe == 1;

    if (pipe(readyPipe) != 0)
        return 3;
    child = fork();
    if (child < 0)
        return 4;
    if (child == 0) {
        close(readyPipe[0]);
        snprintf(displayFd, sizeof(displayFd), "%d", readyPipe[1]);
        execl(argv[1], argv[1], "-displayfd", displayFd, "-ac", "-terminate",
              "-screen", "0", "320x240x24", (char *) NULL);
        _exit(127);
    }

    close(readyPipe[1]);
    readyPipe[1] = -1;
    if (readDisplayNumber(readyPipe[0], display, sizeof(display)) != 0)
        goto cleanup;
    close(readyPipe[0]);
    readyPipe[0] = -1;

    stage = "connecting to the X11 socket";
    if (snprintf(socketPath, sizeof(socketPath), "/tmp/.X11-unix/X%s",
                 display) >= (int) sizeof(socketPath))
        goto cleanup;
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    memcpy(address.sun_path, socketPath, strlen(socketPath) + 1);
    client = socket(AF_UNIX, SOCK_STREAM, 0);
    if (client < 0 || connect(client, (struct sockaddr *) &address,
                              sizeof(address)) != 0)
        goto cleanup;

    stage = "performing the X11 handshake";
    clientPrefix[0] = littleEndian ? 'l' : 'B';
    put16(clientPrefix + 2, X_PROTOCOL_MAJOR);
    if (writeAll(client, clientPrefix, sizeof(clientPrefix)) != 0 ||
        readAll(client, serverPrefix, sizeof(serverPrefix)) != 0 ||
        serverPrefix[0] != X_REPLY)
        goto cleanup;
    setupSize = (size_t) get16(serverPrefix + 6) * 4;
    if (setupSize < SETUP_FIXED_SIZE)
        goto cleanup;
    setup = malloc(setupSize);
    if (setup == NULL || readAll(client, setup, setupSize) != 0)
        goto cleanup;

    resourceBase = get32(setup + 4);
    resourceMask = get32(setup + 8);
    formatCount = setup[21];
    rootOffset = SETUP_FIXED_SIZE + ((get16(setup + 16) + 3U) & ~3U) +
        (size_t) formatCount * 8U;
    if (setup[20] < 1 || rootOffset + 40 > setupSize)
        goto cleanup;
    rootWindow = get32(setup + rootOffset);
    rootVisual = get32(setup + rootOffset + 32);
    windowId = resourceBase | (1U & resourceMask);
    contextId = resourceBase | (2U & resourceMask);
    secondWindowId = resourceBase | (3U & resourceMask);
    singleWindowId = resourceBase | (4U & resourceMask);
    singleGlxWindowId = resourceBase | (5U & resourceMask);
    singleContextId = resourceBase | (6U & resourceMask);
    pbufferId = resourceBase | (7U & resourceMask);
    pixmapId = resourceBase | (8U & resourceMask);
    glxPixmapId = resourceBase | (9U & resourceMask);

    stage = "querying the GLX extension";
    if (queryGlxOpcode(client, &glxOpcode) != 0)
        goto cleanup;

    stage = "negotiating GLX 1.4";
    queryVersion.reqType = glxOpcode;
    queryVersion.glxCode = X_GLXQueryVersion;
    queryVersion.length = sz_xGLXQueryVersionReq / 4;
    queryVersion.majorVersion = 1;
    queryVersion.minorVersion = 4;
    if (writeAll(client, &queryVersion, sizeof(queryVersion)) != 0 ||
        readReply(client, 2, reply) != 0 || get32(reply + 8) != 1 ||
        get32(reply + 12) < 4) {
        fprintf(stderr, "Xmin did not negotiate GLX 1.4\n");
        goto cleanup;
    }

    stage = "querying GLX visual configurations";
    getVisuals.reqType = glxOpcode;
    getVisuals.glxCode = X_GLXGetVisualConfigs;
    getVisuals.length = sz_xGLXGetVisualConfigsReq / 4;
    getVisuals.screen = 0;
    if (writeAll(client, &getVisuals, sizeof(getVisuals)) != 0 ||
        readReply(client, 3, reply) != 0)
        goto cleanup;
    payloadWords = get32(reply + 4);
    if (get32(reply + 8) == 0 || get32(reply + 12) != GLX_VISUAL_PROPERTIES ||
        payloadWords != get32(reply + 8) * GLX_VISUAL_PROPERTIES)
        goto cleanup;
    visuals = malloc((size_t) payloadWords * sizeof(*visuals));
    if (visuals == NULL ||
        readAll(client, visuals, (size_t) payloadWords * sizeof(*visuals)) != 0)
        goto cleanup;
    for (i = 0; i < get32(reply + 8); ++i) {
        uint32_t *visual = visuals + i * GLX_VISUAL_PROPERTIES;
        if (visual[0] == rootVisual && visual[2] != 0 && visual[11] != 0) {
            rootHasGlx = 1;
            break;
        }
    }
    if (!rootHasGlx) {
        fprintf(stderr, "root visual has no double-buffered RGBA GLX config\n");
        goto cleanup;
    }

    stage = "creating the test window and context";
    createWindow.reqType = X_CreateWindow;
    createWindow.depth = CopyFromParent;
    createWindow.length = sz_xCreateWindowReq / 4;
    createWindow.wid = windowId;
    createWindow.parent = rootWindow;
    createWindow.width = TEST_WIDTH;
    createWindow.height = TEST_HEIGHT;
    createWindow.class = InputOutput;
    createWindow.visual = rootVisual;
    if (writeAll(client, &createWindow, sizeof(createWindow)) != 0)
        goto cleanup;

    mapWindow.reqType = X_MapWindow;
    mapWindow.length = sizeof(mapWindow) / 4;
    mapWindow.id = windowId;
    if (writeAll(client, &mapWindow, sizeof(mapWindow)) != 0)
        goto cleanup;

    createContext.reqType = glxOpcode;
    createContext.glxCode = X_GLXCreateContext;
    createContext.length = sz_xGLXCreateContextReq / 4;
    createContext.context = contextId;
    createContext.visual = rootVisual;
    createContext.screen = 0;
    createContext.isDirect = 0;
    if (writeAll(client, &createContext, sizeof(createContext)) != 0)
        goto cleanup;

    makeCurrent.reqType = glxOpcode;
    makeCurrent.glxCode = X_GLXMakeCurrent;
    makeCurrent.length = sz_xGLXMakeCurrentReq / 4;
    makeCurrent.drawable = windowId;
    makeCurrent.context = contextId;
    if (writeAll(client, &makeCurrent, sizeof(makeCurrent)) != 0 ||
        readReply(client, 7, reply) != 0)
        goto cleanup;
    contextTag = get32(reply + 8);
    if (contextTag == 0)
        goto cleanup;

    stage = "submitting indirect GL rendering commands";
    render[0] = glxOpcode;
    render[1] = X_GLXRender;
    put16(render + 2, sizeof(render) / 4);
    memcpy(render + 4, &contextTag, sizeof(contextTag));
    put16(render + 8, 20);
    put16(render + 10, X_GLrop_ClearColor);
    {
        const float red[4] = { 1.0F, 0.0F, 0.0F, 1.0F };
        memcpy(render + 12, red, sizeof(red));
    }
    put16(render + 28, 8);
    put16(render + 30, X_GLrop_Clear);
    {
        const uint32_t mask = GL_COLOR_BUFFER_BIT_VALUE;
        memcpy(render + 32, &mask, sizeof(mask));
    }
    put16(render + 36, 8);
    put16(render + 38, X_GLrop_Begin);
    {
        const uint32_t mode = GL_TRIANGLES_VALUE;
        memcpy(render + 40, &mode, sizeof(mode));
    }
    put16(render + 44, 16);
    put16(render + 46, X_GLrop_Color3fv);
    {
        const float green[3] = { 0.0F, 1.0F, 0.0F };
        memcpy(render + 48, green, sizeof(green));
    }
    put16(render + 60, 12);
    put16(render + 62, X_GLrop_Vertex2fv);
    {
        const float vertex[2] = { -0.75F, -0.75F };
        memcpy(render + 64, vertex, sizeof(vertex));
    }
    put16(render + 72, 12);
    put16(render + 74, X_GLrop_Vertex2fv);
    {
        const float vertex[2] = { 0.75F, -0.75F };
        memcpy(render + 76, vertex, sizeof(vertex));
    }
    put16(render + 84, 12);
    put16(render + 86, X_GLrop_Vertex2fv);
    {
        const float vertex[2] = { 0.0F, 0.75F };
        memcpy(render + 88, vertex, sizeof(vertex));
    }
    put16(render + 96, 4);
    put16(render + 98, X_GLrop_End);
    if (writeAll(client, render, sizeof(render)) != 0)
        goto cleanup;

    stage = "swapping the indirect GLX buffer";
    swapBuffers.reqType = glxOpcode;
    swapBuffers.glxCode = X_GLXSwapBuffers;
    swapBuffers.length = sz_xGLXSwapBuffersReq / 4;
    swapBuffers.contextTag = contextTag;
    swapBuffers.drawable = windowId;
    if (writeAll(client, &swapBuffers, sizeof(swapBuffers)) != 0)
        goto cleanup;

    stage = "reading the rendered pixel";
    getImage.reqType = X_GetImage;
    getImage.format = ZPixmap;
    getImage.length = sz_xGetImageReq / 4;
    getImage.drawable = windowId;
    getImage.x = TEST_WIDTH / 2;
    getImage.y = TEST_HEIGHT / 2;
    getImage.width = 1;
    getImage.height = 1;
    getImage.planeMask = UINT32_MAX;
    if (writeAll(client, &getImage, sizeof(getImage)) != 0 ||
        readReply(client, 10, reply) != 0 || get32(reply + 4) < 1 ||
        readAll(client, &pixel, sizeof(pixel)) != 0)
        goto cleanup;
    if ((pixel & 0x00ffffffU) != 0x0000ff00U) {
        fprintf(stderr, "unexpected rendered pixel: 0x%08x\n", pixel);
        goto cleanup;
    }

    stage = "creating a distinct indirect draw window";
    createWindow.wid = secondWindowId;
    createWindow.width = SECOND_WIDTH;
    createWindow.height = SECOND_HEIGHT;
    if (writeAll(client, &createWindow, sizeof(createWindow)) != 0)
        goto cleanup;
    mapWindow.id = secondWindowId;
    if (writeAll(client, &mapWindow, sizeof(mapWindow)) != 0)
        goto cleanup;

    stage = "binding distinct indirect draw and read windows";
    makeContextCurrent.reqType = glxOpcode;
    makeContextCurrent.glxCode = X_GLXMakeContextCurrent;
    makeContextCurrent.length = sz_xGLXMakeContextCurrentReq / 4;
    makeContextCurrent.oldContextTag = contextTag;
    makeContextCurrent.drawable = secondWindowId;
    makeContextCurrent.readdrawable = windowId;
    makeContextCurrent.context = contextId;
    if (writeAll(client, &makeContextCurrent,
                 sizeof(makeContextCurrent)) != 0 ||
        readReply(client, 13, reply) != 0)
        goto cleanup;
    contextTag = get32(reply + 8);
    if (contextTag == 0)
        goto cleanup;

    stage = "reading from the distinct indirect read window";
    readPixels[0] = glxOpcode;
    readPixels[1] = X_GLsop_ReadPixels;
    put16(readPixels + 2, sizeof(readPixels) / 4);
    memcpy(readPixels + 4, &contextTag, sizeof(contextTag));
    {
        const int32_t x = TEST_WIDTH / 2;
        const int32_t y = TEST_HEIGHT / 2;
        const int32_t width = 1;
        const int32_t height = 1;
        const uint32_t format = GL_RGBA_VALUE;
        const uint32_t type = GL_UNSIGNED_BYTE_VALUE;

        memcpy(readPixels + 8, &x, sizeof(x));
        memcpy(readPixels + 12, &y, sizeof(y));
        memcpy(readPixels + 16, &width, sizeof(width));
        memcpy(readPixels + 20, &height, sizeof(height));
        memcpy(readPixels + 24, &format, sizeof(format));
        memcpy(readPixels + 28, &type, sizeof(type));
    }
    if (writeAll(client, readPixels, sizeof(readPixels)) != 0 ||
        readReply(client, 14, reply) != 0 || get32(reply + 4) != 1 ||
        readAll(client, readPixel, sizeof(readPixel)) != 0)
        goto cleanup;
    if (readPixel[0] != 0 || readPixel[1] != 255 ||
        readPixel[2] != 0 || readPixel[3] != 255) {
        fprintf(stderr,
                "unexpected indirect read pixel: %u,%u,%u,%u\n",
                readPixel[0], readPixel[1], readPixel[2], readPixel[3]);
        goto cleanup;
    }

    stage = "rendering to the distinct indirect draw window";
    clearRender[0] = glxOpcode;
    clearRender[1] = X_GLXRender;
    put16(clearRender + 2, sizeof(clearRender) / 4);
    memcpy(clearRender + 4, &contextTag, sizeof(contextTag));
    put16(clearRender + 8, 20);
    put16(clearRender + 10, X_GLrop_ClearColor);
    {
        const float blue[4] = { 0.0F, 0.0F, 1.0F, 1.0F };
        const uint32_t mask = GL_COLOR_BUFFER_BIT_VALUE;

        memcpy(clearRender + 12, blue, sizeof(blue));
        put16(clearRender + 28, 8);
        put16(clearRender + 30, X_GLrop_Clear);
        memcpy(clearRender + 32, &mask, sizeof(mask));
    }
    if (writeAll(client, clearRender, sizeof(clearRender)) != 0)
        goto cleanup;

    swapBuffers.contextTag = contextTag;
    swapBuffers.drawable = secondWindowId;
    if (writeAll(client, &swapBuffers, sizeof(swapBuffers)) != 0)
        goto cleanup;

    stage = "reading the distinct indirect draw window";
    getImage.drawable = secondWindowId;
    getImage.x = SECOND_WIDTH / 2;
    getImage.y = SECOND_HEIGHT / 2;
    if (writeAll(client, &getImage, sizeof(getImage)) != 0 ||
        readReply(client, 17, reply) != 0 || get32(reply + 4) < 1 ||
        readAll(client, &pixel, sizeof(pixel)) != 0)
        goto cleanup;
    if ((pixel & 0x00ffffffU) != 0x000000ffU) {
        fprintf(stderr, "unexpected distinct draw pixel: 0x%08x\n", pixel);
        goto cleanup;
    }

    stage = "querying a single-buffered GLX FBConfig";
    getFBConfigs.reqType = glxOpcode;
    getFBConfigs.glxCode = X_GLXGetFBConfigs;
    getFBConfigs.length = sz_xGLXGetFBConfigsReq / 4;
    getFBConfigs.screen = 0;
    if (writeAll(client, &getFBConfigs, sizeof(getFBConfigs)) != 0 ||
        readReply(client, 18, reply) != 0)
        goto cleanup;
    {
        uint32_t configCount = get32(reply + 8);
        uint32_t attributeCount = get32(reply + 12);
        uint64_t expectedWords =
            (uint64_t) configCount * attributeCount * 2;

        payloadWords = get32(reply + 4);
        if (configCount == 0 || attributeCount == 0 ||
            expectedWords != payloadWords)
            goto cleanup;
#if SIZE_MAX < UINT32_MAX
        if (payloadWords > SIZE_MAX / sizeof(uint32_t))
            goto cleanup;
#endif
        fbconfigs = malloc((size_t) payloadWords * sizeof(uint32_t));
        if (fbconfigs == NULL ||
            readAll(client, fbconfigs,
                    (size_t) payloadWords * sizeof(uint32_t)) != 0)
            goto cleanup;
        singleFbconfig = findSingleWindowConfig(fbconfigs, configCount,
                                                 attributeCount);
    }
    if (singleFbconfig == 0) {
        fprintf(stderr, "no single-buffered window FBConfig found\n");
        goto cleanup;
    }

    stage = "creating a single-buffered indirect GLX window";
    createWindow.wid = singleWindowId;
    createWindow.width = SECOND_WIDTH;
    createWindow.height = SECOND_HEIGHT;
    if (writeAll(client, &createWindow, sizeof(createWindow)) != 0)
        goto cleanup;
    mapWindow.id = singleWindowId;
    if (writeAll(client, &mapWindow, sizeof(mapWindow)) != 0)
        goto cleanup;

    createGlxWindow.reqType = glxOpcode;
    createGlxWindow.glxCode = X_GLXCreateWindow;
    createGlxWindow.length = sz_xGLXCreateWindowReq / 4;
    createGlxWindow.screen = 0;
    createGlxWindow.fbconfig = singleFbconfig;
    createGlxWindow.window = singleWindowId;
    createGlxWindow.glxwindow = singleGlxWindowId;
    if (writeAll(client, &createGlxWindow, sizeof(createGlxWindow)) != 0)
        goto cleanup;

    createNewContext.reqType = glxOpcode;
    createNewContext.glxCode = X_GLXCreateNewContext;
    createNewContext.length = sz_xGLXCreateNewContextReq / 4;
    createNewContext.context = singleContextId;
    createNewContext.fbconfig = singleFbconfig;
    createNewContext.screen = 0;
    createNewContext.renderType = GLX_RGBA_TYPE;
    createNewContext.isDirect = 0;
    if (writeAll(client, &createNewContext, sizeof(createNewContext)) != 0)
        goto cleanup;

    makeContextCurrent.oldContextTag = contextTag;
    makeContextCurrent.drawable = singleGlxWindowId;
    makeContextCurrent.readdrawable = singleGlxWindowId;
    makeContextCurrent.context = singleContextId;
    if (writeAll(client, &makeContextCurrent,
                 sizeof(makeContextCurrent)) != 0 ||
        readReply(client, 23, reply) != 0)
        goto cleanup;
    contextTag = get32(reply + 8);
    if (contextTag == 0)
        goto cleanup;

    stage = "flushing a single-buffered indirect GLX window";
    memcpy(clearRender + 4, &contextTag, sizeof(contextTag));
    {
        const float red[4] = { 1.0F, 0.0F, 0.0F, 1.0F };

        memcpy(clearRender + 12, red, sizeof(red));
    }
    if (writeAll(client, clearRender, sizeof(clearRender)) != 0)
        goto cleanup;
    flush.reqType = glxOpcode;
    flush.glxCode = X_GLsop_Flush;
    flush.length = sz_xGLXSingleReq / 4;
    flush.contextTag = contextTag;
    if (writeAll(client, &flush, sizeof(flush)) != 0)
        goto cleanup;

    getImage.drawable = singleWindowId;
    getImage.x = SECOND_WIDTH / 2;
    getImage.y = SECOND_HEIGHT / 2;
    if (writeAll(client, &getImage, sizeof(getImage)) != 0 ||
        readReply(client, 26, reply) != 0 || get32(reply + 4) < 1 ||
        readAll(client, &pixel, sizeof(pixel)) != 0)
        goto cleanup;
    if ((pixel & 0x00ffffffU) != 0x00ff0000U) {
        fprintf(stderr, "unexpected single-buffer flush pixel: 0x%08x\n",
                pixel);
        goto cleanup;
    }

    stage = "creating an indirect pbuffer";
    createPbuffer[0] = glxOpcode;
    createPbuffer[1] = X_GLXCreatePbuffer;
    put16(createPbuffer + 2, sizeof(createPbuffer) / 4);
    {
        const uint32_t screen = 0;
        const uint32_t attributeCount = 2;
        const uint32_t widthName = GLX_PBUFFER_WIDTH;
        const uint32_t width = PBUFFER_WIDTH;
        const uint32_t heightName = GLX_PBUFFER_HEIGHT;
        const uint32_t height = PBUFFER_HEIGHT;

        memcpy(createPbuffer + 4, &screen, sizeof(screen));
        memcpy(createPbuffer + 8, &singleFbconfig, sizeof(singleFbconfig));
        memcpy(createPbuffer + 12, &pbufferId, sizeof(pbufferId));
        memcpy(createPbuffer + 16, &attributeCount,
               sizeof(attributeCount));
        memcpy(createPbuffer + 20, &widthName, sizeof(widthName));
        memcpy(createPbuffer + 24, &width, sizeof(width));
        memcpy(createPbuffer + 28, &heightName, sizeof(heightName));
        memcpy(createPbuffer + 32, &height, sizeof(height));
    }
    if (writeAll(client, createPbuffer, sizeof(createPbuffer)) != 0)
        goto cleanup;

    makeContextCurrent.oldContextTag = contextTag;
    makeContextCurrent.drawable = pbufferId;
    makeContextCurrent.readdrawable = pbufferId;
    makeContextCurrent.context = singleContextId;
    if (writeAll(client, &makeContextCurrent,
                 sizeof(makeContextCurrent)) != 0 ||
        readReply(client, 28, reply) != 0)
        goto cleanup;
    contextTag = get32(reply + 8);
    if (contextTag == 0)
        goto cleanup;

    stage = "rendering to an indirect pbuffer";
    memcpy(clearRender + 4, &contextTag, sizeof(contextTag));
    {
        const float green[4] = { 0.0F, 1.0F, 0.0F, 1.0F };

        memcpy(clearRender + 12, green, sizeof(green));
    }
    if (writeAll(client, clearRender, sizeof(clearRender)) != 0)
        goto cleanup;

    stage = "reading indirect pbuffer storage";
    memcpy(readPixels + 4, &contextTag, sizeof(contextTag));
    {
        const int32_t x = PBUFFER_WIDTH / 2;
        const int32_t y = PBUFFER_HEIGHT / 2;

        memcpy(readPixels + 8, &x, sizeof(x));
        memcpy(readPixels + 12, &y, sizeof(y));
    }
    memset(readPixel, 0, sizeof(readPixel));
    if (writeAll(client, readPixels, sizeof(readPixels)) != 0 ||
        readReply(client, 30, reply) != 0 || get32(reply + 4) != 1 ||
        readAll(client, readPixel, sizeof(readPixel)) != 0)
        goto cleanup;
    if (readPixel[0] != 0 || readPixel[1] != 255 ||
        readPixel[2] != 0 || readPixel[3] != 255) {
        fprintf(stderr,
                "unexpected current pbuffer pixel: %u,%u,%u,%u\n",
                readPixel[0], readPixel[1], readPixel[2], readPixel[3]);
        goto cleanup;
    }

    stage = "releasing indirect pbuffer storage";
    makeContextCurrent.oldContextTag = contextTag;
    makeContextCurrent.drawable = 0;
    makeContextCurrent.readdrawable = 0;
    makeContextCurrent.context = 0;
    if (writeAll(client, &makeContextCurrent,
                 sizeof(makeContextCurrent)) != 0 ||
        readReply(client, 31, reply) != 0 || get32(reply + 8) != 0)
        goto cleanup;

    stage = "destroying the released indirect pbuffer";
    destroyPbuffer.reqType = glxOpcode;
    destroyPbuffer.glxCode = X_GLXDestroyPbuffer;
    destroyPbuffer.length = sz_xGLXDestroyPbufferReq / 4;
    destroyPbuffer.pbuffer = pbufferId;
    if (writeAll(client, &destroyPbuffer, sizeof(destroyPbuffer)) != 0)
        goto cleanup;

    stage = "copying indirect GLX context state";
    copyContext.reqType = glxOpcode;
    copyContext.glxCode = X_GLXCopyContext;
    copyContext.length = sz_xGLXCopyContextReq / 4;
    copyContext.source = contextId;
    copyContext.dest = singleContextId;
    copyContext.mask = GL_COLOR_BUFFER_BIT_VALUE;
    copyContext.contextTag = 0;
    if (writeAll(client, &copyContext, sizeof(copyContext)) != 0)
        goto cleanup;

    stage = "creating an indirect GLXPixmap";
    createPixmap.reqType = X_CreatePixmap;
    createPixmap.depth = 24;
    createPixmap.length = sz_xCreatePixmapReq / 4;
    createPixmap.pid = pixmapId;
    createPixmap.drawable = rootWindow;
    createPixmap.width = PIXMAP_WIDTH;
    createPixmap.height = PIXMAP_HEIGHT;
    if (writeAll(client, &createPixmap, sizeof(createPixmap)) != 0)
        goto cleanup;

    createGlxPixmap.reqType = glxOpcode;
    createGlxPixmap.glxCode = X_GLXCreatePixmap;
    createGlxPixmap.length = sz_xGLXCreatePixmapReq / 4;
    createGlxPixmap.screen = 0;
    createGlxPixmap.fbconfig = singleFbconfig;
    createGlxPixmap.pixmap = pixmapId;
    createGlxPixmap.glxpixmap = glxPixmapId;
    if (writeAll(client, &createGlxPixmap, sizeof(createGlxPixmap)) != 0)
        goto cleanup;

    makeContextCurrent.oldContextTag = 0;
    makeContextCurrent.drawable = glxPixmapId;
    makeContextCurrent.readdrawable = glxPixmapId;
    makeContextCurrent.context = singleContextId;
    if (writeAll(client, &makeContextCurrent,
                 sizeof(makeContextCurrent)) != 0 ||
        readReply(client, 36, reply) != 0)
        goto cleanup;
    contextTag = get32(reply + 8);
    if (contextTag == 0)
        goto cleanup;

    stage = "rendering with copied indirect GLX context state";
    clearOnlyRender[0] = glxOpcode;
    clearOnlyRender[1] = X_GLXRender;
    put16(clearOnlyRender + 2, sizeof(clearOnlyRender) / 4);
    memcpy(clearOnlyRender + 4, &contextTag, sizeof(contextTag));
    put16(clearOnlyRender + 8, 8);
    put16(clearOnlyRender + 10, X_GLrop_Clear);
    {
        const uint32_t mask = GL_COLOR_BUFFER_BIT_VALUE;

        memcpy(clearOnlyRender + 12, &mask, sizeof(mask));
    }
    if (writeAll(client, clearOnlyRender, sizeof(clearOnlyRender)) != 0)
        goto cleanup;
    waitGL.reqType = glxOpcode;
    waitGL.glxCode = X_GLXWaitGL;
    waitGL.length = sz_xGLXWaitGLReq / 4;
    waitGL.contextTag = contextTag;
    if (writeAll(client, &waitGL, sizeof(waitGL)) != 0)
        goto cleanup;

    stage = "reading the rendered indirect GLXPixmap";
    getImage.drawable = pixmapId;
    getImage.x = PIXMAP_WIDTH / 2;
    getImage.y = PIXMAP_HEIGHT / 2;
    if (writeAll(client, &getImage, sizeof(getImage)) != 0 ||
        readReply(client, 39, reply) != 0 || get32(reply + 4) < 1 ||
        readAll(client, &pixel, sizeof(pixel)) != 0)
        goto cleanup;
    if ((pixel & 0x00ffffffU) != 0x000000ffU) {
        fprintf(stderr, "unexpected indirect GLXPixmap pixel: 0x%08x\n",
                pixel);
        goto cleanup;
    }

    stage = "releasing the indirect GLXPixmap";
    makeContextCurrent.oldContextTag = contextTag;
    makeContextCurrent.drawable = 0;
    makeContextCurrent.readdrawable = 0;
    makeContextCurrent.context = 0;
    if (writeAll(client, &makeContextCurrent,
                 sizeof(makeContextCurrent)) != 0 ||
        readReply(client, 40, reply) != 0 || get32(reply + 8) != 0)
        goto cleanup;

    destroyGlxPixmap.reqType = glxOpcode;
    destroyGlxPixmap.glxCode = X_GLXDestroyPixmap;
    destroyGlxPixmap.length = sz_xGLXDestroyPixmapReq / 4;
    destroyGlxPixmap.glxpixmap = glxPixmapId;
    if (writeAll(client, &destroyGlxPixmap, sizeof(destroyGlxPixmap)) != 0)
        goto cleanup;
    freePixmap.reqType = X_FreePixmap;
    freePixmap.length = sizeof(freePixmap) / 4;
    freePixmap.id = pixmapId;
    if (writeAll(client, &freePixmap, sizeof(freePixmap)) != 0)
        goto cleanup;

    stage = "checking opposite-endian GLX negotiation";
    if (checkOppositeEndianGlx(socketPath) != 0)
        goto cleanup;

    result = 0;

cleanup:
    free(fbconfigs);
    free(visuals);
    free(setup);
    if (client >= 0)
        close(client);
    if (readyPipe[0] >= 0)
        close(readyPipe[0]);
    if (readyPipe[1] >= 0)
        close(readyPipe[1]);
    if (child > 0 && stopServer(child) != 0) {
        if (result == 0)
            stage = "stopping Xmin";
        result = 1;
    }
    if (result != 0)
        fprintf(stderr, "GLX integration failed while %s\n", stage);
    return result;
}
