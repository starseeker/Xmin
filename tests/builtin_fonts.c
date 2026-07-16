#include <stdarg.h>
#include <stddef.h>
#include <string.h>

#include <X11/fonts/font.h>
#include <X11/fonts/fontstruct.h>
#include <X11/fonts/libxfont2.h>

static const xfont2_fpe_funcs_rec *builtin_fpe;

static int
capture_fpe(const xfont2_fpe_funcs_rec *funcs)
{
    if (funcs == NULL || funcs->version != XFONT2_FPE_FUNCS_VERSION)
        return 1;
    builtin_fpe = funcs;
    return 0;
}

static unsigned long
server_generation(void)
{
    return 1;
}

static int
default_point_size(void)
{
    return 120;
}

static FontResolutionPtr
client_resolutions(int *count)
{
    static FontResolutionRec resolution = { 75, 75, 120 };

    *count = 1;
    return &resolution;
}

static void
discard_error(const char *format, va_list arguments)
{
    (void) format;
    (void) arguments;
}

static FontPtr
open_font(FontPathElementPtr fpe, const char *name)
{
    FontPtr font = NULL;
    char *alias = NULL;
    int status;

    status = builtin_fpe->open_font(NULL, fpe, FontLoadAll,
                                    name, (int) strlen(name),
                                    0, 0, 0, &font, &alias, NULL);
    if (status == FontNameAlias) {
        if (alias == NULL)
            return NULL;
        status = builtin_fpe->open_font(NULL, fpe, FontLoadAll,
                                        alias, (int) strlen(alias),
                                        0, 0, 0, &font, &alias, NULL);
    }
    return status == Successful ? font : NULL;
}

int
main(void)
{
    const xfont2_client_funcs_rec client_funcs = {
        .version = XFONT2_CLIENT_FUNCS_VERSION,
        .verrorf = discard_error,
        .get_client_resolutions = client_resolutions,
        .get_default_point_size = default_point_size,
        .register_fpe_funcs = capture_fpe,
        .get_server_generation = server_generation,
    };
    FontPathElementRec fpe = {
        .name_length = 9,
        .name = "built-ins",
        .refcount = 1,
    };
    FontPtr fixed;
    FontPtr cursor;
    unsigned char character = 'A';
    CharInfoPtr glyph = NULL;
    unsigned long glyph_count = 0;

    if (xfont2_init(&client_funcs) != Successful || builtin_fpe == NULL)
        return 1;
    if (!builtin_fpe->name_check("built-ins") ||
        builtin_fpe->name_check("/usr/share/fonts"))
        return 2;
    if (builtin_fpe->init_fpe(&fpe) != Successful)
        return 3;

    fixed = open_font(&fpe, "fixed");
    if (fixed == NULL || fixed->info.firstRow != 0 ||
        fixed->info.firstCol > 'A' || fixed->info.lastCol < 'A' ||
        fixed->info.fontAscent <= 0)
        return 4;
    if (fixed->get_glyphs(fixed, 1, &character, Linear8Bit,
                          &glyph_count, &glyph) != Successful ||
        glyph_count != 1 || glyph == NULL ||
        glyph->metrics.characterWidth != 6 || glyph->bits == NULL)
        return 5;
    builtin_fpe->close_font(&fpe, fixed);

    cursor = open_font(&fpe, "cursor");
    if (cursor == NULL || cursor->info.firstRow != 0 ||
        cursor->info.lastCol < cursor->info.firstCol)
        return 6;
    builtin_fpe->close_font(&fpe, cursor);

    return builtin_fpe->free_fpe(&fpe) == Successful ? 0 : 7;
}
