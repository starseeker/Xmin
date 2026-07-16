/*
 * Minimal SHA-1 implementation for Xorg's x_sha1_* compatibility API.
 *
 * SHA-1 is retained solely because the RENDER glyph cache uses its digest as
 * an internal content key.  It is not used for authentication or any other
 * security decision.
 */

#include "xsha1.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint32_t state[5];
    uint64_t bit_count;
    unsigned char block[64];
    size_t block_used;
} XminSha1Context;

static uint32_t
rotate_left(uint32_t value, unsigned int count)
{
    return (value << count) | (value >> (32U - count));
}

static uint32_t
load_be32(const unsigned char *input)
{
    return ((uint32_t) input[0] << 24) |
        ((uint32_t) input[1] << 16) |
        ((uint32_t) input[2] << 8) |
        (uint32_t) input[3];
}

static void
store_be32(unsigned char *output, uint32_t value)
{
    output[0] = (unsigned char) (value >> 24);
    output[1] = (unsigned char) (value >> 16);
    output[2] = (unsigned char) (value >> 8);
    output[3] = (unsigned char) value;
}

static void
transform(XminSha1Context *context, const unsigned char block[64])
{
    uint32_t words[80];
    uint32_t a;
    uint32_t b;
    uint32_t c;
    uint32_t d;
    uint32_t e;
    unsigned int index;

    for (index = 0; index < 16; ++index)
        words[index] = load_be32(block + (index * 4));
    for (; index < 80; ++index) {
        words[index] = rotate_left(words[index - 3] ^ words[index - 8] ^
                                   words[index - 14] ^ words[index - 16], 1);
    }

    a = context->state[0];
    b = context->state[1];
    c = context->state[2];
    d = context->state[3];
    e = context->state[4];

    for (index = 0; index < 80; ++index) {
        uint32_t function;
        uint32_t constant;
        uint32_t temporary;

        if (index < 20) {
            function = (b & c) | ((~b) & d);
            constant = UINT32_C(0x5a827999);
        }
        else if (index < 40) {
            function = b ^ c ^ d;
            constant = UINT32_C(0x6ed9eba1);
        }
        else if (index < 60) {
            function = (b & c) | (b & d) | (c & d);
            constant = UINT32_C(0x8f1bbcdc);
        }
        else {
            function = b ^ c ^ d;
            constant = UINT32_C(0xca62c1d6);
        }

        temporary = rotate_left(a, 5) + function + e + constant + words[index];
        e = d;
        d = c;
        c = rotate_left(b, 30);
        b = a;
        a = temporary;
    }

    context->state[0] += a;
    context->state[1] += b;
    context->state[2] += c;
    context->state[3] += d;
    context->state[4] += e;
}

void *
x_sha1_init(void)
{
    XminSha1Context *context = calloc(1, sizeof(*context));

    if (context == NULL)
        return NULL;

    context->state[0] = UINT32_C(0x67452301);
    context->state[1] = UINT32_C(0xefcdab89);
    context->state[2] = UINT32_C(0x98badcfe);
    context->state[3] = UINT32_C(0x10325476);
    context->state[4] = UINT32_C(0xc3d2e1f0);
    return context;
}

int
x_sha1_update(void *opaque_context, void *data, int size)
{
    XminSha1Context *context = opaque_context;
    const unsigned char *input = data;
    size_t remaining;

    if (context == NULL || size < 0 || (data == NULL && size != 0))
        return 0;

    remaining = (size_t) size;
    context->bit_count += (uint64_t) remaining * UINT64_C(8);

    while (remaining != 0) {
        size_t available = sizeof(context->block) - context->block_used;
        size_t amount = remaining < available ? remaining : available;

        memcpy(context->block + context->block_used, input, amount);
        context->block_used += amount;
        input += amount;
        remaining -= amount;

        if (context->block_used == sizeof(context->block)) {
            transform(context, context->block);
            context->block_used = 0;
        }
    }

    return 1;
}

int
x_sha1_final(void *opaque_context, unsigned char result[20])
{
    XminSha1Context *context = opaque_context;
    uint64_t bit_count;
    unsigned int index;

    if (context == NULL || result == NULL)
        return 0;

    bit_count = context->bit_count;
    context->block[context->block_used++] = 0x80;

    if (context->block_used > 56) {
        memset(context->block + context->block_used, 0,
               sizeof(context->block) - context->block_used);
        transform(context, context->block);
        context->block_used = 0;
    }

    memset(context->block + context->block_used, 0, 56 - context->block_used);
    for (index = 0; index < 8; ++index) {
        context->block[63 - index] = (unsigned char) (bit_count >> (index * 8));
    }
    transform(context, context->block);

    for (index = 0; index < 5; ++index)
        store_be32(result + (index * 4), context->state[index]);

    memset(context, 0, sizeof(*context));
    free(context);
    return 1;
}
