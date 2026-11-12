#include "mock.h"
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#define TRY_ALLOC_LIMIT ((size_t)1 << 24)
/* Per-input cumulative cap, so many small allocations (a header claiming
 * thousands of sub-filters) fail like an out-of-memory Redis instead of
 * exhausting the fuzzer's RSS limit. Reset in mock_init(). */
#define ALLOC_BUDGET ((size_t)1 << 27)

static size_t alloc_used;

static int charge(size_t a, size_t b) {
    if (b && a > ALLOC_BUDGET / b) return 0;
    size_t n = a * b;
    if (n > ALLOC_BUDGET - alloc_used) return 0;
    alloc_used += n;
    return 1;
}
static void *m_alloc(size_t n) {
    charge(n, 1);
    return malloc(n ? n : 1);
}
static void *m_calloc(size_t a, size_t b) {
    charge(a, b);
    return calloc(a ? a : 1, b ? b : 1);
}
static void *m_realloc(void *p, size_t n) {
    charge(n, 1);
    return realloc(p, n ? n : 1);
}
static void m_free(void *p) { free(p); }
static void *m_try_alloc(size_t n) {
    if (n > TRY_ALLOC_LIMIT || !charge(n, 1)) return NULL;
    return malloc(n ? n : 1);
}
static void *m_try_calloc(size_t a, size_t b) {
    if ((b && a > TRY_ALLOC_LIMIT / b) || !charge(a, b)) return NULL;
    return calloc(a ? a : 1, b ? b : 1);
}
static void *m_try_realloc(void *p, size_t n) {
    if (n > TRY_ALLOC_LIMIT || !charge(n, 1)) return NULL;
    return realloc(p, n ? n : 1);
}
static char *m_strdup(const char *s) { return strdup(s); }

static int take(RedisModuleIO *io, void *dst, size_t n) {
    if (io->err || io->in_len - io->in_pos < n) {
        io->err = 1;
        memset(dst, 0, n);
        return 0;
    }
    memcpy(dst, io->in + io->in_pos, n);
    io->in_pos += n;
    return 1;
}

static uint64_t m_load_unsigned(RedisModuleIO *io) {
    uint64_t v;
    take(io, &v, sizeof v);
    return v;
}
static int64_t m_load_signed(RedisModuleIO *io) { return (int64_t)m_load_unsigned(io); }
static double m_load_double(RedisModuleIO *io) {
    double v;
    take(io, &v, sizeof v);
    return v;
}
static char *m_load_string_buffer(RedisModuleIO *io, size_t *len) {
    uint64_t n = m_load_unsigned(io);
    *len = 0;
    if (io->err || n > io->in_len - io->in_pos) {
        io->err = 1;
        return NULL;
    }
    char *buf = malloc(n ? n : 1);
    memcpy(buf, io->in + io->in_pos, n);
    io->in_pos += n;
    *len = n;
    return buf;
}
static int m_is_io_error(RedisModuleIO *io) { return io->err; }
static RedisModuleCtx *m_ctx_from_io(RedisModuleIO *io) {
    (void)io;
    return NULL;
}
static void m_log(RedisModuleCtx *ctx, const char *level, const char *fmt, ...) {
    (void)ctx, (void)level, (void)fmt;
}

static void put(RedisModuleIO *io, const void *src, size_t n) {
    if (io->out_len + n > io->out_cap) {
        io->out_cap = (io->out_len + n) * 2;
        io->out = realloc(io->out, io->out_cap);
    }
    memcpy(io->out + io->out_len, src, n);
    io->out_len += n;
}
static void m_save_unsigned(RedisModuleIO *io, uint64_t v) { put(io, &v, sizeof v); }
static void m_save_signed(RedisModuleIO *io, int64_t v) { put(io, &v, sizeof v); }
static void m_save_double(RedisModuleIO *io, double v) { put(io, &v, sizeof v); }
static void m_save_string_buffer(RedisModuleIO *io, const char *s, size_t n) {
    m_save_unsigned(io, n);
    put(io, s, n);
}

void mock_init(void) {
    alloc_used = 0;
    RedisModule_Alloc = m_alloc;
    RedisModule_TryAlloc = m_try_alloc;
    RedisModule_Calloc = m_calloc;
    RedisModule_TryCalloc = m_try_calloc;
    RedisModule_Realloc = m_realloc;
    RedisModule_TryRealloc = m_try_realloc;
    RedisModule_Free = m_free;
    RedisModule_Strdup = m_strdup;
    RedisModule_LoadUnsigned = m_load_unsigned;
    RedisModule_LoadSigned = m_load_signed;
    RedisModule_LoadDouble = m_load_double;
    RedisModule_LoadStringBuffer = m_load_string_buffer;
    RedisModule_IsIOError = m_is_io_error;
    RedisModule_GetContextFromIO = m_ctx_from_io;
    RedisModule_Log = m_log;
    RedisModule_SaveUnsigned = m_save_unsigned;
    RedisModule_SaveSigned = m_save_signed;
    RedisModule_SaveDouble = m_save_double;
    RedisModule_SaveStringBuffer = m_save_string_buffer;
}

void mock_io_reader(RedisModuleIO *io, const uint8_t *data, size_t len) {
    memset(io, 0, sizeof *io);
    io->in = data;
    io->in_len = len;
}
void mock_io_writer(RedisModuleIO *io) { memset(io, 0, sizeof *io); }
void mock_io_release(RedisModuleIO *io) { free(io->out); }
