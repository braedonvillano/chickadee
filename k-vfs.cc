#include "kernel.hh"
#include "k-vfs.hh"
#include "k-devices.hh"

vnode_ioe vnode_ioe::v_ioe;

// function definition for keyboard/console vnode subclass
//		read/write do not need a buffer b/c the data inputted
// 		and read is consumed immediatly, these resemble the originals

size_t vnode::read(uintptr_t buf, size_t sz, size_t& off) {
	return 0;
};

size_t vnode::write(uintptr_t buf, size_t sz, size_t& off) {
	return 0;
};


// function definition for keyboard/console vnode subclass
//		read/write do not need a buffer b/c the data inputted
// 		and read is consumed immediatly, these resemble the originals

size_t vnode_ioe::read(uintptr_t buf, size_t sz, size_t& off) {
	auto& kbd = keyboardstate::get();
    auto irqs = kbd.lock_.lock();

    // mark that we are now reading from the keyboard
    // (so `q` should not power off)
    if (kbd.state_ == kbd.boot) {
        kbd.state_ = kbd.input;
    }
    
    // block until a line is available
    waiter(current()).block_until(kbd.wq_, [&] () {
            return sz == 0 || kbd.eol_ != 0;
        }, kbd.lock_, irqs);

    // read that line or lines
    size_t n = 0;
    while (kbd.eol_ != 0 && n < sz) {
        if (kbd.buf_[kbd.pos_] == 0x04) {
            // Ctrl-D means EOF
            if (n == 0) {
                kbd.consume(1);
            }
            break;
        } else {
            *reinterpret_cast<char*>(buf) = kbd.buf_[kbd.pos_];
            ++buf;
            ++n;
            kbd.consume(1);
        }
    }

    kbd.lock_.unlock(irqs);
    return n;

    // return 0;
}

size_t vnode_ioe::write(uintptr_t buf, size_t sz, size_t& off) {
	auto& csl = consolestate::get();
    auto irqs = csl.lock_.lock();

    size_t n = 0;
    while (n < sz) {
        int ch = *reinterpret_cast<const char*>(buf);
        ++buf;
        ++n;
        console_printf(0x0F00, "%c", ch);
    }

    csl.lock_.unlock(irqs);
    return n;

	// return 0;
}



