#!/usr/bin/env python3
"""Filesystem test harness for ext2/ext3/codefs.

Why this exists
---------------
The filesystem is the one subsystem where "the kernel says it worked" is worth
very little: a driver can happily return success while leaving the on-disk
image inconsistent, and the damage only shows up later. So the authority here is
the *host*, not the guest. After the guest has run its script we dd the
partition back out of the disk image and let real e2fsprogs judge the result:

    e2fsck -fn   -- structural check, never writes
    debugfs -R   -- inspect the actual tree the kernel produced

That needs no root: the partition is carved out with dd, so e2fsprogs sees the
filesystem at offset 0, which is the only offset it can read. (Neither
dumpe2fs nor e2fsck can read a filesystem embedded at a nonzero offset, which
is exactly why the Makefile's `-E offset=1048576` images are awkward to verify
by hand.)

Usage
-----
    ./fs_test.py --fstype ext3          # build image, boot, exercise, verify
    ./fs_test.py --fstype ext2          # same, for comparison
    ./fs_test.py --fstype ext3 --keep   # leave the image in place afterwards
"""

import argparse
import os
import pty
import re
import select
import shutil
import struct
import subprocess
import sys
import time
import tty

HERE = os.path.dirname(os.path.abspath(__file__))
KERNEL_DIR = os.path.normpath(os.path.join(HERE, ".."))
REPO = os.path.normpath(os.path.join(KERNEL_DIR, ".."))
ISO = os.path.join(KERNEL_DIR, "codeos-1-kernel.iso")
KERNEL_BIN = os.path.join(KERNEL_DIR, "codeos-1-kernel.bin")
WORK = "/tmp/codeos-fs-test"

# Matches the Makefile's disk layout: msdos label, single 0x83 partition
# starting at sector 2048, so the filesystem sits at byte offset 1 MiB.
PART_START_SECTOR = 2048
PART_OFFSET = PART_START_SECTOR * 512
DISK_SIZE_M = 64

# The machine type, boot order and disk placement are all dictated by the
# driver's hardware, not by convenience.  Each of these was found by probing
# (see the notes in scripts/fs_test.py's git history); the short version:
#
#  * ata.c probes the legacy PATA ports (0x1F0/0x3F6), master only.  q35 has no
#    PATA controller at all -- only AHCI -- so on qemu -machine q35 an IDE disk
#    is simply invisible ("block: no disk detected").  i440fx, -machine pc, has
#    PATA, so that is what we must use.
#  * The disk therefore has to own the primary master slot.  A CD-ROM does not
#    contend for it: -cdrom lands on the secondary channel, so booting from the
#    ISO and the disk as if=ide,index=0 works at the same time.
#  * -boot order=d is mandatory once a disk is attached.  Without it SeaBIOS
#    prefers the hard disk, iPXE tries to netboot it, and the guest hangs before
#    it ever reaches storage init -- which looks exactly like "disk not
#    detected" and cost a debugging round.
#  * Booting the kernel directly with -kernel does NOT work: the binary has no
#    PVH ELF note, so qemu refuses it ("Error loading uncompressed kernel").
QEMU = ["qemu-system-x86_64", "-machine", "pc", "-m", "1G", "-smp", "2",
        "-vga", "none", "-nographic", "-monitor", "none",
        "-boot", "order=d", "-cdrom", ISO]

PROMPT = b"root# "
BOOT_TIMEOUT = 60
STEP_TIMEOUT = 30
BOOT_RETRIES = 3


def log(msg):
    print(msg, flush=True)


# ───────────────────────────── image construction ─────────────────────────────

