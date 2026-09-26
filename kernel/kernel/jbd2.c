#include "jbd2.h"
#include "ext2.h"
#include "kprintf.h"
#include "string.h"
#include "mm.h"

/* JBD2 journal for the ext2/3 driver.
 *
 * A real JBD2 log: descriptor blocks, data blocks, and a commit block per
 * transaction, written and replayed in the order the format requires.
 *
 * ── Which tag layout is on disk is not this driver's choice ──
 *
 * The stride between consecutive tags in a descriptor is exactly the tag size,
 * and the tag size is fixed by s_feature_incompat in the journal superblock.
 * Stock "mke2fs -t ext3" writes a superblock with s_feature_incompat == 0, so
 * the only conformant tag is the 8-byte v1 one: blocknr, checksum, flags.  A
 * writer has no freedom here at all -- there is no marker saying "the stride
 * is rounded up" -- so getting it wrong does not produce a slightly odd
 * journal, it produces one that nothing can read.
 *
 * Journals carrying checksum features (csum_v2, csum_v3) or 64-bit tags are
 * refused at mount rather than written, falling back to journalless ext2.
 * Those need CRC32C seeded from s_uuid, over a different byte range for each of
 * the three checksums the format defines; a journal that claims a checksum its
 * reader computes differently is worse than no journal, because recovery then
 * fails silently.  See the note in jbd2_init().
 *
 * ── The write ordering is the whole difficulty ──
 *
 * Getting this wrong does not fail loudly. It produces a filesystem that passes
 * every functional test and is quietly corrupt after a power cut. The only
 * ordering that is safe is:
 *
 *     1. write the journal descriptor block(s)   (the tag list)
 *     2. write the journal data blocks           (new content of each tag)
 *     3. write the journal commit block          <-- THE COMMIT POINT
 *     4. only now write the blocks to their home locations
 *
 * Step 4 must follow step 3. If the home location went first and we crashed
 * before the commit block landed, replay would correctly discard the
 * transaction while the home location already held half of it -- an operation
 * applied in part, which is exactly what a journal exists to prevent.
 *
 * Step 2 needs the new content of every tagged block, and step 2 happens at
 * commit time rather than at the call, so that content must survive until
 * commit. Hence the staging buffer: each tagged block's new bytes are copied in
 * as it is tagged, and when the staging area fills the transaction commits and
 * a new one starts. That bounds memory instead of requiring a whole operation's
 * worth of RAM up front, and matches what a real journaling filesystem does
 * when its cache cannot hold an entire operation. The cost is that a large
 * write is split across transactions, so a crash mid-write can leave a file
 * shorter than intended -- consistent, but not atomic. ext3 offers the same
 * guarantee, so this is not a regression.
 *
 * ── What gets journaled ──
 *
 * Everything. ext2.c funnels every block write -- data, bitmaps, inodes,
 * directory blocks, group descriptors -- through one function, and that single
 * choke point is hooked in ext2.c's write_block(). Splitting writes into
 * "metadata" and "data" so that only metadata is journaled would be closer to
 * ext3's default ordered mode, but it requires auditing every write path for a
 * missed case, and a missed case is silent corruption. Journalling data blocks
 * as well is journalled-data mode, which ext3 permits and e2fsck accepts; it
 * costs journal space and buys certainty that no path is unjournalled. The
 * superblock is the deliberate exception: it is the anchor recovery trusts, so
 * it is written directly and never staged.
 */
/* ── byte order ────────────────────────────────────────────────────────────
 * ext2 fields are little-endian on disk, so ext2.c reads them raw and is correct
 * on x86 by construction. JBD2 is the opposite: every field is big-endian
 * regardless of host, so every access here must swap. Reading JBD2 raw on a
 * little-endian host yields a plausible-looking but entirely wrong magic,
 * blocksize and sequence -- so these are not optional.
 */
static inline uint32_t bswap32(uint32_t v) {
    return ((v & 0x000000FFu) << 24) | ((v & 0x0000FF00u) << 8) |
           ((v & 0x00FF0000u) >> 8)  | ((v & 0xFF000000u) >> 24);
}
#define be32(v) bswap32(v)
#define le32(v) bswap32(v)
static inline uint16_t bswap16(uint16_t v) {
    return (uint16_t)(((v & 0x00FFu) << 8) | ((v & 0xFF00u) >> 8));
}
#define be16(v) bswap16(v)
#define le16(v) bswap16(v)

/* ── tunables ────────────────────────────────────────────────────────────
 * STAGE_BLOCKS bounds journal memory (64 KiB with 1 KiB journal blocks, 256 KiB
 * with 4 KiB). It affects only how often a large write is split across
 * transactions, never correctness.
 *
 * It is also the hard ceiling on how many tags one transaction may carry, in
 * both directions: replay refuses a transaction with more tags than this rather
 * than truncating it, because a truncated tag list leaves the data blocks
 * lining up against the wrong home blocks.  At 64 it is well under one
 * descriptor block's capacity (~124 tags in a 1 KiB block with 8-byte tags), so
 * a transaction is always a single descriptor.
 */
#define JBD2_STAGE_BLOCKS 64
#define JBD2_MAX_TAGS     JBD2_STAGE_BLOCKS

/* Refuse to start a transaction that would come within this many blocks of the
 * end of the journal, so a transaction never straddles the wrap point. */
#define JBD2_WRAP_MARGIN  2

static int      j_present;
static uint32_t j_inum;

static uint32_t j_blocksize;
static uint32_t j_maxlen;
static uint32_t j_first;
static uint32_t j_head;        /* offset from 0 of the next free journal block */
static uint32_t j_sequence;    /* sequence of the next transaction to write */
static uint32_t j_seq_next;    /* sequence handed to the transaction in flight */

static int      j_tag_bytes;
static uint32_t j_feature_incompat;

