// nano-ata.h — an ATA PIO disk, so that a file can outlive the machine.
//
// Until now nano-fs.h sat on a RAM disk: fs_dev_init kmalloc'd the blocks and
// said so in a comment. Everything above it -- inodes, directories, indirect
// blocks, rename, truncate -- was real, and none of it survived a reboot,
// which meant the filesystem was a data structure rather than a filesystem.
//
// This is the smallest thing that changes that: the primary ATA channel in
// PIO mode, LBA28, one sector at a time. It is deliberately not a driver with
// a request queue and an interrupt handler -- it polls. A polling driver is
// the honest starting point, because every part of it can be read straight
// through, and a disk that works slowly can be made fast later while a disk
// that works subtly wrongly eats files.
//
// BLK_SIZE is 512 and an ATA sector is 512, so a block is a sector and there
// is no multi-sector arithmetic anywhere in here. That is worth keeping.
//
// THE ONE DESIGN DECISION WORTH ARGUING ABOUT: when no disk is present, this
// does NOT quietly fall back to memory. A fallback that looks like success is
// worse than a failure -- the caller would write files, see them written, and
// lose them at reboot with nothing having reported a problem. ata_init
// returns 0 and the caller decides, out loud.

#ifndef NANO_ATA_H
#define NANO_ATA_H

extern int  inb(int port);
extern void outb(int port, int val);
extern void insw();
extern void outsw();

// Primary channel, I/O base 0x1F0, control base 0x3F6.
#define ATA_DATA     0x1F0
#define ATA_FEATURES 0x1F1
#define ATA_SECCNT   0x1F2
#define ATA_LBA0     0x1F3
#define ATA_LBA1     0x1F4
#define ATA_LBA2     0x1F5
#define ATA_DRIVE    0x1F6
#define ATA_CMD      0x1F7     // write: command
#define ATA_STATUS   0x1F7     // read: status
#define ATA_ALTSTAT  0x3F6     // read: status WITHOUT clearing the interrupt

#define ATA_SR_ERR   0x01
#define ATA_SR_DRQ   0x08
#define ATA_SR_DF    0x20
#define ATA_SR_DRDY  0x40
#define ATA_SR_BSY   0x80

#define ATA_CMD_READ     0x20
#define ATA_CMD_WRITE    0x30
#define ATA_CMD_FLUSH    0xE7
#define ATA_CMD_IDENTIFY 0xEC

// Every wait in here is bounded. A missing or wedged drive must make a
// function return an error, not spin the kernel forever -- this polls with
// interrupts on, so an infinite loop here is a hang with a live timer, which
// looks like a working machine that has stopped doing anything.
#define ATA_TIMEOUT 2000000

long ata_present;
long ata_sectors;              // as reported by IDENTIFY
long ata_reads;                // counters, so a test can assert I/O happened
long ata_writes;
long ata_errors;

// Reading the alternate status port takes about 100ns and has no side
// effects; four of them is the standard settle after selecting a drive or
// issuing a command, before the status register means anything.
void ata_settle() {
    inb(ATA_ALTSTAT); inb(ATA_ALTSTAT); inb(ATA_ALTSTAT); inb(ATA_ALTSTAT);
}

// Wait for BSY to clear. Returns the status, or -1 if it never did.
long ata_wait_bsy() {
    long i;
    long st;
    i = 0;
    while (i < ATA_TIMEOUT) {
        st = inb(ATA_STATUS);
        if (!(st & ATA_SR_BSY)) return st;
        i = i + 1;
    }
    return -1;
}

// Wait until the drive is ready to move a sector: BSY clear and DRQ set.
// Returns 1, or 0 having counted an error. ERR and DF are checked on every
// pass rather than only at the end, because a drive that faults leaves DRQ
// clear forever and the timeout would report the wrong thing.
long ata_wait_drq() {
    long i;
    long st;
    i = 0;
    while (i < ATA_TIMEOUT) {
        st = inb(ATA_STATUS);
        if (st & ATA_SR_ERR) { ata_errors = ata_errors + 1; return 0; }
        if (st & ATA_SR_DF)  { ata_errors = ata_errors + 1; return 0; }
        if (!(st & ATA_SR_BSY) && (st & ATA_SR_DRQ)) return 1;
        i = i + 1;
    }
    ata_errors = ata_errors + 1;
    return 0;
}

