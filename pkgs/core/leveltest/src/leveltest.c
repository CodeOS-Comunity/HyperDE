/* leveltest -- verify that the security level actually gates the kill syscall.
 *
 * The `systemm task` shell builtin is not what enforces the level rule; the
 * kill syscall handler is.  This program calls sys_kill() the way any
 * ordinary program would, so it exercises the real path.  Run it at different
 * levels with `systemm task run --level N /bin/leveltest <pid>` and the
 * results should differ, which is the whole point.
 *
 * Signal 0 is used throughout: it performs the permission check and delivers
 * nothing, so the test cannot kill the machine by mistyping a pid.
 */
#include "unistd.h"
#include "stdio.h"
#include "string.h"

int main(int argc, char **argv) {
    int me = sys_getpid();
    printf("leveltest: pid %d\n", me);

    /* Self-signal. Must always be allowed: a task may act on its own level,
     * and this is the case that would break first if the rule were
     * accidentally strict (>) instead of inclusive (>=). */
    int r_self = sys_kill(me, 0);
    printf("self   kill(%d,0) = %d  %s\n", me, r_self,
           r_self == 0 ? "ALLOWED" : "REFUSED");

    /* A named target, if one was given. */
    if (argc >= 2) {
        int target = 0;
        for (const char *p = argv[1]; *p; p++) {
            if (*p < '0' || *p > '9') break;
            target = target * 10 + (*p - '0');
        }
        if (target <= 0) {
            printf("target: '%s' is not a pid\n", argv[1]);
            return 2;
        }
        int r = sys_kill(target, 0);
        printf("target kill(%d,0) = %d  %s\n", target, r,
               r == 0 ? "ALLOWED" : "REFUSED");

        /* -1 is the kernel's generic failure; the syscall layer maps a
         * refusal to -EPERM (1) and a missing pid to -ESRCH (3), so the
         * value distinguishes "you may not" from "it is not there". */
        if (r == 0)        printf("  -> permitted\n");
        else if (r == -1)  printf("  -> refused (-EPERM) or no such task\n");
        else if (r == -3)  printf("  -> no such task (-ESRCH)\n");
        else if (r == -22) printf("  -> bad pid (-EINVAL)\n");
        else               printf("  -> rc=%d\n", r);
    }

    /* A pid that certainly does not exist, to pin down the ESRCH path. */
    int r_missing = sys_kill(9999, 0);
    printf("absent kill(9999,0) = %d  %s\n", r_missing,
           r_missing == -3 ? "ESRCH" : (r_missing == 0 ? "ALLOWED(BUG)" : "other"));
    return 0;
}
