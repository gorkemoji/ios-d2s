/* SPDX-License-Identifier: GPL-2.0-only
 * Galaxy Note 10+ (d2s, Exynos 9825) Mali-G76 power-on probe (MMU off).
 *
 * Step 1 of the GPU work: power the GPU, read its ID, soft-reset it and
 * power the L2 and shader cores once. Nothing is left running: cores are
 * powered off again, the power domain stays on.
 *
 * Sources (Samsung exynos9820 kernel, LineageOS lineage-23.2):
 * - power domain "pd-embedded_g3d" (stock DT: reg 0x15862100, need_smc
 *   0x18410204): cal-if/exynos9820/flexpmu_cal_local_exynos9820.h
 *   embedded_g3d_on[], run by pmucal_rae.c (WAIT = poll up to 2 ms,
 *   SET_BIT_ATOMIC = write to offset | 0xc000)
 * - after power-on pmucal_local_enable() asks EL3 to restore the block's
 *   TZPC: exynos_pd_tz_restore() = SMC 0x82000410 (SMC_CMD_PREAPRE_PD_ONOFF),
 *   EXYNOS_WAKEUP_PD_DOWN (1), need_smc address, RUNTIME_PM_TZPC_GROUP (2)
 * - Mali registers at 0x18500000 (stock DT mali@18500000); register layout
 *   from the Mali kbase / Linux panfrost headers.
 *
 * GPU registers are only touched when the domain reports on: reading a
 * powered-off block would hang the bus.
 */
#include <stdint.h>
#include "gpu_probe.h"

#define PMU			0x15860000UL
#define EG3D_CONF		(PMU + 0x2100)
#define EG3D_STATUS		(PMU + 0x2104)
#define EG3D_OUT		(PMU + 0x2120)
#define G3D_CONF		(PMU + 0x2380)
#define G3D_STATUS		(PMU + 0x2384)
#define ATOMIC_SET		0xc000

#define CMU_TOP_PLL		0x1A240000UL	/* PLL_G3D */
#define PLL_G3D_CON0		(CMU_TOP_PLL + 0x140)
#define PLL_G3D_CON2		(CMU_TOP_PLL + 0x148)

#define CMU_G3D			0x18400000UL
#define G3D_USER_MUX		(CMU_G3D + 0x100)

#define SMC_PD_ONOFF		0x82000410UL
#define PD_WAKEUP		1
#define G3D_TZPC		0x18410204UL
#define TZPC_GROUP		2

#define MALI			0x18500000UL
#define GPU_ID			0x000
#define L2_FEATURES		0x004
#define CORE_FEATURES		0x008
#define TILER_FEATURES		0x00c
#define MEM_FEATURES		0x010
#define MMU_FEATURES		0x014
#define AS_PRESENT		0x018
#define JS_PRESENT		0x01c
#define GPU_IRQ_RAWSTAT		0x020
#define GPU_IRQ_CLEAR		0x024
#define GPU_IRQ_MASK		0x028
#define GPU_COMMAND		0x030
#define GPU_STATUS		0x034
#define SHADER_PRESENT		0x100
#define TILER_PRESENT		0x110
#define L2_PRESENT		0x120
#define SHADER_READY		0x140
#define TILER_READY		0x150
#define L2_READY		0x160
#define SHADER_PWRON		0x180
#define TILER_PWRON		0x190
#define L2_PWRON		0x1a0
#define SHADER_PWROFF		0x1c0
#define TILER_PWROFF		0x1d0
#define L2_PWROFF		0x1e0
#define CMD_SOFT_RESET		0x01
#define IRQ_RESET_COMPLETED	(1u << 8)

