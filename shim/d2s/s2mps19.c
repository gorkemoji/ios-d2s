/* SPDX-License-Identifier: GPL-2.0-only
 * Galaxy Note 10+ (d2s, Exynos 9825) touch rails on the S2MPS19 PMIC (MMU off).
 *
 * The pin check found the touch side unpowered: SCL/SDA/IRQ all follow the
 * SoC's weak pull-down. Stock DT (tools/d2s_merged.dtb):
 *   tsp_io   = S2MPS19 LDO19M, 1.8 V  (L19CTRL 0x58, 0.7 V + n * 25 mV)
 *   tsp_avdd = S2MPS19 LDO27M, 3.0 V  (L27CTRL 0x60, 1.8 V + n * 25 mV)
 * Enable is bits 6-7 (3 = on), so the values are 0xec and 0xf0.
 *
 * On the 9820 the AP does not drive SPEEDY itself: the APM core owns it and
 * the AP asks over ACPM IPC (Samsung acpm_ipc.c, acpm_mfd.c, s2mps19_core.c).
 * - mailbox registers: 0x15900000 (acpm_ipc reg 0)
 * - APM SRAM: 0x02039000, struct acpm_framework at +0x7000 (initdata-base)
 * - PMIC client: acpm-ipc-channel 2, speedy channel 0, PMIC block address 1
 * The AP's TX queue is the APM's RX queue (rx_front/rx_rear/rx_base) and the
 * reverse for replies. A request is 4 words:
 *   cmd[0] = reg | type << 8 | speedy channel << 12 | seq << 16
 *   cmd[1] = func | value << 8          reply: dest << 8 | return << 24
 * No tables of pointers or strings here: the loader allows no absolute
 * relocations.
 */
#include <stdint.h>
#include "s2mps19.h"

#define MBOX			0x15900000UL
#define INTGR0			0x08
#define INTCR1			0x20
#define INTSR1			0x28
#define SRAM			0x02039000UL
#define INITDATA		(SRAM + 0x7000)
/* struct acpm_framework (fwh/framework.h), all u32 */
#define FW_IPC_CHANNELS		0x08
#define FW_IPC_AP_MAX		0x18
/* struct ipc_channel: id, field, owner, type, channel_info (13 u32), ap_poll */
#define CH_SIZE			72
#define CH_ID			0
#define CH_RX_REAR		16
#define CH_RX_FRONT		20
#define CH_RX_BASE		24
#define CH_TX_REAR		40
#define CH_TX_FRONT		44
#define CH_TX_BASE		48
#define CH_Q_LEN		52
#define CH_Q_ELEM		56

#define PMIC_IPC_CH		2
#define SPEEDY_CH		0
#define TYPE_PMIC		1
#define FUNC_READ		0
#define FUNC_WRITE		1

#define REG_ID			0x00
#define L19CTRL			0x58
#define L27CTRL			0x60
#define L19_1V8			0xec
#define L27_3V0			0xf0

#define TIMER_HZ		26000000UL

static void (*kv)(const char *, uint64_t);
static void (*out)(const char *);

static inline uint32_t rd(uintptr_t a) { return *(volatile uint32_t *)a; }
static inline void wr(uintptr_t a, uint32_t v) { *(volatile uint32_t *)a = v; }

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

static uintptr_t ch;		/* this channel's descriptor in SRAM */
static uint32_t ch_id;
static uint32_t seq;

static uint32_t chr(unsigned off) { return rd(ch + off); }

static int find_channel(void)
{
	uint32_t base = rd(INITDATA + FW_IPC_CHANNELS);
	uint32_t n = rd(INITDATA + FW_IPC_AP_MAX);
	kv("acpm ch base", base);
	kv("ap_max", n);
	out("\n");
	if (!base || base >= 0x30000 || !n || n > 32)
		return -1;
	for (uint32_t i = 0; i < n; i++) {
		uintptr_t c = SRAM + base + i * CH_SIZE;
		if (rd(c + CH_ID) == PMIC_IPC_CH) {
			ch = c;
			ch_id = PMIC_IPC_CH;
			kv("ch", i);
			kv("q_len", chr(CH_Q_LEN));
			kv("elem", chr(CH_Q_ELEM));
			kv("tx front", rd(SRAM + chr(CH_RX_FRONT)));
			kv("rear", rd(SRAM + chr(CH_RX_REAR)));
			out("\n");
			return 0;
		}
	}
	return -1;
}

