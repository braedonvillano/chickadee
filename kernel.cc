#include "kernel.hh"
#include "k-apic.hh"
#include "k-chkfs.hh"
#include "k-devices.hh"
#include "k-vmiter.hh"
#define INIT_PID 1

// kernel.cc
//
//    This is the kernel.

volatile unsigned long ticks;   // # timer interrupts so far on CPU 0
int kdisplay;                   // type of display

spinlock process_hierarchy_lock;

static void kdisplay_ontick();
static void process_setup(pid_t pid, const char* program_name);
static void build_init_proc();
static void canary_check(proc* p = nullptr);
static void exit(proc* p, int flag);


// kernel_start(command)
//    Initialize the hardware and processes and start running. The `command`
//    string is an optional string passed from the boot loader.

void kernel_start(const char* command) {
    assert(read_rbp() % 16 == 0);
    init_hardware();
    console_clear();
    kdisplay = KDISPLAY_MEMVIEWER;

    // Set up process descriptors
    for (pid_t i = 0; i < NPROC; i++) {
        ptable[i] = nullptr;
    }

    // make the initial process
    auto irqs = ptable_lock.lock();
    build_init_proc();
    process_setup(2, "allocexit");
    ptable_lock.unlock(irqs);

    // switch to the first process
    cpus[0].schedule(nullptr);
}

// this function builds the init process for reparenting
void build_init_proc() {
    assert(!ptable[INIT_PID]);
    proc* p = ptable[INIT_PID] = kalloc_proc();
    x86_64_pagetable* npt = kalloc_pagetable();
    assert(p && npt);
    p->init_user(INIT_PID, npt);

    p->ppid_ = INIT_PID;

    int r = p->load("initproc");
    assert(r >= 0);
    p->regs_->reg_rsp = MEMSIZE_VIRTUAL;
    x86_64_page* stkpg = kallocpage();
    assert(stkpg);
    r = vmiter(p, MEMSIZE_VIRTUAL - PAGESIZE).map(ka2pa(stkpg));
    assert(r >= 0);
    r = vmiter(p, ktext2pa(console)).map(ktext2pa(console), PTE_P | PTE_W | PTE_U);
    assert(r >= 0);

    int cpu = INIT_PID % ncpu;
    cpus[cpu].runq_lock_.lock_noirq();
    cpus[cpu].enqueue(p);
    cpus[cpu].runq_lock_.unlock_noirq();
}


// process_setup(pid, name)
//    Load application program `name` as process number `pid`.
//    This loads the application's code and data into memory, sets its
//    %rip and %rsp, gives it a stack page, and marks it as runnable.

void process_setup(pid_t pid, const char* name) {
#ifdef CHICKADEE_FIRST_PROCESS
    name = CHICKADEE_FIRST_PROCESS;
#endif

    assert(!ptable[pid]);
    proc* p = ptable[pid] = kalloc_proc();
    x86_64_pagetable* npt = kalloc_pagetable();
    assert(p && npt);
    p->init_user(pid, npt);

    process_hierarchy_lock.lock_noirq();
    p->ppid_ = INIT_PID;
    p->child_links_.reset();
    ptable[INIT_PID]->child_list.push_front(p);
    p->child_list.reset();
    process_hierarchy_lock.unlock_noirq();

    int r = p->load(name);
    assert(r >= 0);
    p->regs_->reg_rsp = MEMSIZE_VIRTUAL;
    x86_64_page* stkpg = kallocpage();
    assert(stkpg);
    r = vmiter(p, MEMSIZE_VIRTUAL - PAGESIZE).map(ka2pa(stkpg));
    assert(r >= 0);
    r = vmiter(p, ktext2pa(console)).map(ktext2pa(console), PTE_P | PTE_W | PTE_U);
    assert(r >= 0);

    int cpu = pid % ncpu;
    cpus[cpu].runq_lock_.lock_noirq();
    cpus[cpu].enqueue(p);
    cpus[cpu].runq_lock_.unlock_noirq();
}


// proc::exception(reg)
//    Exception handler (for interrupts, traps, and faults).
//
//    The register values from exception time are stored in `reg`.
//    The processor responds to an exception by saving application state on
//    the current CPU stack, then jumping to kernel assembly code (in
//    k-exception.S). That code transfers the state to the current kernel
//    task's stack, then calls proc::exception().

