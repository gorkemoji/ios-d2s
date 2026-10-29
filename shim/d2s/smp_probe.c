/* SPDX-License-Identifier: GPL-2.0-only
 * Galaxy Note 10+ (d2s, Exynos 9825) secondary-CPU probe, run by the loader
 * before XNU (MMU off).
 *
 * Samsung's Exynos 9820/9825 kernel starts secondaries with plain PSCI
 * CPU_ON (arch/arm64/kernel/psci.c); the EL3 firmware powers the core.
 * Topology from the stock DTB: A55 MPIDR 0x0-0x3, A75 0x4-0x5, M4 0x100-0x101.
 *
 * For every secondary this probe issues CPU_ON with a low entry (smp_entry
 * in start.S), waits for the core to mark its slot, and logs the PMU
 * CPUn_CONFIGURATION / STATUS registers before and after. The PMU is only
 * read, never written. A started core turns itself off again (CPU_OFF).
 */
#include <stdint.h>
#include "smp_probe.h"

#define PMU		0x15860000UL	/* pmu_system_controller@15860000 */
#define PMU_CPU_CFG	0x1000UL	/* exynos-pmu.c PMU_CPU_CONFIG_BASE */
#define PMU_CPU_STAT	0x1004UL	/* PMU_CPU_STATUS_BASE */

#define PSCI_VERSION	0x84000000UL
#define PSCI_CPU_ON	0xc4000003UL
#define PSCI_AFF_INFO	0xc4000004UL
#define TIMER_HZ	26000000UL

extern char smp_entry[];
extern volatile uint64_t smp_flags[8];	/* start.S: set by a started core */

static const struct {
	uint64_t mpidr;
	uint32_t pmu_off;	/* exynos-pmu.c pmu_cpu_offset(logical) */
	char name[12];	/* inline: no absolute relocations */
} cores[] = {
	{ 0x001, 0x080, "A55 cpu1" },
	{ 0x002, 0x100, "A55 cpu2" },
	{ 0x003, 0x180, "A55 cpu3" },
	{ 0x004, 0x300, "A75 cpu4" },
	{ 0x005, 0x380, "A75 cpu5" },
	{ 0x100, 0x500, "M4  cpu6" },
	{ 0x101, 0x580, "M4  cpu7" },
};

static inline uint32_t rd(uintptr_t a) { return *(volatile uint32_t *)a; }

static uint64_t ticks(void)
{
	uint64_t v;
	__asm__ volatile("isb; mrs %0, cntpct_el0" : "=r"(v));
	return v;
}

static int64_t smc3(uint64_t fn, uint64_t a1, uint64_t a2, uint64_t a3)
{
	register uint64_t x0 __asm__("x0") = fn;
	register uint64_t x1 __asm__("x1") = a1;
	register uint64_t x2 __asm__("x2") = a2;
	register uint64_t x3 __asm__("x3") = a3;
	__asm__ volatile("smc #0" : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3) : :
			 "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13",
			 "x14", "x15", "x16", "x17", "memory");
	return (int64_t)x0;
}

/* Must match smp_entry in start.S. */
static unsigned slot_of(uint64_t m)
{
	return ((m & 7) + (((m >> 8) & 1) ? 6 : 0)) & 7;
}

static int wait_flag(unsigned slot, unsigned ms)
{
	uint64_t end = ticks() + TIMER_HZ / 1000 * ms;
	while (ticks() < end)
		if (smp_flags[slot])
			return 1;
	return 0;
}

static void pmu_regs(void (*kv)(const char *, uint64_t), uint32_t off)
{
	kv("cfg", rd(PMU + PMU_CPU_CFG + off));
	kv("stat", rd(PMU + PMU_CPU_STAT + off));
}

static inline void wr(uintptr_t a, uint32_t v) { *(volatile uint32_t *)a = v; }

extern const char _head[], _end[];

/*
 * Clean + invalidate the loader image to PoC by address. The boot CPU runs
 * with MMU and caches off, but the A55/A75 secondaries share the DynamIQ
 * L3 with it and may fetch with caches on: stale lines there would hide
 * our code and the flags from them (v1: powered, never ran our entry).
 */