/* The journal's UUID, from s_uuid at offset 0x30 of the journal superblock.
 *
 * Every descriptor block repeats this in its tag stream: the first tag of a
 * descriptor is followed by 16 bytes of UUID, and every later tag sets
 * JBD2_FLAG_SAME_UUID to say "same as the one at the front".  A reader uses it
 * to reject a tag that belongs to a different filesystem, which is what stops
 * a block from some unrelated journal being replayed to the wrong home. */
static uint8_t  j_uuid[16];

/* Revoke table for the transaction in flight.
 *
 * Sized from the FILESYSTEM block count, not the journal length: the bitmap
 * indexes the blocks being revoked, which are ordinary fs blocks and routinely
 * far outnumber the journal's own 4096 blocks. Sizing it from j_maxlen would
 * silently drop revokes for any block past the journal length -- the exact case
 * where a resurrected block does the most damage. */
static uint8_t *j_revoke_bits;
static uint32_t j_revoke_bytes;   /* allocated size of j_revoke_bits */
static uint32_t j_revoke_hwm;     /* highest block revoked this transaction */

/* staging area + tag table for the transaction in flight */
static uint8_t  *j_stage;
static uint32_t  j_tag_block[JBD2_MAX_TAGS];
static uint32_t  j_tag_slot[JBD2_MAX_TAGS];
static int       j_ntags;
static int       j_txn_depth;

static uint8_t  *j_iobuf;      /* scratch for reading journal blocks */
static uint8_t  *j_descbuf;    /* scratch for building descriptor/commit blocks */
static uint8_t  *j_sb;         /* the 1024-byte journal superblock */
static uint8_t  *j_probe;      /* full-block scratch used before j_blocksize is
                                 * known; see jbd2_load_sb() */
static uint8_t  *j_save;       /* holds a staged block across a mid-transaction
                                 * flush; see jbd2_stage_block() */

static int jbd2_replay(void);
static int jbd2_wrap(void);
static int jbd2_flush(void);
static int jbd2_build_map(void);

/* ── journal block addressing ────────────────────────────────────────────
 * The journal is an ordinary file, so journal block N is whatever fs block the
 * journal inode's block map says for index N. Going through the inode rather
 * than assuming contiguity matters: mke2fs happens to lay a journal out
 * contiguously, but nothing guarantees it, and a 4 MiB journal in a 4 KiB-block
 * filesystem needs an indirect block after the first twelve.
 *
 * The map is resolved once, at mount, into j_map. It used to be re-walked on
 * every single journal block access, and that walk runs in the filesystem
 * driver's shared block buffer (read_inode_block is an ext2.c function with no
 * buffer of its own). That was a live corruption bug, not just a slow path: a
 * flush in the middle of staging left the journal inode's indirect block sitting
 * in that shared buffer, and the next staged block was copied from it -- so the
 * journal faithfully wrote a block of block-numbers over the caller's target. On
 * a 1 KiB-block filesystem that landed the journal's indirect block (a run of
 * consecutive block numbers) on top of the group descriptor table.
 *
 * The journal inode is never rewritten while the journal is mounted, so the map
 * cannot change under us and there is nothing to invalidate.
 */
static uint32_t *j_map;
static uint32_t  j_map_len;

static int jphys(uint32_t jblock, uint32_t *out) {
    if (j_map) {
        if (jblock >= j_map_len || !j_map[jblock]) return -1;
        *out = j_map[jblock];
        return 0;
    }
    /* Only reachable while the journal superblock is being read: j_maxlen is
     * what sizes the map, and that is not known until the superblock is in hand.
     * Nothing holds block_buf across those four reads at mount time. */
    uint32_t p = ext2_inode_phys_block((int)j_inum, (int)jblock);
    if (!p) return -1;
    *out = p;
    return 0;
}

/* Resolve the journal's whole block map. A failure here is not fatal: jphys()
 * falls back to walking the inode, which is correct but slow. */
static int jbd2_build_map(void) {
    if (j_maxlen == 0 || j_maxlen > JBD2_MAX_MAP) return -1;
    uint32_t *m = (uint32_t *)malloc(j_maxlen * sizeof(uint32_t));
    if (!m) return -1;
    for (uint32_t i = 0; i < j_maxlen; i++)
        m[i] = ext2_inode_phys_block((int)j_inum, (int)i);
    j_map = m;
    j_map_len = j_maxlen;
    return 0;
}

/* Read one journal block into j_probe.
 *
 * j_probe is always JBD2_MAX_BLOCK_SIZE, which exceeds any legal journal block,
 * so there is no destination a caller can get wrong. Callers copy out what they
 * need (see jread_hdr / jread_block).
 *
 * This previously took a caller-supplied destination, and every call that passed
 * `&some_struct_jbd2_header` wrote a whole 1 KiB block over 12 bytes of stack --
 * which silently smashed the return address and faulted with RIP=0.
 *
 * There is deliberately no `jblock < j_maxlen` check here: j_maxlen is zero
 * until the superblock has been read, so bounding on it would fail every read in
 * the load path. The replay walk checks the bound itself before calling.
 */
static int jprobe(uint32_t jblock) {
    uint32_t phys;
    if (jphys(jblock, &phys) < 0) return -1;
    return ext2_read_block_from(phys, j_probe);
}

/* Copy out just the 12-byte header. `out` is the only caller-sized buffer in
 * the read path, and it is exactly the size of what is copied. */
static int jread_hdr(uint32_t jblock, struct jbd2_header *out) {
    if (jprobe(jblock) < 0) return -1;
    memcpy(out, j_probe, sizeof(*out));
    return 0;
}

/* Copy out a whole block. `dst` must hold at least j_blocksize bytes, which is
 * true of j_iobuf and j_sb by construction. */
