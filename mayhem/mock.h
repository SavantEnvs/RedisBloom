// In-process stand-in for the Redis module API: allocators plus a byte-stream RedisModuleIO,
// enough to drive RedisBloom's RDB loaders and chunk loaders without a Redis server.
#ifndef MAYHEM_MOCK_H
#define MAYHEM_MOCK_H
#include <stddef.h>
#include <stdint.h>
#include "redismodule.h"

struct RedisModuleIO {
    const uint8_t *in;
    size_t in_len, in_pos;
    int err;
    uint8_t *out;
    size_t out_len, out_cap;
};

void mock_init(void);
void mock_io_reader(RedisModuleIO *io, const uint8_t *data, size_t len);
void mock_io_writer(RedisModuleIO *io);
void mock_io_release(RedisModuleIO *io);

void *mh_BFRdbLoad(RedisModuleIO *io, int encver);
void mh_BFRdbSave(RedisModuleIO *io, void *value);
void mh_BFFree(void *value);
void *mh_CFRdbLoad(RedisModuleIO *io, int encver);
void mh_CFRdbSave(RedisModuleIO *io, void *value);
void mh_CFFree(void *value);
void *mh_TopKRdbLoad(RedisModuleIO *io, int encver);
void mh_TopKRdbSave(RedisModuleIO *io, void *value);
void mh_TopKFree(void *value);
#endif
