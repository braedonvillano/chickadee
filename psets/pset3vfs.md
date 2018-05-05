CS 161 Problem Set 3 VFS Design Document
========================================
DESIGN DOCUMENT

# VFS LAYER OBJECTS

1. `fdtable`: file descriptor table, an array in the proc struct that point to files and
organize the open files of a given process.

2. `file`: an abstraction that stores process specific information about a file; like 
permissions and file offset.

3. `vnode`: layer of VFS that performs reads and writes according to the type of file 
described by its members. 
	a. `vnode_ioe`: interfaces directly with keyboard and console hardware to produce 
	read and write functions for standard-in, standard-out, and standard-err.
	b. `vnode_pipe`: specifically manages read and write calls to pipes. 
	c. `vnode_memfile`: handles read and write operations for files loaded into memory.

4. `bbuffer`: facilitates the communication between the read and write ends of a pipe, and
is the primary mechanism used to perform pipe read an writes. 

# INTERFACE

```cpp
struct fdtable {
	fdtable() : refs_(1), table_{nullptr} {
		lock_.clear();
	};

	spinlock lock_;
	int refs_;           // for threading?
	file* table_[NFDS];
};
```
The fdtable requires `refs_` for potential threading. The lock protects the refs, as well as
accessing a certain file from a table (I decided to enforce this because it felt like a safe bet).


```cpp
struct file {
	file() : pread_(true), pwrite_(true), refs_(1), off_(0) {
		lock_.clear();
	};

	enum type_t { normal = 0, pipe, stream };
    type_t type_; 

    bool pread_;
    bool pwrite_;

    vnode* vnode_;

    spinlock lock_;  	 // protects data below it
    int refs_;
    off_t off_;

    void adref();
    void deref();
};
```
The `type_t` member specifies the general file type, which can really be a pipe or a stream
(keyboard/console). The booleans `pread_` and `pwrite_` specify the permissions on this particular
file. `vnode_` points to the vnode struct that is associated to do the actually read and writes. 
Because multiple processes and fdtable entries can reference the same file, I have included a refs
member as well as a lock that must be called to modify it. Locking the offset could be used when 
implementing threads. The `deref` and `adref` functions are used to modify `refs_` if a new file
descriptor starts pointing to the struct or if a previous one stops pointing. The refs are protected
by the lock.


```cpp
struct vnode {
	vnode() : pseek_(false), readers_(1), writers_(1), bb_(nullptr), refs_(1) {
		lock_.clear();
	};

	char* filename_;
	bool pseek_;

	int readers_;
    int writers_;

    wait_queue wq_;

    bbuffer* bb_;

	spinlock lock_;      // protects the refs below
	int refs_;

	void adref();
    virtual void deref();
    void deref(bool flag);

	virtual size_t read(uintptr_t buf, size_t sz, file* fl);
	virtual size_t write(uintptr_t buf, size_t sz, file* fl);
};
```
This is the general (superclass) vnode struct. There are reader and writer integers that are used for
pipes. They specify how many files on the read and write ends, that way, if there are no remaining
readers or writers the pipe can close as no new information will be echanged. `bb_` points to an 
underlying bounded buffer implementation defined below. The `wq_` member is a wait-queue where read 
or write calls can block (for pipes for now). The `deref` and `adref` functions are used to modify 
`refs_` if a new file starts pointing to the struct or if a previous one stops pointing. 


```cpp
struct vnode_ioe : vnode {
	const char* filename_ = "keyboard/console";

	void deref() override;

	size_t read(uintptr_t buf, size_t sz, file* fl) override;
	size_t write(uintptr_t buf, size_t sz, file* fl) override;

	static vnode_ioe v_ioe;
};
```
This is a vnode subclass that interfaces directly with the keyboard and console hardware in order 
to perform the reads and writes for standard-in, standard-out, and standard-err. 


```cpp
struct vnode_pipe : vnode {
	const char* filename_ = "pipe";

	size_t read(uintptr_t buf, size_t sz, file* fl) override;
	size_t write(uintptr_t buf, size_t sz, file* fl) override;
};
```
This is a vnode subclass that interfaces with the bounded buffer defined below in order to facilitate
pipe communication.


```cpp
struct vnode_memfile : vnode {
	vnode_memfile(memfile* m) : m_(m) { filename_ = m->name_; };

	size_t read(uintptr_t buf, size_t sz, file* fl) override;
	size_t write(uintptr_t buf, size_t sz, file* fl) override;

    private:
        memfile* m_;
};
```
This is a vnode subclass that interfaces with the data specified by a memfile.

```cpp
struct bbuffer {
    bbuffer() : pos_(0), len_(0) {};

    spinlock lock_;
    char buf_[BUFSZ];
    size_t pos_;
    size_t len_;
};
```
This is used in the 'pipe' subclass of vnodes. It is comprised of a `lock_` (which should be held for
all operations), a buffer for data, the current size of the data in the buffer, and the position of 
the next read in the buffer. 


```cpp
#define NFDS   256
#define BUFSZ  16

spinlock memfile_lock;

fdtable* kalloc_fdtable() __attribute__((malloc));
file* kalloc_file() __attribute__((malloc));
```
These are some misc. definitions also used in my VFS implementation.



# PERMISSIONS

1. By default a file is constructed with the read and write permissions set to true. In the case of a 
pipe, we must ensure that these be changed such that one end of the pipe only corresponds to reads and
the other end only corresponds to writes.

2. The `pseek_` member specifies whether a file is seekable or not. For these preliminary implemenations
of files in Chickadee, most will be unseekable.



# SYNCHRONIZATION

1. For all of `fdtable`, `file`, and `vnode` the `refs_` must be accurately managed with the `lock_` member
held in order to ensure that nothing disappears or is freed prematurely. Symmetrically, it helps ensure no
race condition cause a memory leak. 

2. I enforce that the fdtable lock be held when accessing any of its underlying files. 

3. The vnode lock should also be used to handle decrementing references (for file freeing), and primarily for
pipe read and write implementations. We must ensure that data changes in a pipe are mutually exclusive.

4. Blocking occurs for the `vnode_ioe` implementations, and for the `vnode_pipe` implementation. In the pipe a
writer must block until there is more space in the pipe, and a reader must block until there is more data in the
pipe. They must both cease blocking if there are no more readers or writers. 

5. Memfiles must lock themselves to enforce synchronosity for reads and writes, and the `memfile_lock` in the 
misc. section above must be held to interface with the `initfs` array. 



