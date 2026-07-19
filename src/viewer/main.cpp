#include "viewer_transport.hpp"

#include <GLFW/glfw3.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

#ifndef GL_BGRA
#define GL_BGRA 0x80E1
#endif

namespace {

struct Options {
    std::string display;
    std::string authority;
    std::string session_info;
    unsigned frames_per_second = 30;
    bool shared_memory = true;
};

struct Viewport {
    double x = 0;
    double y = 0;
    double width = 1;
    double height = 1;
};

struct ViewerState {
    xmin::viewer::GuestTransport *guest = nullptr;
    GLFWwindow *window = nullptr;
    std::array<bool, 256> keys{};
    std::array<bool, 16> buttons{};
    bool pointer_inside = false;
};

void
usage(std::ostream &stream, std::string_view program)
{
    stream << "usage: " << program
           << " [--display DISPLAY] [--auth FILE] [--fps RATE] [--no-shm]\n"
           << "       " << program << " --session-info FILE [--fps RATE] [--no-shm]\n"
           << "\n"
           << "The host window uses the ordinary DISPLAY.  The Xmin guest is\n"
           << "selected by --display or XMIN_DISPLAY, with credentials from\n"
           << "--auth or XMIN_XAUTHORITY.\n";
}

bool
load_session_info(Options &options)
{
    std::ifstream input(options.session_info);
    if (!input)
        return false;
    std::string line;
    while (std::getline(input, line)) {
        constexpr std::string_view display_prefix = "XMIN_DISPLAY=";
        constexpr std::string_view authority_prefix = "XMIN_XAUTHORITY=";
        if (line.rfind(display_prefix, 0) == 0)
            options.display = line.substr(display_prefix.size());
        else if (line.rfind(authority_prefix, 0) == 0)
            options.authority = line.substr(authority_prefix.size());
    }
    return input.eof() && !options.display.empty() &&
        !options.authority.empty();
}

bool
parse_unsigned(std::string_view text, unsigned &value)
{
    if (text.empty())
        return false;
    unsigned parsed = 0;
    for (const char character : text) {
        if (character < '0' || character > '9' ||
            parsed > (std::numeric_limits<unsigned>::max() -
                      static_cast<unsigned>(character - '0')) / 10U) {
            return false;
        }
        parsed = parsed * 10U + static_cast<unsigned>(character - '0');
    }
    value = parsed;
    return true;
}

int
parse_options(int argc, char **argv, Options &options)
{
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument(argv[index]);
        if (argument == "--help") {
            usage(std::cout, argv[0]);
            return 1;
        }
        if (argument == "--no-shm") {
            options.shared_memory = false;
            continue;
        }
        if ((argument == "--display" || argument == "--auth" ||
             argument == "--session-info" ||
             argument == "--fps") && index + 1 < argc) {
            const std::string_view value(argv[++index]);
            if (argument == "--display")
                options.display = value;
            else if (argument == "--auth")
                options.authority = value;
            else if (argument == "--session-info")
                options.session_info = value;
            else if (!parse_unsigned(value, options.frames_per_second) ||
                     options.frames_per_second == 0 ||
                     options.frames_per_second > 240) {
                std::cerr << "xmin-viewer: invalid frame rate: " << value
                          << '\n';
                return -1;
            }
            continue;
        }
        std::cerr << "xmin-viewer: unknown or incomplete option: "
                  << argument << '\n';
        usage(std::cerr, argv[0]);
        return -1;
    }
    if (!options.session_info.empty() && !load_session_info(options)) {
        std::cerr << "xmin-viewer: cannot read session information from "
                  << options.session_info << '\n';
        return -1;
    }
    if (options.display.empty()) {
        if (const char *value = std::getenv("XMIN_DISPLAY"))
            options.display = value;
    }
    if (options.authority.empty()) {
        if (const char *value = std::getenv("XMIN_XAUTHORITY"))
            options.authority = value;
    }
    if (options.display.empty()) {
        std::cerr << "xmin-viewer: no Xmin guest display was specified\n";
        usage(std::cerr, argv[0]);
        return -1;
    }
    return 0;
}

void
glfw_error(int code, const char *description)
{
    std::cerr << "xmin-viewer: GLFW error " << code << ": "
              << (description != nullptr ? description : "unknown error")
              << '\n';
}

