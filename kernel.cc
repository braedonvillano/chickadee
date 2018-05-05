#include "kernel.hh"
#include "k-apic.hh"
#include "k-chkfs.hh"
#include "k-devices.hh"
#include "k-vmiter.hh"
#include "k-vfs.hh"
#define INIT_PID     1
#define WHEEL_SZ     10
#define VALFD(x)     (x < NFDS && x >= 0) 
#define OFF2USR(x)   (uintptr_t) (MEMSIZE_VIRTUAL - PAGESIZE + x)

// kernel.cc
//
//    This is the kernel.

volatile unsigned long ticks;   // # timer interrupts so far on CPU 0
int kdisplay;                   // type of display

spinlock familial_lock;
spinlock interrupt_lock;
spinlock memfile_lock;
wait_queue waitq;
wait_queue sleepq_wheel[WHEEL_SZ];

static void kdisplay_ontick();
static void process_setup(pid_t pid, const char* program_name);
static void build_init_proc();
static void canary_check(proc* p = nullptr);
static void exit(proc* p, int flag, int exit_stat);
static void init_reaper();
static void reap_child(proc* p, wpret* wpr);
static void parenting(proc* p, proc* p_init);
static wpret wait_pid(pid_t pid, proc* parent, int opts = 0);


// kernel_start(command)
//    Initialize the hardware and processes and start running. The `command`
//    string is an optional string passed from the boot loader.

void kernel_start(const char* command) {
    assert(read_rbp() % 16 == 0);
    init_hardware();
    console_clear();
    kdisplay = KDISPLAY_MEMVIEWER;

    // Set up process descriptors and waitqueues
    for (pid_t i = 0; i < NPROC; i++) {
        ptable[i] = nullptr;
    }
    waitq.q_.reset();
    for (int j = 0; j < WHEEL_SZ; j++) {
        sleepq_wheel[0].q_.reset();
    }

    // make the initial process
    auto irqs = ptable_lock.lock();
    build_init_proc();
    process_setup(2, "allocexit");
    ptable_lock.unlock(irqs);

    // switch to the first process
    cpus[0].schedule(nullptr);
}

// init function responsible for reaping zombie children
void init_reaper(proc* p) {
    while (1) {
        wait_pid(0, p, W_NOHANG);
    }
}