/* Job manager and MMU (panfrost_regs.h) */
#define JOB_INT_RAWSTAT		0x1000
#define JOB_INT_CLEAR		0x1004
#define JOB_INT_MASK		0x1008
#define JS0			0x1800
#define JS_HEAD_LO		0x00
#define JS_HEAD_HI		0x04
#define JS_STATUS		0x24
#define JS_HEAD_NEXT_LO		0x40
#define JS_HEAD_NEXT_HI		0x44
#define JS_AFFINITY_NEXT_LO	0x50
#define JS_AFFINITY_NEXT_HI	0x54
#define JS_CONFIG_NEXT		0x58
#define JS_COMMAND_NEXT		0x60
#define JS_CFG_START_FLUSH_CI	(3u << 8)
#define JS_CFG_END_FLUSH_CI	(3u << 12)
#define JS_CFG_THREAD_PRI(n)	((n) << 16)
#define JS_CMD_START		1
#define MMU_INT_RAWSTAT		0x2000
#define MMU_INT_CLEAR		0x2004
#define MMU_INT_MASK		0x2008
#define AS0			0x2400
#define AS_TRANSTAB_LO		0x00
#define AS_TRANSTAB_HI		0x04
#define AS_MEMATTR_LO		0x08
#define AS_MEMATTR_HI		0x0c
#define AS_COMMAND		0x18
#define AS_FAULTSTATUS		0x1c
#define AS_FAULTADDR_LO		0x20
#define AS_FAULTADDR_HI		0x24
#define AS_STATUS		0x28
#define AS_CMD_UPDATE		1
#define AS_ACTIVE		1

/*
 * Mali LPAE page tables (io-pgtable-arm.c, ARM_MALI_LPAE): always 4 levels
 * of 4 KiB tables; table entries type 3; leaves type 1 (also at level 3);
 * permissions stage-2 style (bit 6 read, bit 7 write); attribute index 0
 * (MEMATTR 0x88, the "implementation defined" policy panfrost uses);
 * outer shareable. TRANSTAB = table | READ_INNER | ADRMODE_TABLE.
 */
#define PTE_TABLE		3ull
#define PTE_BLOCK		1ull
#define PTE_RW			((1ull << 6) | (1ull << 7))
#define PTE_SH_OS		(2ull << 8)
#define TRANSTAB_FLAGS		((1u << 2) | 3u)
#define MEMATTR_LO		(0x88u | 0x8du << 8 | 0x88u << 16)

/* L0, L1, L2 tables and one data page, identity mapped (MMU off: VA = PA) */
static uint64_t gpu_mem[4][512] __attribute__((aligned(4096)));

#define TIMER_HZ		26000000UL

static void (*kv)(const char *, uint64_t);
static void (*out)(const char *);

static inline uint32_t rd(uintptr_t a) { return *(volatile uint32_t *)a; }
static inline void wr(uintptr_t a, uint32_t v) { *(volatile uint32_t *)a = v; }
static inline uint32_t gr(unsigned o) { return rd(MALI + o); }
static inline void gw(unsigned o, uint32_t v) { wr(MALI + o, v); }

static uint64_t ticks(void)
{
	uint64_t v;
	__asm__ volatile("isb; mrs %0, cntpct_el0" : "=r"(v));
	return v;
}

static void delay_us(unsigned us)
{
	uint64_t end = ticks() + TIMER_HZ / 1000000 * us;
	while (ticks() < end)
		;
}

static void rmw(uintptr_t a, uint32_t mask, uint32_t val)
{
	wr(a, (rd(a) & ~mask) | (val & mask));
}

/* pmucal WAIT: poll (reg & mask) == val for up to 2 ms */
static int wait_bits(uintptr_t a, uint32_t mask, uint32_t val)
{
	for (unsigned us = 0; us < 2000; us++) {
		if ((rd(a) & mask) == val)
			return 0;
		delay_us(1);
	}
	kv(" timeout reg", a);
	kv("val", rd(a));
	out("\n");
	return -1;
}

static int64_t smc3(uint64_t fn, uint64_t a1, uint64_t a2, uint64_t a3)
{
	register uint64_t x0 __asm__("x0") = fn;
	register uint64_t x1 __asm__("x1") = a1;
	register uint64_t x2 __asm__("x2") = a2;
	register uint64_t x3 __asm__("x3") = a3;
	__asm__ volatile("smc #0" : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3) : :
			 "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12",
			 "x13", "x14", "x15", "x16", "x17", "memory");
	return (int64_t)x0;
}

