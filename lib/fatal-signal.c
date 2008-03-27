#include "fatal-signal.h"
#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "util.h"

/* Signals to catch. */
static const int fatal_signals[] = { SIGTERM, SIGINT, SIGHUP };

/* Signals to catch as a sigset_t. */
static sigset_t fatal_signal_set;

/* Hooks to call upon catching a signal */
struct hook {
    void (*func)(void *aux);
    void *aux;
};
#define MAX_HOOKS 32
static struct hook hooks[MAX_HOOKS];
static size_t n_hooks;

/* Number of nesting signal blockers. */
static int block_level = 0;

/* Signal mask saved by outermost signal blocker. */
static sigset_t saved_signal_mask;

static void call_sigprocmask(int how, sigset_t* new_set, sigset_t* old_set);
static void signal_handler(int sig_nr);

/* Registers 'hook' to be called when a process termination signal is
 * raised. */
void
fatal_signal_add_hook(void (*func)(void *aux), void *aux)
{
    fatal_signal_block();
    assert(n_hooks < MAX_HOOKS);
    hooks[n_hooks].func = func;
    hooks[n_hooks].aux = aux;
    n_hooks++;
    fatal_signal_unblock();
}

/* Blocks program termination signals until fatal_signal_unblock() is called.
 * May be called multiple times with nesting; if so, fatal_signal_unblock()
 * must be called the same number of times to unblock signals.
 *
 * This is needed while adjusting a data structure that will be accessed by a
 * fatal signal hook, so that the hook is not invoked while the data structure
 * is in an inconsistent state. */
void
fatal_signal_block()
{
    static bool inited = false;
    if (!inited) {
        size_t i;

        inited = true;
        sigemptyset(&fatal_signal_set);
        for (i = 0; i < ARRAY_SIZE(fatal_signals); i++) {
            int sig_nr = fatal_signals[i];
            sigaddset(&fatal_signal_set, sig_nr);
            if (signal(sig_nr, signal_handler) == SIG_IGN) {
                signal(sig_nr, SIG_IGN);
            }
        }
    }

    if (++block_level == 1) {
        call_sigprocmask(SIG_BLOCK, &fatal_signal_set, &saved_signal_mask);
    }
}

/* Unblocks program termination signals blocked by fatal_signal_block() is
 * called.  If multiple calls to fatal_signal_block() are nested,
 * fatal_signal_unblock() must be called the same number of times to unblock
 * signals. */
void
fatal_signal_unblock()
{
    assert(block_level > 0);
    if (--block_level == 0) {
        call_sigprocmask(SIG_SETMASK, &saved_signal_mask, NULL);
    }
}

static char **files;
static size_t n_files, max_files;

static void unlink_files(void *aux);
static void do_unlink_files(void);

/* Registers 'file' to be unlinked when the program terminates via exit() or a
 * fatal signal. */
void
fatal_signal_add_file_to_unlink(const char *file)
{
    static bool added_hook = false;
    if (!added_hook) {
        added_hook = true;
        fatal_signal_add_hook(unlink_files, NULL);
        atexit(do_unlink_files);
    }

    fatal_signal_block();
    if (n_files >= max_files) {
        max_files = max_files * 2 + 1;
        files = xrealloc(files, sizeof *files * max_files);
    }
    files[n_files++] = xstrdup(file);
    fatal_signal_unblock();
}

/* Unregisters 'file' from being unlinked when the program terminates via
 * exit() or a fatal signal. */
void
fatal_signal_remove_file_to_unlink(const char *file)
{
    size_t i;

    fatal_signal_block();
    for (i = 0; i < n_files; i++) {
        if (!strcmp(files[i], file)) {
            free(files[i]);
            files[i] = files[--n_files];
            break;
        }
    }
    fatal_signal_unblock();
}

static void
unlink_files(void *aux UNUSED)
{
    do_unlink_files();
}

static void
do_unlink_files(void)
{
    size_t i;

    for (i = 0; i < n_files; i++) {
        unlink(files[i]);
    }
}

static void
call_sigprocmask(int how, sigset_t* new_set, sigset_t* old_set)
{
    int error = sigprocmask(how, new_set, old_set);
    if (error) {
        fprintf(stderr, "sigprocmask: %s\n", strerror(errno));
    }
}

static void
signal_handler(int sig_nr)
{
    volatile sig_atomic_t recurse = 0;
    if (!recurse) {
        size_t i;

        recurse = 1;

        /* Call all the hooks. */
        for (i = 0; i < n_hooks; i++) {
            hooks[i].func(hooks[i].aux);
        }
    }

    /* Re-raise the signal with the default handling so that the program
     * termination status reflects that we were killed by this signal */
    signal(sig_nr, SIG_DFL);
    raise(sig_nr);
}