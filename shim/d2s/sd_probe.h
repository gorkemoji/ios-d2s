/* SPDX-License-Identifier: GPL-2.0-only
 * Galaxy Note 10+ (d2s) MicroSD DWMMC probe.
 */
#ifndef SD_PROBE_H
#define SD_PROBE_H

#include <stdint.h>

void sd_probe(void (*kv)(const char *, uint64_t), void (*out)(const char *));

#endif

