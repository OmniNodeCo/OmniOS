// kernel/filesystem.c
#include "filesystem.h"
// ❌ DO NOT: #include <string.h>  ← THIS CAUSES THE ERROR!

// Use GCC builtins for memset/memcpy (available in freestanding mode)
#define memset __builtin_memset
#define memcpy __builtin_memcpy
#define strlen __builtin_strlen
#define strncmp __builtin_strncmp

// --- Data Structures ---
#define MAX_FILES 16
#define CLUSTER_SIZE 512

typedef struct {
    char name[11];  // "FILENAME.EXT" padded to 11 bytes
    uint8_t data[CLUSTER_SIZE];
    uint16_t size;
    uint8_t is_used;
} file_entry_t;

static file_entry_t file_table[MAX_FILES];

// --- Function Implementations ---

void init_filesystem(void) {
    // Use __builtin_memset (no header needed)
    for (int i = 0; i < MAX_FILES; i++) {
        __builtin_memset(file_table[i].name, ' ', 11);
        file_table[i].is_used = 0;
        file_table[i].size = 0;
        __builtin_memset(file_table[i].data, 0, CLUSTER_SIZE);
    }
}

static file_entry_t* find_file(const char* name) {
    for (int i = 0; i < MAX_FILES; i++) {
        if (file_table[i].is_used && 
            __builtin_strncmp(file_table[i].name, name, 11) == 0) {
            return &file_table[i];
        }
    }
    return NULL;
}

file_t fs_open(const char *filename, char mode) {
    file_entry_t* file = find_file(filename);

    if (mode == 'w') {
        if (!file) {
            // Find free slot
            for (int i = 0; i < MAX_FILES; i++) {
                if (!file_table[i].is_used) {
                    file = &file_table[i];
                    break;
                }
            }
            if (!file) return -1; // No space

            // Initialize new file
            __builtin_strncpy((char*)file->name, filename, 11);
            file->is_used = 1;
            file->size = 0;
        }
    } else if (mode == 'r') {
        if (!file) return -1; // Not found
    } else {
        return -1;
    }

    return (file - file_table) + 1;
}

int fs_write(file_t handle, const void *buffer, int size) {
    if (handle <= 0 || handle > MAX_FILES) return -1;
    file_entry_t *file = &file_table[handle - 1];
    if (!file->is_used) return -1;

    const char *src = (const char *)buffer;
    int written = 0;
    for (int i = 0; i < size; i++) {
        if (file->size < CLUSTER_SIZE) {
            file->data[file->size++] = src[i];
            written++;
        }
    }
    return written;
}

int fs_read(file_t handle, void *buffer, int max_size) {
    if (handle <= 0 || handle > MAX_FILES) return -1;
    file_entry_t *file = &file_table[handle - 1];
    if (!file->is_used || file->size == 0) return 0;

    char *dst = (char *)buffer;
    int read_bytes = 0;
    for (int i = 0; i < file->size && read_bytes < max_size; i++) {
        dst[read_bytes++] = file->data[i];
    }
    return read_bytes;
}

void fs_close(file_t handle) {
    // No-op in this simple model
}
