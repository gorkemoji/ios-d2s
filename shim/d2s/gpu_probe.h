#pragma once
#include <stdint.h>

/* Powers the Mali-G76 domain, reads GPU_ID, soft-resets the GPU and powers
 * its cores once (step 1 of the GPU work). Logs everything. */
void gpu_probe(void (*kv)(const char *, uint64_t), void (*out)(const char *));
