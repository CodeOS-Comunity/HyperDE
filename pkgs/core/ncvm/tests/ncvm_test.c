/* ncvm_test.c — host tests for the in-guest ncvm backend.
 *
 *   make ncvm-check
 *
 * pkgs/core/ncvm/src/ncvm.c reaches the kernel through inline `int $0x80`
 * stubs, so it cannot be tested on the host as it stands.  This file
 * includes it directly (with main() renamed) against tests/codeos_shim.h,
 * which maps every syscall it uses onto POSIX.  Including the .c is
 * deliberate: the functions worth testing -- valid_name(), is_digits(),
 * translate_vm_cmd(), write_file() -- are all static, and the alternative
 * is exporting them purely for the test's benefit.
 *
 * The shim models the *contract*, not the convenience: CodeOS's open()
 * flags are the Linux values, so a missing O_CREAT fails here exactly as
 * it does in the guest.
 */

#include "codeos_shim.h"

/* ncvm.c reports through printf(); capture it so tests can assert on the
 * "ncvm: ..." lines rather than scraping the terminal. */
#define printf shim_printf
#define main ncvm_main
#include "../src/ncvm.c"
#undef main
#undef printf

/* ── tiny test rig ────────────────────────────────────────────────────── */
static int checks, failed;
static char first_fail[256];

static void ok(int cond, const char *what) {
    checks++;
    if (cond) return;
    failed++;
    if (!first_fail[0]) snprintf(first_fail, sizeof(first_fail), "%s", what);
    printf("  FAIL %s\n", what);
}

static void section(const char *name) { printf("== %s\n", name); }

/* ── scratch CMD_DIR, recompiled in via -DCMD_DIR ─────────────────────── */
static void scratch_reset(void) {
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s' && mkdir -p '%s'", CMD_DIR, CMD_DIR);
    if (system(cmd) != 0) { printf("scratch reset failed\n"); exit(2); }
}

static int file_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0;
}

/* Read a whole file with stdio rather than the code under test, so a
 * passing assertion means the bytes really landed on disk. */
static int slurp(const char *path, char *buf, int max) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    int n = (int)fread(buf, 1, (size_t)max - 1, f);
    buf[n] = 0;
    fclose(f);
    return n;
}

/* ── read_file ────────────────────────────────────────────────────────── */
static void test_read_file(void) {
    char path[256];
    char body[64];
    char back[64];
    section("read_file");

    snprintf(path, sizeof(path), "%s/rf.txt", CMD_DIR);
    ok(write_file(path, "hello") > 0, "write_file reports success");
    ok(file_exists(path), "write_file created the file");

    int n = read_file(path, back, sizeof(back));
    ok(n == 5 && !strcmp(back, "hello"), "read_file round-trips the content");

    /* Read it back with stdio too: a passing read_file could in principle
     * be reading its own cache rather than the file. */
    ok(slurp(path, back, sizeof(back)) == 5 && !strcmp(back, "hello"),
       "the bytes are really on disk, not just in read_file's view");

    ok(read_file(path, body, 2) == 1 && body[0] == 'h',
       "read_file honours a short buffer and still NUL-terminates");

    snprintf(path, sizeof(path), "%s/nope", CMD_DIR);
    ok(read_file(path, back, sizeof(back)) < 0, "read_file reports a missing file");
}

/* ── valid_name: the only thing standing between a VM name and a path ── */
static void test_valid_name(void) {
    section("valid_name");
    ok(valid_name("selftest.vm"), "accepts selftest.vm");
    ok(valid_name("a"), "accepts single char");
    ok(valid_name("vm1"), "accepts alnum");
    ok(valid_name("A-b_c.1"), "accepts mixed [A-Za-z0-9._-]");

    ok(!valid_name(""), "rejects empty");
    ok(!valid_name("."), "rejects bare dot");
    ok(!valid_name(".."), "rejects ..");
    ok(!valid_name(".hidden"), "rejects leading dot");
    ok(!valid_name("../evil"), "rejects traversal");
    ok(!valid_name("a/b"), "rejects slash");
    ok(!valid_name("a b"), "rejects space");
    ok(!valid_name("a$b"), "rejects shell metachar");
    ok(!valid_name(NULL), "rejects NULL");
}