static int jread_block(uint32_t jblock, void *dst) {
    if (jprobe(jblock) < 0) return -1;
    memcpy(dst, j_probe, j_blocksize);
    return 0;
}

static int jwrite(uint32_t jblock, const void *src) {
    uint32_t phys;
    if (jphys(jblock, &phys) < 0) return -1;
    return ext2_write_block_from(phys, src);
}

/* ── superblock ──────────────────────────────────────────────────────────
 * The journal superblock is 1024 bytes in the journal inode's first block.
 * Redundant copies follow; we read them in order and keep the highest sequence,
 * which is how we recover if the write of the superblock itself was torn.
 */
static int jbd2_load_sb(void) {
    uint32_t best = 0;
    int have = 0, found = -1;

    for (uint32_t i = 0; i < 4; i++) {
        struct jbd2_header h;
        if (jread_hdr(i, &h) < 0) continue;
        if (be32(h.h_magic) != JBD2_MAGIC_NUMBER) continue;
        if (be32(h.h_blocktype) != JBD2_SUPERBLOCK_V2) continue;
        uint32_t seq = be32(h.h_sequence);
        if (!have || seq > best) { best = seq; have = 1; found = (int)i; }
    }
    if (found < 0) {
        /* Print what is actually there. "no readable journal superblock" is not
         * an actionable diagnosis, and the three ways this can go wrong --
         * wrong inode, wrong endianness, wrong offset -- look identical from
         * the outside. */
        struct jbd2_header d;
        if (jread_hdr(0, &d) == 0)
            kprintf("ext3: inode %u block 0: magic 0x%08x type %u seq %u"
                    " (want magic 0x%08x type %u)\n",
                    j_inum, be32(d.h_magic), be32(d.h_blocktype),
                    be32(d.h_sequence), JBD2_MAGIC_NUMBER, JBD2_SUPERBLOCK_V2);
        else
            kprintf("ext3: inode %u block 0 is unreadable\n", j_inum);
        return -1;
    }

    /* The journal superblock is the 1024 bytes at the head of its block. The
     * block itself may be larger, so copy just the superblock and leave j_sb
     * sized to exactly that. */
    if (jprobe((uint32_t)found) < 0) return -1;
    memset(j_sb, 0, JBD2_HEADER_SIZE);
    memcpy(j_sb, j_probe, JBD2_HEADER_SIZE);

    j_blocksize        = be32(*(uint32_t *)(j_sb + 12));  /* s_blocksize */
    j_maxlen           = be32(*(uint32_t *)(j_sb + 16));  /* s_maxlen */
    j_first            = be32(*(uint32_t *)(j_sb + 20));  /* s_first */
    j_sequence         = be32(*(uint32_t *)(j_sb + 24));  /* s_sequence: first
                                                           * commit ID expected */
    uint32_t s_start   = be32(*(uint32_t *)(j_sb + 28));  /* s_start: blocknr of
                                                           * start of log */
    j_feature_incompat = be32(*(uint32_t *)(j_sb + 40));
    memcpy(j_uuid, j_sb + 48, 16);                        /* s_uuid @ 0x30 */

    if (j_blocksize < JBD2_MIN_BLOCK_SIZE || j_blocksize > JBD2_MAX_BLOCK_SIZE)
        return -1;
    if (j_maxlen <= j_first) return -1;

    /* journal_tag_bytes(), transcribed. The tag is one struct truncated to a
     * feature-dependent length, so the no-feature case is 12-4 = 8 bytes. */
    if (j_feature_incompat & JBD2_FEATURE_INCOMPAT_CSUM_V3) {
        j_tag_bytes = JBD2_TAG_SIZE_CSUM_V3;
    } else {
        int sz = JBD2_TAG_SIZE_64BIT;                 /* 12 */
        if (j_feature_incompat & JBD2_FEATURE_INCOMPAT_CSUM_V2) sz += 2;  /* 14 */
        if (j_feature_incompat & JBD2_FEATURE_INCOMPAT_64BIT) j_tag_bytes = sz;
        else j_tag_bytes = sz - 4;                    /* 10, or 8 with no csum */
    }

    /* s_start == 0 is mke2fs's "log never used" marker, not a block number:
     * block 0 of the journal is the superblock itself, and writing there because
     * of that reading destroys the journal. Normalise to the first log block. */
    if (s_start < j_first || s_start >= j_maxlen) s_start = j_first;

    j_head = s_start;
    return 0;
}

static int jbd2_store_sb(uint32_t s_start, uint32_t s_sequence) {
    *(uint32_t *)(j_sb + 12) = le32(j_blocksize);
    *(uint32_t *)(j_sb + 16) = le32(j_maxlen);
    *(uint32_t *)(j_sb + 20) = le32(j_first);
    *(uint32_t *)(j_sb + 24) = le32(s_sequence);
    *(uint32_t *)(j_sb + 28) = le32(s_start);

    /* Write a whole block, not just the 1024-byte superblock. If the journal
     * block size exceeds JBD2_HEADER_SIZE, writing only the superblock would
     * leave the tail of the block as it was, and ext2_write_block_from moves
     * block_size bytes -- so the tail would be read straight back out of
     * whatever buffer happened to follow. Pad through j_probe. */
    memset(j_probe, 0, j_blocksize);
    memcpy(j_probe, j_sb, JBD2_HEADER_SIZE);
    return jwrite(0, j_probe);
}

/* ── transactions ─────────────────────────────────────────────────────── */

static void jbd2_reset_txn(void) {
    j_ntags = 0;
    j_revoke_hwm = 0;
    if (j_revoke_bits) memset(j_revoke_bits, 0, j_revoke_bytes);
}

static int jbd2_is_revoked(uint32_t b) {
    if (!j_revoke_bits || b > j_revoke_hwm) return 0;
    return (j_revoke_bits[b / 8] >> (b % 8)) & 1;
}