static void show_pmu(void)
{
	kv("eg3d conf", rd(EG3D_CONF));
	kv("status", rd(EG3D_STATUS));
	kv("out", rd(EG3D_OUT));
	kv("g3d conf", rd(G3D_CONF));
	kv("status", rd(G3D_STATUS));
	kv("pll con0", rd(PLL_G3D_CON0));
	kv("con2", rd(PLL_G3D_CON2));
	out("\n");
}

/* embedded_g3d_on[] */
static int power_on(void)
{
	rmw(PLL_G3D_CON2, 0xffffffffu, 0x30000003u);
	rmw(PLL_G3D_CON0, 1u << 4, 1u << 4);
	rmw(G3D_CONF, 1, 0);
	if (wait_bits(G3D_STATUS, 1, 0))
		return -1;
	wr(EG3D_OUT | ATOMIC_SET, 0x2);
	delay_us(0x118);
	rmw(G3D_CONF, 1, 1);
	if (wait_bits(G3D_STATUS, 1, 1))
		return -2;
	delay_us(0x118);
	rmw(EG3D_CONF, 1, 1);
	if (wait_bits(EG3D_STATUS, 1, 1))
		return -3;
	return 0;
}

/* Power a core group on (or off) and wait for its READY bits. */
static uint32_t cores(unsigned pwr, unsigned ready, uint32_t mask, int on)
{
	gw(pwr, mask);
	gw(pwr + 4, 0);
	uint64_t end = ticks() + TIMER_HZ / 100;	/* 10 ms */
	while (ticks() < end) {
		uint32_t r = gr(ready) & mask;
		if (on ? r == mask : r == 0)
			break;
	}
	return gr(ready);
}

static int as_wait(void)
{
	uint64_t end = ticks() + TIMER_HZ / 10;
	while (gr(AS0 + AS_STATUS) & AS_ACTIVE)
		if (ticks() > end)
			return -1;
	return 0;
}

/*
 * GPU step 2: one WRITE_VALUE job (job type 2, genxml v7) through address
 * space 0 on job slot 0. The job manager stores a 32-bit immediate at a
 * GPU virtual address; success means the MMU walk, the job descriptor read
 * and the memory write all work.
 */
