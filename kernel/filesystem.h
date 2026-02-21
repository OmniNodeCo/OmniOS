/* kernel/filesystem.h */
#ifndef FILESYSTEM_H
#define FILESYSTEM_H

#include <stddef.h>  // Provides NULL, size_t
#include <stdint.h>  // Provides uint8_t, int32_t, etc.

typedef int file_t;   // File handle type

void init_filesystem(void);
file_t fs_open(const char *filename, char mode);
int fs_write(file_t handle, const void *buffer, int size);
int fs_read(file_t handle, void *buffer, int max_size);
void fs_close(file_t handle);

#endif // FILESYSTEM_H