/* Record that a block was freed inside the current transaction.  Replaying the
 * transaction would otherwise resurrect the old contents into a block that has
 * since been reallocated, so the tag is dropped.
 *
 * The bitmap is kept only in memory.  It is not written to the journal, because
 * this journal does not advertise JBD2_FEATURE_INCOMPAT_REVOKE and so is not
 * permitted to carry revoke records; see the note in jbd2_flush().  What the
 * bitmap buys is the flush-time sweep, which catches a block revoked before it
 * was ever staged -- jbd2_revoke()'s own tag removal cannot see that case,
 * because the tag does not exist yet.
 */
static int jbd2_revoke(uint32_t fs_block) {
    if (!j_present || j_txn_depth == 0) return 0;

    if (fs_block / 8 >= j_revoke_bytes) return 0;   /* outside our bitmap */
    j_revoke_bits[fs_block / 8] |= (uint8_t)(1u << (fs_block % 8));
    if (fs_block > j_revoke_hwm) j_revoke_hwm = fs_block;

    /* Drop the tag now rather than at flush.  Doing it here means a block that
     * is staged and then freed within one transaction never reaches the
     * descriptor at all, which is the property that matters; the flush-time
     * sweep over the bitmap then has nothing left to remove.  Both are kept
     * because the sweep also covers a block revoked before it was staged, which
     * the loop below cannot see. */
    for (int i = 0; i < j_ntags; i++) {
        if (j_tag_block[i] == fs_block) {
            j_tag_block[i] = j_tag_block[--j_ntags];
            j_tag_slot[i]  = j_tag_slot[j_ntags];
            i--;
        }
    }
    return 0;
}

/* Blocks a transaction of n tags occupies: its descriptor blocks, its data
 * blocks, and its commit block. */
static int jbd2_txn_blocks(int ntags) {
    int per_desc = (int)(j_blocksize - sizeof(struct jbd2_header) - 16) / j_tag_bytes;
    if (per_desc < 1) return 1 << 30;      /* pathological: no tag fits at all */
    int ndesc = (ntags + per_desc - 1) / per_desc;
    if (ndesc < 1) ndesc = 1;
    return ndesc + ntags + 1;
}

/* Move the log back to the first log block for a new epoch.
 *
 * The journal superblock MUST be updated before the first block of the new
 * epoch is written. If we crash after overwriting but before recording the new
 * start and sequence, recovery would begin at the old start expecting the old
 * sequence, meet the new epoch's higher sequence at once, and silently skip
 * every transaction written since the wrap.
 */
static int jbd2_wrap(void) {
    j_head = j_first;
    return jbd2_store_sb(j_first, j_sequence);
}

int jbd2_txn_begin(void) {
    if (!j_present) return -1;
    if (j_txn_depth++ > 0) return 0;      /* nested: outermost owns the commit */

    /* Keep the largest possible transaction clear of the end of the journal, so
     * it cannot straddle the wrap point: replay walks the log linearly and would
     * read the tail of one transaction as the head of the next. */
    if (j_head + (uint32_t)jbd2_txn_blocks(JBD2_MAX_TAGS) + JBD2_WRAP_MARGIN
        >= j_maxlen) {
        /* Flush directly rather than through jbd2_txn_commit(), which would
         * decrement the depth this very function just incremented. */
        if (jbd2_flush() < 0) { j_txn_depth = 0; return -1; }
        if (jbd2_wrap() < 0) { j_txn_depth = 0; return -1; }
    }

    jbd2_reset_txn();
    j_seq_next = j_sequence;
    return 0;
}

int jbd2_txn_commit(void) {
    if (!j_present) return -1;
    if (j_txn_depth > 0 && --j_txn_depth > 0) return 0;
    if (j_txn_depth < 0) j_txn_depth = 0;
    return jbd2_flush();
}

/* Write the staged blocks out as one transaction.
 *
 * This does no depth bookkeeping, so it is reachable both from the public
 * commit above and from the one place that must end a transaction early: the
 * staging area filling up.
 */
