#ifndef APPHOST_H
#define APPHOST_H

#include "types.h"

/* Launch a userspace ELF on a dedicated scheduler thread (ring 3). The
 * caller (Qt desktop) stays a kernel thread and can keep draining the
 * captured stdout / injecting stdin while the app runs. Only one hosted
 * app may run at a time. Returns 0 on success, -1 on failure. */
int  apphost_launch(const char *path);

int  apphost_active(void);
int  apphost_exited(void);
int  apphost_exit_status(void);

/* stdout capture (written from syscall WRITE, drained by the host window) */
int  apphost_write_out(const uint8_t *buf, int len);
int  apphost_drain(uint8_t *buf, int max);

/* stdin injection (written by the host window, read by syscall READ fd 0).
 * When apphost_kill() has been requested, the next read returns a single
 * 0xFF byte so the app can notice and exit cleanly. */
int  apphost_write_in(const uint8_t *buf, int len);
int  apphost_read_in(uint8_t *buf, int max);
void apphost_kill(void);

void apphost_clear(void);

#define APPHOST_QUIT_BYTE 0xFF

#endif
