#ifndef D2S_SMP_PROBE_H
#define D2S_SMP_PROBE_H
#include <stdint.h>
void smp_probe(void (*kv)(const char *, uint64_t), void (*out)(const char *));
#endif