static int jbd2_flush(void) {
    /* Compact away revoked tags: they are not written to the descriptor at all. */
    int n = 0;
    for (int i = 0; i < j_ntags; i++) {
        if (jbd2_is_revoked(j_tag_block[i])) continue;
        j_tag_block[n] = j_tag_block[i];
        j_tag_slot[n]  = j_tag_slot[i];
        n++;
    }

    /* An empty transaction must NOT advance the sequence. Nothing was written to
     * the log, so the next transaction written will still carry j_sequence;
     * bumping it here would leave a gap, and replay -- which stops at the first
     * sequence that is not the expected successor -- would stop at that gap and
     * silently skip every real transaction after it. */
    if (n == 0) { jbd2_reset_txn(); return 0; }

    /* No revoke record is written, and none should be.  Revoke blocks are
     * conditional on JBD2_FEATURE_INCOMPAT_REVOKE, which this journal's
     * superblock does not set -- the kernel's
     * jbd2_journal_write_revoke_records() returns immediately without it.
     * Dropping the revoked tags from the descriptor above is the whole of what
     * a non-revoke journal does, and it is sufficient: a block freed during
     * the transaction has no tag, so replay writes nothing to it, which is the
     * property the revoke table exists to provide.  (The previous version
     * appended the bitmap to the tail of the final descriptor block, where
     * nothing looks for it: a revoke block is its own block type, written
     * before the descriptors.  Reader and writer both ignored it, so it cost
     * journal space and did nothing.) */

    /* Tags per descriptor block.  The 16 subtracted covers the journal UUID
     * that follows the first tag of every descriptor, so a descriptor that
     * reaches this count still has room to write it.  The previous version
     * divided the whole payload by j_tag_bytes, which over-counted by two tags
     * and disagreed with where the write loop actually placed them -- the two
     * numbers came from different assumptions and only stayed in range because
     * JBD2_STAGE_BLOCKS happened to be below both. */
    int per_desc = (int)(j_blocksize - sizeof(struct jbd2_header) - 16) / j_tag_bytes;
    if (per_desc < 1) return -1;
    int ndesc = (n + per_desc - 1) / per_desc;
    if (ndesc < 1) ndesc = 1;

    if (jbd2_txn_blocks(n) + JBD2_WRAP_MARGIN > (int)(j_maxlen - j_head)) {
        kprintf("jbd2: transaction will not fit in journal (%d tags)\n", n);
        return -1;
    }

    uint32_t at = j_head;
    uint32_t seq = j_seq_next;

    /* ── 1. descriptor block(s): the tag list ──
     *
     * The layout, from fs/jbd2/commit.c:
     *
     *     tag0  uuid(16)  tag1  tag2  ...  tagN
     *
     * The first tag of each descriptor is followed by 16 bytes of the journal
     * UUID, and every later tag sets JBD2_FLAG_SAME_UUID to mean "the same
     * UUID as the one at the front of this descriptor".  The last tag of each
     * descriptor sets JBD2_FLAG_LAST_TAG -- per descriptor, not per
     * transaction, so a transaction spanning several descriptors has several
     * tag-stream terminators.
     *
     * Consecutive tags sit exactly j_tag_bytes apart, with no padding.  That is
     * what the previous version got wrong: it advanced by
     * (j_tag_bytes + 3) & ~3, so on the 8-byte-tag journal mke2fs actually
     * produces, every second tag was four bytes out of position and the whole
     * stream was unparseable.  A reader cannot infer a rounded stride, so the
     * padding did not make the journal redundant, it made it unreadable.
     */
    int i = 0;
    for (int d = 0; d < ndesc; d++) {
        int count = n - i;
        if (count > per_desc) count = per_desc;
        int off = (int)sizeof(struct jbd2_header);

        memset(j_descbuf, 0, j_blocksize);
        struct jbd2_header *h = (struct jbd2_header *)j_descbuf;
        h->h_magic     = le32(JBD2_MAGIC_NUMBER);
        h->h_blocktype = le32(JBD2_DESCRIPTOR_BLOCK);
        h->h_sequence  = le32(seq);

        for (int k = 0; k < count; k++, i++) {
            uint16_t flags = 0;
            if (k > 0)  flags |= JBD2_FLAG_SAME_UUID;
            if (k == count - 1) flags |= JBD2_FLAG_LAST_TAG;

            uint8_t *t = j_descbuf + off;
            *(uint32_t *)(t + 0) = le32(j_tag_block[i]);
            /* t_checksum occupies the same slot in every tag size.  It is only
             * meaningful under csum_v2/csum_v3, which this journal does not
             * advertise, so it stays zero. */
            *(uint16_t *)(t + 4) = le16(0);
            *(uint16_t *)(t + 6) = le16(flags);
            off += j_tag_bytes;

            if (k == 0) {
                memcpy(j_descbuf + off, j_uuid, 16);
                off += 16;
            }
        }

        if (jwrite(at++, j_descbuf) < 0) return -1;
    }

    /* ── 2. journal data blocks, in tag order ── */
    for (int k = 0; k < n; k++)
        if (jwrite(at++, j_stage + (uint32_t)j_tag_slot[k] * j_blocksize) < 0)
            return -1;

    /* ── 3. commit block: the point of no return ──
     *
     * Everything past the 12-byte header stays zero.  A journal with no
     * checksum feature has no commit-block checksum: the kernel's
     * jbd2_commit_block_csum_set() returns early unless csum_v2/csum_v3 is
     * set, and the v1 path that would fill in h_chksum_type is guarded by
     * JBD2_FEATURE_COMPAT_CHECKSUM, which the kernel forbids coexisting with
     * csum_v2.  So a conforming v1 commit block is a bare header, and writing
     * a CRC32 into it -- as this did -- advertised a checksum type and size the
     * journal does not implement.
     */
    memset(j_descbuf, 0, j_blocksize);
    struct jbd2_commit *c = (struct jbd2_commit *)j_descbuf;
    c->h.h_magic     = le32(JBD2_MAGIC_NUMBER);
    c->h.h_blocktype = le32(JBD2_COMMIT_BLOCK);
    c->h.h_sequence  = le32(seq);
    if (jwrite(at++, j_descbuf) < 0) return -1;

    /* ── 4. home locations, only now that the commit is durable ── */
    for (int k = 0; k < n; k++)
        ext2_write_block_from(j_tag_block[k],
                              j_stage + (uint32_t)j_tag_slot[k] * j_blocksize);

    j_head = at;
    j_sequence = seq + 1;
    jbd2_reset_txn();
    return 0;
}

/* ── the write path hook ────────────────────────────────────────────────
 * Called by ext2.c in place of a direct block write. Returns non-zero if the
 * caller must also write the block itself; with a journal present it must not.
 */
