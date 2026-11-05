/* SPDX-License-Identifier: GPL-2.0-only
 * Innioasis Y1 second-boot additions to y1-init.c (docs/planning/Y1-SECOND-BOOT-PLAN.md), included only
 * when Y1_SECOND_BOOT is defined, so the first-boot PID1 stays byte-identical. Reads storage read-only
 * (BLKROSET on each disk before its first read) and shows SHA-256 digests to compare with the owner's
 * 2026-11-04 backup; then shows key events for a minute.
 */
typedef unsigned char u8;
typedef unsigned int u32;
typedef unsigned long long u64;

enum { SYS_IOCTL = 54, SYS_LLSEEK = 140 };
enum { O_RDONLY = 0, O_NONBLOCK = 04000, SEEK_SET = 0, EAGAIN = 11, MS_SILENT = 32768 };
enum { BLKROSET = 0x125d, BLKGETSIZE64 = 0x80041272 }; /* _IO(0x12,93), _IOR(0x12,114,size_t) */
enum { CHUNK = 65536, EV_KEY = 1, INPUT_NODES = 5, KEY_WINDOW_TENTHS = 600, REFUSED = -1000 };

/* ---- SHA-256 (FIPS 180-4) ------------------------------------------------------------------------ */

struct sha256 {
    u32 h[8];
    u8 block[64];
    unsigned used;
    u64 bytes;
};

