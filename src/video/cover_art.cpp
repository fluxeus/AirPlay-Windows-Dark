#include "video/cover_art.h"
#include <cstring>
#include <climits>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
}

namespace ap::video {
bool decode_cover_art(const uint8_t* data, std::size_t size,
                      int& width, int& height, std::vector<uint8_t>& rgb) {
    width = height = 0;
    rgb.clear();
    if (!data || !size || size > 16 * 1024 * 1024 || size > INT_MAX) return false;
    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_MJPEG);
    if (!codec) return false;
    AVCodecContext* context = avcodec_alloc_context3(codec);
    AVPacket* packet = av_packet_alloc();
    AVFrame* source = av_frame_alloc();
    AVFrame* converted = av_frame_alloc();
    SwsContext* scaler = nullptr;
    bool ok = context && packet && source && converted;
    if (ok) {
        // Limit allocations before decoding an untrusted compressed image.
        context->max_pixels = 4096LL * 4096;
        ok = avcodec_open2(context, codec, nullptr) == 0
          && av_new_packet(packet, static_cast<int>(size)) == 0;
    }
    if (ok) {
        // av_new_packet supplies AV_INPUT_BUFFER_PADDING_SIZE zero bytes.
        std::memcpy(packet->data, data, size);
        ok = avcodec_send_packet(context, packet) == 0
          && avcodec_receive_frame(context, source) == 0;
    }
    if (ok) {
        ok = source->width > 0 && source->height > 0 &&
             source->width <= 4096 && source->height <= 4096;
    }
    if (ok) {
        converted->format = AV_PIX_FMT_RGB24;
        converted->width = source->width;
        converted->height = source->height;
        // FFmpeg SIMD paths require aligned strides and padding at plane ends.
        ok = av_frame_get_buffer(converted, 32) == 0;
    }
    if (ok) {
        scaler = sws_getContext(source->width, source->height,
            static_cast<AVPixelFormat>(source->format), source->width,
            source->height, AV_PIX_FMT_RGB24, SWS_BILINEAR, nullptr, nullptr, nullptr);
        ok = scaler && sws_scale(scaler, source->data, source->linesize, 0,
            source->height, converted->data, converted->linesize) == source->height;
    }
    if (ok) {
        const auto row = static_cast<std::size_t>(source->width) * 3;
        rgb.resize(row * source->height);
        for (int y = 0; y < source->height; ++y)
            std::memcpy(rgb.data() + y * row,
                        converted->data[0] + y * converted->linesize[0], row);
        width = source->width;
        height = source->height;
    }
    sws_freeContext(scaler);
    av_frame_free(&converted);
    av_frame_free(&source);
    av_packet_free(&packet);
    avcodec_free_context(&context);
    return ok;
}
}