int jbd2_stage_block(uint32_t fs_block, const void *data) {
    if (!j_present) return 1;                     /* no journal: write directly */
    if (j_txn_depth == 0 && jbd2_txn_begin() < 0) return 1;

    if (j_ntags >= JBD2_MAX_TAGS) {
        /* The staging area is full, so this write cannot wait for the caller's
         * commit point. End the transaction here and start another.
         *
         * The depth is saved and restored around the flush so that an enclosing
         * caller's transaction stays open: this split is a memory-pressure
         * boundary, not the end of the caller's operation. Correctness is
         * unaffected either way, because every committed transaction is atomic
         * on its own -- a crash may leave a file shorter than intended, which is
         * the same guarantee ext3 gives.
         *
         * `data` is snapshot into j_save first, and that is load-bearing, not
         * tidiness. The only buffer the caller has is ext2.c's shared block_buf,
         * and jbd2_flush() reaches the disk through ext2.c -- whose own
         * read_inode_block() uses that same buffer as scratch. Without the
         * snapshot, the flush leaves its scratch behind and the memcpy below
         * stages the journal's indirect block, which then gets written to the
         * caller's target. With j_map the flush no longer needs the buffer for
         * journal addressing, but ext2_write_block_from is not the only thing
         * that can touch it, and this is the one place where the two modules
         * share state implicitly.
         */
        memcpy(j_save, data, j_blocksize);
        data = j_save;
        int depth = j_txn_depth;
        if (jbd2_flush() < 0) return 1;
        j_txn_depth = depth;
        jbd2_reset_txn();
        j_seq_next = j_sequence;
    }

    j_tag_block[j_ntags] = fs_block;
    j_tag_slot[j_ntags]  = (uint32_t)j_ntags;
    memcpy(j_stage + (uint32_t)j_ntags * j_blocksize, data, j_blocksize);
    j_ntags++;
    return 0;    /* staged; the journal will place it */
}

void jbd2_note_free(uint32_t fs_block) {
    jbd2_revoke(fs_block);
}

int jbd2_peek_block(uint32_t fs_block, void *buf) {
    if (!j_present) return -1;
    /* Backwards, so a block staged more than once in the transaction reads back
     * as its newest version. See the note in jbd2.h for why this is required at
     * all rather than merely being nice. */
    for (int i = j_ntags - 1; i >= 0; i--)
        if (j_tag_block[i] == fs_block) {
            memcpy(buf, j_stage + (uint32_t)j_tag_slot[i] * j_blocksize, j_blocksize);
            return 0;
        }
    return -1;
}

int jbd2_have_journal(void) { return j_present; }
int jbd2_in_transaction(void) { return j_present && j_txn_depth > 0; }

/* ── replay ──────────────────────────────────────────────────────────────
 * Walk the log from the recorded start, applying committed transactions in
 * order. Stops at the first block that is not a descriptor carrying the
 * expected sequence, at a missing commit block, or at the end of the journal.
 *
 * The sequence walk is what makes stopping safe: a leftover block from an older
 * epoch has an older sequence, so it ends the walk rather than being misapplied.
 * It also means replaying a journal that is already up to date is harmless --
 * every transaction rewrites the same bytes -- so we always replay rather than
 * maintaining the clean-shutdown checksum bookkeeping that ext3 uses to skip it.
 *
 * ── Known gap: replay does not wrap ──
 *
 * The write path calls jbd2_wrap() and the log does wrap, but replay stops at
 * j_maxlen instead of continuing at j_first.  A transaction that has wrapped
 * past the end of the log is therefore not recovered, even though it committed
 * and its home copy may not have been written.
 *
 * That cannot lose data today, and the reason is worth stating precisely
 * because it is a property of the caller rather than of this code: flush()
 * writes every home location immediately after the commit block (step 4), so
 * by the time the log has wrapped there is no committed transaction whose
 * content exists only in the journal.  Replay is re-applying bytes that are
 * already on disk.
 *
 * It becomes a real data-loss bug the moment that stops being true -- which is
 * exactly what checkpointing would do, and is the main reason checkpointing is
 * not a simple addition.  Do not read "replayed N committed transactions" as
 * "every committed transaction was replayed".
 */
/* Returns 0 if a transaction was applied, 1 if the log simply ended, and -1 if
 * the log is malformed.
 *
 * The distinction matters more than it looks.  Running out of log is the normal
 * outcome of every mount: the tail of the journal holds blocks that were never
 * written, or a half-written transaction from the crash being recovered.  Both
 * look like "the next block is not a descriptor I can use", and both are
 * correct, so reporting them as corruption would print a warning on every
 * single boot.  A line that always fires is a line nobody reads, and the one
 * time it mattered -- a descriptor that no real reader can walk -- it would be
 * lost in the noise.  -1 is therefore reserved for a block that claims to be
 * part of the journal and is not walkable at all. */
