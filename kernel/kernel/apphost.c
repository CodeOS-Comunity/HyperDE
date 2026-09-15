#include "apphost.h"
#include "sched.h"
#include "kprintf.h"
#include "elf.h"
#include "process.h"
#include "umode.h"
#include "string.h"

extern uint64_t syscall_kernel_rsp;

#define APPHOST_OUT_SIZE (64 * 1024)
#define APPHOST_IN_SIZE  (4 * 1024)

enum { AH_IDLE = 0, AH_RUNNING, AH_EXITED };

static volatile int ah_state = AH_IDLE;
static volatile int ah_exit_status;
static volatile int ah_kill;

static uint8_t ah_out[APPHOST_OUT_SIZE];
static volatile int ah_out_head;
static volatile int ah_out_tail;
static uint8_t ah_in[APPHOST_IN_SIZE];
static volatile int ah_in_head;
static volatile int ah_in_tail;

static char ah_path[256];

static inline void ah_cli(void) { __asm__ volatile("cli" : : : "memory"); }
static inline void ah_sti(void) { __asm__ volatile("sti" : : : "memory"); }

static void apphost_thread_done(void);

/* ── stdout capture ring ── */

int apphost_write_out(const uint8_t *buf, int len) {
    int accepted = 0;
    ah_cli();
    for (int i = 0; i < len; i++) {
        int next = (ah_out_tail + 1) % APPHOST_OUT_SIZE;
        if (next == ah_out_head) break;
        ah_out[ah_out_tail] = buf[i];
        ah_out_tail = next;
        accepted++;
    }
    ah_sti();
    return accepted;
}

int apphost_drain(uint8_t *buf, int max) {
    int n = 0;
    ah_cli();
    while (n < max && ah_out_head != ah_out_tail) {
        buf[n++] = ah_out[ah_out_head];
        ah_out_head = (ah_out_head + 1) % APPHOST_OUT_SIZE;
    }
    ah_sti();
    return n;
}

/* ── stdin injection ring ── */

int apphost_write_in(const uint8_t *buf, int len) {
    int accepted = 0;
    ah_cli();
    for (int i = 0; i < len; i++) {
        int next = (ah_in_tail + 1) % APPHOST_IN_SIZE;
        if (next == ah_in_head) break;
        ah_in[ah_in_tail] = buf[i];
        ah_in_tail = next;
        accepted++;
    }
    ah_sti();
    return accepted;
}

int apphost_read_in(uint8_t *buf, int max) {
    int n = 0;
    if (max <= 0) return 0;
    ah_cli();
    if (ah_kill) {
        ah_kill = 0;
        buf[n++] = APPHOST_QUIT_BYTE;
    }
    while (n < max && ah_in_head != ah_in_tail) {
        buf[n++] = ah_in[ah_in_head];
        ah_in_head = (ah_in_head + 1) % APPHOST_IN_SIZE;
    }
    ah_sti();
    return n;
}

void apphost_kill(void) {
    ah_cli();
    ah_kill = 1;
    ah_sti();
}

/* ── launch / lifecycle ── */

static void apphost_thread_main(void) {
    uint64_t entry, stack;
    elf_auxv_info_t auxv;

    if (elf_load(ah_path, &entry, &stack, &auxv) < 0) {
        kprintf("apphost: failed to load %s\n", ah_path);
        ah_exit_status = -1;
        ah_state = AH_EXITED;
        sched_exit(1);
        return;
    }

    uint64_t rsp = elf_setup_stack(stack, entry, 0, 0, 0, 0, &auxv);
    kprintf("apphost: starting '%s' entry=0x%lx rsp=0x%lx\n", ah_path, entry, rsp);

    current_process = 0;
    proc_create(ah_path, entry, stack);

    user_mode_set_return(apphost_thread_done);
    user_mode_begin();
    thread_t *cur = sched_current();
    if (cur && cur->syscall_stack_top)
        syscall_kernel_rsp = (uint64_t)cur->syscall_stack_top;
    user_mode_enter(entry, rsp);
    /* not reached: user_mode_enter only returns via user_mode_force_return
     * (see apphost_thread_done) */
}

static void apphost_thread_done(void) {
    ah_exit_status = user_mode_last_exit_status();
    kprintf("apphost: '%s' exited with status %d\n", ah_path, ah_exit_status);
    current_process = 0;
    proc_reap();
    ah_state = AH_EXITED;
    sched_exit(ah_exit_status);
}

int apphost_launch(const char *path) {
    if (ah_state == AH_RUNNING) return -1;
    if (!path || !path[0]) return -1;

    strncpy_safe(ah_path, path, sizeof(ah_path));
    ah_state = AH_RUNNING;
    ah_exit_status = 0;
    ah_kill = 0;
    ah_out_head = ah_out_tail = 0;
    ah_in_head = ah_in_tail = 0;

    int tid = sched_create_thread("apphost", apphost_thread_main);
    if (tid < 0) {
        ah_state = AH_IDLE;
        kprintf("apphost: failed to create thread\n");
        return -1;
    }
    return 0;
}

int apphost_active(void) { return ah_state == AH_RUNNING; }
int apphost_exited(void) { return ah_state == AH_EXITED; }
int apphost_exit_status(void) { return ah_exit_status; }

void apphost_clear(void) {
    ah_cli();
    ah_out_head = ah_out_tail = 0;
    ah_in_head = ah_in_tail = 0;
    ah_sti();
}
