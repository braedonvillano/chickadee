CS 161 Problem Set 1 Answers
============================
Leave your name out of this file. Put collaboration notes and credit in
`pset1collab.md`.

Answers to written questions
----------------------------

#PART A
1. The first address is `0xffff800000001000` which is essentially the kernel address for physical address `0x1000`. This is because the first page of physical memory is 'mem_reserved' to protect the nullptr.

2. The largest address allocated is `0xffff8000001ff000`.

3. The function `kallocpage()` returns high canonical kernal addresses or virtual addresses. This seems to be caused by line 17 - where a physical address is converted to 'ka' or a kernel address using `pa2ka()`.

4. Changing `MEMSIZE_PHYSICAL` or replacing it on line 183 of 'k-init.cc' will allow you to change the amount of physical memory.

5. The code for the loop is below:
	`while (!p && next_free_pa < physical_ranges.limit()) {
	    if (physical_ranges.type(next_free_pa) == mem_available) {
	        p = pa2ka<x86_64_page*>(next_free_pa);
	    }
	    next_free_pa += PAGESIZE;
	}`

6. The `find()` loop can skip over memranges if memory is marked as unavailable, while the `type()` loop has to check all pages. The `find()` loop takes 372 iterations to get to the end of `MEMSIZE_PHYSICAL`, and the `type()` loop has to take around 1048000 iterations to do so (printed iterations in each loop to find data).

7. Neglecting to use the page_lock could allow multiple cores to allocate the same page and would break mutual exclusion. 

#PART B
1. Line 73 marks the physical page 0x40000 as kernel-restricted.

2. Line 82 marks the struct proc memory as kernel-restricted. 

3. The distinction between `ptiter` and `vmiter` is to protect processes from accessing unprivalleged data, to protect processes from one another, and to grant privallege to the kernel. The `ptiter` iterator visits each pagetable page and gives the physical addresses associated, while `vmiter` handles mappings from virtual to physical memory. This means that `vmiter` is concerned with returning pages of data that are not pagetables specifically, but `ptiter` is. If the `vmiter` was allowed to access the steps in mapping a virtual address to physical address, namely the pagetable itself, then processes that use it would be able to tamper with kernel and reserved mappings. 

4. The pages associated with a `pid` in the loop have a constant of `mem_available` because the given by `kallocpage()`. However, the proc struct and the process pagetables are all marked by `f_kernel`. This is because the proc structures have privelleged data that should not be accessed by anything but the kernel and the process that owns it. 

5. If you replace `it.next()` with `it += PAGESIZE`, the QEMU will hang and not display the physical and virtual memory spaces. This is because incrementing by `PAGESIZE` is inefficient because it does not skip over gaps of in physical memory and will take a ridiculously long time to finish the task. QEMU, in response, hangs because it is taking `vmiter` too long to reach the console.

6. I referenced the solutions to solve this problem. I added some backtrace code to `memusage::refresh`, and then identified what functions were calling `kallocpage()`. One explanation for the unmarked pages in the memviewer is the `idle_task_`. The `idle_task_` is a process dedicated to a CPU that runs when there is no other process to be run. The idle task is the reason that adding more CPU's increases the number of unmarked pages (because there are more idle tasks!). The other explanation for the unmarked pages is the memviewer itself. In the backtraces provided by the staff we can see that `memusage::refresh` calls `kallocpage()` to aid in its tracking of the memory usage map. 

7. The code added to solve the unmarked pages problem is below:
	`for (int i = 0; i < ncpu; ++i) {
	    if (cpus[i].idle_task_) {
	        mark(ka2pa(cpus[i].idle_task_), f_kernel);
	    }
	}
	mark(ka2pa(v_), f_kernel);`

#PART C
1. Made sure to check for valid addresses from the user's arguments, then mapped the console using `vmiter`. 

#PART D
1. I made sure to hold the `ptable_lock` while doing any reads-writes to the ptable. I first found an available process pid, and then allocated a page for the proc struct and the initial pagetable. I then mapped all of the memory that the parent had to the child (making sure to handle for the console), and copied the registers. After that, I enqueued the new process on the CPU's runq_.

#PART E
1. The entry points are listed in described below:
	a) `process_main()` is *correctly* aligned
    	- entry point jumping from assembly to user processes

	b) `kernel_start()` is *correctly* aligned
    	- called after the bootloader loads the kernel

	c) `proc::exception(regstate*)` is *correctly* aligned
    	- called when an exception is thrown

	d) `proc::syscall(regstate*)` is *correctly* aligned
    	- called when a process makes a system call

	e) `cpustate::schedule(proc*)` is *incorrectly* aligned
    	- called when a process yields and the kernel calls cpu scheduler

	f) `cpustate::init_ap()` is *incorrectly* aligned
    	- called when a processor is being initialized

	g) `idle()` is *correctly* aligned
    	- entry point jumping from assembly to kernel tasks

2. I have added asserts to all of the above functions (exempting process_main of course). 

3. I would like to note that I am working off the original handout, so I changed `proc::yield` assembly that mirrors code in a future commit to help solve this problem. Other than that, fixing these alignment problems essentially came from subtracting values from the stack pointer as necessary. 

#PART F
1. 

Grading notes
-------------