void proc::exception(regstate* regs) {
    // It can be useful to log events using `log_printf`.
    // Events logged this way are stored in the host's `log.txt` file.
    /*log_printf("proc %d: exception %d\n", this->pid_, regs->reg_intno);*/
    assert(read_rbp() % 16 == 0);

    // Show the current cursor location.
    console_show_cursor(cursorpos);


    // Actually handle the exception.
    switch (regs->reg_intno) {

    case INT_IRQ + IRQ_TIMER: {
        cpustate* cpu = this_cpu();
        if (cpu->index_ == 0) {
            ++ticks;
            kdisplay_ontick();
        }
        lapicstate::get().ack();
        // this->regs_ = regs;
        // this->yield_noreturn();
        this->yield();
        break;                  /* will not be reached */
    }

    case INT_PAGEFAULT: {
       // Analyze faulting address and access type.
        uintptr_t addr = rcr2();
        const char* operation = regs->reg_err & PFERR_WRITE
                ? "write" : "read";
        const char* problem = regs->reg_err & PFERR_PRESENT
                ? "protection problem" : "missing page";

        if (!(regs->reg_err & PFERR_USER)) {
            panic("Kernel page fault for %p (%s %s, rip=%p)!\n",
                  addr, operation, problem, regs->reg_rip);
        }
        error_printf(CPOS(24, 0), 0x0C00,
                     "Process %d page fault for %p (%s %s, rip=%p)!\n",
                     pid_, addr, operation, problem, regs->reg_rip);
        this->state_ = proc::broken;
        this->yield();
        break;
    }

    case INT_IRQ + IRQ_KEYBOARD:
        keyboardstate::get().handle_interrupt();
        break;

    default:
        if (sata_disk && regs->reg_intno == INT_IRQ + sata_disk->irq_) {
            sata_disk->handle_interrupt();
        } else {
            panic("Unexpected exception %d!\n", regs->reg_intno);
        }
        break;                  /* will not be reached */
}

    // Return to the current process.
    // If exception arrived in user mode, the process must be runnable.
    assert((regs->reg_cs & 3) == 0 || this->state_ == proc::runnable);
}

// syscall helper functions below
//    Fork helper function to make process children
//    Exit helper function that essentially clears processes
//    Canary check function ensures structs arent corrupted

void a_holy_bond(proc* p) {
    log_printf("children:");
    proc* p_ = p->child_list.front();
    while (p_) {
        log_printf(" %d", p_->pid_);
    }
    p_ = p->child_list.next(p_);
    log_printf(" \n");
}

void reparent_child(proc* p, proc* init_p) {
    auto irqsp = process_hierarchy_lock.lock();
    p->child_links_.erase();
    proc* p_ = p->child_list.front();
    while (p_) {
        auto next = p->child_list.next(p_);
        p_->ppid_ = INIT_PID;
        p_->child_links_.erase();
        init_p->child_list.push_front(p_);
        p_ = next;
    } 
    process_hierarchy_lock.unlock(irqsp);

}

wpret wait_pid(pid_t pid, proc* parent, int opts = 0) {
    assert(parent);
    wpret wpr;
    // initialize return before sending to parent
    wpr.stat = 0;
    bool wait = false;
    while (1) {
        // log_printf("waitpid -> parent: %d, child: %d\n", parent->pid_, pid);
        auto irqsp = process_hierarchy_lock.lock();
        // a_holy_bond(parent);
        proc* p = parent->child_list.front();
        if (!p && !pid) {
            process_hierarchy_lock.unlock(irqsp);
            wpr.pid_c = E_CHILD;
            return wpr;
        }
        // cycle through the list to find a child
        while (p) {
            if (!pid) {
                if (p->state_ == proc::wexited) {
                    auto irqs = ptable_lock.lock();
                    ptable[pid] = nullptr;
                    ptable_lock.unlock(irqs);
                    process_hierarchy_lock.unlock(irqsp);
                    wpr.stat = p->exit_status_;
                    wpr.pid_c = pid;
                    p->state_ = proc::exited;
                    ptable_lock.unlock(irqs);
                    kfree(p->pagetable_); kfree(p);
                    return wpr;
                }
                wait = true;
            } else if (p->pid_ == pid) {
                if (p->state_ == proc::wexited) {
                    auto irqs = ptable_lock.lock();
                    ptable[pid] = nullptr;
                    ptable_lock.unlock(irqs);
                    process_hierarchy_lock.unlock(irqsp);
                    wpr.stat = p->exit_status_;
                    wpr.pid_c = pid;
                    p->state_ = proc::exited;
                    ptable_lock.unlock(irqs);
                    kfree(p->pagetable_); kfree(p);
                    return wpr;
                }
                wait = true;
                break;
            }
            p = parent->child_list.next(p);
        }
        process_hierarchy_lock.unlock(irqsp);
        if (!wait) { wpr.pid_c = E_CHILD; break; }
        if (opts) { wpr.pid_c = E_AGAIN; break; }
        parent->yield();
    }
    return wpr;
}

