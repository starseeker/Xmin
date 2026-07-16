#include <xcb/xcb.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static int
failed(const char *operation, xcb_generic_error_t *error)
{
    fprintf(stderr, "capture-next-core-keymap: %s failed", operation);
    if (error != NULL)
        fprintf(stderr, " with X11 error %u", error->error_code);
    fputc('\n', stderr);
    free(error);
    return 1;
}

int
main(void)
{
    xcb_connection_t *connection = xcb_connect(NULL, NULL);
    xcb_generic_error_t *error = NULL;
    const xcb_setup_t *setup;
    xcb_get_keyboard_mapping_reply_t *mapping = NULL;
    xcb_get_modifier_mapping_reply_t *modifiers = NULL;
    xcb_get_keyboard_control_reply_t *keyboard = NULL;
    xcb_get_pointer_control_reply_t *pointer = NULL;
    xcb_get_pointer_mapping_reply_t *buttons = NULL;
    uint8_t keycode_count;
    int result = 1;

    if (connection == NULL || xcb_connection_has_error(connection))
        return failed("connecting to DISPLAY", NULL);
    setup = xcb_get_setup(connection);
    keycode_count = (uint8_t) (setup->max_keycode - setup->min_keycode + 1);
    mapping = xcb_get_keyboard_mapping_reply(
        connection,
        xcb_get_keyboard_mapping(
            connection, setup->min_keycode, keycode_count),
        &error);
    if (mapping == NULL || error != NULL)
        goto cleanup;
    modifiers = xcb_get_modifier_mapping_reply(
        connection, xcb_get_modifier_mapping(connection), &error);
    if (modifiers == NULL || error != NULL)
        goto cleanup;
    keyboard = xcb_get_keyboard_control_reply(
        connection, xcb_get_keyboard_control(connection), &error);
    if (keyboard == NULL || error != NULL)
        goto cleanup;
    pointer = xcb_get_pointer_control_reply(
        connection, xcb_get_pointer_control(connection), &error);
    if (pointer == NULL || error != NULL)
        goto cleanup;
    buttons = xcb_get_pointer_mapping_reply(
        connection, xcb_get_pointer_mapping(connection), &error);
    if (buttons == NULL || error != NULL)
        goto cleanup;

    puts("#ifndef XMIN_NEXT_GENERATED_CORE_KEYMAP_HPP");
    puts("#define XMIN_NEXT_GENERATED_CORE_KEYMAP_HPP");
    puts("");
    puts("// Captured from Xmin's pinned xmin-us.xkb map; do not edit.");
    puts("// Run tools/capture-next-core-keymap.c against the legacy oracle.");
    puts("");
    puts("#include <array>");
    puts("#include <cstddef>");
    puts("#include <cstdint>");
    puts("");
    puts("namespace xmin::next {");
    puts("");
    printf("inline constexpr std::uint8_t minimum_keycode = %u;\n",
           setup->min_keycode);
    printf("inline constexpr std::uint8_t maximum_keycode = %u;\n",
           setup->max_keycode);
    printf("inline constexpr std::size_t keysyms_per_keycode = %u;\n",
           mapping->keysyms_per_keycode);
    puts("");
    puts("inline constexpr auto core_keymap = [] {");
    puts("    std::array<std::array<std::uint32_t, keysyms_per_keycode>, 256>");
    puts("        result{};");
    {
        xcb_keysym_t *keysyms = xcb_get_keyboard_mapping_keysyms(mapping);
        unsigned int code;

        for (code = setup->min_keycode; code <= setup->max_keycode; ++code) {
            unsigned int index = code - setup->min_keycode;
            unsigned int level;
            int populated = 0;

            for (level = 0; level < mapping->keysyms_per_keycode; ++level) {
                if (keysyms[index * mapping->keysyms_per_keycode + level] != 0)
                    populated = 1;
            }
            if (!populated)
                continue;
            printf("    result[%u] = {{\n        ", code);
            for (level = 0; level < mapping->keysyms_per_keycode; ++level) {
                printf("0x%08xU%s",
                       keysyms[index * mapping->keysyms_per_keycode + level],
                       level + 1 == mapping->keysyms_per_keycode ? "" : ", ");
            }
            puts("\n    }};");
        }
    }
    puts("    return result;");
    puts("}();");
    puts("");
    printf("inline constexpr std::size_t keys_per_modifier = %u;\n",
           modifiers->keycodes_per_modifier);
    printf("inline constexpr std::array<std::uint8_t, %u> core_modifier_map{{\n",
           modifiers->keycodes_per_modifier * 8U);
    {
        xcb_keycode_t *keycodes =
            xcb_get_modifier_mapping_keycodes(modifiers);
        unsigned int index;
        unsigned int count = modifiers->keycodes_per_modifier * 8U;

        for (index = 0; index < count; ++index) {
            if (index % 8 == 0)
                fputs("    ", stdout);
            printf("%u", keycodes[index]);
            if (index + 1 != count)
                fputc(',', stdout);
            if (index % 8 == 7 || index + 1 == count)
                fputc('\n', stdout);
            else
                fputc(' ', stdout);
        }
    }
    puts("}};");
    printf("inline constexpr std::array<std::uint8_t, 32> "
           "default_auto_repeats{{\n");
    {
        unsigned int index;
        for (index = 0; index < 32; ++index) {
            if (index % 8 == 0)
                fputs("    ", stdout);
            printf("%u", keyboard->auto_repeats[index]);
            if (index != 31)
                fputc(',', stdout);
            if (index % 8 == 7)
                fputc('\n', stdout);
            else
                fputc(' ', stdout);
        }
    }
    puts("}};");
    printf("inline constexpr bool default_global_auto_repeat = %s;\n",
           keyboard->global_auto_repeat ? "true" : "false");
    printf("inline constexpr std::uint8_t default_key_click_percent = %u;\n",
           keyboard->key_click_percent);
    printf("inline constexpr std::uint8_t default_bell_percent = %u;\n",
           keyboard->bell_percent);
    printf("inline constexpr std::uint16_t default_bell_pitch = %u;\n",
           keyboard->bell_pitch);
    printf("inline constexpr std::uint16_t default_bell_duration = %u;\n",
           keyboard->bell_duration);
    printf("inline constexpr std::int16_t default_pointer_acceleration_numerator "
           "= %u;\n", pointer->acceleration_numerator);
    printf("inline constexpr std::int16_t default_pointer_acceleration_denominator "
           "= %u;\n", pointer->acceleration_denominator);
    printf("inline constexpr std::int16_t default_pointer_threshold = %u;\n",
           pointer->threshold);
    {
        int count = xcb_get_pointer_mapping_map_length(buttons);
        uint8_t *map = xcb_get_pointer_mapping_map(buttons);
        int index;

        printf("inline constexpr std::array<std::uint8_t, %d> "
               "default_pointer_map{{", count);
        for (index = 0; index < count; ++index)
            printf("%u%s", map[index], index + 1 == count ? "" : ", ");
        puts("}};");
    }
    puts("");
    puts("} // namespace xmin::next");
    puts("");
    puts("#endif");
    result = 0;

cleanup:
    if (result != 0)
        failed("querying the legacy core keymap", error);
    free(buttons);
    free(pointer);
    free(keyboard);
    free(modifiers);
    free(mapping);
    xcb_disconnect(connection);
    return result;
}
