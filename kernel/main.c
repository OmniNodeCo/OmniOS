// kernel/main.c
#include "video.h"
#include "filesystem.h"
#include "memory.h"  // safe to include (even if empty for now)

void kernel_main(void) {
    clear_screen();
    print_str("Initializing Kernel... OK\n");

    // Initialize filesystem (simulated)
    init_filesystem();

    print_str("Testing file I/O...\n");

    // 1. Create and write to a file
    file_t f = fs_open("HELLO.TXT", 'w');
    if (f >= 0) {
        const char *msg = "Hello from my custom OS!";
        int written = fs_write(f, msg, 24);
        fs_close(f);
        
        if (written == 24) {
            print_str("File 'HELLO.TXT' written successfully.\n");
        } else {
            print_str("Error writing file.\n");
        }
    } else {
        print_str("Failed to create file.\n");
    }

    // 2. Read the file back
    f = fs_open("HELLO.TXT", 'r');
    if (f >= 0) {
        char buffer[100];
        int bytes_read = fs_read(f, buffer, sizeof(buffer) - 1);
        fs_close(f);

        if (bytes_read > 0) {
            buffer[bytes_read] = '\0'; // Null-terminate
            print_str("\nContent of HELLO.TXT: \"");
            print_str(buffer);
            print_str("\"\n");
        } else {
            print_str("File is empty.\n");
        }
    } else {
        print_str("Failed to open file for reading.\n");
    }

    print_str("\nSystem Halted.\n");
    __asm__ __volatile__("hlt");
}
