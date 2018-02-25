#include "kernel.hh"
#include "k-lock.hh"
#include "k-list.hh"
#include "lib.hh"

#define ARR_SZ        10
#define MIN_ORD       12
#define SUB_ORD       13
#define MAX_ORD       21
#define PAGE_ORD(x) (msb(x) - 1)

static spinlock page_lock;
static uintptr_t next_free_pa;

page pages[600];

struct list<page, &page::link_> lists[10];

x86_64_page* kallocpage() {
    // auto irqs = page_lock.lock();

    // x86_64_page* p = nullptr;

    // // skip over reserved and kernel memory
    // auto range = physical_ranges.find(next_free_pa);
    // while (range != physical_ranges.end()) {
    //     if (range->type() == mem_available) {
    //         // use this page
    //         p = pa2ka<x86_64_page*>(next_free_pa);
    //         next_free_pa += PAGESIZE;
    //         break;
    //     } else {
    //         // move to next range
    //         next_free_pa = range->last();
    //         ++range;
    //     }
    // }

    // page_lock.unlock(irqs);
    // return p;

    return reinterpret_cast<x86_64_page*>(kalloc(PAGESIZE));
}



// init_kalloc
//    Initialize stuff needed by `kalloc`. Called from `init_hardware`,
//    after `physical_ranges` is initialized.
void init_kalloc() {
    auto irqs = page_lock.lock();

    // initialize the pages array
    for (int i = 0; i < 512; ++i) {
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
            pages[pn].link_.clear();
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

    // TESTING FOR THIS FUNCTION (CAN BE REMOVED)

    // for (int t = 0; t < 512; ++t){
    //     log_printf("pn is %d, order is %d, free is %d, block_start is %d\n", 
    //         t, pages[t].order, pages[t].free, pages[t].block);
    // }

    // for (int t = 0; t < 10; t++) {
    //     for (page* p = lists[t].front(); p; p = lists[t].next(p)) {
    //         log_printf("pn %d order %d is in list %d\n", p->pn, p->order, t + 12);
    //     }
    // }
}

// void update_pages(int free, int ord, int pgs, int strt) {
//     for (int i = start; i < pgs; i++) {
//         if (free) pages[i].free = free;
//         if (ord) pages[i].order = ord;
//     }
// }



// BRAEDON'S VERSION OF KALLOC (CLEAN)
void* kalloc(size_t sz) {    
    int req_ord = PAGE_ORD(sz) > MIN_ORD ? PAGE_ORD(sz) : MIN_ORD;
    int n = req_ord - MIN_ORD;
    if (req_ord > MAX_ORD || sz == 0) return nullptr;

    auto irqs = page_lock.lock();
    // if non-empty list, return block to user
    if (!lists[n].empty()) {
        page* p = lists[n].pop_front();
        assert(p->free && p->block);
        int pn = p->pn;
        int pgs = 1 << n;
        for (int i = 0; i < pgs; i++) {
            pages[pn + i].free = false;
        }
        page_lock.unlock(irqs);
        return (void*) pa2ka(pn * PAGESIZE);
    }
    // find the next non-empty list
    page* block = nullptr;
    for (int j = req_ord - MIN_ORD; j < ARR_SZ; j++) {
        if (!lists[j].empty()) {
            block = lists[j].pop_front(); 
            break;
        }
    }
    // return null if no memory
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
        pages[nxt].link_.clear();
        lists[indx].push_back(&pages[nxt]);
    }
    // mark the remaining block and return
    int npages = 1 << (ret_ord - MIN_ORD);
    for (int k = pgn; k < npages; k++) {
        pages[k].free = false;
        pages[k].order = ret_ord;
    }
    page_lock.unlock(irqs);

    return (void*) pa2ka(block->pn * PAGESIZE);
}

// kalloc(sz)
//    Allocate and return a pointer to at least `sz` contiguous bytes
//    of memory. Returns `nullptr` if `sz == 0` or on failure.
// void* kalloc(size_t sz) {

//     auto irqs = page_lock.lock();

//     int order = msb(sz)-1;
//     // log_printf("requested order is %d\n",order);
//     if (order > MAX_ORD  || sz == 0){
//         return nullptr;
//     }

//     if (order < MIN_ORD){
//         order = 12;
//     }

//     int n = order - 12;
//     int num_pgs = 1 << n;

//     if (!lists[n].empty()){
//         page* p = lists[n].pop_front();
//         assert(p->free);

//         int pn = p->pn;

//         for (int i = 0; i < num_pgs; ++i){
//             pages[pn+i].free = false;
//         }

//         log_printf("allocated p at addr %u\n",p);

//         for (int t = 0; t < 10; ++t){
//             for (page* p1 = lists[t].front(); p1; p1 = lists[t].next(p1)) {
//                 log_printf("pn %d order %d is in list 1%d\n", p1->pn,p1->order,t+2);
//             }
//         }

//         page_lock.unlock(irqs);

//         return (void*) pa2ka (pn*PAGESIZE);
//     }

//     for (int i = n + 1; i < 10; ++i){
        
//         if (!lists[i].empty()){

//             page* p = lists[i].pop_front();
//             assert(p->free);

//             int pn = p->pn;

//             for (int j = i + 12; j >= order; --j){

//                 if (j == order){

//                     for (int l = 0; l < num_pgs; ++l){
//                         pages[pn+l].free = false;
//                     }

//                     log_printf("allocated p is at addr %u\n",p);

//                     for (int t = 0; t < 10; ++t){
//                         for (page* p1 = lists[t].front(); p1; p1 = lists[t].next(p1)) {
//                             log_printf("pn %d order %d is in list 1%d\n", p1->pn,p1->order,t+2);
//                         }
//                     }

//                     page_lock.unlock(irqs);

//                     return (void*) pa2ka(pn*PAGESIZE);

//                 } else {

//                     int pages_to_break_pt = 1 << (j - 13);

//                     int pn2 = pn + pages_to_break_pt;

//                     pages[pn2].block_start = true;

//                     pages[pn2].link_.clear();
//                     lists[j-13].push_back(&pages[pn2]);

//                     for (int k = 0; k < (2*pages_to_break_pt); ++k){
//                         --pages[pn+k].order;
//                     }
//                 }               
//             }
//         }
//     }

//     page_lock.unlock(irqs);

//     return nullptr;
// }

// kfree(ptr)
//    Free a pointer previously returned by `kalloc`, `kallocpage`, or
//    `kalloc_pagetable`. Does nothing if `ptr == nullptr`.
void kfree(void* ptr) {
    assert(0 && "kfree not implemented yet");
}

// test_kalloc
//    Run unit tests on the kalloc system.
void test_kalloc() {
    // do nothing for now
}