int fork(proc* parent, regstate* regs) {
    auto irqs = ptable_lock.lock();
    proc* p = nullptr;
    pid_t pid = 0;
    // go through ptable to find open proc
    for (pid_t i = 1; i < NPROC; i++) {
        if (!ptable[i]) {
            pid = i;
            break;
        }
    }
    p = ptable[pid] = reinterpret_cast<proc*>(kallocpage());
    x86_64_pagetable* npt = kalloc_pagetable();
    // if there were no empty processes
    if (pid == -1 || !p || !npt) {
        ptable[pid] = nullptr;
        ptable_lock.unlock(irqs);
        kfree(p); kfree(npt);
        return -1;
    }    
    p->init_user(pid, npt);
    ptable_lock.unlock(irqs);
    // loop through virtual memory and copy to child
    for (vmiter it(parent); it.low(); it.next()) {
        if (!it.user() || !it.present()) continue;
        if (it.pa() == ktext2pa(console)) {
            if (vmiter(p, it.va()).map(ktext2pa(console)) < 0) {
                exit(p, 0); kfree(p); kfree(npt);
                return -1;
            }
            continue;
        }
        x86_64_page* pg = kallocpage();
        if (!pg) {
            exit(p, 0); kfree(p); kfree(npt); kfree(pg); 
            return -1;
        }
        memcpy(pg, (void*) it.ka(), PAGESIZE);
        if (vmiter(p, it.va()).map(ka2pa(pg), it.perm()) < 0) {
            exit(p, 0); kfree(p); kfree(npt); kfree(pg); 
            return -1;
        }
    }
    memcpy(p->regs_, regs, sizeof(regstate));
    p->regs_->reg_rax = 0;
    // reparent the new process
    auto irqsp = process_hierarchy_lock.lock();
    p->ppid_ = parent->pid_;
    p->child_links_.reset();
    parent->child_list.push_front(p);
    p->child_list.reset();
    process_hierarchy_lock.unlock(irqsp);
    // put the proc on the runq
    int cpu = pid % ncpu;
    cpus[cpu].runq_lock_.lock_noirq();
    cpus[cpu].enqueue(p);
    cpus[cpu].runq_lock_.unlock_noirq();
    canary_check(parent);

    return pid;
}

void exit(proc* p, int flag) {
    auto irqs = ptable_lock.lock();
    pid_t pid = p->pid_;
    ptable[pid] = nullptr;
    p->state_ = proc::exited;
    proc* init_p = ptable[INIT_PID];
    assert(!ptable[pid]);
    ptable_lock.unlock(irqs);

        
    // reparent the children of the exited process
    // free the process's memory 
    for (vmiter it(p); it.low(); it.next()) {
        if (it.user() && it.present() && it.pa() != ktext2pa(console)) { 
            kfree((void*) it.ka());
        }
    }
    // free the process's page tabeles
    for (ptiter it(p); it.low(); it.next()) {
        kfree((void*) pa2ka(it.ptp_pa()));
    }
    
    if (flag) {
        reparent_child(p, init_p);
    }
}

void canary_check(proc* p) {
    if (p) {
        assert(p->canary_ == CANARY);
    }
    for (int i = 0; i < ncpu; ++i) {
        assert(cpus[i].canary_ == CANARY);
    }
}


// proc::syscall(regs)
//    System call handler.
//
//    The register values from system call time are stored in `regs`.
//    The return value from `proc::syscall()` is returned to the user
//    process in `%rax`.