static int jbd2_replay_one(uint32_t *pos, uint32_t expect,
                           uint32_t *tagbuf, uint8_t *tagdel, int *ntags) {
    uint32_t at = *pos;
    int ntotal = 0;   /* every tag consumes one log block, deleted or not */
    int done = 0;

    for (int guard = 0; !done && guard < 256; guard++) {
        if (at >= j_maxlen) return 1;              /* end of the journal */
        struct jbd2_header h;
        if (jread_hdr(at, &h) < 0) return -1;      /* unreadable: real fault */
        if (be32(h.h_magic) != JBD2_MAGIC_NUMBER)
            return 1;                              /* end of the written log */
        uint32_t btype = be32(h.h_blocktype);

        if (btype == JBD2_COMMIT_BLOCK) break;

        /* Dispatch on block type rather than demanding a descriptor.  Revoke
         * blocks and journal superblocks are legal between a transaction's
         * descriptors, and each is exactly one journal block.  Rejecting
         * anything that is not a descriptor is what made a journal containing
         * one replay as nothing at all. */
        if (btype == JBD2_REVOKE_BLOCK || btype == JBD2_SUPERBLOCK_V1 ||
            btype == JBD2_SUPERBLOCK_V2) {
            at++;
            continue;
        }
        if (btype != JBD2_DESCRIPTOR_BLOCK)
            return -1;                 /* claims to be journal, is not walkable */
        /* A descriptor from an older epoch, or a sequence we have already
         * passed: the normal state of a log that has wrapped. Not a fault. */
        if (be32(h.h_sequence) != expect) return 1;

        uint32_t dblk = at;
        at++;
        if (jread_block(dblk, j_iobuf) < 0) return -1;

        /* Tag stream: j_tag_bytes apart, no padding, with the 16-byte journal
         * UUID following any tag that does not claim SAME_UUID.  A 4-byte
         * rounded stride is what this driver used to use on both the write and
         * the read side, which is why the two agreed with each other and with
         * nothing else. */
        int off = (int)sizeof(struct jbd2_header);
        while (off + (int)j_tag_bytes <= (int)j_blocksize) {
            uint8_t *t = j_iobuf + off;
            uint16_t flags = be16(*(uint16_t *)(t + 6));
            if (flags & JBD2_FLAG_ESCAPE) { done = 1; break; }

            if (ntotal >= JBD2_MAX_TAGS) {
                /* Refuse rather than truncate.  Silently dropping the tail
                 * would leave the data blocks read for the tags that were kept
                 * lining up with the wrong home blocks. */
                kprintf("jbd2: transaction %u has more than %d tags;"
                        " stopping replay\n", expect, JBD2_MAX_TAGS);
                return -1;
            }
            tagbuf[ntotal] = be32(*(uint32_t *)(t + 0));
            tagdel[ntotal] = (flags & JBD2_FLAG_DELETED) ? 1 : 0;
            ntotal++;

            off += j_tag_bytes;
            if (!(flags & JBD2_FLAG_SAME_UUID)) off += 16;
            if (flags & JBD2_FLAG_LAST_TAG) { done = 1; break; }
        }
    }
    if (!done) return -1;      /* hit the guard, or the commit was never reached */

    /* One data block per tag, deleted tags included: the deleted flag says the
     * block's content is not wanted, not that it is absent from the log.  Not
     * counting them left the reader one block short and every subsequent tag
     * matched to the wrong content. */
    for (int i = 0; i < ntotal; i++) {
        if (at >= j_maxlen) return 1;       /* transaction ran off the log */
        if (jread_block(at, j_iobuf) < 0) return -1;
        if (!tagdel[i])
            memcpy(j_stage + (uint32_t)i * j_blocksize, j_iobuf, j_blocksize);
        at++;
    }

    /* No commit block means the transaction never committed: discard it whole.
     * That is the expected shape of a crash's last transaction, so it ends the
     * walk quietly.  Nothing has been written home yet -- the write-back below
     * is the first thing that touches the filesystem -- so returning here
     * leaves the half-transaction entirely unapplied, which is the point. */
    if (at >= j_maxlen) return 1;
    if (jread_block(at, j_iobuf) < 0) return -1;
    if (be32(*(uint32_t *)(j_iobuf)) != JBD2_MAGIC_NUMBER) return 1;
    if (be32(*(uint32_t *)(j_iobuf + 4)) != JBD2_COMMIT_BLOCK) return 1;
    if (be32(*(uint32_t *)(j_iobuf + 8)) != expect) return 1;
    at++;

    for (int i = 0; i < ntotal; i++)
        if (!tagdel[i])
            ext2_write_block_from(tagbuf[i], j_stage + (uint32_t)i * j_blocksize);

    *ntags = ntotal;
    *pos = at;
    return 0;
}

static int jbd2_replay(void) {
    uint32_t pos = j_first;
    uint32_t seq = j_sequence;
    uint32_t tagbuf[JBD2_MAX_TAGS];
    uint8_t  tagdel[JBD2_MAX_TAGS];
    int applied = 0, malformed = 0;

    while (pos < j_maxlen) {
        int n = 0;
        int r = jbd2_replay_one(&pos, seq, tagbuf, tagdel, &n);
        if (r < 0) { malformed = 1; break; }
        if (r > 0) break;                      /* log ended; expected */
        applied++;
        seq++;
    }
    j_head = pos;
    j_sequence = seq;
    if (malformed)
        kprintf("jbd2: WARNING replay stopped after %d transaction(s) at"
                " journal block %u: a journal block there is not walkable.\n"
                "    Transactions before it were applied; the rest of the log"
                " was left alone.\n", applied, pos);
    return applied;
}

/* ── mount / unmount ───────────────────────────────────────────────────── */

/* Release everything jbd2_init() may have allocated. Tolerates a partially
 * built state, because the point is to be callable from the failure paths. */
static void jbd2_free_buffers(void) {
    if (j_sb)         { free(j_sb);         j_sb = 0; }
    if (j_probe)      { free(j_probe);      j_probe = 0; }
    if (j_stage)      { free(j_stage);      j_stage = 0; }
    if (j_iobuf)      { free(j_iobuf);      j_iobuf = 0; }
    if (j_descbuf)    { free(j_descbuf);    j_descbuf = 0; }
    if (j_revoke_bits){ free(j_revoke_bits);j_revoke_bits = 0; }
    if (j_save)       { free(j_save);       j_save = 0; }
    if (j_map)        { free(j_map);        j_map = 0; }
    j_map_len = 0;
}

