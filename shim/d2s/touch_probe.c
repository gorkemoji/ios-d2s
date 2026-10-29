/* SPDX-License-Identifier: GPL-2.0-only
 * Galaxy Note 10+ (SM-N975F, d2s, Exynos 9825) touchscreen probe (MMU off).
 *
 * Controller: Samsung S6SY771 ("sec,sec_ts", reg <0x48>) on hsi2c_10 (0x104b0000)
 * Pins: gpp1-4 (SCL), gpp1-5 (SDA) on pinctrl_5@10430000 (function 2)
 * Interrupt: gpa1-0 on pinctrl_alive@15850000
 * USI block: 0x104b00c0
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

#define HSI2C_BASE		0x104b0000UL
#define USI_CONFIG		(HSI2C_BASE + 0x0c0)
#define USI_CON			(HSI2C_BASE + 0x0c4)
#define USI_OPTION		(HSI2C_BASE + 0x0c8)

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

void touch_probe(void (*kv_fn)(const char *, uint64_t), void (*out_fn)(const char *))
{
	uint8_t c, b[11];
	uint32_t st;

	kv = kv_fn;
	out = out_fn;
	out("\n--- D2S TOUCH PROBE (S6SY771 on hsi2c_10 @ 0x104b0000) ---\n");

	/* 1. Setup gpp1 pins: pin 4 (SCL) and pin 5 (SDA) -> Function 2 (hsi2c10_bus), no pull */
	uint32_t gpp1_con = rd(GPP1_CON);
	gpp1_con &= ~(0xffu << 16);
	gpp1_con |= (0x22u << 16);
	wr(GPP1_CON, gpp1_con);
	wr(GPP1_PUD, rd(GPP1_PUD) & ~(0x0fu << 8));
	kv("gpp1_con", rd(GPP1_CON));

	/* 2. Setup USI block at 0x104b00c0 for I2C mode and release reset */
	wr(USI_CONFIG, 0x1);
	wr(USI_CON, rd(USI_CON) & ~1u);
	kv("usi_con", rd(USI_CON));

	/* 3. Check GPA1-0 IRQ line level */
	kv("gpa1_dat", rd(GPA1_DAT) & 1u);

	/* 4. Reset HSI2C controller */
	hsi2c_reset();
	kv("hsi2c_ctl", hr(CTL));

	/* 5. Probe Touch IC */
	delay_ms(10);
	c = 0x55;	/* BOOT_STATUS */
	st = xfer(&c, 1, b, 1);
	dump("boot_status", b, 1, st);

	c = 0x52;	/* DEVICE_ID */
	st = xfer(&c, 1, b, 3);
	dump("device_id", b, 3, st);

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
