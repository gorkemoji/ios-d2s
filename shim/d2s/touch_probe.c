/* SPDX-License-Identifier: GPL-2.0-only
 * Galaxy Note 10+ (SM-N975F, d2s, Exynos 9825) touchscreen probe (MMU off).
 *
 * Controller: Samsung S6SY771 ("sec,sec_ts", reg <0x48>) on hsi2c_10 (0x104b0000)
 * Pins: gpp1-4 (SCL), gpp1-5 (SDA) on pinctrl_5@10430000 (function 2)
 * Interrupt: gpa1-0 on pinctrl_alive@15850000
 *
 * hsi2c_10 is the USI port of USI03 in PERIC0 (DT clocks 0x1fd/0x1ed =
 * DOUT_CLK_PERIC0_USI03_USI / GATE_USI03_USI). S-Boot leaves its clocks
 * gated: the controller ignored writes and read 0 (boot log). Register
 * addresses are from Samsung's exynos9820 cmucal-sfr.c, the USI v2 mode from
 * usi_v2.c (usi_3 = usi@1042101c, "i2c" = 1 << 2), USI_CON from i2c-exynos5.c.
 * As on the S8, the USI clock is taken from OSCCLK (26 MHz) so the HSI2C
 * timing below and in the kernel (S8_I2C_CLKIN) holds.
 */
#include <stdint.h>
#define NULL ((void *)0)
#include "touch_probe.h"

#define PINCTRL_PERIC0		0x10430000UL
#define GPP1_CON		(PINCTRL_PERIC0 + 0x020)
#define GPP1_DAT		(PINCTRL_PERIC0 + 0x024)
#define GPP1_PUD		(PINCTRL_PERIC0 + 0x028)

#define PINCTRL_ALIVE		0x15850000UL
#define GPA1_CON		(PINCTRL_ALIVE + 0x020)
#define GPA1_DAT		(PINCTRL_ALIVE + 0x024)
#define GPA1_PUD		(PINCTRL_ALIVE + 0x028)

/* Exynos 9820 banks: CON 4 bits, DAT 1 bit, PUD 4 bits per pin
 * (pinctrl-exynos-arm64.c exynos9810_bank_type_off/alive);
 * PUD 0 = none, 1 = pull-down, 3 = pull-up (dt-bindings samsung.h). */
#define NIB(pin)		(0xfu << ((pin) * 4))
#define PUD_DOWN(pin)		(1u << ((pin) * 4))

#define HSI2C_BASE		0x104b0000UL
#define USI_CON			(HSI2C_BASE + 0x0c4)	/* bit 0 = USI reset, active high */
#define USI_OPTION		(HSI2C_BASE + 0x0c8)

#define CMU_PERIC0		0x10400000UL
#define USI03_USER_MUX		(CMU_PERIC0 + 0x01e0)	/* PLL_CON0: MUX_SEL bit 4, 0 = OSCCLK */
#define USI03_DIV		(CMU_PERIC0 + 0x1810)	/* DIVRATIO bits 0-3 */
#define GPIO_PERIC0_PCLK	(CMU_PERIC0 + 0x2018)
#define RSTNSYNC_USI03		(CMU_PERIC0 + 0x2038)
#define SYSREG_PERIC0_PCLK	(CMU_PERIC0 + 0x2058)
#define USI03_IPCLK		(CMU_PERIC0 + 0x209c)
#define USI03_PCLK		(CMU_PERIC0 + 0x20a0)
#define GATE_ON			((1u << 21) | (1u << 20))	/* CG_VAL | MANUAL */
#define SYSREG_USI03		0x1042101cUL		/* usi_3 SW_CONF */
#define USI_V2_I2C		(1u << 2)

#define CTL			0x00
#define FIFO_CTL		0x04
#define TRAILIG_CTL		0x08
#define INT_ENABLE		0x20
#define INT_STATUS		0x24
#define FIFO_STATUS		0x30
#define TX_DATA			0x34
#define RX_DATA			0x38
#define CONF			0x40
#define AUTO_CONF		0x44
#define TIMEOUT			0x48
#define TRANS_STATUS		0x50
#define TIMING_FS1		0x60
#define TIMING_FS2		0x64
#define TIMING_FS3		0x68
#define TIMING_SLA		0x6c
#define ADDR			0x70