static void first_job(uint64_t shaders)
{
	uint64_t *l0 = gpu_mem[0], *l1 = gpu_mem[1], *l2 = gpu_mem[2];
	volatile uint32_t *data = (volatile uint32_t *)gpu_mem[3];
	uint64_t va = (uintptr_t)gpu_mem[3];	/* identity map: VA = PA */
	uint64_t job_va = va, result_va = va + 0x100;

	out("GPU JOB: write-value test\n");
	for (unsigned t = 0; t < 4; t++)
		for (unsigned i = 0; i < 512; i++)
			gpu_mem[t][i] = 0;
	l0[(va >> 39) & 0x1ff] = (uintptr_t)l1 | PTE_TABLE;
	l1[(va >> 30) & 0x1ff] = (uintptr_t)l2 | PTE_TABLE;
	l2[(va >> 21) & 0x1ff] = (va & ~0x1fffffull) | PTE_BLOCK | PTE_RW | PTE_SH_OS;

	/* Write Value Job: header (32 bytes) + payload */
	data[4] = 1u | 2u << 1 | 1u << 16;	/* is64b, type 2, index 1 */
	data[8] = (uint32_t)result_va;
	data[9] = (uint32_t)(result_va >> 32);
	data[10] = 6;				/* Immediate 32 */
	data[12] = 0xD25600D5u;
	data[13] = 0;
	data[0x100 / 4] = 0x11111111u;		/* result, overwritten by the GPU */
	__asm__ volatile("dsb sy" ::: "memory");
	kv("buf", va);
	kv("l0", (uintptr_t)l0);
	out("\n");

	/* address space 0 (panfrost_mmu_enable) */
	gw(MMU_INT_MASK, 0);
	gw(MMU_INT_CLEAR, 0xffffffffu);
	if (as_wait()) {
		out("GPU JOB: AS0 busy before setup\n");
		return;
	}
	uint64_t tt = (uintptr_t)l0 | TRANSTAB_FLAGS;
	gw(AS0 + AS_TRANSTAB_LO, (uint32_t)tt);
	gw(AS0 + AS_TRANSTAB_HI, (uint32_t)(tt >> 32));
	gw(AS0 + AS_MEMATTR_LO, MEMATTR_LO);
	gw(AS0 + AS_MEMATTR_HI, 0);
	gw(AS0 + AS_COMMAND, AS_CMD_UPDATE);
	kv("as update", (uint64_t)(uint32_t)as_wait());
	kv("as status", gr(AS0 + AS_STATUS));
	out("\n");

	/* submit on slot 0 (panfrost_job_hw_submit) */
	gw(JOB_INT_MASK, 0);
	gw(JOB_INT_CLEAR, 0xffffffffu);
	gw(JS0 + JS_HEAD_NEXT_LO, (uint32_t)job_va);
	gw(JS0 + JS_HEAD_NEXT_HI, (uint32_t)(job_va >> 32));
	gw(JS0 + JS_AFFINITY_NEXT_LO, (uint32_t)shaders);
	gw(JS0 + JS_AFFINITY_NEXT_HI, (uint32_t)(shaders >> 32));
	gw(JS0 + JS_CONFIG_NEXT, 0 /* AS 0 */ | JS_CFG_THREAD_PRI(8) |
	   JS_CFG_START_FLUSH_CI | JS_CFG_END_FLUSH_CI);
	uint64_t t0 = ticks();
	gw(JS0 + JS_COMMAND_NEXT, JS_CMD_START);

	uint64_t end = t0 + TIMER_HZ / 10;
	uint32_t raw;
	while (!((raw = gr(JOB_INT_RAWSTAT)) & 0x10001u) && ticks() < end)
		;
	kv("job rawstat", raw);
	kv("us", (ticks() - t0) / (TIMER_HZ / 1000000));
	kv("js status", gr(JS0 + JS_STATUS));
	kv("js head", (uint64_t)gr(JS0 + JS_HEAD_HI) << 32 | gr(JS0 + JS_HEAD_LO));
	out("\n");
	kv("mmu rawstat", gr(MMU_INT_RAWSTAT));
	kv("fault", gr(AS0 + AS_FAULTSTATUS));
	kv("addr", (uint64_t)gr(AS0 + AS_FAULTADDR_HI) << 32 | gr(AS0 + AS_FAULTADDR_LO));
	out("\n");
	kv("hdr exception", data[0]);
	kv("result", data[0x100 / 4]);
	out("\n");
	gw(JOB_INT_CLEAR, 0xffffffffu);
	gw(MMU_INT_CLEAR, 0xffffffffu);

	if (data[0x100 / 4] == 0xD25600D5u)
		out("GPU JOB: OK, the GPU wrote 0xd25600d5 through its MMU\n");
	else if (raw & 0x10000u)
		out("GPU JOB: job slot 0 reported an error\n");
	else if (!(raw & 1u))
		out("GPU JOB: no completion within 100 ms\n");
	else
		out("GPU JOB: completed but the value is wrong\n");

	/* Keep address space 0 active so MMU translation remains valid for runtime jobs */
	out("GPU: keeping AS0 MMU translation active\n");
}