def build_image(disk, fstype, populate=True):
    """Create a partitioned disk with a freshly-mkfs'd filesystem inside it."""
    if os.path.exists(disk):
        os.remove(disk)
    subprocess.run(["truncate", "-s", f"{DISK_SIZE_M}M", disk], check=True)
    subprocess.run(["parted", "-s", disk, "mklabel", "msdos"],
                   check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    subprocess.run(["parted", "-s", disk, "mkpart", "primary", "ext2",
                    f"{PART_START_SECTOR}s", "100%"],
                   check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    # -d populates from a directory, so no root and no loop mount is needed.
    stage = os.path.join(WORK, "stage")
    shutil.rmtree(stage, ignore_errors=True)
    os.makedirs(os.path.join(stage, "etc"), exist_ok=True)
    os.makedirs(os.path.join(stage, "bin"), exist_ok=True)
    if populate:
        with open(os.path.join(stage, "etc", "conf.txt"), "w") as f:
            f.write("alpha content one\n")
        with open(os.path.join(stage, "bin", "tool.sh"), "w") as f:
            f.write("beta content two\n")

    cmd = ["mke2fs", "-q", "-t", fstype, "-F", "-L", "codeosfs",
           "-b", "1024", "-I", "128", "-E", f"offset={PART_OFFSET}"]
    if populate:
        cmd += ["-d", stage]
    cmd.append(disk)
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        log("mke2fs failed:\n" + r.stdout + r.stderr)
        return False
    return True


def extract_partition(disk, out):
    """Carve the filesystem out so e2fsprogs can read it at offset 0."""
    size = os.path.getsize(disk) - PART_OFFSET
    with open(disk, "rb") as fi, open(out, "wb") as fo:
        fi.seek(PART_OFFSET)
        left = size
        while left > 0:
            chunk = fi.read(min(1 << 20, left))
            if not chunk:
                break
            fo.write(chunk)
            left -= len(chunk)
    return os.path.getsize(out)


# ───────────────────────────── guest execution ─────────────────────────────

class Guest:
    def __init__(self, disk, logfile):
        self.disk = disk
        self.logfile = logfile
        self.buf = b""
        self.raw = b""

    def boot(self):
        m, s = pty.openpty()
        # The host pty must be raw, otherwise the guest never sees our bytes
        # as individual keystrokes.
        tty.setraw(s)
        argv = QEMU + ["-drive",
                       f"file={self.disk},format=raw,if=ide,index=0"]
        self.proc = subprocess.Popen(argv, stdin=s, stdout=s, stderr=s,
                                     close_fds=True)
        os.close(s)
        self.master = m
        return self.wait_for(PROMPT, BOOT_TIMEOUT, "boot")

    def pump(self, sec):
        dl = time.time() + sec
        while time.time() < dl:
            r, _, _ = select.select([self.master], [], [], 0.2)
            if not r:
                continue
            try:
                d = os.read(self.master, 65536)
            except OSError:
                return
            if not d:
                return
            self.buf += d
            self.raw += d
            self.logfile.write(d)
            self.logfile.flush()

    def wait_for(self, marker, timeout, what):
        dl = time.time() + timeout
        while time.time() < dl:
            self.pump(0.3)
            if marker in self.buf:
                i = self.buf.index(marker)
                self.buf = self.buf[i + len(marker):]
                return True
        log(f"  !! {what}: {marker!r} not seen within {timeout}s")
        return False

    def send(self, data):
        os.write(self.master, data)

    def cmd(self, line, markers, timeout=STEP_TIMEOUT):
        """Type a shell command and wait for each marker in order."""
        self.send(line.encode() + b"\n")
        return self.wait_all(markers, timeout)

    def wait_all(self, markers, timeout):
        for mk in markers:
            if not self.wait_for(mk, timeout, "marker"):
                return False, mk
        return True, None

    def kill(self):
        try:
            self.proc.terminate()
            self.proc.wait(timeout=10)
        except Exception:
            try:
                self.proc.kill()
            except Exception:
                pass


# ───────────────────────────── host-side verification ─────────────────────────

def fsck(part, fix=False):
    """Run a structural check. Returns (rc, output).

    fix=False is the read-only form used for every pass/fail verdict, so a
    check can never quietly repair the very thing it is meant to judge.
    fix=True is used only by journal_recovery_restore(), which has to give
    e2fsck the chance to replay the journal; there the repair is the point.
    """
    args = ["e2fsck", "-fy" if fix else "-fn", part]
    r = subprocess.run(args, capture_output=True, text=True, errors="replace")
    return r.returncode, r.stdout + r.stderr


def debugfs(part, request):
    # errors="replace" matters: a corrupted filesystem makes debugfs emit raw
    # block bytes, and a strict UTF-8 decode of those aborts the whole run --
    # losing the report for the failure that actually mattered.
    r = subprocess.run(["debugfs", "-R", request, part],
                       capture_output=True, text=True, errors="replace")
    return r.stdout + r.stderr


JBD2_MAGIC = 0xC03B3998
JBD2_DESCRIPTOR, JBD2_COMMIT, JBD2_SB_V2 = 1, 2, 4


def journal_activity(part):
    """Count real JBD2 transaction blocks in the journal. Returns (blocks, note).

    This exists because a filesystem can pass every other check here while
    having no journal at all.  The driver shipped a complete JBD2
    implementation that nothing called: writes went straight to the disk, the
    journal stayed exactly as mke2fs left it, and e2fsck was clean because
    there was never anything to replay.  "e2fsck is happy" cannot tell that
    apart from a working journal, so the log has to be inspected directly.

    Walks the real on-disk structures -- superblock -> group descriptor ->
    inode table -> journal inode -- rather than trusting dumpe2fs, because the
    point is to see the bytes the driver actually produced.
    """
    with open(part, "rb") as f:
        def u16(off, base=0):
            f.seek(base + off)
            return struct.unpack_from("<H", f.read(2))[0]

        def u32(off, base=0):
            f.seek(base + off)
            return struct.unpack_from("<I", f.read(4))[0]

        if u16(0x38, 1024) != 0xEF53:
            return 0, "no ext2 magic"

        journal_inum = u32(0xE0, 1024)
        if journal_inum == 0:
            return 0, "no journal (journal_inum is 0)"

        block_size = 1024 << u32(0x18, 1024)
        inodes_per_group = u32(0x28, 1024)
        inode_size = u16(0x58, 1024) or 128
        first_data = u32(0x14, 1024)

        g = (journal_inum - 1) // inodes_per_group
        idx = (journal_inum - 1) % inodes_per_group
        f.seek((first_data + 1 + g) * block_size + 8)
        inode_table = struct.unpack("<I", f.read(4))[0]
        f.seek(inode_table * block_size + idx * inode_size + 0x28)
        jblocks = list(struct.unpack("<15I", f.read(60)))

        found = []
        for n, blk in enumerate(jblocks):
            if not blk:
                continue
            f.seek(blk * block_size)
            hdr = f.read(12)
            magic, btype, _seq = struct.unpack(">III", hdr)
            if magic == JBD2_MAGIC and btype in (JBD2_DESCRIPTOR, JBD2_COMMIT,
                                                 JBD2_SB_V2):
                # The journal superblock is always block 0 of the journal and is
                # written by mke2fs, so it proves nothing about the driver.
                if n == 0 and btype == JBD2_SB_V2:
                    continue
                found.append((blk, btype))

    if not found:
        return 0, (f"journal inode {journal_inum} present but the log holds no "
                   f"descriptor or commit block -- writes are not journalled")
    desc = sum(1 for _, t in found if t == JBD2_DESCRIPTOR)
    comm = sum(1 for _, t in found if t == JBD2_COMMIT)
    return len(found), f"{desc} descriptor + {comm} commit block(s)"


# JBD2 feature bits (include/linux/jbd2.h)
JBD2_INCOMPAT_REVOKE   = 0x1
JBD2_INCOMPAT_64BIT    = 0x2
JBD2_INCOMPAT_ASYNC    = 0x4
JBD2_INCOMPAT_CSUM_V2  = 0x8
JBD2_INCOMPAT_CSUM_V3  = 0x10
JBD2_FLAG_ESCAPE       = 0x1
JBD2_FLAG_SAME_UUID    = 0x2
JBD2_FLAG_DELETED      = 0x4
JBD2_FLAG_LAST_TAG     = 0x8
JBD2_CRC32C_CHKSUM     = 4


def journal_tag_bytes(sz, feat):
    """Port of journal_tag_bytes() in fs/jbd2/journal.c.

    Transcribed from the kernel rather than from memory, because getting it
    wrong changes where every subsequent tag is read from -- and because a
    plausible-but-wrong version of this function is exactly how the driver's
    own tag layout went unexamined.
    """
    if feat & JBD2_INCOMPAT_CSUM_V3:
        return 16
    if feat & JBD2_INCOMPAT_64BIT:
        if feat & JBD2_INCOMPAT_CSUM_V2:
            return 14
        return 12
    if feat & JBD2_INCOMPAT_CSUM_V2:
        return 10
    return 8


def journal_recovery_restore(part):
    """Corrupt the home copies of the live journalled transaction, replay, and
    check they come back.  Returns (ok, note).

    This is the only check here that can tell a *working* journal from a
    journal that merely exists.  Every other check is satisfied by a
    filesystem whose home copies were already correct, which is the state the
    driver is always in: it writes the home locations immediately after the
    commit block, so there is normally nothing for recovery to do and a
    correct replay and a no-op are indistinguishable.

    So this manufactures the situation recovery exists for.  It reads the
    transaction the driver actually committed, records the home blocks'
    contents, overwrites them with garbage, and asks e2fsck to recover.  If
    the on-disk format is right the journal copy is written back and the
    blocks are restored; if the format is wrong e2fsck cannot parse the
    descriptor, silently recovers nothing, and -- this is the part that
    matters -- still exits 0 and reports a clean filesystem.

    The descriptor is parsed here from the *format* (unpadded tag stride, a
    16-byte journal UUID after the first tag of each descriptor), not from the
    driver's writer, so this is an independent check.  A parser written to
    match the driver would agree with a broken driver and prove nothing.

    Returns (False, reason) rather than raising when there is no committed
    transaction to test, so the caller can report "nothing to check" instead
    of a spurious pass.
    """
    shutil.copyfile(part, part + ".recover")
    work = part + ".recover"

    with open(part, "rb") as f:
        def u16(off, base=0):
            f.seek(base + off); return struct.unpack_from("<H", f.read(2))[0]
        def u32(off, base=0):
            f.seek(base + off); return struct.unpack_from("<I", f.read(4))[0]

        if u16(0x38, 1024) != 0xEF53:
            return False, "no ext2 magic"
        journal_inum = u32(0xE0, 1024)
        if journal_inum == 0:
            return False, "journalless image, nothing to recover"

        bs = 1024 << u32(0x18, 1024)
        blocks_count = u32(0x04, 1024)
        inodes_per_group = u32(0x28, 1024)
        inode_size = u16(0x58, 1024) or 128
        first_data = u32(0x14, 1024)

        g = (journal_inum - 1) // inodes_per_group
        idx = (journal_inum - 1) % inodes_per_group
        f.seek((first_data + 1 + g) * bs + 8)
        itable = struct.unpack("<I", f.read(4))[0]
        f.seek(itable * bs + idx * inode_size + 0x28)
        direct = list(struct.unpack("<12I", f.read(48)))
        f.seek(itable * bs + idx * inode_size + 0x28 + 48)
        ind_blk = struct.unpack("<I", f.read(4))[0]

        def jblock(n):
            """journal block n -> filesystem block number"""
            if n < 12:
                return direct[n]
            if not ind_blk:
                return None
            f.seek(ind_blk * bs + (n - 12) * 4)
            v = struct.unpack("<I", f.read(4))[0]
            return v or None

        # Journal superblock: big-endian, and s_start == 0 means the log has
        # never been used, so there is no committed transaction to test.
        f.seek(direct[0] * bs)
        jsb = f.read(bs)
        jmagic, jtype = struct.unpack(">II", jsb[0:8])
        if jmagic != JBD2_MAGIC:
            return False, f"journal superblock magic is 0x{jmagic:08x}"
        jmaxlen, = struct.unpack(">I", jsb[16:20])
        jfirst, = struct.unpack(">I", jsb[20:24])
        jfeat, = struct.unpack(">I", jsb[40:44])
        tbytes = journal_tag_bytes(bs, jfeat)

        def read_jblk(n):
            b = jblock(n % jmaxlen)
            if b is None:
                return None
            f.seek(b * bs)
            d = f.read(bs)
            return b, d

        # Find the transaction at s_start (or jfirst when s_start is 0).
        start, = struct.unpack(">I", jsb[28:32])
        pos = start if start else jfirst
        tags, seq = [], None
        # Walk the transaction: descriptor, then one data block per tag, then
        # the next descriptor or the commit block.  The data blocks carry no
        # journal magic -- they are raw copies of the home blocks -- so the
        # walk has to step over them by count rather than sniffing for a
        # header, or it stops on the first data block and reports "nothing
        # committed" for a transaction that is sitting right there.
        for _ in range(64):
            got = read_jblk(pos)
            if got is None:
                return False, "journal block map exhausted while walking"
            fblk, d = got
            magic, btype, bseq = struct.unpack(">III", d[0:12])
            if magic != JBD2_MAGIC:
                return False, (f"no journal magic at journal block {pos} -- "
                               f"nothing committed to recover")
            if btype == JBD2_COMMIT:
                if seq is not None and bseq != seq:
                    return False, "commit sequence does not match its descriptors"
                break
            if btype != JBD2_DESCRIPTOR:
                # revoke / superblock / fc blocks are legal between descriptors
                pos += 1
                continue
            if seq is None:
                seq = bseq
            elif bseq != seq:
                return False, "descriptor sequence mismatch mid-transaction"
            # Walk this descriptor's tag stream per the format.
            off = 12
            n_in_desc = 0
            first_tag = True
            while off + tbytes <= bs:
                blk, = struct.unpack(">I", d[off:off + 4])
                flags, = struct.unpack(">H", d[off + 6:off + 8])
                if flags & JBD2_FLAG_ESCAPE:
                    break
                if not (flags & JBD2_FLAG_DELETED):
                    if blk == 0 or blk >= blocks_count:
                        # A tag naming block 0 or a block past the end of the
                        # filesystem means this stream is not a JBD2 tag
                        # stream.  Reporting that directly is the whole point:
                        # the alternative is walking into the middle of the
                        # block until something looks like a commit header,
                        # which produces a baffling "no journal magic" instead
                        # of naming the actual defect.
                        return False, (
                            f"descriptor at journal block {pos} does not "
                            f"parse as a JBD2 tag stream: tag at byte {off} "
                            f"names filesystem block {blk}, which is outside "
                            f"0..{blocks_count - 1}. Parsed per the format with "
                            f"a {tbytes}-byte stride, so the driver's tag "
                            f"spacing does not match the on-disk format and no "
                            f"real JBD2 reader can replay this journal.")
                    tags.append(blk)
                    n_in_desc += 1
                off += tbytes
                if not (flags & JBD2_FLAG_SAME_UUID):
                    off += 16          # journal UUID follows the first tag
                if flags & JBD2_FLAG_LAST_TAG:
                    break
                if first_tag:
                    first_tag = False
            # Skip this descriptor's data blocks to reach the next one.
            pos += 1 + n_in_desc
        else:
            return False, "no commit block found within 64 journal blocks"

        if not tags:
            return False, "the committed transaction names no blocks"

        # Snapshot, corrupt, replay, compare.
        home = [b for b in tags if 0 < b]
        if not home:
            return False, f"transaction names only block 0: {tags}"

        f.seek(0)
        whole = f.read()
        before = {b: whole[b * bs:(b + 1) * bs] for b in home}
        poison = b"\xde\xad\xbe\xef" * (bs // 4)

    with open(work, "r+b") as f:
        for b in home:
            f.seek(b * bs); f.write(poison)

    rc, out = fsck(work, fix=True)

    with open(work, "rb") as f:
        restored, still_bad = [], []
        for b in home:
            f.seek(b * bs)
            d = f.read(bs)
            (still_bad if d == poison else restored).append(b)

    os.unlink(work)

    if still_bad:
        return False, (
            f"e2fsck did not restore {len(still_bad)}/{len(home)} journalled "
            f"block(s) {still_bad} from the journal after a crash-window "
            f"corruption (e2fsck rc={rc}, i.e. it reported success anyway). "
            f"Tag stream parsed per the format with a {tbytes}-byte stride; the "
            f"journal is present but its descriptor block is not in a form a "
            f"real JBD2 reader can walk.")
    return True, (f"e2fsck replay restored {len(restored)}/{len(home)} "
                  f"journalled block(s) {sorted(restored)} (rc={rc})")


# ───────────────────────────── the test itself ─────────────────────────────

def run(fstype, keep):
    os.makedirs(WORK, exist_ok=True)
    disk = os.path.join(WORK, f"disk-{fstype}.img")
    part = os.path.join(WORK, f"part-{fstype}.img")
    serial = os.path.join(WORK, f"serial-{fstype}.log")
    report = os.path.join(WORK, f"report-{fstype}.txt")

    results = []
    lines = []

    def step(ok, label, detail="", evidence=""):
        # `detail` explains a failure and is therefore only worth printing when
        # the step failed.  `evidence` is the positive counterpart: the measured
        # result of a step that passed, kept so a later reader can tell a check
        # that did real work from one that passed vacuously.
        results.append(ok)
        tail = ""
        if evidence:
            tail = f"  ({evidence})"
        elif detail and not ok:
            tail = f"  {detail}"
        lines.append(f"  [{'ok ' if ok else 'FAIL'}] {label}{tail}")
        log(lines[-1])

    log(f"=== building {fstype} image ===")
    if not build_image(disk, fstype):
        lines.append("mke2fs FAILED")
        open(report, "w").write("\n".join(lines))
        return False
    step(True, f"built {DISK_SIZE_M}M disk with {fstype}")

    # Confirm the image really is what we think it is, before blaming the kernel.
    rc, out = fsck(extract_partition(disk, part) and part)
    step(rc == 0, "freshly built image passes e2fsck", f"rc={rc}\n{out}")

    log(f"=== booting guest with {fstype} disk ===")
    g = Guest(disk, open(serial, "wb"))
    booted = False
    for attempt in range(1, BOOT_RETRIES + 1):
        if g.boot():
            booted = True
            break
        log(f"  boot attempt {attempt} failed; retrying")
        g.kill()
        time.sleep(2)
    step(booted, "guest booted to shell prompt")
    if not booted:
        open(report, "w").write("\n".join(lines))
        return False

    # The partition is auto-mounted at boot (main.c: codefs, then ext2).
    # NB: the shell's own ls/cat/mkdir go through fs.c, which is the in-memory
    # initramfs node table and never touches the disk driver, so they prove
    # nothing about ext2. Everything below uses ext2-native entry points.
    ok, miss = g.cmd("els /", [b"/"])
    step(ok, "els lists the mounted ext2 root", f"missing {miss!r}")

    ok, miss = g.cmd("ecat /etc/conf.txt", [b"alpha content one"])
    step(ok, "ecat reads a file mke2fs populated", f"missing {miss!r}")

    ok, miss = g.cmd("fstest", [b"FSTEST RESULT"])
    step(ok, "fstest ran to completion", f"missing {miss!r}")

    # Parse the per-check lines out of the captured serial output.
    checks = re.findall(rb"FSTEST (ok|FAIL) (\S+)", g.raw)
    named = [(s.decode(), n.decode()) for s, n in checks]
    failed = [n for s, n in named if s == "FAIL"]
    for state, name in named:
        lines.append(f"       fstest {state:4s} {name}")
    step(bool(named) and not failed,
         f"all {len(named)} ext2 driver checks passed",
         "failed: " + ", ".join(failed) if failed else "no checks reported")

    # Flush everything before we pull the plug, so what we verify is what the
    # kernel committed rather than whatever happened to be in its buffers.
    g.cmd("sync", [PROMPT])
    g.pump(1)
    g.kill()

    log("=== verifying the image from the host ===")
    extract_partition(disk, part)
    rc, out = fsck(part)
    step(rc == 0, "e2fsck clean after guest writes", f"rc={rc}\n{out}")

    ls = debugfs(part, "ls -l /")
    step("fstest_small" in ls, "kernel created /fstest_small on disk", ls)
    step("fstest_big" in ls, "kernel created /fstest_big on disk", ls)
    ls2 = debugfs(part, "ls -l /fstest_dir")
    step("File not found" in ls2, "kernel removed /fstest_dir again", ls2)

    st = debugfs(part, "stat /fstest_big")
    m = re.search(r"Size:\s+(\d+)", st)
    step(bool(m) and int(m.group(1)) == 20480,
         "big file has the right size on disk", st)

    cat = debugfs(part, "cat /etc/conf.txt")
    step("alpha content one" in cat,
         "pre-populated file still intact", cat)

    # Only ext3 has a journal to check. An ext2 image is journalless by
    # definition, so asking for transaction blocks there would be a false
    # failure rather than a finding.
    if fstype != "ext2":
        nblocks, note = journal_activity(part)
        step(nblocks > 0, "writes reached the JBD2 journal", note)

        # EXT3_FEATURE_INCOMPAT_RECOVER = 0x0004 in s_feature_incompat, which
        # lives at byte 1024 + 0x60 of the partition.
        #
        # e2fsck only *replays* a journal when this bit is set.  With it clear
        # e2fsck treats the filesystem as cleanly unmounted and clears the
        # journal instead, which discards any committed-but-not-checkpointed
        # transaction and leaves its blocks half-old, half-new.  CodeOS has no
        # unmount path, so the bit must always be set after a journalled write.
        with open(part, "rb") as fh:
            fh.seek(1024 + 0x60)
            incompat = struct.unpack("<I", fh.read(4))[0]
        step(bool(incompat & 0x0004),
             "superblock is marked as needing journal recovery",
             f"feature_incompat=0x{incompat:08x} RECOVER(0x4) "
             f"{'set' if incompat & 0x0004 else 'CLEAR'}")

        # The decisive journal check. Everything above it is satisfied by a
        # journal that exists but cannot be read, because the driver writes
        # the home copies right after the commit block, so there is normally
        # nothing left for recovery to do.
        ok, note = journal_recovery_restore(part)
        step(ok, "e2fsck can actually replay the journal", note,
             evidence=note if ok else "")

    panic = b"OWPANIC" in g.raw
    pf = b"!!! PF at" in g.raw
    step(not panic, "no OWPANIC in serial log")
    step(not pf, "no page fault in serial log")

    allok = all(results)
    lines.insert(0, f"VERDICT: {'ALL STEPS PASSED' if allok else 'FAILURES PRESENT'}"
                    f"  ({fstype})")
    open(report, "w").write("\n".join(lines) + "\n")
    log("")
    log(f"VERDICT: {'ALL STEPS PASSED' if allok else 'FAILURES PRESENT'} ({fstype})")
    log(f"report: {report}")

    if not keep:
        for f in (disk, part):
            try:
                os.remove(f)
            except OSError:
                pass
    return allok


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--fstype", default="ext3",
                    choices=["ext2", "ext3", "ext4"])
    ap.add_argument("--keep", action="store_true",
                    help="keep the disk image for inspection")
    args = ap.parse_args()
    if not os.path.exists(ISO):
        log(f"missing {ISO} -- build it with: make -C {KERNEL_DIR} codeos-1-kernel.iso")
        return 2
    return 0 if run(args.fstype, args.keep) else 1


if __name__ == "__main__":
    sys.exit(main())
