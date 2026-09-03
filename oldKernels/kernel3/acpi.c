// acpi.c — read the firmware's ACPI tables and idle the CPU with them.
//
// Headless, so `make acpitest` can check every claim. The interesting output
// is the idle measurement at the end: the ACPI power-management timer runs
// regardless of what the core is doing, so it can measure how long a halted
// core was halted, which a CPU-driven clock cannot.

#include "nano-kernel.h"
#include "nano-int.h"
#include "nano-acpi.h"

void print_sig(long table) {
    long i;
    i = 0;
    while (i < 4) { putc(mem8(table + i)); i = i + 1; }
}

int main() {
    long i;

    serial_init();
    vga_clear();
    kbd_init();
    interrupts_init(100);

    puts("nano-os ACPI bring-up\n");

    if (!acpi_init()) {
        puts("ACPI: no RSDP found\n");
        cpu_halt_forever();
    }

    printf("RSDP  at 0x%x, revision %d\n", acpi_rsdp, acpi_rev);
    printf("root  at 0x%x (%s), %d tables\n",
           acpi_root, acpi_root_is_xsdt ? "XSDT" : "RSDT", acpi_ntables);

    puts("tables:");
    i = 0;
    while (i < acpi_ntables) {
        long t;
        t = acpi_table_at(i);
        if (t) { putc(' '); print_sig(t); }
        i = i + 1;
    }
    putc('\n');

    printf("FADT  at 0x%x\n", acpi_fadt);
    printf("DSDT  at 0x%x\n", acpi_dsdt);
    printf("MADT  at 0x%x, %d usable CPUs\n", acpi_madt, acpi_cpus);
    printf("PM1a_CNT port 0x%x, SMI_CMD 0x%x\n", acpi_pm1a_cnt, acpi_smi_cmd);
    printf("PM timer port 0x%x, %d-bit\n", acpi_pm_tmr, acpi_pm_tmr_32 ? 32 : 24);
    printf("P_LVL2 latency %d us, P_LVL3 latency %d us\n", acpi_c2_lat, acpi_c3_lat);
    printf("P_BLK 0x%x (from the DSDT Processor object)\n", acpi_pblk);

    if (acpi_enable()) puts("ACPI mode is enabled (SCI_EN set)\n");
    else puts("ACPI mode not enabled; PM1a_CNT unavailable or no SMI_CMD\n");

    acpi_pick_cstate();
    printf("idle state chosen: %s\n", acpi_cstate_name());
    if (acpi_c2_lat > 100)  puts("  C2 not offered by this firmware (latency > 100us)\n");
    if (acpi_c3_lat > 1000) puts("  C3 not offered by this firmware (latency > 1000us)\n");

    // --- the PM timer has to actually run ---
    if (acpi_pm_tmr) {
        long a;
        long b;
        long d;
        a = pm_timer_read();
        sleep_ms(200);
        b = pm_timer_read();
        d = pm_timer_delta(a, b);
        // 200 ms at 3.579545 MHz is about 715909 counts. Allow a wide band --
        // this is checking that it runs at roughly the right rate, not
        // calibrating anything.
        printf("PM timer moved %d counts in ~200ms\n", d);
        if (d > 500000 && d < 950000) puts("pm timer ok\n");
        else puts("PM TIMER OUT OF RANGE\n");
    } else {
        puts("no PM timer; skipping the rate check\n");
    }

    // --- and the core has to actually stop ---
    // Idle for a fixed number of timer ticks and count the wake-ups. A core
    // that is spinning racks up millions; a core that is halting wakes once
    // per interrupt.
    {
        long t0;
        long n;
        g_c1_entries = 0;
        g_c2_entries = 0;
        t0 = g_ticks;
        n = 0;
        while (g_ticks < t0 + 100) { acpi_idle(); n = n + 1; }
        printf("over 100 ticks (1s): %d idle entries\n", n);
        printf("  C1 %d, C2 %d\n", g_c1_entries, g_c2_entries);
        if (n < 400) puts("idle ok: the core stopped between interrupts\n");
        else puts("NOT IDLING: too many wake-ups for a halted core\n");
    }

    puts("acpi bring-up complete\n");
    cpu_halt_forever();
    return 0;
}
