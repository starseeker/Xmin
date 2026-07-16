#include <dix-config.h>

#include "inputstr.h"
#include "exevents.h"

#include <X11/Xatom.h>

#include <stdlib.h>

int
main(void)
{
    CARD8 values8[] = { 3, 7, 255 };
    CARD16 values16[] = { 1024, 4096, 65535 };
    CARD32 values32[] = { 1, 65536, 0xffffffffU };
    XIPropertyValueRec value = {
        .type = XA_INTEGER,
        .format = 8,
        .size = 3,
        .data = values8,
    };
    int supplied[2] = { 0, 0 };
    int *result = NULL;
    int count = 0;

    if (XIPropToInt(&value, &count, &result) != Success || count != 3 ||
        result == NULL || result[0] != 3 || result[1] != 7 ||
        result[2] != 255) {
        return 1;
    }
    free(result);

    value.format = 16;
    value.data = values16;
    result = supplied;
    count = 2;
    if (XIPropToInt(&value, &count, &result) != Success || count != 2 ||
        supplied[0] != 1024 || supplied[1] != 4096) {
        return 2;
    }

    value.format = 32;
    value.data = values32;
    result = NULL;
    count = 2;
    if (XIPropToInt(&value, &count, &result) != BadLength)
        return 3;

    value.format = 24;
    result = supplied;
    count = 2;
    if (XIPropToInt(&value, &count, &result) != BadValue)
        return 4;

    value.format = 32;
    value.type = XA_STRING;
    if (XIPropToInt(&value, &count, &result) != BadMatch)
        return 5;

    return 0;
}
