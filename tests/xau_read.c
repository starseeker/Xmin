#include <X11/Xauth.h>

#include <stdio.h>
#include <string.h>

static int
write_short(FILE *file, unsigned short value)
{
    const unsigned char bytes[2] = {
        (unsigned char) (value >> 8),
        (unsigned char) value
    };

    return fwrite(bytes, sizeof(bytes), 1, file) == 1;
}

static int
write_field(FILE *file, const void *data, unsigned short length)
{
    return write_short(file, length) &&
        (length == 0 || fwrite(data, length, 1, file) == 1);
}

int
main(void)
{
    static const char address[] = "xmin-host";
    static const char number[] = "91";
    static const char name[] = "MIT-MAGIC-COOKIE-1";
    static const unsigned char cookie[] = {
        0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
        0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff
    };
    FILE *file = tmpfile();
    Xauth *auth;

    if (file == NULL)
        return 1;
    if (!write_short(file, FamilyLocal) ||
        !write_field(file, address, sizeof(address) - 1) ||
        !write_field(file, number, sizeof(number) - 1) ||
        !write_field(file, name, sizeof(name) - 1) ||
        !write_field(file, cookie, sizeof(cookie)) ||
        fflush(file) != 0 || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return 2;
    }

    auth = XauReadAuth(file);
    if (auth == NULL || auth->family != FamilyLocal ||
        auth->address_length != sizeof(address) - 1 ||
        memcmp(auth->address, address, sizeof(address) - 1) != 0 ||
        auth->number_length != sizeof(number) - 1 ||
        memcmp(auth->number, number, sizeof(number) - 1) != 0 ||
        auth->name_length != sizeof(name) - 1 ||
        memcmp(auth->name, name, sizeof(name) - 1) != 0 ||
        auth->data_length != sizeof(cookie) ||
        memcmp(auth->data, cookie, sizeof(cookie)) != 0) {
        XauDisposeAuth(auth);
        fclose(file);
        return 3;
    }

    XauDisposeAuth(auth);
    if (XauReadAuth(file) != NULL) {
        fclose(file);
        return 4;
    }
    fclose(file);

    file = tmpfile();
    if (file == NULL)
        return 5;
    if (!write_short(file, FamilyLocal) || !write_short(file, 4) ||
        fwrite("xy", 2, 1, file) != 1 || fflush(file) != 0 ||
        fseek(file, 0, SEEK_SET) != 0 || XauReadAuth(file) != NULL) {
        fclose(file);
        return 6;
    }
    fclose(file);
    return 0;
}