/* ── is_digits ────────────────────────────────────────────────────────── */
static void test_is_digits(void) {
    section("is_digits");
    ok(is_digits("0"), "digits: 0");
    ok(is_digits("4096"), "digits: 4096");
    ok(!is_digits(""), "not digits: empty");
    ok(!is_digits("-1"), "not digits: -1");
    ok(!is_digits("1G"), "not digits: 1G");
    ok(!is_digits("1.5"), "not digits: 1.5");
    ok(!is_digits(" 1"), "not digits: leading space");
}

/* ── tokenize ─────────────────────────────────────────────────────────── */
static void test_tokenize(void) {
    section("tokenize");
    char a[] = "one  two\tthree";
    char *argv[8];
    int argc = tokenize(a, argv, 8);
    ok(argc == 3, "splits on runs of space and tab");
    ok(argv[0] && !strcmp(argv[0], "one"), "token 0");
    ok(argv[2] && !strcmp(argv[2], "three"), "token 2");

    char b[] = "   ";
    argc = tokenize(b, argv, 8);
    ok(argc == 0, "all-whitespace yields 0 args");

    char c[] = "a b c d e";
    argc = tokenize(c, argv, 3);
    ok(argc == 2, "respects the max argument limit");
}

/* ── translate_vm_cmd: crosvm command line -> ncvm/QEMU argv ─────────── */
#define T_MAX MAX_ARGS

/* Build the argv for one crosvm-style command line.  The returned array
 * points into static buffers, so it is only valid until the next call.
 * T_MAX must be >= MAX_ARGS: translate_vm_cmd() writes out[outc] as its
 * NUL terminator and indexes to MAX_ARGS-1, so a smaller array here would
 * be the harness's own overflow. */
static int translate(const char *cmd, const char *vmm, char *out[T_MAX]) {
    static char scratch[MAX_CMD_LEN];
    int outc = 0;
    snprintf(scratch, sizeof(scratch), "%s", cmd);
    /* Do NOT tokenize here: translate_vm_cmd() tokenizes the buffer itself
     * and tokenize() writes NULs in place, so a second pass would see a
     * one-word string and silently translate nothing. */
    synused = 0;              /* syn() is process-global; see the test below */
    translate_vm_cmd(out, &outc, scratch, vmm);
    return outc;
}

/* True when `tok` appears in out[0..outc). */
static int has_tok(char *out[], int outc, const char *tok) {
    for (int i = 0; i < outc; i++)
        if (out[i] && !strcmp(out[i], tok)) return 1;
    return 0;
}

/* Index of the first occurrence of `tok`, or -1. */
static int idx_tok(char *out[], int outc, const char *tok) {
    for (int i = 0; i < outc; i++)
        if (out[i] && !strcmp(out[i], tok)) return i;
    return -1;
}

static int count_tok(char *out[], int outc, const char *tok) {
    int n = 0;
    for (int i = 0; i < outc; i++)
        if (out[i] && !strcmp(out[i], tok)) n++;
    return n;
}