#define CTL_I2C_MASTER		((1u << 0) | (1u << 3))
#define CTL_RXCHON		(1u << 6)
#define CTL_TXCHON		(1u << 7)
#define CTL_SW_RST		(1u << 31)
#define CONF_AUTO_MODE		(1u << 31)
#define AUTO_READ		(1u << 16)
#define AUTO_STOP		(1u << 17)
#define AUTO_RUN		(1u << 31)
#define INT_DONE		(1u << 7)
#define INT_ERRS		(0xfu << 8)
#define RX_EMPTY		(1u << 24)
#define TX_FULL			(1u << 7)

#define TS_ADDR			0x48
#define TIMER_HZ		26000000UL
#define CLKIN			26000000u
#define SCL_HZ			400000u

static void (*kv)(const char *, uint64_t);
static void (*out)(const char *);

static inline uint32_t rd(uintptr_t a) { return *(volatile uint32_t *)a; }
static inline void wr(uintptr_t a, uint32_t v) { *(volatile uint32_t *)a = v; }
static inline uint32_t hr(unsigned o) { return rd(HSI2C_BASE + o); }
static inline void hw(unsigned o, uint32_t v) { wr(HSI2C_BASE + o, v); }

static uint64_t ticks(void)
{
	uint64_t v;
	__asm__ volatile("isb; mrs %0, cntpct_el0" : "=r"(v));
	return v;
}

static void delay_ms(unsigned ms)
{
	uint64_t end = ticks() + TIMER_HZ / 1000 * ms;
	while (ticks() < end)
		;
}

static void set_timing(void)
{
	unsigned ftl = (hr(CONF) >> 16) & 7;
	unsigned temp = CLKIN / SCL_HZ - (ftl + 3) * 2;
	unsigned div = temp / 512;
	unsigned cyc = (temp + ((ftl + 3) % (div + 1)) * 2) / (div + 1) - 2;
	unsigned l = cyc * 64 / 100, h = cyc - l;
	hw(TIMING_FS1, l << 24 | l << 16 | l << 8);
	hw(TIMING_FS2, (l / 2) << 24 | l << 8 | h);
	hw(TIMING_FS3, div << 16 | cyc);
	hw(TIMING_SLA, l / 2);
}

static void hsi2c_reset(void)
{
	hw(CTL, hr(CTL) | CTL_SW_RST);
	hw(CTL, hr(CTL) & ~CTL_SW_RST);
	set_timing();
	hw(TIMEOUT, hr(TIMEOUT) & ~(1u << 31));
	hw(CTL, CTL_I2C_MASTER);
	hw(TRAILIG_CTL, 0xf);
	hw(CONF, hr(CONF) | CONF_AUTO_MODE);
}

static uint32_t wait_done(void)
{
	uint64_t end = ticks() + TIMER_HZ / 20;
	for (;;) {
		uint32_t s = hr(INT_STATUS);
		if (s & (INT_DONE | INT_ERRS)) {
			hw(INT_STATUS, s);
			return (s & INT_ERRS) ? s : 0;
		}
		if (ticks() > end)
			return 0x80000000u | hr(TRANS_STATUS);
	}
}

static uint32_t xfer(const uint8_t *w, unsigned wlen, uint8_t *r, unsigned rlen)
{
	uint32_t st;
	hw(INT_STATUS, hr(INT_STATUS));
	hw(ADDR, (TS_ADDR & 0x3ff) << 10);
	hw(INT_ENABLE, INT_DONE | INT_ERRS);
	if (wlen) {
		hw(FIFO_CTL, 3);
		hw(CTL, CTL_I2C_MASTER | CTL_TXCHON);
		hw(AUTO_CONF, (rlen ? 0 : AUTO_STOP) | wlen | AUTO_RUN);
		for (unsigned i = 0; i < wlen; i++) {
			uint64_t end = ticks() + TIMER_HZ / 100;
			while ((hr(FIFO_STATUS) & TX_FULL) && ticks() < end)
				;
			hw(TX_DATA, w[i]);
		}
		if ((st = wait_done()))
			goto bad;
	}
	if (rlen) {
		hw(FIFO_CTL, 3);
		hw(CTL, CTL_I2C_MASTER | CTL_RXCHON);
		hw(AUTO_CONF, AUTO_READ | AUTO_STOP | rlen | AUTO_RUN);
		for (unsigned i = 0; i < rlen; i++) {
			uint64_t end = ticks() + TIMER_HZ / 20;
			while ((hr(FIFO_STATUS) & RX_EMPTY) && ticks() < end)
				;
			if (hr(FIFO_STATUS) & RX_EMPTY) {
				st = 0x40000000u | i;
				goto bad;
			}
			r[i] = (uint8_t)hr(RX_DATA);
		}
		if ((st = wait_done()))
			goto bad;
	}
	hw(CTL, CTL_I2C_MASTER);
	return 0;
bad:
	hsi2c_reset();
	return st;
}

