// BF.LOADCHUNK path: SBChain header decode, then a stream of SCANDUMP chunks into it.
// Input: [u8 header_len][header][chunks...], chunk = [le64 iter][le16 len][len bytes].
#include <stdlib.h>
#include <string.h>
#include "mock.h"
#include "sb.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size < 1) return -1;
    size_t hdr_len = data[0];
    data++, size--;
    if (hdr_len > size) return -1;
    mock_init();

    char *hdr = malloc(hdr_len ? hdr_len : 1);
    memcpy(hdr, data, hdr_len);
    const char *errmsg = NULL;
    SBChain *sb = SB_NewChainFromHeader(hdr, hdr_len, &errmsg);
    free(hdr);
    data += hdr_len, size -= hdr_len;
    if (!sb) return 0;

    while (size >= 10) {
        long long iter;
        uint16_t len;
        memcpy(&iter, data, 8);
        memcpy(&len, data + 8, 2);
        data += 10, size -= 10;
        if (len > size) len = (uint16_t)size;
        char *chunk = malloc(len ? len : 1);
        memcpy(chunk, data, len);
        SBChain_LoadEncodedChunk(sb, iter, chunk, len, &errmsg);
        free(chunk);
        data += len, size -= len;
    }
    SBChain_Free(sb);
    return 0;
}
