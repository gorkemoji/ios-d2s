/* SPDX-License-Identifier: GPL-2.0-only
 * Galaxy Note 10+ (SM-N975F, d2s, Exynos 9825) MicroSD probe and init.
 */
#include <stdint.h>
#include <stddef.h>
#include "sd_probe.h"

#define PD_FSYS0A_PWR_CFG	0x15862200UL
#define PD_FSYS0A_PWR_STA	0x15862204UL

#define DWMMC_BASE		0x13d00000UL
#define REG_CTRL		0x000u
#define REG_PWREN		0x004u
#define REG_CLKDIV		0x008u
#define REG_CLKSRC		0x00cu
#define REG_CLKENA		0x010u
#define REG_TMOUT		0x014u
#define REG_CTYPE		0x018u
#define REG_BLKSIZ		0x01cu
#define REG_BYTCNT		0x020u
#define REG_INTMASK		0x024u
#define REG_CMDARG		0x028u
#define REG_CMD			0x02cu
#define REG_RESP0		0x030u
#define REG_RESP1		0x034u
#define REG_RESP2		0x038u
#define REG_RESP3		0x03cu
#define REG_RINTSTS		0x044u
#define REG_STATUS		0x048u
#define REG_CDETECT		0x050u
#define REG_WRTPRT		0x054u
#define REG_VERID		0x06cu
#define REG_HCON		0x070u
#define REG_DATA		0x200u

#define CMD_START		(1u << 31)
#define CMD_USE_HOLD_REG	(1u << 29)
#define CMD_UPDATE_CLK_ONLY	(1u << 21)
#define CMD_WAIT_PRVDATA	(1u << 13)
#define CMD_DATA_EXPECTED	(1u << 9)
#define CMD_CHECK_CRC		(1u << 8)
#define CMD_RESP_LONG		(1u << 7)
#define CMD_RESP_EXPECTED	(1u << 6)

#define INT_CMD_DONE		(1u << 2)
#define INT_DTO			(1u << 3)
#define INT_RTO			(1u << 8)

static inline uint32_t rd(uintptr_t a) { return *(volatile uint32_t *)a; }
static inline void wr(uintptr_t a, uint32_t v) { *(volatile uint32_t *)a = v; }

static void delay(uint32_t count)
{
	for (volatile uint32_t i = 0; i < count; i++)
		__asm__ volatile("nop");
}

static int dwmmc_update_clk(void)
{
	wr(DWMMC_BASE + REG_CMD, CMD_START | CMD_UPDATE_CLK_ONLY | CMD_WAIT_PRVDATA);
	for (int i = 0; i < 200000; i++) {
		if (!(rd(DWMMC_BASE + REG_CMD) & CMD_START))
			return 0;
	}
	return -1;
}

static int dwmmc_cmd(uint32_t cmd_idx, uint32_t arg, uint32_t flags, uint32_t *resp0)
{
	wr(DWMMC_BASE + REG_RINTSTS, 0xffffffffu);
	wr(DWMMC_BASE + REG_CMDARG, arg);
	wr(DWMMC_BASE + REG_CMD, CMD_START | CMD_USE_HOLD_REG | flags | (cmd_idx & 0x3fu));

	for (int i = 0; i < 200000; i++) {
		uint32_t rint = rd(DWMMC_BASE + REG_RINTSTS);
		if (rint & INT_CMD_DONE) {
			if (resp0)
				*resp0 = rd(DWMMC_BASE + REG_RESP0);
			wr(DWMMC_BASE + REG_RINTSTS, 0xffffffffu);
			return 0;
		}
		if (rint & INT_RTO) {
			wr(DWMMC_BASE + REG_RINTSTS, 0xffffffffu);
			return -1;
		}
	}
	return -2; /* timeout */
}

