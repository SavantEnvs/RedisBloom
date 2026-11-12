// Known-answer probes of RedisBloom's data structures, linked against the normally-built
// redisbloom.so. One `KAT(` line per probe: it prints "KAT PASS|FAIL <name>"; mayhem/test.sh
// counts those lines against the number of probes in this file.
#include <math.h>
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
void *CMSRdbLoad(RedisModuleIO *io, int encver);
void TDigestRdbSave(RedisModuleIO *io, void *obj);
void *TDigestRdbLoad(RedisModuleIO *io, int encver);

static int failures;
static void kat(const char *name, int ok, const char *detail) {
    printf("KAT %s %s%s%s\n", ok ? "PASS" : "FAIL", name, ok ? "" : " : ", ok ? "" : detail);
    if (!ok) failures++;
}
#define KAT(name, cond) kat(name, (cond), #cond)
#define KAT_NEAR(name, got, want, tol) kat(name, fabs((double)(got) - (want)) <= (tol), #got " != " #want)

static void probe_bloom(void) {
    bloom_hashval h = bloom_calc_hash("hello", 5);
    KAT("bloom-hash-hello-a", h.a == 0x7f1ddbbdULL);
    KAT("bloom-hash-hello-b", h.b == 0xed999d2dULL);

    int err = 0;
    SBChain *sb = SB_NewChain(100, 0.01, 0, 2, &err);
    KAT("sb-created", sb != NULL && sb->nfilters == 1 && sb->size == 0);
    KAT("sb-hashes-for-1pct", sb->filters[0].inner.hashes == 8);
    KAT("sb-power-of-two-sizing", sb->filters[0].inner.bits == 2048 && sb->filters[0].inner.n2 == 11 &&
                                   sb->filters[0].inner.entries == 185);
    KAT("sb-add-new", SBChain_Add(sb, "alpha", 5) == 1);
    KAT("sb-add-dup", SBChain_Add(sb, "alpha", 5) == 0);
    KAT("sb-check-present", SBChain_Check(sb, "alpha", 5) != 0);
    KAT("sb-check-absent", SBChain_Check(sb, "omega", 5) == 0);
    KAT("sb-size-counts-unique", sb->size == 1);
    SBChain_Free(sb);

    sb = SB_NewChain(10, 0.01, 0, 2, &err);
    char key[16];
    for (int i = 0; i < 100; i++) SBChain_Add(sb, key, (size_t)snprintf(key, sizeof key, "k%d", i));
    KAT("sb-scaled-links", sb->nfilters == 3);
    KAT("sb-scaled-size", sb->size == 99);
    int all = 1;
    for (int i = 0; i < 100; i++) all &= SBChain_Check(sb, key, (size_t)snprintf(key, sizeof key, "k%d", i)) != 0;
    KAT("sb-scaled-no-false-negative", all);

    size_t hl;
    char *hdr = SBChain_GetEncodedHeader(sb, &hl);
    const char *msg = NULL;
    SBChain *copy = SB_NewChainFromHeader(hdr, hl, &msg);
    KAT("sb-header-roundtrip", copy && copy->nfilters == sb->nfilters && copy->size == sb->size);
    long long iter = SB_CHUNKITER_INIT;
    size_t len;
    const char *chunk;
    int rc = 0;
    while ((chunk = SBChain_GetEncodedChunk(sb, &iter, &len, 32)) != NULL)
        rc |= SBChain_LoadEncodedChunk(copy, iter, chunk, len, &msg);
    KAT("sb-chunks-load", rc == 0);
    all = 1;
    for (int i = 0; i < 100; i++) all &= SBChain_Check(copy, key, (size_t)snprintf(key, sizeof key, "k%d", i)) != 0;
    KAT("sb-roundtrip-keeps-items", all);
    KAT("sb-bad-header-rejected", SB_NewChainFromHeader(hdr, hl - 1, &msg) == NULL);
    SB_FreeEncodedHeader(hdr);
    SBChain_Free(copy);
    SBChain_Free(sb);
}

