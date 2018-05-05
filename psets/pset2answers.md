CS 161 Problem Set 2 Answers
============================
Leave your name out of this file. Put collaboration notes and credit in
`pset2collab.md`.

Answers to written questions
----------------------------

#PART A

1. I derived a lot of benefit from using, the `print_struct` function I wrote in `k-alloc.cc`. It was a 
very useful visually debugging aid that helped me build my allocator correctly (was more useful than `test_kalloc`).

#PART B

1. Changes to this were made throughout building the rest of the pset. 

#PART C

1. Below is my original, non-blocking implementation of `sys_msleep`.

```
unsigned long want_ticks = ticks + (regs->reg_rdi + 9) / 10;
sti();
while (long(want_ticks - ticks) > 0) {
    this->yield();
}
return 0;
```

#PART D

1. I started by including a `ppid_` member on the proc struct. This allowed me to track a process's parent 
throughout its existence. I then added a `list_links` and list to the proc struct in order to track a process's
parent. These members where handled in the first process, fork, and exit. The first process was assigned to the 
init proc's child list and its ppid_ was marked accordingly. Every process created by a fork after that recieved
was assigned a ppid_ equivalent to the pid_ of the process that called fork to create them. Finally, in exit I made
sure to assigne all of the children of a process to the init process upon the parent's death. All of the interactions
with the parent/child members where handled while holding the `familial_lock` defined in `kernel.hh`. 

2. Synchronization: 
    a. `familial_lock` is meant to be more granular than `ptable_lock` because it needs to be
    held when accessing certain members of the process hierarchy structure only.
    b. Read/Writes to `ppid_`, `child_list`, or `child_links` requires the `familial_lock`
    c. This worked before adding code for part G, something went wrong, and now I believe it 
    has a race-condition. 

#PART E

1. Below is my `waitpid` helper function before I added blocking:

```
wpret wait_pid_(pid_t pid, proc* parent, int opts) {
    wpret wpr;
    bool wait = false;
    waiter w(parent);
    while (1) {
        auto irqsp = familial_lock.lock();
        // check if the child list is empty
        proc* p = parent->child_list.front();
        if (!p) {
            familial_lock.unlock(irqsp);
            wpr.pid_c = E_CHILD;
            return wpr;
        }
        // cycle through the list to find a child
        while (p) {
            if (!pid) {
                if (p->state_ == proc::wexited) {
                    reap_child(p, &wpr);
                    familial_lock.unlock(irqsp);
                    kfree(p->pagetable_); kfree(p);
                    return wpr;
                }
                wait = true;
            } else if (p->pid_ == pid) {
                if (p->state_ == proc::wexited) {
                    reap_child(p, &wpr);
                    familial_lock.unlock(irqsp);
                    kfree(p->pagetable_); kfree(p);
                    return wpr;
                }
                wait = true;
                break;
            }
            p = parent->child_list.next(p);
        }
        familial_lock.unlock(irqsp);
        if (!wait) { wpr.pid_c = E_CHILD; break; }
        if (opts) { wpr.pid_c = E_AGAIN; break; }
        parent->yield();
    }
    return wpr;
}
```

```
void reap_child(proc* p, wpret* wpr) {
    auto irqs = ptable_lock.lock();
    p->child_links_.erase();
    ptable[p->pid_] = nullptr;
    wpr->stat = p->exit_status_;
    wpr->pid_c = p->pid_;
    p->state_ = proc::dead;
    ptable_lock.unlock(irqs);
}
```

2. Synchronization: because `waitpid` makes use of the parent/child relational data of a process
I enforce that the `familial_lock` is held.
    a. `familial_lock` must be held when accessing `ppid_` or the `child_list` structure in any
    capacity.
    b. `ptable_lock` is held in `reap_child` function because it modifies the process table to make
    the awaited process's pid available again. 

#PART F

1. I closely followed the wait-queue notes/lecture to build my wait-queue abstraction. I added a 
member function to wait-queue that specifically woke up a process of a certain pid rather than the 
whole queue. This was used in exit to wake a blocked process in waitpid waiting for a specific 
child to exit. 

2. Because of my delayed progress in the course, I was able to use `block_until` in my final implementation
of `waitpid`. I pass in a struct defined in `kernel.hh` that basically holds information on what the process's
decision should be after checking its children. It can block if necessary, or reap the process it found. The
exit status, as well as the aforementioned info, is all stored in the `wpret` struct. 

#PART G

1. I added a `sleepq_` member to the struct in order to know which queue a process was sleeping on. In exit, 
if a parent was on a sleep-queue a child would set the `sleepq_` member to `-1` and wake up the process. 

2. Because this involved parent and child processes I ensured that the `familial_lock` was held.

3. I have the understanding that this part kinda screwed up some of the synchronization I enforced earlier on
in the pset. In particular, I believe it made me sloppy with my usage of the `familial_lock`. 


Grading notes
-------------
