CS 161 Problem Set 2 Answers
============================
Leave your name out of this file. Put collaboration notes and credit in
`pset2collab.md`.

Answers to written questions
----------------------------


`wpret wait_pid(pid_t pid, proc* parent, int opts) {
    wpret wpr;
    bool wait = false;
    while (1) {
        auto irqsp = process_hierarchy_lock.lock();
        // check if the child list is empty
        proc* p = parent->child_list.front();
        if (!p) {
            process_hierarchy_lock.unlock(irqsp);
            wpr.pid_c = E_CHILD;
            return wpr;
        }
        // cycle through the list to find a child
        while (p) {
            if (!pid) {
                if (p->state_ == proc::wexited) {
                    reap_child(p, &wpr);
                    process_hierarchy_lock.unlock(irqsp);
                    kfree(p->pagetable_); kfree(p);
                    return wpr;
                }
                wait = true;
            } else if (p->pid_ == pid) {
                if (p->state_ == proc::wexited) {
                    reap_child(p, &wpr);
                    process_hierarchy_lock.unlock(irqsp);
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
}`

`void reap_child(proc* p, wpret* wpr) {
    auto irqs = ptable_lock.lock();
    p->child_links_.erase();
    ptable[p->pid_] = nullptr;
    wpr->stat = p->exit_status_;
    wpr->pid_c = p->pid_;
    p->state_ = proc::dead;
    ptable_lock.unlock(irqs);
}`



Grading notes
-------------