Viewport
content_viewport(GLFWwindow *window, std::uint16_t guest_width,
                 std::uint16_t guest_height, bool framebuffer)
{
    int width = 1;
    int height = 1;
    if (framebuffer)
        glfwGetFramebufferSize(window, &width, &height);
    else
        glfwGetWindowSize(window, &width, &height);
    width = std::max(width, 1);
    height = std::max(height, 1);
    const double guest_aspect =
        static_cast<double>(guest_width) / guest_height;
    const double window_aspect = static_cast<double>(width) / height;
    Viewport result;
    if (window_aspect > guest_aspect) {
        result.height = height;
        result.width = result.height * guest_aspect;
        result.x = (width - result.width) * 0.5;
    }
    else {
        result.width = width;
        result.height = result.width / guest_aspect;
        result.y = (height - result.height) * 0.5;
    }
    return result;
}

std::uint8_t
guest_keycode(int key)
{
    switch (key) {
    case GLFW_KEY_ESCAPE: return 9;
    case GLFW_KEY_1: return 10;
    case GLFW_KEY_2: return 11;
    case GLFW_KEY_3: return 12;
    case GLFW_KEY_4: return 13;
    case GLFW_KEY_5: return 14;
    case GLFW_KEY_6: return 15;
    case GLFW_KEY_7: return 16;
    case GLFW_KEY_8: return 17;
    case GLFW_KEY_9: return 18;
    case GLFW_KEY_0: return 19;
    case GLFW_KEY_MINUS: return 20;
    case GLFW_KEY_EQUAL: return 21;
    case GLFW_KEY_BACKSPACE: return 22;
    case GLFW_KEY_TAB: return 23;
    case GLFW_KEY_Q: return 24;
    case GLFW_KEY_W: return 25;
    case GLFW_KEY_E: return 26;
    case GLFW_KEY_R: return 27;
    case GLFW_KEY_T: return 28;
    case GLFW_KEY_Y: return 29;
    case GLFW_KEY_U: return 30;
    case GLFW_KEY_I: return 31;
    case GLFW_KEY_O: return 32;
    case GLFW_KEY_P: return 33;
    case GLFW_KEY_LEFT_BRACKET: return 34;
    case GLFW_KEY_RIGHT_BRACKET: return 35;
    case GLFW_KEY_ENTER: return 36;
    case GLFW_KEY_LEFT_CONTROL: return 37;
    case GLFW_KEY_A: return 38;
    case GLFW_KEY_S: return 39;
    case GLFW_KEY_D: return 40;
    case GLFW_KEY_F: return 41;
    case GLFW_KEY_G: return 42;
    case GLFW_KEY_H: return 43;
    case GLFW_KEY_J: return 44;
    case GLFW_KEY_K: return 45;
    case GLFW_KEY_L: return 46;
    case GLFW_KEY_SEMICOLON: return 47;
    case GLFW_KEY_APOSTROPHE: return 48;
    case GLFW_KEY_GRAVE_ACCENT: return 49;
    case GLFW_KEY_LEFT_SHIFT: return 50;
    case GLFW_KEY_BACKSLASH: return 51;
    case GLFW_KEY_Z: return 52;
    case GLFW_KEY_X: return 53;
    case GLFW_KEY_C: return 54;
    case GLFW_KEY_V: return 55;
    case GLFW_KEY_B: return 56;
    case GLFW_KEY_N: return 57;
    case GLFW_KEY_M: return 58;
    case GLFW_KEY_COMMA: return 59;
    case GLFW_KEY_PERIOD: return 60;
    case GLFW_KEY_SLASH: return 61;
    case GLFW_KEY_RIGHT_SHIFT: return 62;
    case GLFW_KEY_KP_MULTIPLY: return 63;
    case GLFW_KEY_LEFT_ALT: return 64;
    case GLFW_KEY_SPACE: return 65;
    case GLFW_KEY_CAPS_LOCK: return 66;
    case GLFW_KEY_F1: return 67;
    case GLFW_KEY_F2: return 68;
    case GLFW_KEY_F3: return 69;
    case GLFW_KEY_F4: return 70;
    case GLFW_KEY_F5: return 71;
    case GLFW_KEY_F6: return 72;
    case GLFW_KEY_F7: return 73;
    case GLFW_KEY_F8: return 74;
    case GLFW_KEY_F9: return 75;
    case GLFW_KEY_F10: return 76;
    case GLFW_KEY_NUM_LOCK: return 77;
    case GLFW_KEY_SCROLL_LOCK: return 78;
    case GLFW_KEY_KP_7: return 79;
    case GLFW_KEY_KP_8: return 80;
    case GLFW_KEY_KP_9: return 81;
    case GLFW_KEY_KP_SUBTRACT: return 82;
    case GLFW_KEY_KP_4: return 83;
    case GLFW_KEY_KP_5: return 84;
    case GLFW_KEY_KP_6: return 85;
    case GLFW_KEY_KP_ADD: return 86;
    case GLFW_KEY_KP_1: return 87;
    case GLFW_KEY_KP_2: return 88;
    case GLFW_KEY_KP_3: return 89;
    case GLFW_KEY_KP_0: return 90;
    case GLFW_KEY_KP_DECIMAL: return 91;
    case GLFW_KEY_F11: return 95;
    case GLFW_KEY_F12: return 96;
    case GLFW_KEY_KP_ENTER: return 104;
    case GLFW_KEY_RIGHT_CONTROL: return 105;
    case GLFW_KEY_KP_DIVIDE: return 106;
    case GLFW_KEY_RIGHT_ALT: return 108;
    case GLFW_KEY_HOME: return 110;
    case GLFW_KEY_UP: return 111;
    case GLFW_KEY_PAGE_UP: return 112;
    case GLFW_KEY_LEFT: return 113;
    case GLFW_KEY_RIGHT: return 114;
    case GLFW_KEY_END: return 115;
    case GLFW_KEY_DOWN: return 116;
    case GLFW_KEY_PAGE_DOWN: return 117;
    case GLFW_KEY_INSERT: return 118;
    case GLFW_KEY_DELETE: return 119;
    case GLFW_KEY_PAUSE: return 127;
    case GLFW_KEY_LEFT_SUPER: return 133;
    case GLFW_KEY_RIGHT_SUPER: return 134;
    case GLFW_KEY_MENU: return 135;
    default: return 0;
    }
}

