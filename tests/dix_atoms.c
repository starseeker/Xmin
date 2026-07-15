#include <X11/X.h>
#include <X11/Xatom.h>
#include <X11/Xdefs.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern void InitAtoms(void);
extern void FreeAllAtoms(void);
extern Atom MakeAtom(const char *string, unsigned len, Bool makeit);
extern Bool ValidAtom(Atom atom);
extern const char *NameForAtom(Atom atom);

void
FatalError(const char *format, ...)
{
    va_list arguments;

    va_start(arguments, format);
    vfprintf(stderr, format, arguments);
    va_end(arguments);
    abort();
}

int
main(void)
{
    static const char name[] = "XMIN_TEST_ATOM";
    Atom atom;

    InitAtoms();
    if (!ValidAtom(XA_PRIMARY) || strcmp(NameForAtom(XA_PRIMARY), "PRIMARY") != 0)
        return 1;

    atom = MakeAtom(name, (unsigned) (sizeof(name) - 1), 1);
    if (!ValidAtom(atom) || strcmp(NameForAtom(atom), name) != 0)
        return 1;
    if (MakeAtom(name, (unsigned) (sizeof(name) - 1), 0) != atom)
        return 1;

    FreeAllAtoms();
    return 0;
}
