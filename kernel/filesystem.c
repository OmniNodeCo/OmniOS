// kernel/filesystem.c
#include <stdint.h>
#include <string.h>

#define DISK_SIZE 1024 * 1024 // 1MB Virtual Disk
#define CLUSTER_SIZE 512
#define ROOT_DIR_SIZE 32
#define MAX_FILES 16

// Simple File Control Block
typedef struct {
    char name[11]; // 8.3 Filename format (FILENAME EXT)
    uint8_t data[CLUSTER_SIZE];
    uint16_t cluster_start;
    uint16_t size;
    uint8_t is_used;
} file_t;

// The "Disk" Image in Memory
static uint8_t disk_image[DISK_SIZE];
static file_t open_files[MAX_FILES];

// Initialize the virtual disk with empty structures
void init_filesystem() {
    memset(disk_image, 0, DISK_SIZE);
    for(int i = 0; i < MAX_FILES; i++) {
        open_files[i].is_used = 0;
        memset(open_files[i].name, ' ', 11);
    }
}

// Find an empty slot in our "open file table"
int fs_open(char *filename, char mode) {
    int slot = -1;
    for(int i = 0; i < MAX_FILES; i++) {
        if(!open_files[i].is_used) {
            slot = i;
            break;
        }
    }
    if(slot == -1) return -1; // No slots left

    // Logic to find file on "disk" or create new one
    // For simplicity, we are simulating a fresh disk every boot
    // In a real implementation, you would scan the Root Directory sector here
    
    strncpy(open_files[slot].name, filename, 11);
    open_files[slot].is_used = 1;
    open_files[slot].size = 0;
    
    // If reading, we'd load from disk_image, but here we just clear it
    if(mode == 'w') {
        memset(open_files[slot].data, 0, CLUSTER_SIZE);
    }

    return slot;
}

int fs_write(int handle, void *buffer, int size) {
    if(handle < 0 || handle >= MAX_FILES) return -1;
    if(!open_files[handle].is_used) return -1;

    int bytes_written = 0;
    char *src = (char*)buffer;
    
    // Simple write to the cluster
    for(int i = 0; i < size; i++) {
        if(open_files[handle].size < CLUSTER_SIZE) {
            open_files[handle].data[open_files[handle].size] = src[i];
            open_files[handle].size++;
            bytes_written++;
        }
    }
    return bytes_written;
}

int fs_read(int handle, void *buffer, int max_size) {
    if(handle < 0 || handle >= MAX_FILES) return -1;
    if(!open_files[handle].is_used) return -1;

    int bytes_read = 0;
    char *dst = (char*)buffer;
    
    // Copy from cluster to buffer
    for(int i = 0; i < open_files[handle].size && i < max_size; i++) {
        dst[i] = open_files[handle].data[i];
        bytes_read++;
    }
    return bytes_read;
}

void fs_close(int handle) {
    if(handle >= 0 && handle < MAX_FILES) {
        open_files[handle].is_used = 0;
    }
}