void
release_inputs(ViewerState &state)
{
    for (std::size_t key = 0; key < state.keys.size(); ++key) {
        if (state.keys[key]) {
            state.guest->key(static_cast<std::uint8_t>(key), false);
            state.keys[key] = false;
        }
    }
    for (std::size_t button = 1; button < state.buttons.size(); ++button) {
        if (state.buttons[button]) {
            state.guest->button(static_cast<std::uint8_t>(button), false);
            state.buttons[button] = false;
        }
    }
    state.guest->flush();
}

void
key_callback(GLFWwindow *window, int key, int, int action, int)
{
    auto *state = static_cast<ViewerState *>(glfwGetWindowUserPointer(window));
    if (state == nullptr)
        return;
    const std::uint8_t code = guest_keycode(key);
    if (code == 0)
        return;
    const bool pressed = action != GLFW_RELEASE;

    // A release may be lost when the host changes focus while a guest menu is
    // handling the corresponding press.  GLFW then reports later presses as
    // repeats, so treat both PRESS and REPEAT as recoverable transitions.  A
    // host repeat becomes one release/press pair in the guest, while releases
    // are forwarded even when our local state is already clear.
    if (pressed && state->keys[code])
        state->guest->key(code, false);
    state->keys[code] = pressed;
    state->guest->key(code, pressed);
    state->guest->flush();
}

void
focus_callback(GLFWwindow *window, int focused)
{
    auto *state = static_cast<ViewerState *>(glfwGetWindowUserPointer(window));
    if (state != nullptr && focused == GLFW_FALSE)
        release_inputs(*state);
}

bool
guest_pointer(ViewerState &state, double host_x, double host_y,
              std::int16_t &guest_x, std::int16_t &guest_y)
{
    const auto area = content_viewport(
        state.window, state.guest->width(), state.guest->height(), false);
    if (host_x < area.x || host_y < area.y ||
        host_x >= area.x + area.width || host_y >= area.y + area.height) {
        return false;
    }
    const double scaled_x = (host_x - area.x) * state.guest->width() /
        area.width;
    const double scaled_y = (host_y - area.y) * state.guest->height() /
        area.height;
    guest_x = static_cast<std::int16_t>(std::clamp(
        scaled_x, 0.0, static_cast<double>(state.guest->width() - 1)));
    guest_y = static_cast<std::int16_t>(std::clamp(
        scaled_y, 0.0, static_cast<double>(state.guest->height() - 1)));
    return true;
}

