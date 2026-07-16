#include <stdlib.h>
#include <string.h>

#include "libxfontint.h"
#include "builtin.h"

typedef struct {
    int offset;
    BuiltinFilePtr file;
} XminBuiltinIORec, *XminBuiltinIOPtr;

static int
xmin_builtin_fill(BufFilePtr buffer)
{
    XminBuiltinIOPtr io = (XminBuiltinIOPtr) buffer->private;
    int left = io->file->len - io->offset;
    int length;

    if (left <= 0) {
        buffer->left = 0;
        return BUFFILEEOF;
    }

    length = left < BUFFILESIZE ? left : BUFFILESIZE;
    memcpy(buffer->buffer, io->file->bits + io->offset, (size_t) length);
    io->offset += length;
    buffer->left = length - 1;
    buffer->bufp = buffer->buffer + 1;
    return buffer->buffer[0];
}

static int
xmin_builtin_skip(BufFilePtr buffer, int count)
{
    XminBuiltinIOPtr io = (XminBuiltinIOPtr) buffer->private;
    int current = (int) (buffer->bufp - buffer->buffer);
    int buffered_end = current + buffer->left;

    if (current + count <= buffered_end) {
        buffer->bufp += count;
        buffer->left -= count;
    }
    else {
        io->offset += count - (buffered_end - current);
        if (io->offset > io->file->len)
            io->offset = io->file->len;
        if (io->offset < 0)
            io->offset = 0;
        buffer->left = 0;
    }
    return count;
}

static int
xmin_builtin_close(BufFilePtr buffer, int unused)
{
    (void) unused;
    free(buffer->private);
    return 1;
}

FontFilePtr
BuiltinFileOpen(const char *name)
{
    XminBuiltinIOPtr io;
    BufFilePtr buffer;
    int index;

    if (*name == '/')
        name++;
    for (index = 0; index < builtin_files_count; index++) {
        if (strcmp(name, builtin_files[index].name) == 0)
            break;
    }
    if (index == builtin_files_count)
        return NULL;

    io = malloc(sizeof *io);
    if (!io)
        return NULL;
    io->offset = 0;
    io->file = (BuiltinFilePtr) &builtin_files[index];

    buffer = BufFileCreate((char *) io, xmin_builtin_fill, NULL,
                           xmin_builtin_skip, xmin_builtin_close);
    if (!buffer) {
        free(io);
        return NULL;
    }
    return (FontFilePtr) buffer;
}

int
BuiltinFileClose(FontFilePtr file, int unused)
{
    (void) unused;
    return BufFileClose((BufFilePtr) file, TRUE);
}