uintptr_t proc::syscall(regstate* regs) {
    assert(read_rbp() % 16 == 0);
    switch (regs->reg_rax) {

    case SYSCALL_KDISPLAY:
        if (kdisplay != (int) regs->reg_rdi) {
            console_clear();
        }
        kdisplay = regs->reg_rdi;
        canary_check(this);
        return 0;

    case SYSCALL_PANIC:
        panic(NULL);
        break;                  // will not be reached

    case SYSCALL_GETPID:
        return pid_;

    case SYSCALL_YIELD:
        this->regs_ = regs;
        regs->reg_rax = 0;
        this->yield_noreturn(); // NB does not return
        break;

    case SYSCALL_PAGE_ALLOC: {
        uintptr_t addr = regs->reg_rdi;
        if (addr >= VA_LOWEND || addr & 0xFFF) {
            return -1;
        }
        x86_64_page* pg = kallocpage();
        if (!pg || vmiter(this, addr).map(ka2pa(pg)) < 0) {
            return -1;
        }
        canary_check(this);
        return 0;
    }

    case SYSCALL_WAITPID: {
        pid_t pid = regs->reg_rdi;
        int opts = regs->reg_rsi;
        wpret wpr = wait_pid(pid, this, opts);
        // log_printf("made it here: %d and %d \n", wpr.pid_c, wpr.stat);
        asm("" : : "c" (wpr.stat));
        return wpr.pid_c;
    }

    case SYSCALL_PAUSE: {
        sti();
        for (uintptr_t delay = 0; delay < 1000000; ++delay) {
            pause();
        }
        cli();
        return 0;
    }

    case SYSCALL_EXIT: {
        exit(this, 1);
        this->yield_noreturn();
    }

    case SYSCALL_MSLEEP: {
        unsigned long want_ticks = ticks + (regs->reg_rdi + 9) / 10;
        sti();
        while (long(want_ticks - ticks) > 0) {
            this->yield();
        }
        return 0;
    }

    case SYSCALL_GETPPID: {
        return ppid_;
    }

    // this is to map a the console to a process (BV)
    case SYSCALL_MAP_CONSOLE: {
        uintptr_t addr = regs->reg_rdi;
        if (addr >= VA_LOWMAX || addr & PAGEOFFMASK) {
            return -1;
        }
        int r = vmiter(this, addr).map(ktext2pa(console), PTE_P | PTE_W | PTE_U);
        if (r < 0) {
            return -1;
        }
        canary_check(this);
        return 0;
    }

    case SYSCALL_FORK: {
        return fork(this, regs);
    }

    case SYSCALL_READ: {
        int fd = regs->reg_rdi;
        uintptr_t addr = regs->reg_rsi;
        size_t sz = regs->reg_rdx;

        auto& kbd = keyboardstate::get();
        auto irqs = kbd.lock_.lock();

        // mark that we are now reading from the keyboard
        // (so `q` should not power off)
        if (kbd.state_ == kbd.boot) {
            kbd.state_ = kbd.input;
        }

        // block until a line is available
        waiter(this).block_until(kbd.wq_, [&] () {
                return sz == 0 || kbd.eol_ != 0;
            }, kbd.lock_, irqs);

        // read that line or lines
        size_t n = 0;
        while (kbd.eol_ != 0 && n < sz) {
            if (kbd.buf_[kbd.pos_] == 0x04) {
                // Ctrl-D means EOF
                if (n == 0) {
                    kbd.consume(1);
                }
                break;
            } else {
                *reinterpret_cast<char*>(addr) = kbd.buf_[kbd.pos_];
                ++addr;
                ++n;
                kbd.consume(1);
            }
        }

        kbd.lock_.unlock(irqs);
        return n;
    }

    case SYSCALL_WRITE: {
        int fd = regs->reg_rdi;
        uintptr_t addr = regs->reg_rsi;
        size_t sz = regs->reg_rdx;

        auto& csl = consolestate::get();
        auto irqs = csl.lock_.lock();

        size_t n = 0;
        while (n < sz) {
            int ch = *reinterpret_cast<const char*>(addr);
            ++addr;
            ++n;
            console_printf(0x0F00, "%c", ch);
        }

        csl.lock_.unlock(irqs);
        return n;
    }

    case SYSCALL_READDISKFILE: {
        const char* filename = reinterpret_cast<const char*>(regs->reg_rdi);
        unsigned char* buf = reinterpret_cast<unsigned char*>(regs->reg_rsi);
        uintptr_t sz = regs->reg_rdx;
        uintptr_t off = regs->reg_r10;

        if (!sata_disk) {
            return E_IO;
        }

        return chickadeefs_read_file_data(filename, buf, sz, off);
    }

    case SYSCALL_SYNC:
        return bufcache::get().sync(regs->reg_rdi != 0);

    default:
        // no such system call
        log_printf("%d: no such system call %u\n", pid_, regs->reg_rax);
        return E_NOSYS;

    }
}


// memshow()
//    Draw a picture of memory (physical and virtual) on the CGA console.
//    Switches to a new process's virtual memory map every 0.25 sec.
//    Uses `console_memviewer()`, a function defined in `k-memviewer.cc`.

static void memshow() {
    static unsigned last_ticks = 0;
    static int showing = 1;

    // switch to a new process every 0.25 sec
    if (last_ticks == 0 || ticks - last_ticks >= HZ / 2) {
        last_ticks = ticks;
        showing = (showing + 1) % NPROC;
    }

    auto irqs = ptable_lock.lock();

    int search = 0;
    while ((!ptable[showing]
            || !ptable[showing]->pagetable_
            || ptable[showing]->pagetable_ == early_pagetable)
           && search < NPROC) {
        showing = (showing + 1) % NPROC;
        ++search;
    }

    extern void console_memviewer(proc* vmp);
    console_memviewer(ptable[showing]);

    ptable_lock.unlock(irqs);
}


// kdisplay_ontick()
//    Shows the currently-configured kdisplay. Called once every tick
//    (every 0.01 sec) by CPU 0.

void kdisplay_ontick() {
    if (kdisplay == KDISPLAY_MEMVIEWER) {
        memshow();
    }
}
