#include "kernel.hh"
#include "k-lock.hh"
#include "k-list.hh"
#include "lib.hh"

#define ARR_SZ          10
#define MIN_ORD         12
#define SUB_ORD         13
#define MAX_ORD         21
#define ALL_PGS         512
#define LST_IND(x)      x - MIN_ORD  
#define PAGE_ORD(x)    (msb(x) - 1)
#define BUD_PAGE(x, y) (x ^ (1 << y)) / PAGESIZE

static spinlock page_lock;
static uintptr_t next_free_pa;
static void print_struct(int info = 0);

page pages[600];
list<page, &page::link_> lists[10];



// kallocpage
//    calls kalloc with a pagesize of one, and returns

x86_64_page* kallocpage() {
    return reinterpret_cast<x86_64_page*>(kalloc(PAGESIZE));
}

// init_kalloc
//    Initialize stuff needed by `kalloc`. Called from `init_hardware`,
//    after `physical_ranges` is initialized.

void init_kalloc() {
    auto irqs = page_lock.lock();
    // initialize the pages array
    for (int i = 0; i < ALL_PGS; ++i) {
        pages[i].pn = i;
        pages[i].order = -1;
        pages[i].free = false;
        pages[i].block = false;
    }
    int pn, blk_ord;
    uintptr_t curr_pa = 0;
    // we can make this better by using lsb to check the alignment?
    auto range = physical_ranges.find(curr_pa);
    for (range; range != physical_ranges.end(); range++) {
        if (range->type() != mem_available) continue;
        curr_pa = range->first();
        while (curr_pa < range->last()) {
            size_t sz = range->last() - curr_pa;
            for (blk_ord = PAGE_ORD(sz); blk_ord >= MIN_ORD; --blk_ord) {
                if (curr_pa % (1 << blk_ord) == 0) break;
            }
            pn = curr_pa / PAGESIZE;
            pages[pn].block = true;
            // add the block to apropriate list
            pages[pn].link_.reset();
            lists[blk_ord - MIN_ORD].push_back(&pages[pn]);
            // update block-associated pages 
            int num_pgs = 1 << (blk_ord - MIN_ORD);
            for (int j = 0; j < num_pgs; j++) {
                pages[pn + j].order = blk_ord;
                pages[pn + j].free = true;
            }
            curr_pa += (num_pgs * PAGESIZE);
        }
    }
    page_lock.unlock(irqs);
}

// kalloc(sz)
//    Allocate and return a pointer to at least `sz` contiguous bytes
//    of memory. Returns `nullptr` if `sz == 0` or on failure.

void* kalloc(size_t sz) {    
    int req_ord = PAGE_ORD(sz) > MIN_ORD ? PAGE_ORD(sz) : MIN_ORD;
    int n = req_ord - MIN_ORD;
    if (req_ord > MAX_ORD || sz == 0) return nullptr;
    // find the next non-empty list
    auto irqs = page_lock.lock();
    page* block = nullptr;
    for (int j = req_ord - MIN_ORD; j < ARR_SZ; j++) {
        if (!lists[j].empty()) {
            block = lists[j].pop_front(); 
            break;
        }
    }
    // return null if no memory to alloc
    if (!block) {
        page_lock.unlock(irqs);
        return nullptr; 
    }
    assert(pages[block->pn].block);
    assert(req_ord >= MIN_ORD);
    // break down the block to requested size
    int pgn = block->pn;
    int ret_ord = block->order;
    for (ret_ord; ret_ord > req_ord; --ret_ord) {
        int indx = ret_ord - SUB_ORD;
        int nxt = (1 << indx) + pgn;
        for (int l = pgn; l < 2 * nxt - pgn; l++) {
            assert(pages[l].free);
            --pages[l].order;
        }
        pages[nxt].block = true;
        pages[nxt].link_.reset();
        lists[indx].push_back(&pages[nxt]);
    }
    // mark the remaining block and return
    int npages = 1 << (ret_ord - MIN_ORD);
    for (int k = pgn; k < pgn + npages; k++) {
        pages[k].free = false;
        pages[k].order = ret_ord;
    }
    page_lock.unlock(irqs);
    return (void*) pa2ka(block->pn * PAGESIZE);
}

// kfree(ptr)
//    Free a pointer previously returned by `kalloc`, `kallocpage`, or
//    `kalloc_pagetable`. Does nothing if `ptr == nullptr`.

void kfree(void* ptr) {
    if (!ptr) return;
    auto irqs = page_lock.lock();
    uintptr_t addr = ka2pa(ptr);
    int pgn = addr / PAGESIZE;
    assert(lsb(addr) >= 12);
    assert(pgn <= ALL_PGS);
    assert(pages[pgn].block);
    // mark the block before coalescing
    int ord = pages[pgn].order;
    int num_pgs = (1 << ord) / PAGESIZE;
    for (int i = pgn; i < pgn + num_pgs; i++) {
        assert(!pages[i].free);
        pages[i].free = true;
    }
    // find all buddies and coalesce them
    for (ord; ord < MAX_ORD; ord++) {
        int b_pgn = BUD_PAGE(addr, ord);
        if (b_pgn < 0 || b_pgn > ALL_PGS) break; 
        if (!pages[b_pgn].free || pages[b_pgn].order != ord) break;
        // remove buddy from list and reorder
        pages[b_pgn].link_.erase();
        int start = MIN(pgn, b_pgn);
        int mid = MAX(pgn, b_pgn);
        int half_pgs = (1 << ord) / PAGESIZE;
        // mark the new, coalesced block
        pages[start].block = true;
        pages[mid].block = false;
        for (int j = start; j < mid + half_pgs; j++) {
            assert(pages[j].free);
            pages[j].order++;
        }
        addr = start * PAGESIZE;
        pgn = start;
    }
    pages[pgn].link_.reset();
    lists[ord - MIN_ORD].push_back(&pages[pgn]);
    page_lock.unlock(irqs);
}

// test_kalloc
//    Run unit tests on the kalloc system.

void test_kalloc() {
    // do nothing for now
}

// prints memory struct, set info flag to get page details
void print_struct(int info) {
    for (int i = 0; i < 10; i++) {
        log_printf("List %d:  ", i + MIN_ORD);
        page* n = lists[i].front();
        while (n) {
            if (info) {
                log_printf("[p: %d o: %d f: %d]", n->pn, n->order, n->free);
            } else {
                log_printf("[x]", n->pn, n->order, n->free);
            }
            log_printf(" -> ");
            n = lists[i].next(n);
        }
        log_printf("null\n");
    }
}

