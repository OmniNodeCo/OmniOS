// kernel/main.c
#include "video.h"
#include "filesystem.h"

void kernel_main(void) {
    clear_screen();
    print_str("Initializing Kernel... OK\n");

    // Initialize the virtual "Hard Drive"
    init_filesystem();

    print_str("Loading files...\n");
    
    // Create a test file
    file_t *f = fs_open("HELLO.TXT", 'w');
    if(f) {
        fs_write(f, "Hello from my custom OS!", 24);
        fs_close(f);
        print_str("File 'HELLO.TXT' created.\n");
    }

    // Read the file back
    f = fs_open("HELLO.TXT", 'r');
    if(f) {
        char buffer[100];
        int bytes_read = fs_read(f, buffer, 99);
        buffer[bytes_read] = '\0'; // Null terminate
        print_str("Content of HELLO.TXT: ");
        print_str(buffer);
        fs_close(f);
    }
    
    print_str("\nSystem Halted.");
    __asm__ __volatile__("hlt");
}