void sd_probe(void (*kv)(const char *, uint64_t), void (*out)(const char *))
{
	out("--- MicroSD (DWMMC2) Hardware Bringup v5 ---\n");

	/* 1. Power on pd-fsys0a */
	wr(PD_FSYS0A_PWR_CFG, 0x0fu);
	for (int i = 0; i < 50000 && (rd(PD_FSYS0A_PWR_STA) & 3u) != 3u; i++)
		delay(10);

	kv("pd_fsys0a", rd(PD_FSYS0A_PWR_STA));
	uint32_t cdetect = rd(DWMMC_BASE + REG_CDETECT);
	uint32_t wrtprt = rd(DWMMC_BASE + REG_WRTPRT);
	kv("cdetect", cdetect & 1u); /* 0 = card present, 1 = no card */
	kv("wrtprt", wrtprt & 1u);
	out("\n");

	/* 2. Reset Controller & FIFO */
	wr(DWMMC_BASE + REG_CTRL, 0x07u);
	delay(5000);
	for (int i = 0; i < 50000 && (rd(DWMMC_BASE + REG_CTRL) & 0x07u); i++)
		delay(10);

	/* 3. Turn on Power and wait 20ms using safe CPU loop */
	wr(DWMMC_BASE + REG_PWREN, 1u);
	delay(40000000u); /* ~20ms at 2 GHz */

	/* 4. Set Initial Clock (400 kHz) with Continuous Clocking */
	wr(DWMMC_BASE + REG_TMOUT, 0xffffffffu);
	wr(DWMMC_BASE + REG_INTMASK, 0u);
	wr(DWMMC_BASE + REG_CLKDIV, 0x20u);
	dwmmc_update_clk();
	wr(DWMMC_BASE + REG_CLKENA, 1u);
	dwmmc_update_clk();

	/* 5. Clock pulse train before first command */
	delay(20000000u);

	kv("pwren", rd(DWMMC_BASE + REG_PWREN));
	kv("clkena", rd(DWMMC_BASE + REG_CLKENA));
	out("\n");

	/* 6. CMD0: Reset card to IDLE (twice) */
	uint32_t resp = 0;
	int rc = dwmmc_cmd(0, 0, 0, NULL);
	delay(1000000u);
	rc = dwmmc_cmd(0, 0, 0, NULL);
	delay(1000000u);
	kv("CMD0 rc", (uint64_t)(int64_t)rc);

	/* 7. CMD8: Check voltage */
	for (int a = 0; a < 3; a++) {
		rc = dwmmc_cmd(8, 0x000001aau, CMD_RESP_EXPECTED | CMD_CHECK_CRC, &resp);
		if (rc == 0)
			break;
		delay(2000000u);
	}
	kv("CMD8 rc", (uint64_t)(int64_t)rc);
	kv("resp", resp);
	out("\n");

	/* 8. ACMD41 loop */
	int ready = 0;
	for (int attempt = 0; attempt < 30; attempt++) {
		rc = dwmmc_cmd(55, 0, CMD_RESP_EXPECTED | CMD_CHECK_CRC, &resp);
		if (rc != 0) {
			delay(1000000u);
			continue;
		}
		rc = dwmmc_cmd(41, 0x40ff8000u, CMD_RESP_EXPECTED, &resp);
		if (rc == 0 && (resp & (1u << 31))) {
			ready = 1;
			break;
		}
		delay(2000000u);
	}
	kv("ACMD41 ready", ready);
	kv("OCR", resp);
	out("\n");

	if (!ready) {
		out("[-] MicroSD unpowered or unselected; proceeding to XNU\n");
		return;
	}

	/* 9. CMD2: ALL_SEND_CID */
	rc = dwmmc_cmd(2, 0, CMD_RESP_EXPECTED | CMD_RESP_LONG | CMD_CHECK_CRC, &resp);
	kv("CMD2 rc", (uint64_t)(int64_t)rc);

	/* 10. CMD3: SEND_RELATIVE_ADDR */
	rc = dwmmc_cmd(3, 0, CMD_RESP_EXPECTED | CMD_CHECK_CRC, &resp);
	uint32_t rca = resp & 0xffff0000u;
	kv("RCA", (uint64_t)rca >> 16);
	out("\n");

	/* 11. CMD7: SELECT_CARD (moves to TRAN state) */
	rc = dwmmc_cmd(7, rca, CMD_RESP_EXPECTED | CMD_CHECK_CRC, &resp);
	kv("CMD7 rc", (uint64_t)(int64_t)rc);
	out("\n");

	/* 12. Switch to 25 MHz (CLKDIV = 1) */
	wr(DWMMC_BASE + REG_CLKDIV, 1u);
	dwmmc_update_clk();

	/* 13. CMD16: SET_BLOCKLEN = 512 */
	rc = dwmmc_cmd(16, 512u, CMD_RESP_EXPECTED | CMD_CHECK_CRC, &resp);

	/* 14. CMD17: Read LBA 0 (MBR) */
	wr(DWMMC_BASE + REG_BLKSIZ, 512u);
	wr(DWMMC_BASE + REG_BYTCNT, 512u);
	wr(DWMMC_BASE + REG_RINTSTS, 0xffffffffu);
	wr(DWMMC_BASE + REG_CMDARG, 0u);
	wr(DWMMC_BASE + REG_CMD, CMD_START | CMD_USE_HOLD_REG | CMD_DATA_EXPECTED |
				 CMD_WAIT_PRVDATA | CMD_RESP_EXPECTED | CMD_CHECK_CRC | 17u);

	uint32_t mbr_sig = 0;
	int read_words = 0;
	for (int i = 0; i < 200000 && read_words < 128; i++) {
		uint32_t st = rd(DWMMC_BASE + REG_STATUS);
		while (!(st & (1u << 10)) && read_words < 128) {
			uint32_t w = rd(DWMMC_BASE + REG_DATA);
			if (read_words == 127)
				mbr_sig = w >> 16;
			read_words++;
			st = rd(DWMMC_BASE + REG_STATUS);
		}
		if (rd(DWMMC_BASE + REG_RINTSTS) & INT_DTO)
			break;
	}

	kv("CMD17 words", read_words);
	kv("MBR", mbr_sig);
	out("\n");
	if (mbr_sig == 0xaa55 || mbr_sig == 0x55aa) {
		out("[+] SUCCESS: MicroSD MBR 0x55AA Verified!\n");
	}
}
