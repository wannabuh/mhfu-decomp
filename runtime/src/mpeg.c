/* sceMpeg: the game's movies (PSMF: H.264 video and ATRAC3+ audio). Not played yet - every stream reports that it has
 * no more data, so a movie ends at once and the game goes on. TODO: decode them (FFmpeg). */
#include "hle.h"

#define ERROR_MPEG_NO_DATA 0x80618001u

void hle_sceMpegInit(Ctx *c) { RET(0); }
void hle_sceMpegFinish(Ctx *c) { RET(0); }
void hle_sceMpegQueryMemSize(Ctx *c) { RET(0x10000); }

/* (packets) -> bytes: each 2048-byte packet and its 104 bytes of bookkeeping */
void hle_sceMpegRingbufferQueryMemSize(Ctx *c) { RET(ARG0 * (2048 + 104)); }
void hle_sceMpegRingbufferConstruct(Ctx *c) { RET(0); }
void hle_sceMpegRingbufferDestruct(Ctx *c) { RET(0); }
void hle_sceMpegRingbufferAvailableSize(Ctx *c) { RET(0); }
void hle_sceMpegRingbufferPut(Ctx *c) { RET(0); }

/* (handle out, data, size, ringbuffer, frame width, mode, ddr top): the handle is the data block */
void hle_sceMpegCreate(Ctx *c) {
    if (ARG0)
        SW(ARG0, ARG1);
    RET(0);
}

void hle_sceMpegDelete(Ctx *c) { RET(0); }
void hle_sceMpegRegistStream(Ctx *c) { RET(0x40); }         /* a stream id */
void hle_sceMpegUnRegistStream(Ctx *c) { RET(0); }
void hle_sceMpegMallocAvcEsBuf(Ctx *c) { RET(1); }
void hle_sceMpegFreeAvcEsBuf(Ctx *c) { RET(0); }
void hle_sceMpegInitAu(Ctx *c) { RET(0); }
void hle_sceMpegQueryAtracEsSize(Ctx *c) {
    if (ARG1) SW(ARG1, 2112);
    if (ARG2) SW(ARG2, 8192);
    RET(0);
}
void hle_sceMpegQueryStreamOffset(Ctx *c) {
    if (ARG2) SW(ARG2, 2048);
    RET(0);
}
void hle_sceMpegQueryStreamSize(Ctx *c) {
    if (ARG1) SW(ARG1, 0);
    RET(0);
}
void hle_sceMpegFlushAllStream(Ctx *c) { RET(0); }
void hle_sceMpegGetAvcAu(Ctx *c) { RET(ERROR_MPEG_NO_DATA); }
void hle_sceMpegGetAtracAu(Ctx *c) { RET(ERROR_MPEG_NO_DATA); }
void hle_sceMpegAvcDecode(Ctx *c) { RET(0); }
void hle_sceMpegAvcDecodeStop(Ctx *c) { RET(0); }
void hle_sceMpegAtracDecode(Ctx *c) { RET(0); }