void
cursor_callback(GLFWwindow *window, double x, double y)
{
    auto *state = static_cast<ViewerState *>(glfwGetWindowUserPointer(window));
    if (state == nullptr)
        return;
    std::int16_t guest_x = 0;
    std::int16_t guest_y = 0;
    state->pointer_inside = guest_pointer(
        *state, x, y, guest_x, guest_y);
    if (state->pointer_inside) {
        state->guest->pointer(guest_x, guest_y);
        state->guest->flush();
    }
}

std::uint8_t
guest_button(int button)
{
    switch (button) {
    case GLFW_MOUSE_BUTTON_LEFT: return 1;
    case GLFW_MOUSE_BUTTON_MIDDLE: return 2;
    case GLFW_MOUSE_BUTTON_RIGHT: return 3;
    default:
        return button >= 3 && button <= 10
            ? static_cast<std::uint8_t>(button + 5)
            : 0;
    }
}

void
button_callback(GLFWwindow *window, int button, int action, int)
{
    auto *state = static_cast<ViewerState *>(glfwGetWindowUserPointer(window));
    if (state == nullptr)
        return;
    double x = 0;
    double y = 0;
    glfwGetCursorPos(window, &x, &y);
    std::int16_t guest_x = 0;
    std::int16_t guest_y = 0;
    if (!guest_pointer(*state, x, y, guest_x, guest_y))
        return;
    state->guest->pointer(guest_x, guest_y);
    const std::uint8_t code = guest_button(button);
    if (code == 0)
        return;
    const bool pressed = action == GLFW_PRESS;
    state->buttons[code] = pressed;
    state->guest->button(code, pressed);
    state->guest->flush();
}

void
scroll_callback(GLFWwindow *window, double, double y)
{
    auto *state = static_cast<ViewerState *>(glfwGetWindowUserPointer(window));
    if (state == nullptr || !state->pointer_inside || y == 0)
        return;
    const std::uint8_t button = y > 0 ? 4 : 5;
    const unsigned count = static_cast<unsigned>(std::max(1.0, std::abs(y)));
    for (unsigned index = 0; index < count; ++index) {
        state->guest->button(button, true);
        state->guest->button(button, false);
    }
    state->guest->flush();
}

void
draw_frame(GLFWwindow *window, GLuint texture,
           const xmin::viewer::FrameView &frame,
           std::uint16_t guest_width, std::uint16_t guest_height,
           std::vector<std::uint8_t> &converted)
{
    const std::uint8_t *pixels = frame.pixels;
    GLenum format = GL_BGRA;
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    converted.resize(static_cast<std::size_t>(frame.width) * frame.height * 4U);
    const auto *source = reinterpret_cast<const std::uint32_t *>(frame.pixels);
    for (std::size_t index = 0;
         index < static_cast<std::size_t>(frame.width) * frame.height; ++index) {
        const std::uint32_t value = source[index];
        converted[index * 4U] = static_cast<std::uint8_t>(value >> 16);
        converted[index * 4U + 1U] = static_cast<std::uint8_t>(value >> 8);
        converted[index * 4U + 2U] = static_cast<std::uint8_t>(value);
        converted[index * 4U + 3U] = 255;
    }
    pixels = converted.data();
    format = GL_RGBA;
#else
    static_cast<void>(converted);
#endif

    glBindTexture(GL_TEXTURE_2D, texture);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexSubImage2D(GL_TEXTURE_2D, 0, frame.x, frame.y,
                    frame.width, frame.height,
                    format, GL_UNSIGNED_BYTE, pixels);
    if (frame.more)
        return;

    int framebuffer_width = 1;
    int framebuffer_height = 1;
    glfwGetFramebufferSize(window, &framebuffer_width, &framebuffer_height);
    glViewport(0, 0, framebuffer_width, framebuffer_height);
    constexpr float color_scale = 1.0F / 255.0F;
    glClearColor(32.0F * color_scale, 37.0F * color_scale,
                 43.0F * color_scale, 1.0F);
    glClear(GL_COLOR_BUFFER_BIT);
    const auto area = content_viewport(
        window, guest_width, guest_height, true);
    const float left = static_cast<float>(
        area.x / framebuffer_width * 2.0 - 1.0);
    const float right = static_cast<float>(
        (area.x + area.width) / framebuffer_width * 2.0 - 1.0);
    const float top = static_cast<float>(
        1.0 - area.y / framebuffer_height * 2.0);
    const float bottom = static_cast<float>(
        1.0 - (area.y + area.height) / framebuffer_height * 2.0);
    glEnable(GL_TEXTURE_2D);
    glColor3f(1.0F, 1.0F, 1.0F);
    glBegin(GL_QUADS);
    glTexCoord2f(0.0F, 1.0F); glVertex2f(left, bottom);
    glTexCoord2f(1.0F, 1.0F); glVertex2f(right, bottom);
    glTexCoord2f(1.0F, 0.0F); glVertex2f(right, top);
    glTexCoord2f(0.0F, 0.0F); glVertex2f(left, top);
    glEnd();
    glfwSwapBuffers(window);
}

