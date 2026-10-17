#include "video/cover_art.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <vector>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
}
void require(bool value, const char* text) { if (!value) throw std::runtime_error(text); }
std::vector<uint8_t> jpeg_fixture(int w, int h, AVPixelFormat format) {
    auto* context = avcodec_alloc_context3(avcodec_find_encoder(AV_CODEC_ID_MJPEG));
    require(context != nullptr, "MJPEG encoder");
    context->width = w; context->height = h; context->pix_fmt = format;
    context->time_base = {1, 1}; context->color_range = AVCOL_RANGE_JPEG;
    require(avcodec_open2(context, context->codec, nullptr) >= 0, "MJPEG encoder init");
    auto* frame = av_frame_alloc(); frame->width = w; frame->height = h; frame->format = format;
    require(av_frame_get_buffer(frame, 32) >= 0, "JPEG fixture pixels");
    for (int y = 0; y < h; ++y) std::memset(frame->data[0] + y * frame->linesize[0], 80, w);
    const int chroma_w = format == AV_PIX_FMT_YUVJ444P ? w : (w + 1) / 2;
    const int chroma_h = format == AV_PIX_FMT_YUVJ420P ? (h + 1) / 2 : h;
    for (int c = 1; c < 3; ++c)
        for (int y = 0; y < chroma_h; ++y)
            std::memset(frame->data[c] + y * frame->linesize[c], 128, chroma_w);
    require(avcodec_send_frame(context, frame) == 0, "JPEG fixture encode");
    auto* packet = av_packet_alloc();
    require(avcodec_receive_packet(context, packet) == 0, "JPEG encoded bytes");
    std::vector<uint8_t> result(packet->data, packet->data + packet->size);
    av_packet_free(&packet); av_frame_free(&frame); avcodec_free_context(&context);
    return result;
}

// Isolate the old tightly-packed destination inside a larger guarded allocation.
// This can prove writes beyond w*h*3 without corrupting the process heap.
int probe_old_destination(const std::vector<uint8_t>& jpeg) {
    auto* context = avcodec_alloc_context3(avcodec_find_decoder(AV_CODEC_ID_MJPEG));
    auto* packet = av_packet_alloc(); auto* frame = av_frame_alloc();
    require(avcodec_open2(context, context->codec, nullptr) == 0, "Probe decoder init");
    require(av_new_packet(packet, static_cast<int>(jpeg.size())) == 0, "Probe packet");
    std::memcpy(packet->data, jpeg.data(), jpeg.size());
    require(avcodec_send_packet(context, packet) == 0 && avcodec_receive_frame(context, frame) == 0, "Probe JPEG decode");
    const auto logical = static_cast<std::size_t>(frame->width) * frame->height * 3;
    std::vector<uint8_t> guarded(logical + 256, 0xa5);
    uint8_t* planes[4] = {guarded.data(), nullptr, nullptr, nullptr};
    int strides[4] = {frame->width * 3, 0, 0, 0};
    auto* scaler = sws_getContext(frame->width, frame->height, static_cast<AVPixelFormat>(frame->format),
        frame->width, frame->height, AV_PIX_FMT_RGB24, SWS_BILINEAR, nullptr, nullptr, nullptr);
    require(scaler != nullptr, "Probe converter");
    require(sws_scale(scaler, frame->data, frame->linesize, 0, frame->height, planes, strides) == frame->height, "Probe conversion");
    const auto overwritten = std::count_if(guarded.begin() + logical, guarded.end(), [](uint8_t b) { return b != 0xa5; });
    std::printf("OLD BUFFER PROBE %dx%d: %lld byte(s) written beyond packed RGB allocation\n",
        frame->width, frame->height, static_cast<long long>(overwritten));
    sws_freeContext(scaler); av_frame_free(&frame); av_packet_free(&packet); avcodec_free_context(&context);
    return static_cast<int>(overwritten);
}

int main(int argc, char** argv) {
    av_log_set_level(AV_LOG_ERROR);
    try {
        for (const auto format : {AV_PIX_FMT_YUVJ420P, AV_PIX_FMT_YUVJ422P, AV_PIX_FMT_YUVJ444P}) {
            for (const auto dimensions : {std::pair<int,int>{600,600}, {601,433}, {17,19}, {1,1}}) {
                const auto jpeg = jpeg_fixture(dimensions.first, dimensions.second, format);
                if (format == AV_PIX_FMT_YUVJ420P) probe_old_destination(jpeg);
                for (int i = 0; i < 50; ++i) {
                    int w = 0, h = 0; std::vector<uint8_t> rgb;
                    require(ap::video::decode_cover_art(jpeg.data(), jpeg.size(), w, h, rgb), "Safe JPEG decode");
                    require(w == dimensions.first && h == dimensions.second && rgb.size() == static_cast<std::size_t>(w) * h * 3, "Packed dimensions");
                    require(std::all_of(rgb.begin(), rgb.end(), [](uint8_t v) { return v >= 77 && v <= 83; }), "All pixels and row boundaries preserved");
                }
            }
        }
        int w = 1, h = 1; std::vector<uint8_t> rgb(100);
        const uint8_t invalid[] = {0xff, 0xd8, 0xff};
        require(!ap::video::decode_cover_art(invalid, sizeof(invalid), w, h, rgb), "Reject truncated JPEG");
        require(w == 0 && h == 0 && rgb.empty(), "Clear output after decode failure");
        require(!ap::video::decode_cover_art(invalid, 17 * 1024 * 1024, w, h, rgb), "Reject oversized compressed input before reading");
        if (argc > 1) {
            std::ifstream input(argv[1], std::ios::binary);
            std::vector<uint8_t> jpeg{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
            require(!jpeg.empty(), "Received cover fixture");
            probe_old_destination(jpeg);
            for (int i = 0; i < 500; ++i)
                require(ap::video::decode_cover_art(jpeg.data(), jpeg.size(), w, h, rgb), "Repeated received-cover decode");
            std::printf("PASS: 500 repeated conversions of received %dx%d cover\n", w, h);
        }
        std::puts("PASS: 600 JPEG conversions, odd/tiny sizes, chroma formats, pixel integrity and malformed input");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what()); return 1;
    }
}
