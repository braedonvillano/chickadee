#ifndef CHICKADEE_K_VFS_HH
#define CHICKADEE_K_VFS_HH
#define NFDS 256

struct fdtable;
struct file;
struct vnode;

// we are going to write some sort of constructor
struct fdtable {
	fdtable() : refs_(1), table_{nullptr} {
		lock_.clear();
	};

	spinlock lock_;
	int refs_;           // for threading?
	file* table_[NFDS];
};

// make a fdtable, pass it to init_user
fdtable* kalloc_fdtable() __attribute__((malloc));

struct file {
	file() : pread_(true), pwrite_(true), refs_(1), off_(0) {
		lock_.clear();
	};

	enum type_t { normal = 0, pipe, stream };
    type_t type_; 

    const bool pread_;
    const bool pwrite_;

    vnode* vnode_;

    spinlock lock_;  	 // protects data below it
    int refs_;
    off_t off_;

    void adref();
    void deref();
};

// make a fdtable, pass it to init_user
file* kalloc_file() __attribute__((malloc));

struct vnode {
	vnode() : pseek_(false), refs_(1) {
		lock_.clear();
	};

	const char* filename_;
	const bool pseek_;

	spinlock lock_;      // protects the refs below
	int refs_;

	void adref();
    virtual void deref();

	virtual size_t read(uintptr_t buf, size_t sz, off_t& off);
	virtual size_t write(uintptr_t buf, size_t sz, off_t& off);
};

// vnode subclass for standard - (in, out, err)
struct vnode_ioe : vnode {
	const char* filename = "keyboard/console";

	void deref() override;

	size_t read(uintptr_t buf, size_t sz, off_t& off) override;
	size_t write(uintptr_t buf, size_t sz, off_t& off) override;

	static vnode_ioe v_ioe;
};

// vnode subclass for pipes
struct vnode_pipe : vnode {
	const char* filename = "pipes";

	size_t read(uintptr_t buf, size_t sz, off_t& off) override;
	size_t write(uintptr_t buf, size_t sz, off_t& off) override;
};

#endif
