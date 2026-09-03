// nano-acpi.h — find the firmware's ACPI tables and use what they describe.
//
// Compiled by nano_cc with --kernel. Include AFTER nano-int.h.
//
// Scope, stated up front because ACPI invites overclaiming. This reads the
// STATIC tables: the RSDP, the RSDT/XSDT directory, the FADT and the MADT.
// Those are plain C structures at fixed offsets and can be read honestly.
//
// It does NOT interpret AML. Most of what ACPI can tell you -- including the
// _CST method that describes a processor's real C-states -- is bytecode in the
// DSDT, and running it needs an interpreter, which is a project rather than a
// feature. The one AML thing done here is scanning the DSDT for the fixed-form
// `Processor` declaration (opcode 5B 83), which has the P_BLK address at a
// known offset inside it. That is a byte scan of a known encoding, not an
// interpreter, and it is the difference between knowing the C2 entry port and
// guessing it.

#ifndef NANO_ACPI_H
#define NANO_ACPI_H

// mem8/mem16/mem32/mem64 live in nano-kernel.h -- nano-mm.h needs them too.

long sig_matches(long addr, char *want) {
    long i;
    i = 0;
    while (i < 4) {
        if (mem8(addr + i) != (want[i] & 255)) return 0;
        i = i + 1;
    }
    return 1;
}

// Every ACPI table's bytes must sum to zero in the low 8 bits. A table that
// fails this is not a table -- it is whatever happened to be at that address,
// and following its pointers would walk into nothing.
long checksum_ok(long addr, long len) {
    long sum;
    long i;
    sum = 0;
    i = 0;
    while (i < len) { sum = sum + mem8(addr + i); i = i + 1; }
    return (sum & 255) == 0;
}

// ---------- discovered state ----------
long acpi_rsdp;
long acpi_rev;          // 0 = ACPI 1.0 (RSDT only), 2+ = XSDT available
long acpi_root;         // RSDT or XSDT address
long acpi_root_is_xsdt;
long acpi_ntables;

long acpi_fadt;
long acpi_dsdt;
long acpi_madt;

long acpi_pm_tmr;       // I/O port of the power-management timer, 0 if none
long acpi_pm_tmr_32;    // 1 if it counts to 32 bits, 0 if 24
long acpi_pm1a_cnt;
long acpi_smi_cmd;
long acpi_enable_val;
long acpi_c2_lat;       // microseconds; > 100 means C2 is not supported
long acpi_c3_lat;       // > 1000 means C3 is not supported
long acpi_pblk;         // per-CPU control block from the DSDT, 0 if not found
long acpi_cpus;

// ---------- step 1: find the RSDP ----------
// It lives either in the first kilobyte of the Extended BIOS Data Area, whose
// segment is at 0x40E, or in the BIOS area from 0xE0000 to 0xFFFFF. It is
// aligned to 16 bytes in both.
long rsdp_scan(long from, long to) {
    long a;
    a = from;
    while (a < to) {
        if (sig_matches(a, "RSD ") && sig_matches(a + 4, "PTR ")) {
            if (checksum_ok(a, 20)) return a;      // the v1 part is 20 bytes
        }
        a = a + 16;
    }
    return 0;
}

long acpi_find_rsdp() {
    long ebda;
    long r;
    ebda = mem16(0x40E) * 16;
    if (ebda > 0x400 && ebda < 0xA0000) {
        r = rsdp_scan(ebda, ebda + 1024);
        if (r) return r;
    }
    return rsdp_scan(0xE0000, 0x100000);
}

// ---------- step 2: walk the directory ----------
long acpi_table_at(long index) {
    long entries;
    entries = acpi_root + 36;                      // past the table header
    if (acpi_root_is_xsdt) return mem64(entries + index * 8);
    return mem32(entries + index * 4);
}

long acpi_find_table(char *sig) {
    long i;
    i = 0;
    while (i < acpi_ntables) {
        long t;
        t = acpi_table_at(i);
        if (t && sig_matches(t, sig)) return t;
        i = i + 1;
    }
    return 0;
}

// ---------- step 3: the FADT ----------
// Fixed offsets from the ACPI specification. Named constants rather than bare
// numbers, because an off-by-four here reads a different field entirely and
// still returns a plausible number.
#define FADT_DSDT        40
#define FADT_SMI_CMD     48
#define FADT_ACPI_ENABLE 52
#define FADT_PM1A_CNT    64
#define FADT_PM_TMR_BLK  76
#define FADT_PM_TMR_LEN  91
#define FADT_P_LVL2_LAT  96
#define FADT_P_LVL3_LAT  98
#define FADT_FLAGS       112
#define FADT_FLAG_TMR32  256          // bit 8: the PM timer is 32-bit

