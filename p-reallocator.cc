#include "p-lib.hh"
#define ALLOC_SLOWDOWN 10

extern uint8_t end[];

uint8_t* heap_top;
uint8_t* stack_bottom;

void process_main(void) {
    // Check stack alignment.
    // assert(!(read_rbp() & 0xF));

    // Fork three new copies.
    if (sys_fork() > 0) {
        // Test trying to allocate console address.
        sys_map_console((void*) 0x1000);
        sys_page_alloc(console);

        // Print "MOO!" in the top-right hand corner of the console in green.
        memcpy((void*) 0x1000, "M\xAO\xAO\xA!\xA", 8);
    }
    (void) sys_fork();

    pid_t p = sys_getpid();
    srand(p);

    // The heap starts on the page right after the 'end' symbol,
    // whose address is the first address not allocated to process code
    // or data.
    heap_top = ROUNDUP((uint8_t*) end, PAGESIZE);

    // The bottom of the stack is the first address on the current
    // stack page (this process never needs more than one stack page).
    stack_bottom = ROUNDDOWN((uint8_t*) read_rsp() - 1, PAGESIZE);

    while (1) {
        if (rand(0, ALLOC_SLOWDOWN - 1) < p) {
            if (heap_top == stack_bottom || sys_page_alloc(heap_top) < 0) {
                break;
            }
            *heap_top = p;      /* check we have write access to new page */

            // If p % 2 == 0, we try to allocate the same virtual address!
            if (p % 2 == 0) {
                heap_top += PAGESIZE;
            }
        }
        sys_yield();
        if (rand() < RAND_MAX / 32) {
            sys_pause();
        }
    }

    // After running out of memory, do nothing forever
    while (1) {
        sys_yield();
    }
}
