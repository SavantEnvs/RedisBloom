// RedisBloom's BF/CF/TopK RDB callbacks are `static`; including their translation units here
// reaches them without editing upstream. This file replaces rebloom.o and rm_topk.o in the link.
#include "../src/rebloom.c"
#include "../src/rm_topk.c"
#include "mock.h"

void *mh_BFRdbLoad(RedisModuleIO *io, int encver) { return BFRdbLoad(io, encver); }
void mh_BFRdbSave(RedisModuleIO *io, void *value) { BFRdbSave(io, value); }
void mh_BFFree(void *value) { BFFree(value); }
void *mh_CFRdbLoad(RedisModuleIO *io, int encver) { return CFRdbLoad(io, encver); }
void mh_CFRdbSave(RedisModuleIO *io, void *value) { CFRdbSave(io, value); }
void mh_CFFree(void *value) { CFFree(value); }
void *mh_TopKRdbLoad(RedisModuleIO *io, int encver) { return TopKRdbLoad(io, encver); }
void mh_TopKRdbSave(RedisModuleIO *io, void *value) { TopKRdbSave(io, value); }
void mh_TopKFree(void *value) { TopKFree(value); }