int jbd2_init(uint32_t journal_inode) {
    if (j_present) return 0;
    if (!journal_inode) {
        kprintf("ext3: HAS_JOURNAL is set but s_journal_inum is 0\n");
        return -1;
    }

    j_inum = journal_inode;
    j_sb = (uint8_t *)malloc(JBD2_HEADER_SIZE);
    if (!j_sb) return -1;

    /* j_probe has to exist before the superblock is read, because the block size
     * is not known until then and every read has to land somewhere full-size. */
    j_probe = (uint8_t *)malloc(JBD2_MAX_BLOCK_SIZE);
    if (!j_probe) { free(j_sb); j_sb = 0; return -1; }

    if (jbd2_load_sb() < 0) {
        kprintf("ext3: inode %u has no readable journal superblock\n", j_inum);
        jbd2_free_buffers();
        return -1;
    }

    /* The tag layout on disk is fixed by the feature bits in the journal
     * superblock, and a writer has no choice about it: the stride between
     * consecutive tags is exactly the tag size, so a tag written at the wrong
     * size puts every subsequent tag at the wrong offset and no reader can
     * walk the stream.
     *
     * This driver previously overrode the disk's feature set, force-enabled
     * csum_v2 in memory and wrote 10-byte tags, while leaving the superblock
     * saying it had no checksum features at all.  A stock "mke2fs -t ext3"
     * superblock has s_feature_incompat == 0 and s_checksum_type == 0, so a
     * conforming writer must emit 8-byte tags, and every journal this driver
     * wrote was unparseable: e2fsck's replay silently recovered nothing and
     * still exited 0.  Writing 8-byte tags for a featureless journal is a real
     * JBD2 journal, so that is what is done here.
     *
     * Checksummed journals are refused rather than faked.  csum_v2 and csum_v3
     * need CRC32C seeded from s_uuid over a different byte range for each of
     * the three checksums, and a journal that claims a checksum the reader
     * computes differently is worse than no journal at all.  Falling back to
     * journalless ext2 is the existing, tested degradation path. */
    if (j_feature_incompat) {
        const char *why = "unknown";
        if (j_feature_incompat & JBD2_FEATURE_INCOMPAT_CSUM_V3) why = "csum_v3";
        else if (j_feature_incompat & JBD2_FEATURE_INCOMPAT_64BIT) why = "64bit";
        else if (j_feature_incompat & JBD2_FEATURE_INCOMPAT_CSUM_V2) why = "csum_v2";
        else if (j_feature_incompat & JBD2_FEATURE_INCOMPAT_ASYNC_COMMIT)
            why = "async_commit";
        else if (j_feature_incompat & JBD2_FEATURE_INCOMPAT_REVOKE)
            why = "revoke";
        kprintf("ext3: journal needs features 0x%08x (%s) -- not implemented,"
                " mounting without a journal\n", j_feature_incompat, why);
        jbd2_free_buffers();
        return -1;
    }
    j_tag_bytes = JBD2_TAG_SIZE_DEFAULT;      /* 8: blocknr + cksum + flags */

    /* One bit per filesystem block, not per journal block. */
    uint32_t fs_blocks = ext2_block_count();
    j_revoke_bytes = (fs_blocks + 7) / 8;
    if (j_revoke_bytes == 0) j_revoke_bytes = 1;

    /* The journal block size must match the filesystem's, and the reason is a
     * buffer rather than a format preference: the journal stages whole blocks
     * through ext2.c's block buffer, which is sized for the filesystem's block
     * size, so a larger journal block would read and write past it. mke2fs never
     * creates such a journal (s_blocksize is a copy of the fs block size), so
     * this is a refusal of malformed input rather than a real layout. */
    if (j_blocksize != ext2_block_size()) {
        kprintf("ext3: journal block size %u does not match filesystem block"
                " size %u -- mounting without a journal\n",
                j_blocksize, ext2_block_size());
        jbd2_free_buffers();
        return -1;
    }

    /* Now that j_maxlen is known, resolve the journal's block map. This is an
     * optimisation with a correctness motive: without it, every journal block
     * access walks the journal inode, and that walk happens in the filesystem
     * driver's shared block buffer -- so a flush running between "caller filled
     * block_buf" and "caller staged block_buf" silently staged the journal
     * inode's indirect block instead. Failure here only costs speed; jphys()
     * falls back to walking the inode, which is correct. */
    if (jbd2_build_map() < 0)
        kprintf("ext3: journal block map not cached (%u blocks), using slow path\n",
                j_maxlen);

    j_stage = (uint8_t *)malloc(JBD2_STAGE_BLOCKS * j_blocksize);
    j_iobuf = (uint8_t *)malloc(j_blocksize);
    j_descbuf = (uint8_t *)malloc(j_blocksize);
    j_revoke_bits = (uint8_t *)malloc(j_revoke_bytes);
    j_save = (uint8_t *)malloc(j_blocksize);
    if (!j_stage || !j_iobuf || !j_descbuf || !j_revoke_bits || !j_save) {
        kprintf("ext3: cannot allocate journal buffers (stage %u, io %u,"
                " desc %u, revoke %u, save %u)\n",
                JBD2_STAGE_BLOCKS * j_blocksize, j_blocksize, j_blocksize,
                j_revoke_bytes, j_blocksize);
        jbd2_free_buffers();
        return -1;
    }
    memset(j_revoke_bits, 0, j_revoke_bytes);

    j_present = 1;
    jbd2_reset_txn();

    int n = jbd2_replay();
    kprintf("ext3: journal on inode %u, %u x %u B, %d B tags, log at block %u\n",
            j_inum, j_maxlen, j_blocksize, j_tag_bytes, j_head);
    if (n > 0)
        kprintf("ext3: replayed %d committed transaction%s\n", n, n == 1 ? "" : "s");
    else
        kprintf("ext3: journal clean, nothing to replay\n");
    return 0;
}

void jbd2_shutdown(int clean) {
    if (!j_present) return;
    /* `clean` does not change what the journal does: the log position is
     * recorded either way, because the journal has to be left in a state the
     * next mount can walk. Whether the FILESYSTEM claims a clean unmount is the
     * superblock's business (s_state), and ext2_unmount() sets that separately.
     * Honouring it here too would mean marking the fs clean while leaving
     * replayable transactions on disk, which is exactly the state that makes
     * e2fsck report a filesystem needing recovery. */
    (void)clean;
    if (j_txn_depth > 0) { j_txn_depth = 1; jbd2_txn_commit(); }
    jbd2_store_sb(j_head, j_sequence);
    j_present = 0;
    /* Free the staging and map buffers here rather than leaking them: unmount is
     * not necessarily terminal (a remount re-runs jbd2_init, and jbd2_build_map
     * would otherwise hand back a second copy of the journal's block map). */
    jbd2_free_buffers();
}
