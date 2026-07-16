#include "xsha1.h"

#include <stddef.h>
#include <string.h>

static int
digest_matches(const void *data, int size, const unsigned char expected[20])
{
    unsigned char actual[20];
    void *context = x_sha1_init();

    if (context == NULL)
        return 0;
    if (!x_sha1_update(context, (void *) data, size))
        return 0;
    if (!x_sha1_final(context, actual))
        return 0;
    return memcmp(actual, expected, sizeof(actual)) == 0;
}

int
main(void)
{
    static const unsigned char empty_digest[20] = {
        0xda, 0x39, 0xa3, 0xee, 0x5e, 0x6b, 0x4b, 0x0d, 0x32, 0x55,
        0xbf, 0xef, 0x95, 0x60, 0x18, 0x90, 0xaf, 0xd8, 0x07, 0x09
    };
    static const unsigned char abc_digest[20] = {
        0xa9, 0x99, 0x3e, 0x36, 0x47, 0x06, 0x81, 0x6a, 0xba, 0x3e,
        0x25, 0x71, 0x78, 0x50, 0xc2, 0x6c, 0x9c, 0xd0, 0xd8, 0x9d
    };
    static const unsigned char split_digest[20] = {
        0x84, 0x98, 0x3e, 0x44, 0x1c, 0x3b, 0xd2, 0x6e, 0xba, 0xae,
        0x4a, 0xa1, 0xf9, 0x51, 0x29, 0xe5, 0xe5, 0x46, 0x70, 0xf1
    };
    static const char split_message[] =
        "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    unsigned char actual[20];
    void *context;
    size_t offset;

    if (!digest_matches(NULL, 0, empty_digest))
        return 1;
    if (!digest_matches("abc", 3, abc_digest))
        return 2;

    context = x_sha1_init();
    if (context == NULL)
        return 3;
    for (offset = 0; offset < sizeof(split_message) - 1; ++offset) {
        if (!x_sha1_update(context, (void *) &split_message[offset], 1))
            return 4;
    }
    if (!x_sha1_final(context, actual) ||
        memcmp(actual, split_digest, sizeof(actual)) != 0) {
        return 5;
    }

    if (x_sha1_update(NULL, NULL, 0) != 0)
        return 6;
    if (x_sha1_final(NULL, actual) != 0)
        return 7;

    return 0;
}