static void dump(const char *what, const uint8_t *b, unsigned n, uint32_t st)
{
	out(what);
	kv(" st", st);
	if (!st) {
		uint64_t v = 0;
		for (unsigned i = 0; i < n && i < 8; i++)
			v = v << 8 | b[i];
		kv(" data", v);
	}
	out("\n");
}

/*
 * Read-only power check, no PMIC access. SCL/SDA (gpp1-4/5) and the
 * interrupt (gpa1-0) are pulled up on the touch side; with the touch rails
 * off nothing drives them. Make the three pins inputs with the SoC's weak
 * pull-down: a powered touch side still reads 1, an unpowered one reads 0.
 * The interrupt pin is restored afterwards; the I2C pins are set up below.
 */
static void pin_check(void)
{
	uint32_t pcon = rd(GPP1_CON), ppud = rd(GPP1_PUD);
	uint32_t acon = rd(GPA1_CON), apud = rd(GPA1_PUD);
	out("PIN CHECK (read only, no PMIC)\n");
	kv("gpp1 con", pcon);
	kv("dat", rd(GPP1_DAT));
	kv("pud", ppud);
	kv("gpa1 con", acon);
	kv("dat", rd(GPA1_DAT));
	kv("pud", apud);
	out("\n");
	/* 1. as left by S-Boot */
	uint32_t d = rd(GPP1_DAT), a = rd(GPA1_DAT);
	kv("as-is scl", (d >> 4) & 1);
	kv("sda", (d >> 5) & 1);
	kv("irq", a & 1);
	out("\n");
	/* 2. inputs, no pull */
	wr(GPP1_CON, pcon & ~(NIB(4) | NIB(5)));
	wr(GPP1_PUD, ppud & ~(NIB(4) | NIB(5)));
	wr(GPA1_CON, acon & ~NIB(0));
	wr(GPA1_PUD, apud & ~NIB(0));
	delay_ms(5);
	d = rd(GPP1_DAT);
	a = rd(GPA1_DAT);
	kv("float scl", (d >> 4) & 1);
	kv("sda", (d >> 5) & 1);
	kv("irq", a & 1);
	out("\n");
	/* 3. inputs, weak pull-down: only a powered pull-up keeps them at 1 */
	wr(GPP1_PUD, (ppud & ~(NIB(4) | NIB(5))) | PUD_DOWN(4) | PUD_DOWN(5));
	wr(GPA1_PUD, (apud & ~NIB(0)) | PUD_DOWN(0));
	delay_ms(5);
	unsigned scl = 0, sda = 0, irq = 0;
	for (unsigned i = 0; i < 8; i++) {
		d = rd(GPP1_DAT);
		a = rd(GPA1_DAT);
		scl += (d >> 4) & 1;
		sda += (d >> 5) & 1;
		irq += a & 1;
		delay_ms(1);
	}
	kv("pulldown scl", scl);
	kv("sda", sda);
	kv("irq", irq);
	out(" (of 8)\n");
	if (scl == 8 && sda == 8)
		out("PIN CHECK: I2C lines held high -> touch I/O rail looks ON\n");
	else if (scl == 0 && sda == 0)
		out("PIN CHECK: I2C lines low -> touch I/O rail looks OFF\n");
	else
		out("PIN CHECK: mixed levels -> inconclusive\n");
	/* restore the interrupt pin; I2C pins are configured by the caller */
	wr(GPA1_PUD, apud);
	wr(GPA1_CON, acon);
	wr(GPP1_PUD, ppud);
}

