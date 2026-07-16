#include "xmin/config.h"

#include <pixman.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int dix_main(int argc, char **argv, char **envp);

static void
print_version(void)
{
    printf("%s %s\n", XMIN_NAME, XMIN_VERSION);
    printf("default screen: %dx%dx%d at %d DPI\n",
           XMIN_DEFAULT_WIDTH,
           XMIN_DEFAULT_HEIGHT,
           XMIN_DEFAULT_DEPTH,
           XMIN_DEFAULT_DPI);
    printf("server features: MIT-SHM=%s TCP=%s\n",
           XMIN_HAVE_MITSHM ? "on" : "off",
           XMIN_ENABLE_TCP ? "on" : "off");
    printf("GLX/OSMesa integration: %s\n",
           XMIN_BUILD_GLX ? "enabled (indirect software renderer)" : "disabled");
    printf("embedded pixman: %s (generic C)\n", pixman_version_string());
}

int
main(int argc, char **argv, char **envp)
{
    if (argc == 2 && strcmp(argv[1], "--help") == 0) {
        argv[1] = (char *) "-help";
    }
    if (argc == 2 && strcmp(argv[1], "--version") == 0) {
        print_version();
        return EXIT_SUCCESS;
    }

    return dix_main(argc, argv, envp);
}
