#include "rdb_harness.h"
void *TDigestRdbLoad(RedisModuleIO *io, int encver);
void TDigestFree(void *value);
RDB_FUZZ_TARGET(TDigestRdbLoad, TDigestFree, 1)
