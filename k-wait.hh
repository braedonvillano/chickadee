#ifndef CHICKADEE_K_WAIT_HH
#define CHICKADEE_K_WAIT_HH
#include "kernel.hh"
#include "k-list.hh"
struct wait_queue;

struct waiter {
    proc* p_;
    wait_queue* wq_;
    list_links links_;

    inline waiter(proc* p);
    inline ~waiter();
    NO_COPY_OR_ASSIGN(waiter);
    inline void prepare(wait_queue& wq);
    inline void prepare(wait_queue* wq);
    inline void block();
    inline void clear();
    inline void wake();

    template <typename F>
    inline void block_until(wait_queue& wq, F predicate);
    template <typename F>
    inline irqstate block_until(wait_queue& wq, F predicate, spinlock& lock);
    template <typename F>
    inline void block_until(wait_queue& wq, F predicate,
                            spinlock& lock, irqstate& irqs);
};


struct wait_queue {
    list<waiter, &waiter::links_> q_;
    mutable spinlock lock_;

    // you might want to provide some convenience methods here
    inline void wake_all();
    inline void wake_pid(pid_t pid);
};


inline waiter::waiter(proc* p)
    : p_(p), wq_(nullptr) {
}

inline waiter::~waiter() {
    // optional error-checking code
}

inline void waiter::prepare(wait_queue& wq) {
    prepare(&wq);
}

inline void waiter::prepare(wait_queue* wq) {
    wq_ = wq;
    auto irqs = wq->lock_.lock();
    p_->state_ == proc::blocked;
    this->links_.reset();
    wq->q_.push_back(this);
    wq->lock_.unlock(irqs);
}

inline void waiter::block() {
    p_->yield();
    clear();
}

inline void waiter::clear() {
    auto irqs = wq_->lock_.lock();
    p_->state_ == proc::runnable;
    if (links_.is_linked()) {
        links_.erase();
    }
    wq_->lock_.unlock(irqs);
}

inline void waiter::wake() {
    p_->wake();
}


// waiter::block_until(wq, predicate)
//    Block on `wq` until `predicate()` returns true.
template <typename F>
inline void waiter::block_until(wait_queue& wq, F predicate) {
    while (1) {
        prepare(wq);
        if (predicate()) {
            break;
        }
        block();
    }
    clear();
}

// waiter::block_until(wq, predicate, lock)
//    Lock `lock`, then block on `wq` until `predicate()` returns
//    true. All calls to `predicate` have `lock` locked. Returns
//    with `lock` locked; the return value is the relevant `irqstate`.
template <typename F>
inline irqstate waiter::block_until(wait_queue& wq, F predicate,
                                    spinlock& lock) {
    auto irqs = lock.lock();
    block_until(wq, predicate, lock, irqs);
    return std::move(irqs);
}

// waiter::block_until(wq, predicate, lock, irqs)
//    Block on `wq` until `predicate()` returns true. The `lock`
//    must be locked; it is unlocked before blocking (if blocking
//    is necessary). All calls to `predicate` have `lock` locked,
//    and `lock` is locked on return.
template <typename F>
inline void waiter::block_until(wait_queue& wq, F predicate,
                                spinlock& lock, irqstate& irqs) {
    while (1) {
        prepare(wq);
        if (predicate()) {
            break;
        }
        lock.unlock(irqs);
        block();
        irqs = lock.lock();
    }
    clear();
}

// wait_queue::wake_all()
//    Lock the wait queue, then clear it by waking all waiters.
inline void wait_queue::wake_all() {
    auto irqs = lock_.lock();
    while (auto w = q_.pop_front()) {
        w->wake();
    }
    lock_.unlock(irqs);
}

inline void wait_queue::wake_pid(pid_t pid) {
    auto irqs = lock_.lock();
    for (auto w = q_.front(); w; w = q_.next(w)) {
        if (w->p_->pid_ == pid) {
            w->wake(); 
            q_.erase(w);
            break;
        }
    }
    lock_.unlock(irqs);
}

#endif