void touch_probe(void (*kv_fn)(const char *, uint64_t), void (*out_fn)(const char *))
{
	uint8_t c, b[11];
	uint32_t st;

	kv = kv_fn;
	out = out_fn;
	out("\n--- D2S TOUCH PROBE (S6SY771 on hsi2c_10 @ 0x104b0000) ---\n");
	pin_check();

	/* 0. Clocks: ungate USI03 (bus + IP clock, reset sync), GPIO and
	 * SYSREG of PERIC0; USI03 from OSCCLK, divider 1; USI mode I2C. */
	kv("pclk", rd(USI03_PCLK));
	kv("ipclk", rd(USI03_IPCLK));
	kv("mux", rd(USI03_USER_MUX));
	kv("div", rd(USI03_DIV));
	kv("sw_conf", rd(SYSREG_USI03));
	out("\n");
	wr(GPIO_PERIC0_PCLK, rd(GPIO_PERIC0_PCLK) | GATE_ON);
	wr(SYSREG_PERIC0_PCLK, rd(SYSREG_PERIC0_PCLK) | GATE_ON);
	wr(RSTNSYNC_USI03, rd(RSTNSYNC_USI03) | GATE_ON);
	wr(USI03_PCLK, rd(USI03_PCLK) | GATE_ON);
	wr(USI03_IPCLK, rd(USI03_IPCLK) | GATE_ON);
	wr(USI03_USER_MUX, rd(USI03_USER_MUX) & ~(1u << 4));
	wr(USI03_DIV, rd(USI03_DIV) & ~0xfu);
	delay_ms(1);
	wr(SYSREG_USI03, USI_V2_I2C);
	kv("pclk", rd(USI03_PCLK));
	kv("mux", rd(USI03_USER_MUX));
	kv("div", rd(USI03_DIV));
	kv("sw_conf", rd(SYSREG_USI03));
	out("\n");

	/* 1. Setup gpp1 pins: pin 4 (SCL) and pin 5 (SDA) -> Function 2 (hsi2c10_bus), no pull */
	uint32_t gpp1_con = rd(GPP1_CON);
	gpp1_con &= ~(0xffu << 16);
	gpp1_con |= (0x22u << 16);
	wr(GPP1_CON, gpp1_con);
	wr(GPP1_PUD, rd(GPP1_PUD) & ~(NIB(4) | NIB(5)));	/* no pull (4-bit fields) */
	kv("gpp1_con", rd(GPP1_CON));

	/* 2. Release the USI reset (i2c-exynos5.c exynos_usi_init) */
	kv("usi_con", rd(USI_CON));
	wr(USI_CON, 0);
	kv("usi_con", rd(USI_CON));

	/* 3. Check GPA1-0 IRQ line level */
	kv("gpa1_dat", rd(GPA1_DAT) & 1u);

	/* 4. Reset HSI2C controller */
	hsi2c_reset();
	kv("hsi2c_ctl", hr(CTL));
	kv("conf", hr(CONF));
	out("\n");

	/* 5. Probe Touch IC */
	delay_ms(10);
	c = 0x55;	/* BOOT_STATUS */
	st = xfer(&c, 1, b, 1);
	dump("boot_status", b, 1, st);

	c = 0x52;	/* READ_ID: Android reports 53 45 37 71 00 */
	st = xfer(&c, 1, b, 5);
	dump("device_id", b, 5, st);

	c = 0xaf;	/* READ_TS_STATUS: Android reports 00 02 01 00 */
	st = xfer(&c, 1, b, 4);
	dump("ts_status", b, 4, st);

	/* Send TOUCH_FUNCTION and SENSE_ON */
	uint8_t func[3] = {0x30, 0x01, 0x00};
	st = xfer(func, 3, NULL, 0);
	kv("func_st", st);

	uint8_t sense = 0x10;
	st = xfer(&sense, 1, NULL, 0);
	kv("sense_st", st);

	out("--- TOUCH PROBE DONE ---\n\n");
}

void battery_probe(void (*kv_fn)(const char *, uint64_t), void (*out_fn)(const char *))
{
	(void)kv_fn;
	(void)out_fn;
}
