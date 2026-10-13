#include <cstddef>
#include <cstdint>
#include <new>

#include "JPEGDEC.h"

// The decoder is ~18 KB: callers hand it memory (a workspace) rather than
// take it out of a task's stack.
static_assert(alignof(JPEGDEC) <= 8, "the workspace is 8-aligned");

namespace {

// Builds a decoder in `workspace`, and destroys it at the end of the scope.
class WorkspaceDecoder {
 public:
  explicit WorkspaceDecoder(void *workspace) : decoder_(new (workspace) JPEGDEC()) {}
  ~WorkspaceDecoder() {
    decoder_->close();
    decoder_->~JPEGDEC();
  }
  WorkspaceDecoder(const WorkspaceDecoder &) = delete;
  WorkspaceDecoder &operator=(const WorkspaceDecoder &) = delete;

  JPEGDEC *operator->() const { return decoder_; }

 private:
  JPEGDEC *decoder_;
};

}  // namespace

extern "C" {

size_t jpegdec_workspace_size() { return sizeof(JPEGDEC); }

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

int jpegdec_get_dimensions(void *workspace, const uint8_t *data, int size, int *width,
                           int *height) {
    WorkspaceDecoder decoder(workspace);
    if (decoder->openRAM(const_cast<uint8_t *>(data), size, nullptr) != 1) {
        return 0;
    }
    *width = decoder->getWidth();
    *height = decoder->getHeight();
    return 1;
}

int jpegdec_decode_rgb8(void *workspace, const uint8_t *data, int size, uint8_t *output,
                        int *width, int *height) {
    WorkspaceDecoder decoder(workspace);
    if (decoder->openRAM(const_cast<uint8_t *>(data), size, jpeg_draw_callback) != 1) {
        return 0;
    }

    decoder->setPixelType(RGB8888);

    JpegDecodeContext ctx;
    ctx.output = output;
    ctx.width = decoder->getWidth();
    decoder->setUserPointer(&ctx);

    *width = ctx.width;
    *height = decoder->getHeight();

    return decoder->decode(0, 0, 0);
}

} // extern "C"
