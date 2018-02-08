CS 161 Problem Set 1 Answers
============================
Leave your name out of this file. Put collaboration notes and credit in
`pset1collab.md`.

Answers to written questions
----------------------------
PART A:

1. The first address is `0xffff800000001000`. This is because the first page of virtual memory is reserved completely to protect usage of the null pointer at address zero. 

2. The larges address allocated is `0xffff8000001ff000`.

3. The function kallocpage() returns high canonical kernal addresses. This seems to be caused by line 17 - where a physical address is converted to 'ka' or a kernel address.

4. Changing `MEMSIZE_PHYSICAL` on line 177 in `k-init.cc` will allow the allocator to use more memory. 

5. A simplified loop looks like:

   `while (next_free_pa < physical_ranges.limit()) {
        if (physical_ranges.type(next_free_pa) == mem_available) {
            // use this page
            p = pa2ka<x86_64_page*>(next_free_pa);
            next_free_pa += PAGESIZE;
            break;
        } else {
            // move to next range
            next_free_pa += PAGESIZE;
        }
    }`

6. The loop using `find()` has the benefit of skipping over full memranges as opposed to searching through each page checking if it is available. We can see this quantitatively in the form of the number of iterations the `kallocpage()` function goes through.

7. If there were no page_lock then multiple cores would be able to allocate and this would prevent mutual exclusion.

PART B:

1. Line 73 marks the physical page 0x40000 as kernel-restricted.

2. Line 82 marks the struct proc memory as kernel-restricted. 

3. The distinction between `ptiter` and `vmiter` is to protect processes from accessing unprivalleged data, to protect processes from one another, and to grant privallege to the kernel. The `ptiter` iterator visits each pagetable page and gives the physical addresses associated, while `vmiter` handles mappings from virtual to physical memory. This means that `vmiter` is concerned with returning pages of data that are not pagetables specifically, but `ptiter` is. If the `vmiter` was allowed to access the steps in mapping a virtual address to physical address, namely the pagetable itself, then processes that use it would be able to tamper with kernel and reserved mappings. 

4. The memory type that all of the `proc *`'s have is `f_kernel`. This is because the proc structures have privelleged data that should not be accessed by anything but the kernel and the process that owns it. 

5. If you replace `it.next()` with `it += PAGESIZE`, the QEMU will hang and not display the physical and virtual memory spaces. This is because incrementing by `PAGESIZE` is inefficient because it does not skip over gaps of in physical memory and will take a ridiculously long time to finish the task. QEMU, in response, hangs because it is taking `vmiter` too long to reach the console.

6. 

Grading notes
-------------
