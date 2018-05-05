#include "kernel.hh"
#include "k-vfs.hh"
#include "k-devices.hh"

vnode_ioe vnode_ioe::v_ioe;

void file::adref() {
	auto irqs = lock_.lock();
	refs_++;
	lock_.unlock(irqs);
}

void file::deref() {
	auto irqs = lock_.lock();
	refs_--;
	if (!refs_) {
        if (type_ == file::pipe) {
            vnode_->deref(pwrite_);
        } else {
            vnode_->deref();
        }
		kfree(this);
	}
	lock_.unlock(irqs);
}

void vnode::adref() {
	auto irqs = lock_.lock();
	refs_++;
	lock_.unlock(irqs);
}

void vnode::deref() {
	refs_--;
	if (!refs_) {
        kfree(bb_);
		kfree(this);
	}
}

void vnode::deref(bool flag) {
    auto irqs = lock_.lock();
    if (flag) {
        writers_--;
    } else {
        readers_--;
    }
    if (!writers_ || !readers_) {
        wq_.wake_all();
    }
    refs_--; 
    if (!refs_) {
        kfree(bb_);
		kfree(this);
	}
	lock_.unlock(irqs);

}

// function definition for keyboard/console vnode subclass
//		read/write do not need a buffer b/c the data inputted
// 		and read is consumed immediatly, these resemble the originals

size_t vnode::read(uintptr_t buf, size_t sz, file* fl) {
	return 0;
};

size_t vnode::write(uintptr_t buf, size_t sz, file* fl) {
	return 0;
};


// function definition for keyboard/console vnode subclass
//		read/write do not need a buffer b/c the data inputted
// 		and read is consumed immediatly, these resemble the originals

void vnode_ioe::deref() {
	auto irqs = lock_.lock();
	refs_--;
	assert(refs_ >= 0);
	lock_.unlock(irqs);
}

size_t vnode_ioe::read(uintptr_t buf, size_t sz, file* fl) {
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
}

size_t vnode_ioe::write(uintptr_t buf, size_t sz, file* fl) {
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
}

size_t vnode_pipe::read(uintptr_t buf, size_t sz, file* fl) {
    auto irqs = lock_.lock();

    waiter(current()).block_until(wq_, [&] () { 
        return bb_->len_ > 0 || !writers_; }, lock_, irqs);

    if (!writers_) {
        fl->deref();
        wq_.wake_all();
        lock_.unlock(irqs);
        return 0;
    }

    size_t pos = 0;
    while (pos < sz && bb_->len_ > 0) {
        size_t ncopy = sz - pos;
        if (ncopy > BUFSZ - bb_->pos_) {
            ncopy = BUFSZ - bb_->pos_;
        }
        if (ncopy > bb_->len_) {
            ncopy = bb_->len_;
        }
        memcpy(&((char*) buf)[pos], &bb_->buf_[bb_->pos_], ncopy);
        bb_->pos_ = (bb_->pos_ + ncopy) % BUFSZ;
        bb_->len_ -= ncopy;
        pos += ncopy;
    }
    wq_.wake_all();
    lock_.unlock(irqs);

    return pos;
}

size_t vnode_pipe::write(uintptr_t buf, size_t sz, file* fl) {
    auto irqs = lock_.lock();

    waiter(current()).block_until(wq_, [&] () { 
        return bb_->len_ < BUFSZ || !readers_; }, lock_, irqs);

    if (!readers_) {
        fl->deref();            // this should handle marking down the file, which should handle the vnode
        wq_.wake_all();         // hoping this will cause a chain reaction of decrementing references
        lock_.unlock(irqs);
        return E_PIPE;
    }

    size_t pos = 0;
    while (pos < sz && bb_->len_ < BUFSZ) {
        size_t bindex = (bb_->pos_ + bb_->len_) % BUFSZ;
        size_t ncopy = sz - pos;
        if (ncopy > BUFSZ - bindex) {
            ncopy = BUFSZ - bindex;
        }
        if (ncopy > BUFSZ - bb_->len_) {
            ncopy = BUFSZ - bb_->len_;
        }
        memcpy(&bb_->buf_[bindex], &((char*) buf)[pos], ncopy);
        pos += ncopy;
        bb_->len_ += ncopy;
    }
    wq_.wake_all();
    lock_.unlock(irqs);

    return pos;
}

size_t vnode_memfile::read(uintptr_t buf, size_t sz, file* fl) {
    size_t off = fl->off_;
    char* src = (char*) m_->data_;
    size_t ncopy = sz; 

    if (sz + off > m_->len_) {
        ncopy = m_->len_ - off;
    }
    memcpy(((char*) buf), src + off, ncopy);
    fl->off_ += ncopy;

    return ncopy;
};

size_t vnode_memfile::write(uintptr_t buf, size_t sz, file* fl) {
    size_t off = fl->off_;
    char* des = (char*) m_->data_;
    size_t ncopy = sz;

    if (sz > m_->len_ - off) {
        ncopy = m_->len_ - off;
    }
    memcpy(des + off, ((char*) buf), ncopy);
    fl->off_ += ncopy;

    return ncopy;
};