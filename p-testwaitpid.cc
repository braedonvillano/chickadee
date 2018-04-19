#include "p-lib.hh"

static const int order[] = {
    2, 7, 6, 4, 1, 8, 5, 3
};

static void make_children(pid_t* children) {
    for (size_t i = 0; i != arraysize(order); ++i) {
        pid_t p = sys_fork();
        if (p == 0) {
            sys_msleep(order[i] * 100);
            pid_t r = sys_getpid();
            // if (r == )
            // console_printf("pid %d is about to exit\n", r);
            sys_exit(order[i]);
        }
        assert_gt(p, 0);
        children[i] = p;
    }
}

void process_main() {
    sys_kdisplay(KDISPLAY_NONE);

    console_printf("waitpid(0, W_NOHANG) tests...\n");
    pid_t children[arraysize(order)];
    make_children(children);
    for (size_t i = 0; i != arraysize(order); ++i) {
        pid_t ch;
        int status = 0;
        while ((ch = sys_waitpid(0, &status, W_NOHANG)) == E_AGAIN) {
            sys_yield();
        }
        assert_gt(ch, 0);

        size_t idx = 0;
        while (idx != arraysize(order) && children[idx] != ch) {
            ++idx;
        }
        assert(idx < arraysize(order));
        children[idx] = 0;

        console_printf("%d @%lu: exit status %d\n", ch, idx, status);
        assert_eq(order[idx], status);
    }
    assert_eq(sys_waitpid(0, nullptr, W_NOHANG), E_CHILD);
    console_printf("waitpid(0, W_NOHANG) tests succeed.\n");


    console_printf("waitpid(pid, W_NOHANG) tests...\n");
    make_children(children);
    for (size_t i = 0; i != arraysize(order); ++i) {
        pid_t ch;
        int status = 0;
        while ((ch = sys_waitpid(children[i], &status, W_NOHANG)) == E_AGAIN) {
            sys_yield();
        }
        assert_eq(ch, children[i]);

        console_printf("%d @%lu: exit status %d\n", ch, i, status);
        assert_eq(order[i], status);
    }
    assert_eq(sys_waitpid(0, nullptr, W_NOHANG), E_CHILD);
    console_printf("waitpid(pid, W_NOHANG) tests succeed.\n");


    console_printf("waitpid(0) blocking tests...\n");
    make_children(children);
    for (size_t i = 0; i != arraysize(order); ++i) {
        int status = 0;
        pid_t ch = sys_waitpid(0, &status);
        assert_gt(ch, 0);

        size_t idx = 0;
        while (idx != arraysize(order) && children[idx] != ch) {
            ++idx;
        }
        assert(idx < arraysize(order));
        children[idx] = 0;

        console_printf("%d @%lu: exit status %d\n", ch, idx, status);
        assert_eq(order[idx], status);
    }
    assert_eq(sys_waitpid(0), E_CHILD);
    console_printf("waitpid(0) blocking tests succeed.\n");

    int count = 0;
    console_printf("waitpid(pid) blocking tests...\n");
    make_children(children);
    console_printf("the children are:");
    for (unsigned int i = 0; i < arraysize(order); i++) {
        console_printf(" %d", children[i]);
    }
    console_printf("\n");
    for (size_t i = 0; i != arraysize(order); ++i) {
        int status = 0;
        pid_t ch = sys_waitpid(children[i], &status);
        console_printf("iteration %d with pid: %d and status: %d\n", count, children[i], order[i]);
        assert_eq(ch, children[i]);
        // console_printf("what in the ass is children: %d\n", children[i]);


        console_printf("%d @%lu: exit status %d\n", ch, i, status);
        assert_eq(order[i], status);
        count++;
    }
    assert_eq(sys_waitpid(0), E_CHILD);
    console_printf("waitpid(pid) blocking tests succeed.\n");


    console_printf("testwaitpid succeeded.\n");
    sys_exit(0);
}
