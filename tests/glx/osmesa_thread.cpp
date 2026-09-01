#include <OSMesa/osmesa.h>
#include <OSMesa/glext.h>

#include <array>
#include <cstddef>
#include <thread>

namespace {

constexpr GLsizei buffer_width = 4;
constexpr GLsizei buffer_height = 4;
constexpr std::size_t channel_count = 4;

struct render_data {
    OSMesaContext context;
    std::array<unsigned char,
        buffer_width * buffer_height * channel_count> buffer{};
    int result = 1;
};

void
render_on_thread(render_data *data)
{
    auto bind_framebuffer = reinterpret_cast<PFNGLBINDFRAMEBUFFEREXTPROC>(
        OSMesaGetProcAddress("glBindFramebufferEXT"));

    if (!bind_framebuffer ||
        !OSMesaMakeCurrent(data->context, data->buffer.data(),
            GL_UNSIGNED_BYTE, buffer_width, buffer_height))
        return;

    bind_framebuffer(GL_FRAMEBUFFER_EXT, 0);
    if (glGetError() != GL_NO_ERROR) {
        data->result = 2;
    } else {
        glClearColor(1.0F, 0.0F, 0.0F, 1.0F);
        glClear(GL_COLOR_BUFFER_BIT);
        data->result = glGetError() == GL_NO_ERROR ? 0 : 3;
    }

    if (!OSMesaMakeCurrent(nullptr, nullptr, GL_UNSIGNED_BYTE, 0, 0) &&
        data->result == 0)
        data->result = 4;
}

} // namespace

int
main()
{
    render_data data;
    data.context = OSMesaCreateContextExt(OSMESA_RGBA, 24, 8, 0, nullptr);
    if (!data.context)
        return 1;
    if (!OSMesaMakeCurrent(data.context, data.buffer.data(),
            GL_UNSIGNED_BYTE, buffer_width, buffer_height)) {
        OSMesaDestroyContext(data.context);
        return 1;
    }

    glClearColor(0.0F, 0.0F, 1.0F, 1.0F);
    glClear(GL_COLOR_BUFFER_BIT);
    if (glGetError() != GL_NO_ERROR ||
        !OSMesaMakeCurrent(nullptr, nullptr, GL_UNSIGNED_BYTE, 0, 0)) {
        OSMesaDestroyContext(data.context);
        return 2;
    }

    std::thread worker(render_on_thread, &data);
    worker.join();

    int result = data.result != 0 ? data.result + 2 : 0;
    if (result == 0 && (data.buffer[0] != 255 || data.buffer[1] != 0 ||
        data.buffer[2] != 0 || data.buffer[3] != 255))
        result = 7;

    OSMesaDestroyContext(data.context);
    return result;
}