static void probe_cuckoo(void) {
    CuckooFilter cf;
    KAT("cf-init", CuckooFilter_Init(&cf, 1024, 2, 20, 1) == 0);
    KAT("cf-shape", cf.numFilters == 1 && cf.bucketSize == 2 && cf.numBuckets == 512);
    int ok = 1;
    for (uint64_t i = 1; i <= 200; i++) ok &= CuckooFilter_Insert(&cf, i * 0x9e3779b97f4a7c15ULL) == CuckooInsert_Inserted;
    KAT("cf-insert-200", ok && cf.numItems == 200);
    ok = 1;
    for (uint64_t i = 1; i <= 200; i++) ok &= CuckooFilter_Check(&cf, i * 0x9e3779b97f4a7c15ULL) != 0;
    KAT("cf-no-false-negative", ok);
    KAT("cf-insert-unique-dup", CuckooFilter_InsertUnique(&cf, 1 * 0x9e3779b97f4a7c15ULL) == CuckooInsert_Exists);
    KAT("cf-delete", CuckooFilter_Delete(&cf, 5 * 0x9e3779b97f4a7c15ULL) == 1);
    KAT("cf-deleted-absent", CuckooFilter_Check(&cf, 5 * 0x9e3779b97f4a7c15ULL) == 0);
    KAT("cf-count", CuckooFilter_Count(&cf, 7 * 0x9e3779b97f4a7c15ULL) == 1);
    KAT("cf-counters", cf.numItems == 199 && cf.numDeletes == 1);
    CFHeader hdr = fillCFHeader(&cf);
    CuckooFilter *copy = CFHeader_Load(&hdr);
    long long pos = 1;
    size_t len;
    const char *chunk;
    int rc = 0;
    while ((chunk = CF_GetEncodedChunk(&cf, &pos, &len, 100)) != NULL)
        rc |= CF_LoadEncodedChunk(copy, pos, chunk, len);
    KAT("cf-chunks-load", copy != NULL && rc == 0);
    KAT("cf-roundtrip-keeps-items", CuckooFilter_Check(copy, 7 * 0x9e3779b97f4a7c15ULL) != 0 &&
                                        CuckooFilter_Check(copy, 5 * 0x9e3779b97f4a7c15ULL) == 0);
    KAT("cf-chunk-out-of-range-rejected", CF_LoadEncodedChunk(copy, 5000000, "xx", 2) != 0);
    CuckooFilter_Free(copy);
    RedisModule_Free(copy);
    CuckooFilter_Free(&cf);
}

static void probe_cms(void) {
    size_t w, d;
    CMS_DimFromProb(0.001, 0.01, &w, &d);
    KAT("cms-dims", w == 2000 && d == 7);
    CMSketch *c = NewCMSketch(100, 5, 4);
    uint64_t n = 0;
    KAT("cms-incr-a", CMS_IncrBy(c, "a", 1, 5, &n) == CMS_STATUS_OK && n == 5);
    KAT("cms-incr-b", CMS_IncrBy(c, "bb", 2, 7, &n) == CMS_STATUS_OK && n == 7);
    KAT("cms-incr-a-again", CMS_IncrBy(c, "a", 1, 3, &n) == CMS_STATUS_OK && n == 8);
    KAT("cms-query", CMS_Query(c, "a", 1) == 8 && CMS_Query(c, "bb", 2) == 7 && CMS_Query(c, "zz", 2) == 0);
    KAT("cms-total", c->counter == 15);
    KAT("cms-underflow-rejected", CMS_IncrBy(c, "a", 1, -100, &n) == CMS_STATUS_UNDERFLOW);
    KAT("cms-underflow-left-untouched", CMS_Query(c, "a", 1) == 8);

    CMSketch *m = NewCMSketch(100, 5, 4);
    const CMSketch *src[] = {c};
    long long wt[] = {2};
    KAT("cms-merge", CMS_Merge(m, 1, src, wt) == 0 && CMS_Query(m, "a", 1) == 16 && m->counter == 30);

    RedisModuleIO io;
    mock_io_writer(&io);
    CMSRdbSave(&io, c);
    RedisModuleIO rd;
    mock_io_reader(&rd, io.out, io.out_len);
    CMSketch *back = CMSRdbLoad(&rd, 1);
    KAT("cms-rdb-roundtrip", back && back->width == 100 && back->depth == 5 && back->counter == 15 &&
                                 CMS_Query(back, "a", 1) == 8 && CMS_Query(back, "bb", 2) == 7);
    mock_io_reader(&rd, io.out, io.out_len - 1);
    KAT("cms-rdb-truncated-rejected", CMSRdbLoad(&rd, 1) == NULL);
    mock_io_release(&io);
    CMS_Destroy(back);
    CMS_Destroy(m);
    CMS_Destroy(c);
}

