// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/assert.h"
#include "core/libraries/ajm/ajm_mp3.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
}

namespace Libraries::Ajm {

// Following tables have been reversed from AJM library
static constexpr std::array<std::array<s32, 3>, 3> SamplerateTable = {{
    {0x5622, 0x5DC0, 0x3E80},
    {0xAC44, 0xBB80, 0x7D00},
    {0x2B11, 0x2EE0, 0x1F40},
}};

static constexpr std::array<std::array<s32, 15>, 2> BitrateTable = {{
    {0, 0x20, 0x28, 0x30, 0x38, 0x40, 0x50, 0x60, 0x70, 0x80, 0xA0, 0xC0, 0xE0, 0x100, 0x140},
    {0, 0x8, 0x10, 0x18, 0x20, 0x28, 0x30, 0x38, 0x40, 0x50, 0x60, 0x70, 0x80, 0x90, 0xA0},
}};

static constexpr std::array<s32, 2> UnkTable = {0x48, 0x90};

SwrContext* swr_context{};

AVFrame* ConvertAudioFrame(AVFrame* frame) {
    auto pcm16_frame = av_frame_clone(frame);
    pcm16_frame->format = AV_SAMPLE_FMT_S16;

    if (swr_context) {
        swr_free(&swr_context);
        swr_context = nullptr;
    }
    AVChannelLayout in_ch_layout = frame->ch_layout;
    AVChannelLayout out_ch_layout = pcm16_frame->ch_layout;
    swr_alloc_set_opts2(&swr_context, &out_ch_layout, AV_SAMPLE_FMT_S16, frame->sample_rate,
                        &in_ch_layout, AVSampleFormat(frame->format), frame->sample_rate, 0,
                        nullptr);
    swr_init(swr_context);
    const auto res = swr_convert_frame(swr_context, pcm16_frame, frame);
    if (res < 0) {
        LOG_ERROR(Lib_AvPlayer, "Could not convert to S16: {}", av_err2str(res));
        return nullptr;
    }
    av_frame_free(&frame);
    return pcm16_frame;
}

AjmMp3Decoder::AjmMp3Decoder() {
    codec = avcodec_find_decoder(AV_CODEC_ID_MP3);
    ASSERT_MSG(codec, "MP3 codec not found");
    parser = av_parser_init(codec->id);
    ASSERT_MSG(parser, "Parser not found");
    AjmMp3Decoder::Reset();
}

AjmMp3Decoder::~AjmMp3Decoder() {
    avcodec_free_context(&c);
    av_free(c);
}

void AjmMp3Decoder::Reset() {
    if (c) {
        avcodec_free_context(&c);
        av_free(c);
    }
    c = avcodec_alloc_context3(codec);
    ASSERT_MSG(c, "Could not allocate audio codec context");
    int ret = avcodec_open2(c, codec, nullptr);
    ASSERT_MSG(ret >= 0, "Could not open codec");
    decoded_samples = 0;
    num_frames = 0;
}

std::tuple<u32, u32> AjmMp3Decoder::Decode(const u8* buf, u32 in_size, u8* out_buf, u32 out_size,
                                           AjmJobOutput* output) {
    AVPacket* pkt = av_packet_alloc();
    while (in_size > 0 && out_size > 0) {
        int ret = av_parser_parse2(parser, c, &pkt->data, &pkt->size, buf, in_size, AV_NOPTS_VALUE,
                                   AV_NOPTS_VALUE, 0);
        ASSERT_MSG(ret >= 0, "Error while parsing {}", ret);
        buf += ret;
        in_size -= ret;

        if (pkt->size) {
            // Send the packet with the compressed data to the decoder
            pkt->pts = parser->pts;
            pkt->dts = parser->dts;
            pkt->flags = (parser->key_frame == 1) ? AV_PKT_FLAG_KEY : 0;
            ret = avcodec_send_packet(c, pkt);
            ASSERT_MSG(ret >= 0, "Error submitting the packet to the decoder {}", ret);

            // Read all the output frames (in general there may be any number of them
            while (ret >= 0) {
                AVFrame* frame = av_frame_alloc();
                ret = avcodec_receive_frame(c, frame);
                if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                    break;
                } else if (ret < 0) {
                    UNREACHABLE_MSG("Error during decoding");
                }
                if (frame->format != AV_SAMPLE_FMT_S16) {
                    frame = ConvertAudioFrame(frame);
                }
                const auto size = frame->ch_layout.nb_channels * frame->nb_samples * sizeof(u16);
                std::memcpy(out_buf, frame->data[0], size);
                file.write((const char*)frame->data[0], size);
                out_buf += size;
                out_size -= size;
                decoded_samples += frame->nb_samples;
                num_frames++;
                av_frame_free(&frame);
            }
        }
    }
    av_packet_free(&pkt);
    if (output->p_mframe) {
        output->p_mframe->num_frames += num_frames;
    }
    return std::make_tuple(in_size, out_size);
}

int AjmMp3Decoder::ParseMp3Header(const u8* buf, u32 stream_size, int parse_ofl,
                                  AjmDecMp3ParseFrame* frame) {
    const u32 unk_idx = buf[1] >> 3 & 1;
    const s32 version_idx = (buf[1] >> 3 & 3) ^ 2;
    const s32 sr_idx = buf[2] >> 2 & 3;
    const s32 br_idx = (buf[2] >> 4) & 0xf;
    const s32 padding_bit = (buf[2] >> 1) & 0x1;

    frame->sample_rate = SamplerateTable[version_idx][sr_idx];
    frame->bitrate = BitrateTable[version_idx != 1][br_idx] * 1000;
    frame->num_channels = (buf[3] < 0xc0) + 1;
    frame->frame_size = (UnkTable[unk_idx] * frame->bitrate) / frame->sample_rate + padding_bit;
    frame->samples_per_channel = UnkTable[unk_idx] * 8;
    frame->encoder_delay = 0;
    if (parse_ofl == 0) {
        return 0;
    }

    return 0;
}

} // namespace Libraries::Ajm
