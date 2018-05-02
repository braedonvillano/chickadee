#include "p-lib.hh"

#define P2_PAGES 180

extern uint8_t end[];

void process_main() {
    sys_kdisplay(KDISPLAY_MEMVIEWER);

    // Print "MOO!" in the top-right hand corner of the console in green.
    memcpy((void*) console, "M\xAO\xAO\xA!\xA", 8);

    if (sys_getpid() == 2) {
        if (sys_fork() > 0) {
            uint8_t* heap_top = ROUNDUP((uint8_t*) end, PAGESIZE);
            for (int i = 0; i < P2_PAGES; i++) {
                assert(sys_page_alloc(heap_top) >= 0);
                heap_top += PAGESIZE;
            }
            while(1) {
                sys_yield();
            }
        } else {
            sys_msleep(500);
            while (sys_fork()) {
                while(sys_waitpid(0, nullptr, W_NOHANG) > 0) ;
            }
        }
    }

    int p = sys_getpid();
    char buf[80];
    for (int i = 0; i < 20; i++) {
        snprintf(buf, sizeof(buf), "proc_%X_file_%d", p, i);
        int f = sys_open(buf, OF_READ | OF_WRITE | OF_TRUNC | OF_CREATE);
        if (f > 0) {
            sys_write(f, buf, strlen(buf));
        }
        sys_close(f);
    }
    sys_exit(0);
}