static void probe_topk(void) {
    TopK *t = TopK_Create(2, 32, 5, 0.9);
    uint64_t rng = 42;
    for (int i = 0; i < 20; i++) free(TopK_Add(t, "hot", 3, 1, &rng));
    for (int i = 0; i < 10; i++) free(TopK_Add(t, "warm", 4, 1, &rng));
    free(TopK_Add(t, "cold", 4, 1, &rng));
    KAT("topk-hot-listed", TopK_Query(t, "hot", 3));
    KAT("topk-warm-listed", TopK_Query(t, "warm", 4));
    KAT("topk-count-hot", TopK_Count(t, "hot", 3) == 20);
    KAT("topk-count-warm", TopK_Count(t, "warm", 4) == 10);
    KAT("topk-never-seen", !TopK_Query(t, "ice", 3));
    HeapBucket *l = TopK_List(t);
    int hot = 0, warm = 0;
    for (uint32_t i = 0; i < t->k; i++) {
        if (l[i].itemlen == 3 && !memcmp(l[i].item, "hot", 3)) hot = l[i].count;
        if (l[i].itemlen == 4 && !memcmp(l[i].item, "warm", 4)) warm = l[i].count;
    }
    KAT("topk-list-counts", hot == 20 && warm == 10);
    TopK_Destroy(t);
}

static void probe_tdigest(void) {
    td_histogram_t *td = td_new(100);
    for (int i = 1; i <= 1000; i++) td_add(td, i, 1);
    KAT("td-size", td_size(td) == 1000);
    KAT("td-min-max", td_min(td) == 1.0 && td_max(td) == 1000.0);
    KAT_NEAR("td-median", td_quantile(td, 0.5), 500.0, 5.0);
    KAT_NEAR("td-p90", td_quantile(td, 0.9), 900.0, 9.0);
    KAT_NEAR("td-cdf-mid", td_cdf(td, 500.0), 0.5, 0.01);
    KAT("td-quantile-extremes", td_quantile(td, 0.0) == 1.0 && td_quantile(td, 1.0) == 1000.0);
    KAT_NEAR("td-trimmed-mean", td_trimmed_mean(td, 0.1, 0.9), 500.5, 5.0);

    td_histogram_t *other = td_new(100);
    for (int i = 1001; i <= 2000; i++) td_add(other, i, 1);
    KAT("td-merge", td_merge(td, other) == 0 && td_size(td) == 2000 && td_max(td) == 2000.0);
    KAT_NEAR("td-merged-median", td_quantile(td, 0.5), 1000.0, 10.0);

    RedisModuleIO io;
    mock_io_writer(&io);
    TDigestRdbSave(&io, td);
    RedisModuleIO rd;
    mock_io_reader(&rd, io.out, io.out_len);
    td_histogram_t *back = TDigestRdbLoad(&rd, 0);
    KAT("td-rdb-roundtrip", back && td_size(back) == 2000 && td_min(back) == 1.0 &&
                                td_quantile(back, 0.5) == td_quantile(td, 0.5));
    mock_io_reader(&rd, io.out, io.out_len / 2);
    KAT("td-rdb-truncated-rejected", TDigestRdbLoad(&rd, 0) == NULL);
    mock_io_release(&io);
    td_free(back);
    td_free(other);
    td_free(td);
}

int main(void) {
    mock_init();
    probe_bloom();
    probe_cuckoo();
    probe_cms();
    probe_topk();
    probe_tdigest();
    return failures ? 1 : 0;
}