// Select the master drive and load an LBA28 address. The top nibble of the
// address goes into the low bits of the drive register alongside 0xE0, which
// is what says "master, LBA addressing" -- 0xA0 there would mean CHS and the
// drive would seek somewhere else entirely.
void ata_select(long lba) {
    outb(ATA_DRIVE, 0xE0 | ((lba >> 24) & 0x0F));
    ata_settle();
    outb(ATA_FEATURES, 0);
    outb(ATA_SECCNT, 1);
    outb(ATA_LBA0, lba & 0xFF);
    outb(ATA_LBA1, (lba >> 8) & 0xFF);
    outb(ATA_LBA2, (lba >> 16) & 0xFF);
}

long ata_read_sector(long lba, char *buf) {
    if (!ata_present) return 0;
    if (lba < 0 || (ata_sectors > 0 && lba >= ata_sectors)) return 0;
    if (ata_wait_bsy() < 0) { ata_errors = ata_errors + 1; return 0; }
    ata_select(lba);
    outb(ATA_CMD, ATA_CMD_READ);
    ata_settle();
    if (!ata_wait_drq()) return 0;
    insw(ATA_DATA, buf, 256);
    ata_reads = ata_reads + 1;
    return 1;
}

long ata_write_sector(long lba, char *buf) {
    if (!ata_present) return 0;
    if (lba < 0 || (ata_sectors > 0 && lba >= ata_sectors)) return 0;
    if (ata_wait_bsy() < 0) { ata_errors = ata_errors + 1; return 0; }
    ata_select(lba);
    outb(ATA_CMD, ATA_CMD_WRITE);
    ata_settle();
    if (!ata_wait_drq()) return 0;
    outsw(ATA_DATA, buf, 256);
    // CACHE FLUSH, and it is not optional. The drive may acknowledge a write
    // that is still only in its own buffer; without this, a write followed by
    // a power cut -- or by qemu exiting -- can leave the sector unchanged on
    // the file behind it. The whole point of this milestone is that the write
    // is still there next boot.
    if (ata_wait_bsy() < 0) { ata_errors = ata_errors + 1; return 0; }
    outb(ATA_CMD, ATA_CMD_FLUSH);
    if (ata_wait_bsy() < 0) { ata_errors = ata_errors + 1; return 0; }
    ata_writes = ata_writes + 1;
    return 1;
}

// IDENTIFY. Returns 1 and fills ata_sectors if a drive answered.
//
// The status register reading 0 means "no drive on this channel" -- a floating
// bus reads 0xFF, a present-but-idle drive reads something with DRDY in it.
// Both are checked, because the two failure modes look different and a driver
// that only knows one of them hangs on the other.
// The IDENTIFY response, as a global rather than 512 bytes of kernel stack.
char g_ata_id[512];

long ata_identify() {
    long st;
    long i;

    outb(ATA_DRIVE, 0xE0);
    ata_settle();
    outb(ATA_SECCNT, 0);
    outb(ATA_LBA0, 0);
    outb(ATA_LBA1, 0);
    outb(ATA_LBA2, 0);
    outb(ATA_CMD, ATA_CMD_IDENTIFY);
    ata_settle();

    st = inb(ATA_STATUS);
    if (st == 0 || st == 0xFF) return 0;          // nothing there

    if (ata_wait_bsy() < 0) return 0;
    // LBA1 and LBA2 non-zero after IDENTIFY means this is not an ATA device
    // -- an ATAPI drive answers here with 0x14/0xEB, and reading its data
    // register as if it were a disk gets nonsense.
    if (inb(ATA_LBA1) != 0 || inb(ATA_LBA2) != 0) return 0;
    if (!ata_wait_drq()) return 0;

    insw(ATA_DATA, g_ata_id, 256);

    // Words 60..61 hold the LBA28 sector count, little-endian, low word first.
    //
    // MASKED, NOT CAST. nano_cc accepts `unsigned` and ignores it, so
    // (unsigned char)id[120] is still a signed byte and any sector-count byte
    // above 0x7F would sign-extend and corrupt the whole number. `& 0xFF` is
    // the only spelling that means what it says here. This is the same trap
    // as the a>>60 one.
    ata_sectors = (g_ata_id[120] & 0xFF) |
                  ((g_ata_id[121] & 0xFF) << 8) |
                  ((g_ata_id[122] & 0xFF) << 16) |
                  ((g_ata_id[123] & 0xFF) << 24);
    i = ata_sectors;
    if (i < 0) ata_sectors = 0;                   // nonsense; treat as unknown
    return 1;
}

long ata_init() {
    ata_present = 0;
    ata_sectors = 0;
    ata_reads = 0;
    ata_writes = 0;
    ata_errors = 0;
    if (!ata_identify()) return 0;
    ata_present = 1;
    return 1;
}

#endif