void acpi_read_fadt() {
    long flags;
    if (!acpi_fadt) return;
    acpi_dsdt        = mem32(acpi_fadt + FADT_DSDT);
    acpi_smi_cmd     = mem32(acpi_fadt + FADT_SMI_CMD);
    acpi_enable_val  = mem8(acpi_fadt + FADT_ACPI_ENABLE);
    acpi_pm1a_cnt    = mem32(acpi_fadt + FADT_PM1A_CNT);
    acpi_pm_tmr      = mem32(acpi_fadt + FADT_PM_TMR_BLK);
    acpi_c2_lat      = mem16(acpi_fadt + FADT_P_LVL2_LAT);
    acpi_c3_lat      = mem16(acpi_fadt + FADT_P_LVL3_LAT);
    flags            = mem32(acpi_fadt + FADT_FLAGS);
    acpi_pm_tmr_32   = (flags & FADT_FLAG_TMR32) ? 1 : 0;
    if (mem8(acpi_fadt + FADT_PM_TMR_LEN) != 4) acpi_pm_tmr = 0;   // must be 4
}

// ---------- step 4: the MADT, for a CPU count ----------
// A header then a list of variable-length entries: type byte, length byte,
// then the body. Type 0 is a local APIC, and its flags bit 0 says whether the
// processor is usable.
void acpi_read_madt() {
    long len;
    long p;
    long end;
    acpi_cpus = 0;
    if (!acpi_madt) return;
    len = mem32(acpi_madt + 4);
    p = acpi_madt + 44;                            // past header + fixed fields
    end = acpi_madt + len;
    while (p + 2 <= end) {
        long type;
        long elen;
        type = mem8(p);
        elen = mem8(p + 1);
        if (elen < 2) return;                      // malformed: stop, do not spin
        if (type == 0) {
            long fl;
            fl = mem32(p + 4);
            if (fl & 1) acpi_cpus = acpi_cpus + 1;
        }
        p = p + elen;
    }
}

// ---------- step 5: the DSDT's Processor declaration ----------
// AML encodes `Processor (CPU0, 0, 0x0410, 6)` as:
//   5B 83 <PkgLength> <NameString 4 bytes> <ProcID 1> <P_BLK addr 4> <len 1>
// The PkgLength's first byte says in its top two bits how many extra length
// bytes follow, which is what makes the offset to P_BLK variable.
//
// This is a scan for a known byte pattern, NOT an AML interpreter. It can be
// fooled by those bytes appearing inside something else, so the result is
// sanity-checked as an I/O port before it is believed.
long acpi_find_pblk() {
    long len;
    long p;
    long end;
    if (!acpi_dsdt) return 0;
    len = mem32(acpi_dsdt + 4);
    if (len < 36 || len > 1048576) return 0;
    p = acpi_dsdt + 36;
    end = acpi_dsdt + len;
    while (p + 16 < end) {
        if (mem8(p) == 0x5B && mem8(p + 1) == 0x83) {
            long lead;
            long extra;
            long q;
            long pblk;
            long plen;
            lead = mem8(p + 2);
            extra = (lead >> 6) & 3;               // 0..3 further length bytes
            q = p + 3 + extra + 4 + 1;             // skip name (4) and ProcID (1)
            pblk = mem32(q);
            plen = mem8(q + 4);
            // A P_BLK is an I/O port, so it lives below 64 K, and the block is
            // six bytes long. Anything else means the pattern was a
            // coincidence.
            if (pblk > 0 && pblk < 0x10000 && plen >= 5) return pblk;
        }
        p = p + 1;
    }
    return 0;
}

