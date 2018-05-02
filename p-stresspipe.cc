#include "p-lib.hh"
#define ALLOC_SLOWDOWN 30

extern uint8_t end[];

uint8_t* heap_top;
uint8_t* stack_bottom;

void process_main(void) {
    sys_kdisplay(KDISPLAY_MEMVIEWER);

    // Print "MOO!" in the top-right hand corner of the console in green.
    memcpy((void*) console, "M\xAO\xAO\xA!\xA", 8);

    // Process 1 never allocates; it alternates between forking children
    // and yielding. Each forked child allocates.
    while (1) {
        if (rand(0, ALLOC_SLOWDOWN - 1) == 0) {
            if (sys_fork() == 0) {
                break;
            }
        } else {
            (void) sys_waitpid(0, nullptr, W_NOHANG);
            sys_yield();
        }
    }

    pid_t p = sys_getpid();
    srand(p);

    // The heap starts on the page right after the 'end' symbol,
    // whose address is the first address not allocated to process code
    // or data.
    heap_top = ROUNDUP((uint8_t*) end, PAGESIZE);

    // The bottom of the stack is the first address on the current
    // stack page (this process never needs more than one stack page).
    stack_bottom = ROUNDDOWN((uint8_t*) read_rsp() - 1, PAGESIZE);

    // Allocate heap pages until (1) hit the stack (out of address space)
    // or (2) allocation fails (out of physical memory).
    while (1) {
        int x = rand(0, 8 * ALLOC_SLOWDOWN - 1);
        if (x < 8 * p && heap_top != stack_bottom) {
            if (sys_page_alloc(heap_top) >= 0) {
                *heap_top = p;      // check we have write access to new page
                heap_top += PAGESIZE;
            }
        } else if (x == 8 * p) {
            int fds[2] = {-1, -1};
            if (sys_pipe(fds) >= 0) {
                sys_close(fds[0]);
                sys_close(fds[1]);
            }
            if (sys_fork() == 0) {
                p = sys_getpid();
            }
        } else if (x == 8 * p + 1
                   || (x < p && heap_top == stack_bottom)) {
            sys_exit(0);
        } else {
            (void) sys_waitpid(0, nullptr, W_NOHANG);
            sys_yield();
        }
    }
}