static void flush_image(void)
{
	uint64_t ctr;
	__asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
	uintptr_t line = 4UL << ((ctr >> 16) & 0xf);
	for (uintptr_t p = (uintptr_t)_head & ~(line - 1); p < (uintptr_t)_end; p += line)
		__asm__ volatile("dc civac, %0" : : "r"(p) : "memory");
	__asm__ volatile("dsb sy; ic iallu; dsb sy; isb" ::: "memory");
}

static void delay_ms(unsigned ms)
{
	uint64_t end = ticks() + TIMER_HZ / 1000 * ms;
	while (ticks() < end)
		;
}

static int wait_bit(uintptr_t a, uint32_t mask, uint32_t want, unsigned ms)
{
	uint64_t end = ticks() + TIMER_HZ / 1000 * ms;
	while (ticks() < end)
		if ((rd(a) & mask) == want)
			return 1;
	return 0;
}

static int try_core(unsigned i, int pmu_cycle,
		    void (*kv)(const char *, uint64_t), void (*out)(const char *))
{
	uint64_t m = cores[i].mpidr;
	uintptr_t cfg = PMU + PMU_CPU_CFG + cores[i].pmu_off;
	uintptr_t stat = PMU + PMU_CPU_STAT + cores[i].pmu_off;
	unsigned slot = slot_of(m);
	int64_t rc;
	int up;

	out(cores[i].name);
	out(": ");
	kv("mpidr", m);
	pmu_regs(kv, cores[i].pmu_off);
	out("\n  ");
	smp_flags[slot] = 0;
	flush_image();
	if (pmu_cycle) {
		/* S8 order (patch 0124): power off, CPU_ON, power 0 -> 1, CPU_ON. */
		wr(cfg, rd(cfg) & ~1u);
		kv("off", wait_bit(stat, 1, 0, 100));
		rc = smc3(PSCI_CPU_ON, m, (uintptr_t)smp_entry, slot);
		kv("on1 rc", (uint64_t)rc);
		wr(cfg, rd(cfg) | 1u);
		kv("pwr", wait_bit(stat, 1, 1, 100));
		pmu_regs(kv, cores[i].pmu_off);
		up = wait_flag(slot, 300);
		kv("up1", up);
		if (!up) {
			flush_image();
			rc = smc3(PSCI_CPU_ON, m, (uintptr_t)smp_entry, slot);
			up = wait_flag(slot, 1000);
			kv("on2 rc", (uint64_t)rc);
			kv("STARTED", up);
		}
	} else {
		rc = smc3(PSCI_CPU_ON, m, (uintptr_t)smp_entry, slot);
		up = wait_flag(slot, 1000);
		kv("CPU_ON rc", (uint64_t)rc);
		kv("STARTED", up);
		if (!up) {
			flush_image();
			rc = smc3(PSCI_CPU_ON, m, (uintptr_t)smp_entry, slot);
			up = wait_flag(slot, 500);
			kv("again rc", (uint64_t)rc);
			kv("STARTED2", up);
		}
	}
	pmu_regs(kv, cores[i].pmu_off);
	out("\n");
	delay_ms(50);	/* let a started core finish CPU_OFF */
	return up;
}

void smp_probe(void (*kv)(const char *, uint64_t), void (*out)(const char *))
{
	uint64_t self = 0;
	__asm__ volatile("mrs %0, mpidr_el1" : "=r"(self));
	out("D2S SMP probe v2 (cache flush, retries, cpu1 PMU cycle)\n");
	kv("boot mpidr", self & 0xffffff);
	kv("entry", (uintptr_t)smp_entry);
	kv("image", (uintptr_t)_head);
	kv("end", (uintptr_t)_end);
	out("\n");
	unsigned up_count = 0;
	/* cpu2..cpu5 and the M4s: PSCI only, with cache flush and retry */
	for (unsigned i = 1; i < sizeof(cores) / sizeof(cores[0]); i++)
		up_count += try_core(i, 0, kv, out);
	/* cpu1 last: S8-style PMU power cycle */
	up_count += try_core(0, 1, kv, out);
	kv("SMP PROBE CORES STARTED", up_count);
	out("of 7\nD2S SMP probe done\n");
}
