// CF.LOADCHUNK path: CFHeader decode, then a stream of SCANDUMP chunks into the filter.
// Input: [CFHeader, 38 bytes][chunks...], chunk = [le64 pos][le16 len][len bytes].
#include <stdlib.h>
#include <string.h>
#include "mock.h"
#include "cf.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size < sizeof(CFHeader)) return -1;
    mock_init();

    CFHeader *hdr = malloc(sizeof *hdr);
    memcpy(hdr, data, sizeof *hdr);
    CuckooFilter *cf = CFHeader_Load(hdr);
    free(hdr);
    data += sizeof(CFHeader), size -= sizeof(CFHeader);
    if (!cf) return 0;

    while (size >= 10) {
        long long pos;
        uint16_t len;
        memcpy(&pos, data, 8);
        memcpy(&len, data + 8, 2);
        data += 10, size -= 10;
        if (len > size) len = (uint16_t)size;
        char *chunk = malloc(len ? len : 1);
        memcpy(chunk, data, len);
        CF_LoadEncodedChunk(cf, pos, chunk, len);
        free(chunk);
        data += len, size -= len;
    }
    CuckooFilter_Free(cf);
    RedisModule_Free(cf);
    return 0;
}