/* One request on the PMIC channel; returns cmd[1] of the reply or < 0. */
static int64_t ipc(uint32_t cmd0, uint32_t cmd1)
{
	uint32_t len = chr(CH_Q_LEN), size = chr(CH_Q_ELEM);
	uintptr_t tx_front = SRAM + chr(CH_RX_FRONT), tx_rear = SRAM + chr(CH_RX_REAR);
	uintptr_t tx_base = SRAM + chr(CH_RX_BASE);
	uintptr_t rx_front = SRAM + chr(CH_TX_FRONT), rx_rear = SRAM + chr(CH_TX_REAR);
	uintptr_t rx_base = SRAM + chr(CH_TX_BASE);
	if (!len || len > 64 || size < 8 || size > 64)
		return -1;

	uint32_t front = rd(tx_front), next = (front + 1) % len;
	uint64_t end = ticks() + TIMER_HZ / 50;
	while (rd(tx_rear) == next)		/* queue full */
		if (ticks() > end)
			return -2;

	if (++seq >= 64)
		seq = 1;
	cmd0 |= seq << 16;
	uintptr_t slot = tx_base + size * front;
	wr(slot, cmd0);
	wr(slot + 4, cmd1);
	for (uint32_t o = 8; o < size; o += 4)
		wr(slot + o, 0);
	__asm__ volatile("dsb sy");
	wr(tx_front, next);
	__asm__ volatile("dsb sy");
	wr(MBOX + INTGR0, (1u << ch_id) << 16);

	end = ticks() + TIMER_HZ / 20;		/* 50 ms, as the kernel's sync wait */
	while (ticks() < end) {
		uint32_t f = rd(rx_front), r = rd(rx_rear);
		for (uint32_t i = r; i != f; i = (i + 1) % len) {
			uintptr_t e = rx_base + size * i;
			if (((rd(e) >> 16) & 0x3f) != seq)
				continue;
			uint32_t reply = rd(e + 4);
			/* dequeue as check_response() does: move rear's entry here */
			if (i != r)
				for (uint32_t o = 0; o < size; o += 4)
					wr(e + o, rd(rx_base + size * r + o));
			r = (r + 1) % len;
			wr(rx_rear, r);
			if (r == rd(rx_front))
				wr(MBOX + INTCR1, 1u << ch_id);
			return reply;
		}
	}
	kv("ipc timeout sr1", rd(MBOX + INTSR1));
	return -3;
}

static int pmic_read(uint8_t reg)
{
	int64_t r = ipc(reg | TYPE_PMIC << 8 | SPEEDY_CH << 12, FUNC_READ);
	if (r < 0)
		return (int)r;
	if ((r >> 24) & 0xff)
		return -16 - (int)((r >> 24) & 0xff);
	return (int)((r >> 8) & 0xff);
}

static int pmic_write(uint8_t reg, uint8_t val)
{
	int64_t r = ipc(reg | TYPE_PMIC << 8 | SPEEDY_CH << 12, FUNC_WRITE | (uint32_t)val << 8);
	if (r < 0)
		return (int)r;
	return (r >> 24) & 0xff ? -16 - (int)((r >> 24) & 0xff) : 0;
}

static void show(const char *k, uint8_t reg)
{
	int v = pmic_read(reg);
	kv(k, v < 0 ? 0xdead0000u | (uint32_t)(-v) : (uint64_t)v);
}

int s2mps19_touch_rails_on(void (*kv_fn)(const char *, uint64_t), void (*out_fn)(const char *))
{
	kv = kv_fn;
	out = out_fn;
	out("TOUCH RAILS (S2MPS19 over ACPM ch 2)\n");
	if (find_channel()) {
		out("TOUCH RAILS: no ACPM PMIC channel, skipped\n");
		return -1;
	}
	int id = pmic_read(REG_ID);
	kv("pmic id", id < 0 ? 0xdead0000u | (uint32_t)(-id) : (uint64_t)id);
	if (id < 0) {
		out("\nTOUCH RAILS: PMIC read failed, nothing written\n");
		return -1;
	}
	show("L19", L19CTRL);
	show("L27", L27CTRL);
	out("\n");

	/* sec_ts_power(): dvdd (tsp_io), 1 ms, avdd, then the probe waits 70 ms */
	int r1 = pmic_write(L19CTRL, L19_1V8);
	delay_us(1000);
	int r2 = pmic_write(L27CTRL, L27_3V0);
	delay_us(5000);
	kv("write rc", (uint64_t)(uint32_t)r1 << 32 | (uint32_t)r2);
	show("L19", L19CTRL);
	show("L27", L27CTRL);
	out("\n");
	delay_us(70000);
	if (r1 || r2) {
		out("TOUCH RAILS: write failed\n");
		return -1;
	}
	out("TOUCH RAILS: on\n");
	return 0;
}
