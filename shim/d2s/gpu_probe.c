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
	kv("shader off", cores(SHADER_PWROFF, SHADER_READY, sh, 0));
	kv("tiler off", cores(TILER_PWROFF, TILER_READY, ti, 0));
	kv("l2 off", cores(L2_PWROFF, L2_READY, l2, 0));
	out("\n--- GPU PROBE DONE ---\n");
}