// this is the kernel task (duhhh) initial process
void build_init_proc() {
    assert(!ptable[INIT_PID]);
    proc* p = ptable[INIT_PID] = kalloc_proc();
    assert(p);

    p->init_kernel(INIT_PID, init_reaper);

    int cpu = p->cpu_ = INIT_PID % ncpu;
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
    fdtable* fdt = kalloc_fdtable();
    file* fl = kalloc_file();
    assert(p && npt && fdt && fl);
    p->init_user(pid, npt, fdt);

    p->fdtable_->lock_.lock_noirq();
    fl->type_ = file::stream;
    fl->vnode_ = &vnode_ioe::v_ioe;
    p->fdtable_->table_[0] = fl;
    fl->adref();
    p->fdtable_->table_[1] = fl;
    fl->adref();
    p->fdtable_->table_[2] = fl;
    p->fdtable_->lock_.unlock_noirq();

    familial_lock.lock_noirq();
    p->ppid_ = INIT_PID;
    p->child_links_.reset();
    ptable[INIT_PID]->child_list.push_front(p);
    p->child_list.reset();
    familial_lock.unlock_noirq();

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
        this->regs_ = regs;
        this->yield_noreturn();
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


// below are the syscall main-wrapper functions
//    Fork helper function to make process children
//    Exit helper function that essentially clears processes
//    Canary check function ensures structs arent corrupted
//    Waitpid helper to reap a process after exiting

void wait_pid_cond(proc* parent, wpret* wpr, pid_t pid, int opts) {
    wpr->clear();
    auto irqsp = familial_lock.lock();
    proc* p = parent->child_list.front(); 
    if (!p) { familial_lock.unlock(irqsp); return; }
    while (p) {
        if (p->state_ == proc::wexited) {
            if (!pid || p->pid_ == pid) {
                wpr->p = p; wpr->exit = true; break;
            }
        } else if (!pid || p->pid_ == pid) {
            wpr->block = (bool) !opts; 
            if (opts) { wpr->pid_c = E_AGAIN; }
        }
        p = parent->child_list.next(p);
    }
    familial_lock.unlock(irqsp);
}
// these functions collab for waitpid, the condition (^) finds a child
wpret wait_pid(pid_t pid, proc* parent, int opts) {
    wpret wpr;
    waiter(parent).block_until(waitq, 
        [&] () { wait_pid_cond(parent, &wpr, pid, opts); return !wpr.block; });
    if (wpr.exit) { 
        auto irqs = familial_lock.lock();
        reap_child(wpr.p, &wpr); 
        familial_lock.unlock(irqs);
        kfree(wpr.p->pagetable_); kfree(wpr.p);        
    }
    return wpr;
}

int fork(proc* parent, regstate* regs) {
    auto irqs = ptable_lock.lock();
    proc* p = nullptr;
    pid_t pid = 0;
    for (pid_t i = 1; i < NPROC; i++) {
        if (!ptable[i]) {
            pid = i;
            break;
        } 
    }
    p = ptable[pid] = reinterpret_cast<proc*>(kallocpage());
    x86_64_pagetable* npt = kalloc_pagetable();
    fdtable* fdt = kalloc_fdtable();
    // free if no available proc or memory
    if (!pid || !p || !npt || !fdt) {
        ptable[pid] = nullptr;
        ptable_lock.unlock(irqs);
        kfree(p); kfree(npt); kfree(fdt);
        return -1;
    }    
    p->init_user(pid, npt, fdt);
    ptable_lock.unlock(irqs);
    // copy the fdtable structur to the child
    auto irqsf = p->fdtable_->lock_.lock();
    for (int j = 0; j < NFDS; j++) {
        file* ptr = parent->fdtable_->table_[j];
        if (ptr) {
            ptr->adref();
            p->fdtable_->table_[j] = ptr;
        }
    }
    p->fdtable_->lock_.unlock(irqsf);
    // loop through virtual memory and copy to child
    for (vmiter it(parent); it.low(); it.next()) {
        if (!it.user() || !it.present()) continue;
        if (it.pa() == ktext2pa(console)) {
            if (vmiter(p, it.va()).map(ktext2pa(console)) < 0) {
                exit(p, 0, -1); kfree(p); kfree(npt);
                return -1;
            }
            continue;
        }
        x86_64_page* pg = kallocpage();
        if (!pg) {
            exit(p, 0, -1); kfree(p); kfree(npt); kfree(pg); 
            return -1;
        }
        memcpy(pg, (void*) it.ka(), PAGESIZE);
        if (vmiter(p, it.va()).map(ka2pa(pg), it.perm()) < 0) {
            exit(p, 0, -1); kfree(p); kfree(npt); kfree(pg); 
            return -1;
        }
    }
    memcpy(p->regs_, regs, sizeof(regstate));
    p->regs_->reg_rax = 0;
    // reparent the new process
    auto irqsp = familial_lock.lock();
    p->ppid_ = parent->pid_;
    // p->child_links_.reset();
    parent->child_list.push_front(p);
    p->child_list.reset();
    familial_lock.unlock(irqsp);
    // put the proc on the runq
    int cpu = pid % ncpu;
    auto irqsc = cpus[cpu].runq_lock_.lock();
    int r = cpus[cpu].enqueue(p);
    if (r < 0) p->cpu_ = cpu;
    cpus[cpu].runq_lock_.unlock(irqsc);
    canary_check(parent);

    return pid;
}

// this cleans a proc, flag not set if called from fork
void exit(proc* p, int flag, int exit_stat) {
    auto irqs = ptable_lock.lock();
    pid_t pid = p->pid_;
    proc* p_init = ptable[INIT_PID];
    p->state_ = proc::exited;
    p->exit_status_ = exit_stat;
    if (!flag) { ptable[pid] = nullptr; }
    // handle the fdtable and file structure
    auto irqsf = p->fdtable_->lock_.lock();
    for (int i = 0; i < NFDS; i++) {
        file* ptr = p->fdtable_->table_[i];
        if (ptr) { ptr->deref(); }
    }
    p->fdtable_->lock_.unlock(irqsf);
    kfree(p->fdtable_);
    // free the process's memory 
    for (vmiter it(p); it.low(); it.next()) {
        if (it.user() && it.present() && it.pa() != ktext2pa(console)) { 
            kfree((void*) it.ka());
        }
    }
    for (ptiter it(p); it.low(); it.next()) {
        kfree((void*) pa2ka(it.ptp_pa()));
    }
    ptable_lock.unlock(irqs);
    // reparent children and wake sleeping parent
    if (flag) { parenting(p, p_init); }
}

// needs to be called with fdt lock
int close(fdtable* fdt, int fd) {
    file* fl = fdt->table_[fd];
    fdt->table_[fd] = nullptr;
    if (!fl) { return E_BADF; }
    fl->deref();
    return 0;
}


// helper functions to the main-wrapper syscall functions above
//    reparenting: reparents a process and wakes a sleepq_
//    reap_child: finishes the reaping process in waitpid
//    msleep_cond: is the condition used in msleep's block_until
//    any remaing should be self explanatory

void parenting(proc* p, proc* p_init) {
    auto irqs = familial_lock.lock();
    while (proc* p_ = p->child_list.pop_front()) {
        p_->ppid_ = INIT_PID;
        p_init->child_list.push_front(p_);
    }
    familial_lock.unlock(irqs);
    auto irqsp = interrupt_lock.lock();
    waitq.wake_pid(p->ppid_);
    proc* prt = ptable[p->ppid_];
    int index = prt->sleepq_;
    if (index >= 0) {
        prt->sleepq_ = -1;
        sleepq_wheel[index].wake_pid(p->ppid_);
    }
    interrupt_lock.unlock(irqsp);
}

void reap_child(proc* p, wpret* wpr) {
    auto irqs = ptable_lock.lock();
    p->child_links_.erase();
    ptable[p->pid_] = nullptr;
    wpr->stat = p->exit_status_;
    wpr->pid_c = p->pid_;
    p->state_ = proc::dead;
    ptable_lock.unlock(irqs);
}

bool msleep_cond(proc* p, unsigned long want, int* res) {
    if (!(long(want - ticks) > 0)) { *res = 0; return true; }
    auto irqs = interrupt_lock.lock();

    if (p->sleepq_ < 0) { 
        *res = E_INTR;
        interrupt_lock.unlock(irqs);
        return true; 
    }
    interrupt_lock.unlock(irqs);

    return false;
}

void canary_check(proc* p) {
    if (p) { 
        assert(p->canary_ == CANARY); 
    }
    for (int i = 0; i < ncpu; ++i) {
        assert(cpus[i].canary_ == CANARY);
    }
}

// call with the fdtable lock held
int find_fd(fdtable* fdt) {
    for (int i = 0; i < NFDS; i++) {
        if (!fdt->table_[i]) {
            fdt->table_[i] = (file*) 0xDEADBEEF;
            return i;
        }
    }
    return -1;
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
        this->yield();
        return 0;

    case SYSCALL_PAGE_ALLOC: {
        uintptr_t addr = regs->reg_rdi;
        if (addr >= VA_LOWMAX || addr & PAGEOFFMASK) {
            return -1;
        }
        x86_64_page* pg = kallocpage();
        if (!pg || vmiter(this, addr).map(ka2pa(pg)) < 0) {
            return -1;
        }
        canary_check(this);
        return 0;
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
        exit(this, 1, regs->reg_rdi);
        this->yield_noreturn();
    }

    case SYSCALL_MSLEEP: {
        int res;
        unsigned long want = ticks + (regs->reg_rdi + 9) / 10;
        auto irqs = interrupt_lock.lock();
        int indx = sleepq_ = want % WHEEL_SZ;
        interrupt_lock.unlock(irqs);
        waiter(this).block_until(sleepq_wheel[indx], 
            [&] () { return msleep_cond(this, want, &res); });

        return res;
    }

    case SYSCALL_GETPPID: {
        auto irqs = familial_lock.lock();
        pid_t ppid = this->ppid_;
        familial_lock.unlock(irqs);
        return ppid;
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

    case SYSCALL_WAITPID: {
        pid_t pid = regs->reg_rdi;
        int opts = regs->reg_rsi;
        wpret wpr = wait_pid(pid, this, opts);
        asm("" : : "c" (wpr.stat));
        return wpr.pid_c;
    }

    case SYSCALL_OPEN: {
        uintptr_t name = regs->reg_rdi;
        int flags = regs->reg_rsi;

        if (vmiter(this, name).str_perm(PTE_P | PTE_W | PTE_U) < 0) {
            return E_FAULT;
        }

        auto irqs = memfile_lock.lock();
        memfile* mf = memfile::initfs_lookup((char*) name);
        if (!mf) {
            if (!(flags & OF_CREATE)) {
                memfile_lock.unlock(irqs);
                return E_NOENT;
            }
            mf = knew<memfile>((char*) name, nullptr, nullptr);
        }
        // do i need to add this file somehow to the array?
        memfile_lock.unlock(irqs);
        file* fl = kalloc_file();
        vnode* vn = knew<vnode_memfile>(mf);
        fdtable_->lock_.lock_noirq();
        int fd = find_fd(fdtable_);
        // ensure that a new file can be made
        if (!mf || !fl || !vn || fd < 0) {
            fdtable_->table_[fd] = nullptr;
            fdtable_->lock_.unlock_noirq();
            kfree(mf); kfree(fl); kfree(vn);
            return -1;
        }        
        fl->vnode_ = vn;
        fdtable_->table_[fd] = fl;
        fdtable_->lock_.unlock_noirq();

        if (flags & OF_TRUNC) {
            mf->len_ = 0;
        } 
        fl->pread_ = (flags & OF_READ);
        fl->pwrite_ = (flags & OF_WRITE);

        return fd;
    }

    case SYSCALL_CLOSE: {
        int fd = regs->reg_rdi;
        auto irqs = fdtable_->lock_.lock(); 
        int r = close(fdtable_, fd);
        fdtable_->lock_.unlock(irqs);

        return r;
    }

    case SYSCALL_DUP2: {
        int oldfd = regs->reg_rdi;
        int newfd = regs->reg_rsi;

        if (!VALFD(oldfd) || !VALFD(newfd)) {
            return E_BADF;
        }        

        fdtable* fdt = this->fdtable_;
        auto irqs = fdt->lock_.lock();
        if (!fdt->table_[oldfd]) {
            fdt->lock_.unlock(irqs);
            return E_BADF;
        }
        if (oldfd == newfd) {
            fdt->lock_.unlock(irqs);
            return newfd;
        }
        // if there is an open file, then close it 
        if (fdt->table_[newfd]) {
            close(fdt, newfd);
        }
        fdt->table_[newfd] = fdt->table_[oldfd];
        file* fl = fdt->table_[newfd];
        fl->adref();
        fdt->lock_.unlock(irqs);

        return newfd;
    }

    case SYSCALL_PIPE: {
        fdtable* fdt = this->fdtable_;
        auto irqs = fdt->lock_.lock();
        int rfd = find_fd(fdt);
        int wfd = find_fd(fdt);

        if (rfd < 0 || wfd < 0) {
            fdt->table_[rfd] = nullptr;
            fdt->table_[wfd] = nullptr;
            fdt->lock_.unlock(irqs);
            return -1;
        } 
        // set up either end of the pipe
        vnode* pipe = knew<vnode_pipe>();
        pipe->bb_ = knew<bbuffer>();
        file* flr = kalloc_file();
        file* flw = kalloc_file();
        if (!pipe || !pipe->bb_ || !flw || !flr) {
            fdt->table_[rfd] = nullptr;
            fdt->table_[wfd] = nullptr;
            kfree(flr); kfree(flw);
            kfree(pipe); kfree(pipe->bb_);
            fdt->lock_.unlock(irqs);
            return -1;
        }
        pipe->adref();
        flr->pwrite_ = false; 
        flr->type_ = file::pipe;
        flr->vnode_ = pipe;
        fdt->table_[rfd] = flr;
        flw->pread_ = false; 
        flw->type_ = file::pipe;
        flw->vnode_ = pipe;
        fdt->table_[wfd] = flw;

        fdt->lock_.unlock(irqs);
        return (uint64_t) rfd | ((uint64_t) wfd << 32);
    }


    case SYSCALL_READ: {
        int fd = regs->reg_rdi;
        uintptr_t addr = regs->reg_rsi;
        size_t sz = regs->reg_rdx;

        if (!sz) return 0;
        if (!VALFD(fd)) return E_BADF;
        if (!vmiter(this, addr).perm_range(PTE_P | PTE_W | PTE_U, sz)) {
            return E_FAULT;
        }

        fdtable* fdt = this->fdtable_;
        auto irqsf = fdt->lock_.lock();
        file* fl = fdt->table_[fd];
        if (!fl || !fl->pread_) {
            fdt->lock_.unlock(irqsf);
            return E_BADF;
        }
        fl->lock_.lock_noirq();
        fdt->lock_.unlock_noirq();
        fl->refs_++;
        fl->lock_.unlock(irqsf);
        // make the read call
        size_t n = fl->vnode_->read(addr, sz, fl);
        fl->deref();

        return n;
    }

    case SYSCALL_WRITE: {
        int fd = regs->reg_rdi;
        uintptr_t addr = regs->reg_rsi;
        size_t sz = regs->reg_rdx;

        if (!sz) return 0;
        if (!VALFD(fd)) return E_BADF;
        if (!vmiter(this, addr).perm_range(PTE_P | PTE_U, sz)) {
            return E_FAULT;
        }

        fdtable* fdt = this->fdtable_;
        auto irqsf = fdt->lock_.lock();
        file* fl = fdt->table_[fd];
        if (!fl || !fl->pwrite_) {
            fdt->lock_.unlock(irqsf);
            return E_BADF;
        }
        fl->lock_.lock_noirq();
        fdt->lock_.unlock_noirq();
        fl->refs_++;
        fl->lock_.unlock(irqsf);
        // make the write call
        size_t n = fl->vnode_->write(addr, sz, fl);
        fl->deref();

        return n;
    }

    case SYSCALL_EXECV: {
        uintptr_t pathname = regs->reg_rdi;
        char** argv = (char**) regs->reg_rsi;
        int argc = regs->reg_rdx;

        if (vmiter(this, pathname).str_perm(PTE_P | PTE_W | PTE_U) < 0) {
            return E_FAULT;
        }
        x86_64_pagetable* pt = kalloc_pagetable();
        if (!pt) return -1;
        regstate new_regs;
        memfile_loader ml;

        memcpy(&new_regs, regs, sizeof(regstate));

        ml.pagetable_ = pt;
        ml.mf_ = memfile::initfs_lookup((char*) pathname);
        int r = load(ml);
        if (r >= 0) {
            new_regs.reg_rip = ml.entry_rip_;
        } else {
            kfree(pt);
            return r;
        }
        x86_64_pagetable* old_pt = pagetable_;
        pagetable_ = pt;
        regs_ = &new_regs;
        regs_->reg_rdi = argc;
        regs_->reg_rsp = MEMSIZE_VIRTUAL;
        x86_64_page* stkpg = kallocpage();
        if (!stkpg) {
            kfree(pt); kfree(stkpg);
            return -1;
        }

        int n = vmiter(this, MEMSIZE_VIRTUAL - PAGESIZE).map(ka2pa(stkpg));
        int m = vmiter(this, ktext2pa(console)).map(ktext2pa(console), PTE_P | PTE_W | PTE_U);
        if (n < 0 || m < 0) {
            kfree(pt); kfree(stkpg);
            return -1;
        }
        size_t n_args = argc + 1;
        char* new_args[n_args];
        char* stkoff = ((char*) stkpg) + PAGESIZE - 1;
        // copy the arguments into the stack page
        for (int i = 0; i < argc; i++) {
            size_t len = strlen(argv[i]);
            strcpy(stkoff - len, argv[i]);
            new_args[i] = MEMSIZE_VIRTUAL - PAGESIZE + ((stkoff - len) - (uintptr_t) stkpg);
            stkoff -= len + 1;
        }
        new_args[n_args] = nullptr;
        stkoff -= sizeof(char*) * (n_args);
        memcpy(stkoff, new_args, sizeof(char*) * (n_args));
        regs_->reg_rsi = OFF2USR(stkoff - (uintptr_t) stkpg);
        regs_->reg_rsp = OFF2USR(stkoff - (uintptr_t) stkpg) - 1;
        // set pagetable and then clear old memory
        set_pagetable(pt);
        for (vmiter it(old_pt); it.low(); it.next()) {
            if (it.user() && it.present() && it.pa() != ktext2pa(console)) { 
                kfree((void*) it.ka());
            }
        }
        for (ptiter it(old_pt); it.low(); it.next()) {
            kfree((void*) pa2ka(it.ptp_pa()));
        }
        yield_noreturn();
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


// the following functions can likely be deleted
//     they need to be held with locks in most cases
//     they are visual debugging functions to unfuck things

void print_used_pids() {
    log_printf("the following pids are taken:");
    // go through ptable to find open proc
    for (pid_t i = 1; i < NPROC; i++) {
        if (ptable[i]) {
            log_printf(" %d", ptable[i]->pid_);
        }
    }
    log_printf("\n");
}

void print_runq__(proc* forked, proc* parent) {
    int find = 0;
    for (proc* p = this_cpu()->runq_.front(); p; p = this_cpu()->runq_.next(p)) {
        if (p->pid_ == forked->pid_) {
            find = p->pid_;
        }
    }
    assert(find);
}

void print_waitq_(wait_queue& wq) {
    log_printf("the things on the runq_ are: ");
    auto irqs = wq.lock_.lock();
    for (auto w = wq.q_.front(); w; w = wq.q_.next(w)) {
       log_printf(" %d", w->p_->pid_);
    }
    wq.lock_.unlock(irqs);
    log_printf("\n");
}

bool child_exists(proc* parent, proc* check) {
    for (proc* p = parent->child_list.front(); p;
          p = parent->child_list.next(p)) {
        if (p == check) { return true; }
    }
    return false;
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