int
run(int argc, char **argv)
{
    Options options;
    const int parsed = parse_options(argc, argv, options);
    if (parsed != 0)
        return parsed > 0 ? 0 : 2;
    glfwSetErrorCallback(glfw_error);
    if (glfwInit() == GLFW_FALSE) {
        std::cerr << "xmin-viewer: cannot initialize the host window system\n";
        return 1;
    }

    std::string transport_error;
    auto guest = xmin::viewer::create_guest_transport(
        options.display, options.authority, options.shared_memory,
        transport_error);
    if (!guest) {
        std::cerr << "xmin-viewer: " << transport_error << '\n';
        glfwTerminate();
        return 1;
    }

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 2);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 1);
    const int initial_width = std::min<int>(guest->width(), 1280);
    const int initial_height = std::max<int>(
        1, initial_width * guest->height() / guest->width());
    GLFWwindow *window = glfwCreateWindow(
        initial_width, initial_height, "Xmin Viewer", nullptr, nullptr);
    if (window == nullptr) {
        std::cerr << "xmin-viewer: cannot create the host window\n";
        guest.reset();
        glfwTerminate();
        return 1;
    }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);
    ViewerState state{guest.get(), window};
    glfwSetWindowUserPointer(window, &state);
    glfwSetKeyCallback(window, key_callback);
    glfwSetWindowFocusCallback(window, focus_callback);
    glfwSetCursorPosCallback(window, cursor_callback);
    glfwSetMouseButtonCallback(window, button_callback);
    glfwSetScrollCallback(window, scroll_callback);

    GLuint texture = 0;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, guest->width(), guest->height(),
                 0, GL_BGRA, GL_UNSIGNED_BYTE, nullptr);

    std::cerr << "xmin-viewer: attached to " << options.display << " ("
              << guest->width() << 'x' << guest->height() << ", "
              << (guest->using_shared_memory() ? "MIT-SHM" : "GetImage")
              << ")\n";
    std::vector<std::uint8_t> converted;
    using FrameClock = std::chrono::steady_clock;
    const auto frame_period = std::chrono::duration_cast<FrameClock::duration>(
        std::chrono::duration<double>(1.0 / options.frames_per_second));
    auto next_frame = FrameClock::now();
    while (glfwWindowShouldClose(window) == GLFW_FALSE) {
        const auto now = FrameClock::now();
        if (now >= next_frame) {
            if (guest->frame_pending()) {
                bool more = false;
                do {
                    xmin::viewer::FrameView frame;
                    if (!guest->capture(frame)) {
                        std::cerr << "xmin-viewer: " << guest->error()
                                  << '\n';
                        glfwSetWindowShouldClose(window, GLFW_TRUE);
                        break;
                    }
                    more = frame.more;
                    draw_frame(window, texture, frame, guest->width(),
                               guest->height(), converted);
                } while (more);
            }
            next_frame += frame_period;
            const auto completed = FrameClock::now();
            if (next_frame <= completed)
                next_frame = completed + frame_period;
        }
        const auto wait = std::max(
            FrameClock::duration::zero(), next_frame - FrameClock::now());
        glfwWaitEventsTimeout(std::chrono::duration<double>(wait).count());
    }

    release_inputs(state);
    glDeleteTextures(1, &texture);
    glfwDestroyWindow(window);
    guest.reset();
    glfwTerminate();
    return 0;
}

} // namespace

int
main(int argc, char **argv)
{
    return run(argc, argv);
}