static void test_translate_basics(void) {
    char *out[T_MAX];
    int n;
    section("translate_vm_cmd");

    n = translate("crosvm run -m 512 -cpus 4", "/vmm", out);
    ok(n > 0 && !strcmp(out[0], "/vmm"), "argv[0] is the VMM");
    /* QEMU takes -m as two argv entries, and crosvm's -m is too, but the
     * translation collapses the pair into the single token "512M": the -m
     * flag is dropped and only the suffixed value survives.  QEMU accepts
     * a bare "512M" in that position, so this is not a runtime failure --
     * it is asserted as-is so a future change has to be deliberate. */
    ok(idx_tok(out, n, "-m") < 0 && has_tok(out, n, "512M"),
       "-m 512 collapses to a bare 512M token (no -m flag)");
    ok(idx_tok(out, n, "-smp") >= 0 && !strcmp(out[idx_tok(out, n, "-smp") + 1], "4"),
       "-cpus 4 becomes -smp 4");

    n = translate("crosvm run --kernel /boot/vmlinux", "/vmm", out);
    ok(idx_tok(out, n, "-kernel") >= 0 &&
       !strcmp(out[idx_tok(out, n, "-kernel") + 1], "/boot/vmlinux"),
       "--kernel becomes -kernel");

    n = translate("crosvm run --root /images/rootfs.img", "/vmm", out);
    ok(idx_tok(out, n, "-drive") >= 0 &&
       !strcmp(out[idx_tok(out, n, "-drive") + 1],
              "file=/images/rootfs.img,format=raw,if=virtio"),
       "--root FILE becomes -drive file=...,format=raw,if=virtio");

    n = translate("crosvm run --root /shared/dir/", "/vmm", out);
    ok(idx_tok(out, n, "-virtfs") >= 0 &&
       !strcmp(out[idx_tok(out, n, "-virtfs") + 1],
              "local,path=/shared/dir/,mount_tag=root,security_model=none"),
       "--root DIR/ becomes -virtfs local,path=...,mount_tag=root");

    n = translate("crosvm run --serial type=stdio", "/vmm", out);
    ok(idx_tok(out, n, "-serial") >= 0 && !strcmp(out[idx_tok(out, n, "-serial") + 1], "stdio"),
       "--serial type=stdio becomes -serial stdio");

    n = translate("crosvm run --serial type=file,path=/tmp/tty.log", "/vmm", out);
    ok(idx_tok(out, n, "-serial") >= 0 &&
       !strcmp(out[idx_tok(out, n, "-serial") + 1], "file:/tmp/tty.log"),
       "--serial type=file,path=P becomes -serial file:P");

    n = translate("crosvm run --rwdisk /images/d1", "/vmm", out);
    ok(idx_tok(out, n, "-drive") >= 0 &&
       !strcmp(out[idx_tok(out, n, "-drive") + 1],
              "file=/images/d1,format=raw,if=virtio"),
       "--rwdisk becomes -drive");
}

static void test_translate_drops_crosvm_only(void) {
    char *out[T_MAX];
    int n;
    section("translate_vm_cmd / dropped flags");

    n = translate("crosvm run --rng /dev/urandom --rng 42", "/vmm", out);
    ok(!has_tok(out, n, "--rng"), "--rng is dropped");
    ok(!has_tok(out, n, "/dev/urandom"), "--rng consumes its argument");
    ok(!has_tok(out, n, "42"), "--rng consumes a bare number argument too");

    n = translate("crosvm run --disable-sandbox --no-sandbox", "/vmm", out);
    ok(!has_tok(out, n, "--disable-sandbox"), "--disable-sandbox is dropped");
    ok(!has_tok(out, n, "--no-sandbox"), "--no-sandbox is dropped");
}

static void test_translate_accel_and_machine(void) {
    char *out[T_MAX];
    int n;
    section("translate_vm_cmd / accel + machine");

    n = translate("crosvm run -m 256", "/vmm", out);
    ok(has_tok(out, n, "-accel"), "no accel given: -accel is appended");
    ok(count_tok(out, n, "-accel") == 1, "exactly one -accel");
    /* -machine/-accel are appended after the caller's flags, so the tail is
     * not "-machine q35" unless the caller supplied an accelerator.  Assert
     * the pair is present and contiguous instead of guessing its position. */
    ok(idx_tok(out, n, "-machine") >= 0 &&
       !strcmp(out[idx_tok(out, n, "-machine") + 1], "q35"),
       "-machine q35 is appended");
    ok(count_tok(out, n, "-machine") == 1, "exactly one -machine");

    n = translate("crosvm run -accel kvm:tcg", "/vmm", out);
    ok(count_tok(out, n, "-accel") == 1,
       "accel given: the tcg default is not appended on top");
    ok(idx_tok(out, n, "-accel") >= 0 && !strcmp(out[idx_tok(out, n, "-accel") + 1], "kvm:tcg"),
       "the caller's -accel value survives");
}

int main(void) {
    printf("ncvm host tests (CMD_DIR=%s)\n", CMD_DIR);
    scratch_reset();

    test_valid_name();
    test_is_digits();
    test_tokenize();
    test_read_file();
    test_translate_basics();
    test_translate_drops_crosvm_only();
    test_translate_accel_and_machine();

    printf("\nchecks=%d failed=%d\n", checks, failed);
    if (failed) {
        printf("ncvm: FAIL (first: %s)\n", first_fail);
        return 1;
    }
    printf("ncvm: host tests OK\n");
    return 0;
}