static const u32 sha256_k[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

static u32 ror(u32 x, unsigned n) { return x >> n | x << (32 - n); }

static void sha256_block(struct sha256 *s, const u8 *p)
{
    u32 w[64], v[8];
    for (unsigned i = 0; i < 16; ++i)
        w[i] = (u32)p[4 * i] << 24 | (u32)p[4 * i + 1] << 16 | (u32)p[4 * i + 2] << 8 | p[4 * i + 3];
    for (unsigned i = 16; i < 64; ++i)
        w[i] = w[i - 16] + (ror(w[i - 15], 7) ^ ror(w[i - 15], 18) ^ w[i - 15] >> 3) + w[i - 7] +
               (ror(w[i - 2], 17) ^ ror(w[i - 2], 19) ^ w[i - 2] >> 10);
    for (unsigned i = 0; i < 8; ++i) v[i] = s->h[i];
    for (unsigned i = 0; i < 64; ++i) {
        u32 t1 = v[7] + (ror(v[4], 6) ^ ror(v[4], 11) ^ ror(v[4], 25)) + ((v[4] & v[5]) ^ (~v[4] & v[6])) +
                 sha256_k[i] + w[i];
        u32 t2 = (ror(v[0], 2) ^ ror(v[0], 13) ^ ror(v[0], 22)) + ((v[0] & v[1]) ^ (v[0] & v[2]) ^ (v[1] & v[2]));
        for (unsigned j = 7; j; --j) v[j] = v[j - 1];
        v[4] += t1;
        v[0] = t1 + t2;
    }
    for (unsigned i = 0; i < 8; ++i) s->h[i] += v[i];
}

static void sha256_init(struct sha256 *s)
{
    static const u32 iv[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                              0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    for (unsigned i = 0; i < 8; ++i) s->h[i] = iv[i];
    s->used = 0;
    s->bytes = 0;
}

static void sha256_update(struct sha256 *s, const u8 *p, unsigned n)
{
    s->bytes += n;
    while (n) {
        s->block[s->used++] = *p++;
        --n;
        if (s->used == 64) { sha256_block(s, s->block); s->used = 0; }
    }
}

static void sha256_final(struct sha256 *s, u8 out[32])
{
    u64 bits = s->bytes * 8;
    u8 pad = 0x80, zero = 0, length[8];
    sha256_update(s, &pad, 1);
    while (s->used != 56) sha256_update(s, &zero, 1);
    for (unsigned i = 0; i < 8; ++i) length[i] = (u8)(bits >> (56 - 8 * i));
    sha256_update(s, length, 8);
    for (unsigned i = 0; i < 32; ++i) out[i] = (u8)(s->h[i / 4] >> (24 - 8 * (i % 4)));
}

static void to_hex(const u8 *p, unsigned n, char *out)
{
    for (unsigned i = 0; i < n; ++i) {
        out[2 * i] = "0123456789abcdef"[p[i] >> 4];
        out[2 * i + 1] = "0123456789abcdef"[p[i] & 15];
    }
    out[2 * n] = 0;
}

static int same(const char *a, const char *b)
{
    while (*a && *a == *b) { ++a; ++b; }
    return *a == *b;
}

/* "abc" (FIPS 180-4 example), run by the build under QEMU through --selftest. */
static int sha256_selftest(void)
{
    struct sha256 s;
    u8 digest[32];
    char hex[65];
    sha256_init(&s);
    sha256_update(&s, (const u8 *)"abc", 3);
    sha256_final(&s, digest);
    to_hex(digest, 32, hex);
    return same(hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

/* ---- storage, read-only ------------------------------------------------------------------------- */

static u8 chunk[CHUNK];

static void sleep_tenths(unsigned tenths)
{
    struct timespec64 request = { tenths / 10, (s64)(tenths % 10) * 100000000 };
    while (call5(SYS_CLOCK_NANOSLEEP_TIME64, CLOCK_MONOTONIC, 0, (long)&request, (long)&request, 0) == -EINTR)
        ;
}

static void feed(long watchdog)
{
    if (watchdog >= 0) call5(SYS_WRITE, watchdog, (long)"k", 1, 0, 0);
}

/* Opens a disk and makes it read-only before anything reads it; refuses the disk otherwise. */
static long open_read_only(const char *path)
{
    long fd = call5(SYS_OPEN, (long)path, O_RDONLY, 0, 0, 0);
    int one = 1;
    if (fd < 0) return fd;
    if (call5(SYS_IOCTL, fd, BLKROSET, (long)&one, 0, 0) != 0) {
        say(path); say(": cannot set read-only, not read\n");
        call5(SYS_CLOSE, fd, 0, 0, 0, 0);
        return REFUSED;
    }
    return fd;
}

static long wait_for_disk(const char *path, unsigned tenths, long watchdog)
{
    for (unsigned t = 0; t <= tenths; ++t) {
        long fd = open_read_only(path);
        if (fd >= 0 || fd == REFUSED) return fd;
        if (t % 10 == 0) feed(watchdog);
        sleep_tenths(1);
    }
    say(path); say(": not present\n");
    return -2;
}

static u64 disk_size(long fd)
{
    u64 size = 0; /* the kernel writes 64 bits whatever size_t is */
    return call5(SYS_IOCTL, fd, BLKGETSIZE64, (long)&size, 0, 0) == 0 ? size : 0;
}

/* SHA-256 of [offset, offset + length), compared with the backup's digest when one is given. */
static void digest(long fd, const char *label, u64 offset, u64 length, const char *expected, long watchdog)
{
    struct sha256 s;
    u8 out[32];
    char hex[65];
    long long position;
    sha256_init(&s);
    if (call5(SYS_LLSEEK, fd, (long)(offset >> 32), (long)offset, (long)&position, SEEK_SET) != 0) {
        say(label); say(": seek failed\n");
        return;
    }
    for (u64 done = 0; done < length;) {
        unsigned want = length - done < CHUNK ? (unsigned)(length - done) : CHUNK;
        long n = call5(SYS_READ, fd, (long)chunk, want, 0, 0);
        if (n == -EINTR) continue;
        if (n <= 0) { say(label); say(": read failed\n"); return; }
        sha256_update(&s, chunk, (unsigned)n);
        done += (unsigned)n;
        if ((done & 0xfffff) == 0) feed(watchdog);
    }
    sha256_final(&s, out);
    to_hex(out, 32, hex);
    say(label); say(" "); write_all(hex, 16); say("...");
    say(!expected ? "\n" : same(hex, expected) ? " MATCHES backup\n" : " DIFFERS from backup\n");
}

static void show_file(const char *path)
{
    long fd = call5(SYS_OPEN, (long)path, O_RDONLY, 0, 0, 0);
    long n = fd < 0 ? fd : call5(SYS_READ, fd, (long)chunk, 2048, 0, 0);
    if (fd >= 0) call5(SYS_CLOSE, fd, 0, 0, 0, 0);
    if (n > 0) write_all((const char *)chunk, (unsigned)n);
}

static void say_hex32(u32 value)
{
    char text[11] = "0x00000000";
    for (unsigned i = 0; i < 8; ++i) text[9 - i] = "0123456789abcdef"[(value >> (4 * i)) & 15];
    write_all(text, 10);
}

/* Where the boot-image headers and the 55AA tables sit on the physical eMMC (the first 64 MiB, every
 * 64 KiB): settles how the mtkclient dump's offsets map to Linux's (run 3 still differed). */
static void locate_markers(long fd, long watchdog)
{
    unsigned images = 0, tables = 0;
    say("markers (physical): ");
    for (u32 offset = 0; offset < 0x4000000; offset += 0x10000) {
        long long position;
        if (call5(SYS_LLSEEK, fd, 0, (long)offset, (long)&position, SEEK_SET) != 0 ||
            call5(SYS_READ, fd, (long)chunk, 512, 0, 0) != 512) {
            say("read failed\n");
            return;
        }
        if (images < 3 && chunk[0] == 'A' && chunk[1] == 'N' && chunk[2] == 'D' && chunk[3] == 'R' &&
            chunk[4] == 'O' && chunk[5] == 'I' && chunk[6] == 'D' && chunk[7] == '!') {
            say("ANDROID! "); say_hex32(offset); say(" ");
            ++images;
        }
        if (tables < 3 && chunk[510] == 0x55 && chunk[511] == 0xaa) {
            say("55AA "); say_hex32(offset); say(" ");
            ++tables;
        }
        if ((offset & 0xfffff) == 0) feed(watchdog);
    }
    say("\n");
}

static void show_disks(long watchdog)
{
    say("\nY1 SECOND BOOT: storage (read-only)\n");
    /* Physical eMMC offsets: the 2026-11-04 mtkclient dump is MediaTek's logical space, BOOT0, BOOT1 and
     * RPMB (2 MiB each) first and the user area from 0x600000 on (second boot run 4 markers). Digests are
     * of partitions stock Android does not rewrite; NVRAM and PROTECT change while it runs. */
    long emmc = wait_for_disk("/dev/mmcblk0", 100, watchdog);
    if (emmc >= 0) {
        say("mmcblk0 MiB: "); say_number((unsigned)(disk_size(emmc) >> 20)); say("\n");
        locate_markers(emmc, watchdog);
        digest(emmc, "MBR+EBR1", 0xe00000, 0x100000,
               "2b0367160b125595474bc22947d6fd78b68696d65c1ae2857920c3c0134a42a5", watchdog);
        digest(emmc, "UBOOT (LK)", 0x2b20000, 0x60000,
               "66c92662f064f7a8ba4f18d23a263cac59942f35975a87b56102852c958a0627", watchdog);
        digest(emmc, "RECOVERY", 0x3180000, 0x600000,
               "e9fe26a20e2037c12631047a9b40b62790731e9d0a078cf8f913947cbb5da6fd", watchdog);
        digest(emmc, "LOGO", 0x3840000, 0x300000,
               "ef315556538b9f5d86133e3590b7d39a5a3ee22458d8417c26906531bc259f14", watchdog);
    }
    static const char *const boot_areas[] = {"/dev/mmcblk0boot0", "/dev/mmcblk0boot1"};
    static const char *const boot_digests[] = {
        "f3748d6a392f94f29621f38b0a7b2ea64303f6f92e91b8fc7042729954517713",
        "5647f05ec18958947d32874eeb788fa396a05d0bab7c1b71f112ceb7e9b31eee",
    };
    for (unsigned i = 0; i < 2; ++i) {
        long fd = wait_for_disk(boot_areas[i], 10, watchdog);
        if (fd >= 0) digest(fd, boot_areas[i] + 5, 0, disk_size(fd), boot_digests[i], watchdog);
    }
    long sd = wait_for_disk("/dev/mmcblk1", 30, watchdog);
    if (sd >= 0) {
        say("microSD MiB: "); say_number((unsigned)(disk_size(sd) >> 20)); say("\n");
        digest(sd, "microSD first MiB", 0, 0x100000, 0, watchdog);
    }
    show_file("/proc/partitions");
}

/* ---- keys --------------------------------------------------------------------------------------- */

struct input_event32 {
    u32 sec, usec;
    unsigned short type, code;
    int value;
};

static const char *key_name(unsigned code)
{
    switch (code) {
    case 28: return "ENTER (centre)";
    case 103: return "UP (wheel)";
    case 104: return "PAGEUP (wheel)";
    case 108: return "DOWN (wheel)";
    case 109: return "PAGEDOWN (wheel)";
    case 158: return "BACK (top)";
    case 163: return "NEXTSONG (right)";
    case 164: return "PLAYPAUSE (bottom)";
    case 165: return "PREVIOUSSONG (left)";
    }
    return "other";
}

static void show_event(const struct input_event32 *e)
{
    if (e->type != EV_KEY) return;
    say("key "); say_number(e->code); say(" "); say(key_name(e->code));
    say(e->value ? " down\n" : " up\n");
}

__attribute__((unused)) static void show_keys(long watchdog)
{
    char path[] = "/dev/input/event0";
    long fds[INPUT_NODES];
    for (unsigned i = 0; i < INPUT_NODES; ++i) {
        path[16] = (char)('0' + i);
        fds[i] = call5(SYS_OPEN, (long)path, O_RDONLY | O_NONBLOCK, 0, 0, 0);
    }
    say("\nY1 SECOND BOOT: press the wheel and the buttons (60 s)\n");
    for (unsigned t = 0; t < KEY_WINDOW_TENTHS; ++t) {
        for (unsigned i = 0; i < INPUT_NODES; ++i) {
            struct input_event32 e;
            while (fds[i] >= 0 && call5(SYS_READ, fds[i], (long)&e, sizeof(e), 0, 0) == (long)sizeof(e))
                show_event(&e);
        }
        if (t % 10 == 0) feed(watchdog);
        sleep_tenths(1);
    }
}

__attribute__((unused)) static void second_boot(long watchdog)
{
    if (call5(SYS_MOUNT, (long)"devtmpfs", (long)"/dev", (long)"devtmpfs", MS_SILENT, 0) != 0)
        say("devtmpfs: mount failed\n");
    show_disks(watchdog);
    show_keys(watchdog);
}
