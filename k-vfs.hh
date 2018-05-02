#ifndef CHICKADEE_K_VFS_HH
#define CHICKADEE_K_VFS_HH
#include "k-wait.hh"
#include "k-devices.hh"
#define NFDS   256
#define BUFSZ  16

struct fdtable;
struct file;
struct vnode;
struct bbuffer;

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

    bool pread_;
    bool pwrite_;

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

// vnode subclass for standard - (in, out, err)
struct vnode_ioe : vnode {
	const char* filename_ = "keyboard/console";

	void deref() override;

	size_t read(uintptr_t buf, size_t sz, file* fl) override;
	size_t write(uintptr_t buf, size_t sz, file* fl) override;

	static vnode_ioe v_ioe;
};

// vnode subclass for pipes
struct vnode_pipe : vnode {
	const char* filename_ = "pipe";

	size_t read(uintptr_t buf, size_t sz, file* fl) override;
	size_t write(uintptr_t buf, size_t sz, file* fl) override;
};

struct vnode_memfile : vnode {
	vnode_memfile(memfile* m) : m_(m) { filename_ = m->name_; };

	size_t read(uintptr_t buf, size_t sz, file* fl) override;
	size_t write(uintptr_t buf, size_t sz, file* fl) override;

    private:
        memfile* m_;
};
 
struct bbuffer {
    bbuffer() : pos_(0), len_(0) {};

    spinlock lock_;
    char buf_[BUFSZ];
    size_t pos_;
    size_t len_;
};

#endif
