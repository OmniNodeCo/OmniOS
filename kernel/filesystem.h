// kernel/filesystem.h
#ifndef FILESYSTEM_H
#define FILESYSTEM_H

#include <stdint.h>

// File handle type (simple integer index into open file table)
typedef int file_t;

// Initialize the virtual filesystem (called at boot)
void init_filesystem(void);

// Open a file: mode = 'r' (read) or 'w' (write)
// Returns: file handle (>= 0) or -1 on error
file_t fs_open(const char *filename, char mode);

// Write up to `size` bytes from `buffer` into file
// Returns: number of bytes written
int fs_write(file_t handle, const void *buffer, int size);

// Read up to `max_size` bytes from file into `buffer`
// Returns: number of bytes read
int fs_read(file_t handle, void *buffer, int max_size);

// Close a file
void fs_close(file_t handle);

#endif // FILESYSTEM_H
