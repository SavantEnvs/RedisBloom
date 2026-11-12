#ifndef MAYHEM_RDB_HARNESS_H
#define MAYHEM_RDB_HARNESS_H
#include "mock.h"

// Input: [encver byte, only when the type has several encodings] + the serialized RDB payload.
// Each input is loaded by the real loader from a fresh mock stream; a loader that accepts it must
// also free it cleanly.
#define RDB_FUZZ_TARGET(LOAD, FREE, NENC)                                                          \
    int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {                                 \
        int encver = 0;                                                                            \
        if (NENC > 1) {                                                                            \
            if (size < 1) return -1;                                                               \
            encver = data[0] % (NENC);                                                             \
            data++, size--;                                                                        \
        }                                                                                          \
        mock_init();                                                                               \
        RedisModuleIO io;                                                                          \
        mock_io_reader(&io, data, size);                                                           \
        void *obj = LOAD(&io, encver);                                                             \
        if (obj) FREE(obj);                                                                        \
        return 0;                                                                                  \
    }
#endif
