#include "xmin/config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void
print_usage(FILE *stream, const char *program)
{
    fprintf(stream,
            "Usage: %s [--help] [--version]\n"
            "\n"
            "Xmin is currently a build-system stub. The X11 server core has not "
            "yet been imported.\n",
            program);
}

static void
print_version(void)
{
    printf("%s %s (server stub)\n", XMIN_NAME, XMIN_VERSION);
    printf("planned default screen: %dx%dx%d at %d DPI\n",
           XMIN_DEFAULT_WIDTH,
           XMIN_DEFAULT_HEIGHT,
           XMIN_DEFAULT_DEPTH,
           XMIN_DEFAULT_DPI);
    printf("configured features: GLX=%s MIT-SHM=%s TCP=%s\n",
           XMIN_BUILD_GLX ? "on" : "off",
           XMIN_HAVE_MITSHM ? "on" : "off",
           XMIN_ENABLE_TCP ? "on" : "off");
}

int
main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "--help") == 0) {
        print_usage(stdout, argv[0]);
        return EXIT_SUCCESS;
    }
    if (argc == 2 && strcmp(argv[1], "--version") == 0) {
        print_version();
        return EXIT_SUCCESS;
    }
    if (argc > 1) {
        fprintf(stderr, "%s: unsupported stub argument: %s\n", argv[0], argv[1]);
        print_usage(stderr, argv[0]);
        return EXIT_FAILURE;
    }

    fprintf(stderr,
            "%s %s: the X11 server implementation has not been added yet.\n",
            XMIN_NAME,
            XMIN_VERSION);
    return EXIT_FAILURE;
}