void gpu_probe(void (*kv_fn)(const char *, uint64_t), void (*out_fn)(const char *))
{
	kv = kv_fn;
	out = out_fn;
	out("\n--- D2S GPU PROBE (Mali-G76 @ 0x18500000) ---\n");
	show_pmu();

	if (!(rd(EG3D_STATUS) & 1)) {
		out("GPU: domain off, powering on\n");
		int rc = power_on();
		kv("power_on rc", (uint64_t)(uint32_t)rc);
		out("\n");
		show_pmu();
		if (rc) {
			out("GPU: power-on failed, GPU registers not touched\n--- GPU PROBE DONE ---\n");
			return;
		}
		int64_t s = smc3(SMC_PD_ONOFF, PD_WAKEUP, G3D_TZPC, TZPC_GROUP);
		kv("tzpc restore smc", (uint64_t)s);
		out("\n");
	} else {
		out("GPU: domain already on\n");
	}
	if (!(rd(EG3D_STATUS) & 1)) {
		out("GPU: domain not on, GPU registers not touched\n--- GPU PROBE DONE ---\n");
		return;
	}
	kv("g3d user mux", rd(G3D_USER_MUX));
	out("\n");

	uint32_t id = gr(GPU_ID);
	kv("GPU_ID", id);
	kv("product", id >> 16);
	kv("major", (id >> 12) & 0xf);
	kv("minor", (id >> 4) & 0xff);
	kv("status", id & 0xf);
	out("\n");
	if ((id >> 16) == 0x7211)
		out("GPU: Mali-G76 identified\n");
	else
		out("GPU: unexpected GPU_ID (G76 is product 0x7211)\n");
	kv("l2 feat", gr(L2_FEATURES));
	kv("core feat", gr(CORE_FEATURES));
	kv("tiler feat", gr(TILER_FEATURES));
	kv("mem feat", gr(MEM_FEATURES));
	kv("mmu feat", gr(MMU_FEATURES));
	out("\n");
	kv("as present", gr(AS_PRESENT));
	kv("js present", gr(JS_PRESENT));
	kv("shader present", (uint64_t)gr(SHADER_PRESENT + 4) << 32 | gr(SHADER_PRESENT));
	kv("tiler", gr(TILER_PRESENT));
	kv("l2", gr(L2_PRESENT));
	out("\n");

	/* soft reset: wait for RESET_COMPLETED in the raw IRQ status */
	gw(GPU_IRQ_MASK, 0);
	gw(GPU_IRQ_CLEAR, 0xffffffffu);
	gw(GPU_COMMAND, CMD_SOFT_RESET);
	uint64_t t0 = ticks(), end = t0 + TIMER_HZ / 10;
	while (!(gr(GPU_IRQ_RAWSTAT) & IRQ_RESET_COMPLETED) && ticks() < end)
		;
	uint32_t raw = gr(GPU_IRQ_RAWSTAT);
	kv("reset rawstat", raw);
	kv("us", (ticks() - t0) / (TIMER_HZ / 1000000));
	kv("gpu status", gr(GPU_STATUS));
	out("\n");
	if (!(raw & IRQ_RESET_COMPLETED)) {
		out("GPU: soft reset did not complete\n--- GPU PROBE DONE ---\n");
		return;
	}
	gw(GPU_IRQ_CLEAR, 0xffffffffu);
	out("GPU: soft reset OK\n");

	/* power L2, tiler and shader cores once, then off again */
	uint32_t l2 = gr(L2_PRESENT), sh = gr(SHADER_PRESENT), ti = gr(TILER_PRESENT);
	kv("l2 ready", cores(L2_PWRON, L2_READY, l2, 1));
	kv("tiler ready", cores(TILER_PWRON, TILER_READY, ti, 1));
	kv("shader ready", cores(SHADER_PWRON, SHADER_READY, sh, 1));
	out("\n");
	int all = (gr(L2_READY) & l2) == l2 && (gr(SHADER_READY) & sh) == sh &&
		  (gr(TILER_READY) & ti) == ti;
	out(all ? "GPU: L2, tiler and all shader cores powered\n"
		: "GPU: some cores did not report ready\n");
	if (all)
		first_job((uint64_t)gr(SHADER_PRESENT + 4) << 32 | sh);
	/* Keep GPU L2, Tiler, and Shader cores powered ON for XNU kernel runtime! */
	out("GPU: keeping L2, tiler and all shader cores powered on for XNU\n");
	out("\n--- GPU PROBE DONE (Mali-G76 Active) ---\n");
}
