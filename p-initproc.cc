#include "p-lib.hh"
#define ALLOC_SLOWDOWN 10

void process_main(void) {
    sys_map_console(console);
    for (int i = 0; i < CONSOLE_ROWS * CONSOLE_COLUMNS; ++i) {
      console[i] = '*' | 0x3000;
    }

    while (1) {
        sys_yield();
        // sys_waitpid(0);
    }
}