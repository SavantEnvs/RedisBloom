#include "rdb_harness.h"
void *CMSRdbLoad(RedisModuleIO *io, int encver);
void CMSFree(void *value);
RDB_FUZZ_TARGET(CMSRdbLoad, CMSFree, 2)
