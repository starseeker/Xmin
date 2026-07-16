#ifndef XMIN_LAUNCHER_H
#define XMIN_LAUNCHER_H

#include <stddef.h>

#define XMIN_COOKIE_SIZE 16

int xmin_random_bytes(unsigned char *buffer, size_t size);
int xmin_write_authority(const char *path,
                         const unsigned char *cookie,
                         size_t cookie_size);

#endif
