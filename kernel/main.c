/* kernel/main.c */
// ✅ NO #include <stdio.h>, <stdlib.h>, <stddef.h>
// Include ONLY our own types and headers

#include "types.h"
#include "video.h"
#include "filesystem.h"
#include "memory.h"

void kernel_main(void) {
    clear_screen();
    print_str("Initializing Kernel... OK\n");

    init_filesystem();

    print_str("Testing file I/O...\n");

    // Write file
    file_t f = fs_open("HELLO.TXT", 'w');
    if (f >= 0) {
        const char *msg = "Hello from my custom OS!";
        int written = fs_write(f, msg, 24);
        fs_close(f);

        if (written == 24) {
            print_str("File 'HELLO.TXT' written.\n");
        } else {
            print_str("Write error.\n");
        }
    } else {
        print_str("Failed to open file for write.\n");
    }

    // Read file
    f = fs_open("HELLO.TXT", 'r');
    if (f >= 0) {
        char buffer[100];
        int bytes_read = fs_read(f, buffer, sizeof(buffer) - 1);
        fs_close(f);

        if (bytes_read > 0) {
            buffer[bytes_read] = '\0';
            print_str("\nContent: \"");
            print_str(buffer);
            print_str("\"\n");
        } else {
            print_str("File empty.\n");
        }
    } else {
        print_str("Failed to open file for read.\n");
    }

    print_str("\nSystem Halted.\n");
    __asm__ __volatile__("hlt");
}
