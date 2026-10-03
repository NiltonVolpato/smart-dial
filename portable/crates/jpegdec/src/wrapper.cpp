#include <cstdint>
#include "JPEGDEC.h"

extern "C" {

struct JpegDecodeContext {
    uint8_t *output;
    int width;
};

static int jpeg_draw_callback(JPEGDRAW *pDraw) {
    auto *ctx = static_cast<JpegDecodeContext *>(pDraw->pUser);
    if (ctx == nullptr || ctx->output == nullptr) {
        return 1;
    }

    const auto *src = static_cast<const uint32_t *>(static_cast<const void *>(pDraw->pPixels));
    const int src_pitch = pDraw->iWidth; // pixels per source row
    const int dst_pitch = ctx->width;

    for (int row = 0; row < pDraw->iHeight; ++row) {
        const int y = pDraw->y + row;
        if (y < 0) {
            continue;
        }

        uint8_t *dst_row = ctx->output + (y * dst_pitch + pDraw->x) * 3;
        for (int col = 0; col < pDraw->iWidthUsed; ++col) {
            const uint32_t rgba = src[row * src_pitch + col];
            dst_row[col * 3 + 0] = static_cast<uint8_t>(rgba & 0xFF);         // R
            dst_row[col * 3 + 1] = static_cast<uint8_t>((rgba >> 8) & 0xFF);  // G
            dst_row[col * 3 + 2] = static_cast<uint8_t>((rgba >> 16) & 0xFF); // B
        }
    }
    return 1;
}

int jpegdec_get_dimensions(const uint8_t *data, int size, int *width, int *height) {
    JPEGDEC decoder;
    if (decoder.openFLASH(const_cast<uint8_t *>(data), size, nullptr) != 1) {
        return 0;
    }
    *width = decoder.getWidth();
    *height = decoder.getHeight();
    decoder.close();
    return 1;
}

int jpegdec_decode_rgb8(const uint8_t *data, int size, uint8_t *output, int *width, int *height) {
    JPEGDEC decoder;
    if (decoder.openFLASH(const_cast<uint8_t *>(data), size, jpeg_draw_callback) != 1) {
        return 0;
    }

    decoder.setPixelType(RGB8888);

    JpegDecodeContext ctx;
    ctx.output = output;
    ctx.width = decoder.getWidth();
    decoder.setUserPointer(&ctx);

    *width = ctx.width;
    *height = decoder.getHeight();

    const int rc = decoder.decode(0, 0, 0);
    decoder.close();
    return rc;
}

} // extern "C"
