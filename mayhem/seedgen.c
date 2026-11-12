// Writes starter seeds by running the real serializers (RDB save, SCANDUMP) over small structures.
// Not used by the build's tests; run: bin/seedgen <outdir>, then copy into <target>/testsuite/.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mock.h"
#include "sb.h"
#include "cf.h"
#include "cms.h"
#include "topk.h"
#include "tdigest.h"

void CMSRdbSave(RedisModuleIO *io, void *obj);
void TDigestRdbSave(RedisModuleIO *io, void *obj);

static const char *outdir;

static void emit(const char *name, const uint8_t *pre, size_t prelen, const uint8_t *buf, size_t len) {
    char path[512];
    snprintf(path, sizeof path, "%s/%s", outdir, name);
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); exit(1); }
    if (prelen) fwrite(pre, 1, prelen, f);
    fwrite(buf, 1, len, f);
    fclose(f);
}

static void emit_io(const char *name, uint8_t encver, int with_encver, RedisModuleIO *io) {
    emit(name, &encver, with_encver, io->out, io->out_len);
    mock_io_release(io);
}

static SBChain *make_bf(uint64_t cap, double err, unsigned opts, unsigned growth, int items) {
    int e;
    SBChain *sb = SB_NewChain(cap, err, opts, growth, &e);
    char key[32];
    for (int i = 0; i < items; i++) {
        int n = snprintf(key, sizeof key, "item%d", i);
        SBChain_Add(sb, key, n);
    }
    return sb;
}

static void le64(uint8_t *p, uint64_t v) { memcpy(p, &v, 8); }

static void bf_seeds(void) {
    struct { const char *name; uint64_t cap; double err; unsigned opts, growth; int items; } c[] = {
        {"bf_rdb_small", 8, 0.1, 0, 2, 4},
        {"bf_rdb_scaled", 4, 0.05, 0, 2, 40},
        {"bf_rdb_noscaling", 16, 0.01, 4, 0, 8},
    };
    for (size_t i = 0; i < sizeof c / sizeof *c; i++) {
        SBChain *sb = make_bf(c[i].cap, c[i].err, c[i].opts, c[i].growth, c[i].items);
        RedisModuleIO io;
        mock_io_writer(&io);
        mh_BFRdbSave(&io, sb);
        emit_io(c[i].name, 4, 1, &io);

        // LOADCHUNK stream: header then chunks.
        size_t hl;
        char *hdr = SBChain_GetEncodedHeader(sb, &hl);
        uint8_t *buf = malloc(1 + hl + (1 << 16));
        size_t n = 0;
        buf[n++] = (uint8_t)hl;
        memcpy(buf + n, hdr, hl);
        n += hl;
        long long iter = SB_CHUNKITER_INIT;
        size_t len;
        const char *chunk;
        while ((chunk = SBChain_GetEncodedChunk(sb, &iter, &len, 64)) != NULL) {
            le64(buf + n, iter);
            uint16_t l16 = (uint16_t)len;
            memcpy(buf + n + 8, &l16, 2);
            memcpy(buf + n + 10, chunk, len);
            n += 10 + len;
        }
        char name[64];
        snprintf(name, sizeof name, "bf_loadchunk_%s", c[i].name + 7);
        emit(name, NULL, 0, buf, n);
        free(buf);
        SB_FreeEncodedHeader(hdr);
        SBChain_Free(sb);
    }
}

static void cf_seeds(void) {
    struct { const char *name; uint64_t cap; uint16_t bsz, iters, exp; int items; } c[] = {
        {"cf_rdb_small", 16, 2, 20, 1, 6},
        {"cf_rdb_expanding", 8, 2, 20, 2, 60},
    };
    for (size_t i = 0; i < sizeof c / sizeof *c; i++) {
        CuckooFilter *cf = RedisModule_Calloc(1, sizeof *cf);
        CuckooFilter_Init(cf, c[i].cap, c[i].bsz, c[i].iters, c[i].exp);
        for (int k = 0; k < c[i].items; k++) CuckooFilter_Insert(cf, (CuckooHash)(k + 1) * 0x9e3779b97f4a7c15ULL);
        RedisModuleIO io;
        mock_io_writer(&io);
        mh_CFRdbSave(&io, cf);
        emit_io(c[i].name, 4, 1, &io);

        CFHeader hdr = fillCFHeader(cf);
        uint8_t *buf = malloc(sizeof hdr + (1 << 16));
        size_t n = 0;
        memcpy(buf, &hdr, sizeof hdr);
        n += sizeof hdr;
        long long pos = 1;
        size_t len;
        const char *chunk;
        while ((chunk = CF_GetEncodedChunk(cf, &pos, &len, 64)) != NULL) {
            le64(buf + n, (uint64_t)pos);
            uint16_t l16 = (uint16_t)len;
            memcpy(buf + n + 8, &l16, 2);
            memcpy(buf + n + 10, chunk, len);
            n += 10 + len;
        }
        char name[64];
        snprintf(name, sizeof name, "cf_loadchunk_%s", c[i].name + 7);
        emit(name, NULL, 0, buf, n);
        free(buf);
        mh_CFFree(cf);
    }
}

static void cms_seeds(void) {
    int cells[] = {4, 8};
    for (int i = 0; i < 2; i++) {
        CMSketch *cms = NewCMSketch(16, 4, cells[i]);
        uint64_t cnt;
        CMS_IncrBy(cms, "a", 1, 5, &cnt);
        CMS_IncrBy(cms, "bb", 2, 11, &cnt);
        RedisModuleIO io;
        mock_io_writer(&io);
        CMSRdbSave(&io, cms);
        char name[32];
        snprintf(name, sizeof name, "cms_rdb_cell%d", cells[i]);
        emit_io(name, 1, 1, &io);
        CMS_Destroy(cms);
    }
}

static void topk_seed(void) {
    TopK *t = TopK_Create(4, 16, 3, 0.9);
    uint64_t rng = 1;
    const char *items[] = {"x", "y", "x", "zz", "x", "y", "w", "v", "x"};
    for (size_t i = 0; i < sizeof items / sizeof *items; i++) {
        char *ex = TopK_Add(t, items[i], strlen(items[i]), 1, &rng);
        if (ex) RedisModule_Free(ex);
    }
    RedisModuleIO io;
    mock_io_writer(&io);
    mh_TopKRdbSave(&io, t);
    emit_io("topk_rdb_small", 0, 0, &io);
    TopK_Destroy(t);
}

static void tdigest_seed(void) {
    for (int big = 0; big < 2; big++) {
        td_histogram_t *td = td_new(big ? 100 : 10);
        for (int i = 1; i <= (big ? 500 : 12); i++) td_add(td, i * 1.5, 1 + i % 3);
        RedisModuleIO io;
        mock_io_writer(&io);
        TDigestRdbSave(&io, td);
        emit_io(big ? "tdigest_rdb_large" : "tdigest_rdb_small", 0, 0, &io);
        td_free(td);
    }
}

int main(int argc, char **argv) {
    if (argc != 2) { fprintf(stderr, "usage: %s <outdir>\n", argv[0]); return 2; }
    outdir = argv[1];
    mock_init();
    bf_seeds();
    cf_seeds();
    cms_seeds();
    topk_seed();
    tdigest_seed();
    return 0;
}