// ---------- bring-up ----------
long acpi_init() {
    acpi_rsdp = acpi_find_rsdp();
    if (!acpi_rsdp) return 0;

    acpi_rev = mem8(acpi_rsdp + 15);
    acpi_root_is_xsdt = 0;
    acpi_root = 0;

    // Prefer the XSDT when the firmware claims ACPI 2.0 or later, but only if
    // it validates -- some firmware advertises a revision it does not honour,
    // and a bad XSDT pointer is a walk into unmapped memory.
    if (acpi_rev >= 2) {
        long xsdt;
        xsdt = mem64(acpi_rsdp + 24);
        if (xsdt && sig_matches(xsdt, "XSDT") && checksum_ok(xsdt, mem32(xsdt + 4))) {
            acpi_root = xsdt;
            acpi_root_is_xsdt = 1;
        }
    }
    if (!acpi_root) {
        long rsdt;
        rsdt = mem32(acpi_rsdp + 16);
        if (rsdt && sig_matches(rsdt, "RSDT") && checksum_ok(rsdt, mem32(rsdt + 4)))
            acpi_root = rsdt;
    }
    if (!acpi_root) return 0;

    {
        long len;
        len = mem32(acpi_root + 4);
        if (acpi_root_is_xsdt) acpi_ntables = (len - 36) / 8;
        else                   acpi_ntables = (len - 36) / 4;
        if (acpi_ntables < 0 || acpi_ntables > 256) acpi_ntables = 0;
    }

    acpi_fadt = acpi_find_table("FACP");           // the FADT's signature
    acpi_madt = acpi_find_table("APIC");           // the MADT's signature
    acpi_read_fadt();
    acpi_read_madt();
    acpi_pblk = acpi_find_pblk();
    return 1;
}

// ---------- the power-management timer ----------
// A free-running counter at 3.579545 MHz that keeps going regardless of what
// the CPU is doing -- which is exactly what makes it the right clock for
// measuring how long a halted core was halted.
#define PM_TMR_HZ 3579545

long pm_timer_read() {
    if (!acpi_pm_tmr) return 0;
    return inl(acpi_pm_tmr);
}

// The counter is 24 or 32 bits wide and wraps. Subtracting raw readings gives
// a large negative number across a wrap; masking to the real width gives the
// right answer, which is why the width is read from the FADT and not assumed.
long pm_timer_delta(long start, long end) {
    long mask;
    mask = acpi_pm_tmr_32 ? 0xFFFFFFFF : 0xFFFFFF;
    return (end - start) & mask;
}

// ---------- switching ACPI mode on ----------
// The firmware may start in legacy mode. Writing the enable value to SMI_CMD
// asks it to hand control over; SCI_EN in PM1a_CNT says when it has. If
// SMI_CMD is zero the system is already in ACPI mode -- which is the usual
// case under QEMU and on anything UEFI.
long acpi_enable() {
    long tries;
    if (!acpi_pm1a_cnt) return 0;
    if (inw(acpi_pm1a_cnt) & 1) return 1;          // SCI_EN already set
    if (!acpi_smi_cmd) return 0;
    outb(acpi_smi_cmd, acpi_enable_val);
    tries = 0;
    while (tries < 300) {
        if (inw(acpi_pm1a_cnt) & 1) return 1;
        sleep_ms(1);
        tries = tries + 1;
    }
    return 0;
}

// ---------- idle ----------
// Which state we can actually enter, decided from what the firmware says
// rather than from what would be impressive:
//
//   C1  always: `hlt` stops the core until an interrupt. No table needed.
//   C2  a READ from P_BLK+4, allowed only if the FADT's P_LVL2_LAT is 100 us
//       or less. A latency above that is the specification's way of saying
//       the state does not exist.
//   C3  P_BLK+5, with P_LVL3_LAT of 1000 us or less. Also needs bus-master
//       traffic to be quiesced, which needs the PM2 control register and a
//       lot of care -- so it is detected and reported, not entered.
long acpi_cstate;       // the deepest state we will actually use: 1 or 2

void acpi_pick_cstate() {
    acpi_cstate = 1;
    if (acpi_pblk && acpi_c2_lat > 0 && acpi_c2_lat <= 100) acpi_cstate = 2;
}

long g_c1_entries;
long g_c2_entries;

// Enter the deepest state we decided on. Both paths return when an interrupt
// arrives; the C2 read is a bus cycle the chipset answers only after the core
// has stopped.
void acpi_idle() {
    if (acpi_cstate >= 2) {
        g_c2_entries = g_c2_entries + 1;
        sti_();
        inb(acpi_pblk + 4);
        return;
    }
    g_c1_entries = g_c1_entries + 1;
    cpu_idle();
}

char *acpi_cstate_name() {
    if (acpi_cstate >= 2) return "C2 (P_LVL2 read)";
    return "C1 (hlt)";
}

#endif
