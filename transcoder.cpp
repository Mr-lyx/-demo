#include "transcoder.h"
#include "progress.h"
#include <cstdio>
#include <ctime>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/opt.h>
#include <libavutil/imgutils.h>
#include <libavutil/channel_layout.h>
#include <libavutil/samplefmt.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
}

void Transcoder::print_error(int ret, const char* context) {
    char buf[256];
    av_strerror(ret, buf, sizeof(buf));
    fprintf(stderr, "Error: %s: %s\n", context, buf);
}

bool Transcoder::open_input(const std::string& filename) {
    int ret = avformat_open_input(&in_fmt_ctx_, filename.c_str(), nullptr, nullptr);
    if (ret < 0) { print_error(ret, "avformat_open_input"); return false; }

    ret = avformat_find_stream_info(in_fmt_ctx_, nullptr);
    if (ret < 0) { print_error(ret, "avformat_find_stream_info"); return false; }

    av_dump_format(in_fmt_ctx_, 0, filename.c_str(), 0);

    for (unsigned i = 0; i < in_fmt_ctx_->nb_streams; i++) {
        AVCodecParameters* par = in_fmt_ctx_->streams[i]->codecpar;
        if (par->codec_type == AVMEDIA_TYPE_VIDEO && v_stream_idx_ < 0) {
            v_stream_idx_ = i;
        } else if (par->codec_type == AVMEDIA_TYPE_AUDIO && a_stream_idx_ < 0) {
            a_stream_idx_ = i;
        }
    }

    if (v_stream_idx_ < 0 && a_stream_idx_ < 0) {
        fprintf(stderr, "Error: no video or audio stream found\n");
        return false;
    }
    return true;
}

bool Transcoder::init_video_decoder(int stream_index) {
    AVStream* stream = in_fmt_ctx_->streams[stream_index];
    const AVCodec* codec = avcodec_find_decoder(stream->codecpar->codec_id);
    if (!codec) { fprintf(stderr, "Error: video decoder not found\n"); return false; }

    v_dec_ctx_ = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(v_dec_ctx_, stream->codecpar);
    v_dec_ctx_->framerate = av_guess_frame_rate(in_fmt_ctx_, stream, nullptr);

    int ret = avcodec_open2(v_dec_ctx_, codec, nullptr);
    if (ret < 0) { print_error(ret, "avcodec_open2 video decoder"); return false; }

    total_frames_ = stream->nb_frames > 0 ? stream->nb_frames : 0;
    if (total_frames_ <= 0 && stream->duration > 0 && stream->time_base.den > 0) {
        AVRational avg_fps = av_guess_frame_rate(in_fmt_ctx_, stream, nullptr);
        if (avg_fps.num > 0) {
            total_frames_ = (int64_t)(av_q2d(stream->time_base) * stream->duration * av_q2d(avg_fps));
        }
    }
    return true;
}

bool Transcoder::init_audio_decoder(int stream_index) {
    AVStream* stream = in_fmt_ctx_->streams[stream_index];
    const AVCodec* codec = avcodec_find_decoder(stream->codecpar->codec_id);
    if (!codec) { fprintf(stderr, "Error: audio decoder not found\n"); return false; }

    a_dec_ctx_ = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(a_dec_ctx_, stream->codecpar);

    int ret = avcodec_open2(a_dec_ctx_, codec, nullptr);
    if (ret < 0) { print_error(ret, "avcodec_open2 audio decoder"); return false; }
    return true;
}

bool Transcoder::open_output(const std::string& filename, const TranscodeConfig& config) {
    int ret = avformat_alloc_output_context2(&out_fmt_ctx_, nullptr, nullptr, filename.c_str());
    if (ret < 0) { print_error(ret, "avformat_alloc_output_context2"); return false; }

    if (v_stream_idx_ >= 0) {
        if (!init_video_decoder(v_stream_idx_)) return false;
        if (!init_video_encoder(config, v_stream_idx_)) return false;
        AVStream* v_out = avformat_new_stream(out_fmt_ctx_, nullptr);
        avcodec_parameters_from_context(v_out->codecpar, v_enc_ctx_);
        v_out->time_base = v_enc_ctx_->time_base;
        v_out_idx_ = v_out->index;
    }

    if (a_stream_idx_ >= 0 && !config.no_audio) {
        if (!init_audio_decoder(a_stream_idx_)) return false;
        if (!init_audio_encoder(config, a_stream_idx_)) return false;
        AVStream* a_out = avformat_new_stream(out_fmt_ctx_, nullptr);
        avcodec_parameters_from_context(a_out->codecpar, a_enc_ctx_);
        a_out->time_base = a_enc_ctx_->time_base;
        a_out_idx_ = a_out->index;
    }

    if (v_stream_idx_ >= 0) {
        if (!init_video_scaler(v_stream_idx_)) return false;
    }
    if (a_stream_idx_ >= 0 && !config.no_audio) {
        if (!init_audio_resampler(a_stream_idx_)) return false;
    }

    if (!(out_fmt_ctx_->oformat->flags & AVFMT_NOFILE)) {
        ret = avio_open(&out_fmt_ctx_->pb, filename.c_str(), AVIO_FLAG_WRITE);
        if (ret < 0) { print_error(ret, "avio_open"); return false; }
    }

    ret = avformat_write_header(out_fmt_ctx_, nullptr);
    if (ret < 0) { print_error(ret, "avformat_write_header"); return false; }

    fprintf(stderr, "Output format: %s, Video codec: %s",
            out_fmt_ctx_->oformat->long_name ? out_fmt_ctx_->oformat->long_name : out_fmt_ctx_->oformat->name,
            v_enc_ctx_ ? v_enc_ctx_->codec->long_name : "none");
    if (a_enc_ctx_) fprintf(stderr, ", Audio codec: %s", a_enc_ctx_->codec->long_name);
    fprintf(stderr, "\n");
    return true;
}

bool Transcoder::init_video_encoder(const TranscodeConfig& config, int input_stream_index) {
    const AVCodec* codec = avcodec_find_encoder_by_name(config.video_codec.c_str());
    if (!codec) {
        fprintf(stderr, "Error: video encoder '%s' not found\n", config.video_codec.c_str());
        return false;
    }

    v_enc_ctx_ = avcodec_alloc_context3(codec);

    AVStream* in_stream = in_fmt_ctx_->streams[input_stream_index];
    v_enc_ctx_->width = config.width > 0 ? config.width : v_dec_ctx_->width;
    v_enc_ctx_->height = config.height > 0 ? config.height : v_dec_ctx_->height;
    v_enc_ctx_->sample_aspect_ratio = v_dec_ctx_->sample_aspect_ratio;

    v_enc_ctx_->framerate = v_dec_ctx_->framerate;
    if (config.fps > 0) {
        v_enc_ctx_->framerate = { config.fps, 1 };
    }
    // Match stream time base to encoder framerate
    v_enc_ctx_->time_base = av_inv_q(v_enc_ctx_->framerate);

    v_enc_ctx_->pix_fmt = AV_PIX_FMT_YUV420P;
    // Try to find a better match from encoder-supported formats
    const AVPixelFormat* supported = nullptr;
    int nb_fmts = 0;
    if (avcodec_get_supported_config(v_enc_ctx_, codec, AV_CODEC_CONFIG_PIX_FORMAT,
                                      0, (const void**)&supported, &nb_fmts) >= 0 && nb_fmts > 0) {
        v_enc_ctx_->pix_fmt = avcodec_find_best_pix_fmt_of_list(
            supported, v_dec_ctx_->pix_fmt, 1, nullptr);
        if (v_enc_ctx_->pix_fmt == AV_PIX_FMT_NONE) {
            v_enc_ctx_->pix_fmt = supported[0];
        }
    }

    if (config.video_bitrate > 0) {
        v_enc_ctx_->bit_rate = config.video_bitrate;
    }

    if (out_fmt_ctx_->oformat->flags & AVFMT_GLOBALHEADER) {
        v_enc_ctx_->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }

    av_opt_set(v_enc_ctx_->priv_data, "preset", config.preset.c_str(), 0);
    if (config.video_bitrate <= 0) {
        av_opt_set_int(v_enc_ctx_->priv_data, "crf", config.crf, 0);
    }

    int ret = avcodec_open2(v_enc_ctx_, codec, nullptr);
    if (ret < 0) { print_error(ret, "avcodec_open2 video encoder"); return false; }
    return true;
}

bool Transcoder::init_audio_encoder(const TranscodeConfig& config, int input_stream_index) {
    const AVCodec* codec = avcodec_find_encoder(out_fmt_ctx_->oformat->audio_codec);
    if (!codec) {
        fprintf(stderr, "Error: audio encoder not found for output format\n");
        return false;
    }

    a_enc_ctx_ = avcodec_alloc_context3(codec);
    a_enc_ctx_->sample_rate = a_dec_ctx_->sample_rate;
    av_channel_layout_copy(&a_enc_ctx_->ch_layout, &a_dec_ctx_->ch_layout);
    a_enc_ctx_->sample_fmt = AV_SAMPLE_FMT_FLTP;
    const AVSampleFormat* supported_sf = nullptr;
    int nb_sf = 0;
    if (avcodec_get_supported_config(a_enc_ctx_, codec, AV_CODEC_CONFIG_SAMPLE_FORMAT,
                                      0, (const void**)&supported_sf, &nb_sf) >= 0 && nb_sf > 0) {
        a_enc_ctx_->sample_fmt = supported_sf[0];
    }
    a_enc_ctx_->bit_rate = config.audio_bitrate;
    a_enc_ctx_->time_base = { 1, a_enc_ctx_->sample_rate };

    if (out_fmt_ctx_->oformat->flags & AVFMT_GLOBALHEADER) {
        a_enc_ctx_->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }

    int ret = avcodec_open2(a_enc_ctx_, codec, nullptr);
    if (ret < 0) { print_error(ret, "avcodec_open2 audio encoder"); return false; }
    return true;
}

bool Transcoder::init_video_scaler(int input_stream_index) {
    if (v_dec_ctx_->width == v_enc_ctx_->width &&
        v_dec_ctx_->height == v_enc_ctx_->height &&
        v_dec_ctx_->pix_fmt == v_enc_ctx_->pix_fmt) {
        return true; // no scaling needed
    }
    sws_ctx_ = sws_getContext(v_dec_ctx_->width, v_dec_ctx_->height, v_dec_ctx_->pix_fmt,
                               v_enc_ctx_->width, v_enc_ctx_->height, v_enc_ctx_->pix_fmt,
                               SWS_BICUBIC, nullptr, nullptr, nullptr);
    if (!sws_ctx_) { fprintf(stderr, "Error: sws_getContext failed\n"); return false; }
    return true;
}

bool Transcoder::init_audio_resampler(int input_stream_index) {
    int ret = swr_alloc_set_opts2(&swr_ctx_,
        &a_enc_ctx_->ch_layout, a_enc_ctx_->sample_fmt, a_enc_ctx_->sample_rate,
        &a_dec_ctx_->ch_layout, a_dec_ctx_->sample_fmt, a_dec_ctx_->sample_rate,
        0, nullptr);
    if (ret < 0) { print_error(ret, "swr_alloc_set_opts2"); return false; }
    ret = swr_init(swr_ctx_);
    if (ret < 0) { print_error(ret, "swr_init"); return false; }
    return true;
}

bool Transcoder::encode_write_frame(AVFrame* frame, int stream_index) {
    AVCodecContext* enc = (stream_index == v_out_idx_) ? v_enc_ctx_ : a_enc_ctx_;
    AVStream* out_stream = out_fmt_ctx_->streams[stream_index];

    int ret = avcodec_send_frame(enc, frame);
    if (ret < 0) { print_error(ret, "avcodec_send_frame"); return false; }

    AVPacket* pkt = av_packet_alloc();
    while (ret >= 0) {
        ret = avcodec_receive_packet(enc, pkt);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
        if (ret < 0) { print_error(ret, "avcodec_receive_packet"); av_packet_free(&pkt); return false; }

        av_packet_rescale_ts(pkt, enc->time_base, out_stream->time_base);
        pkt->stream_index = stream_index;
        ret = av_interleaved_write_frame(out_fmt_ctx_, pkt);
        if (ret < 0) { print_error(ret, "av_interleaved_write_frame"); av_packet_free(&pkt); return false; }
        av_packet_unref(pkt);
    }
    av_packet_free(&pkt);
    return true;
}

bool Transcoder::flush_encoder(int stream_index) {
    return encode_write_frame(nullptr, stream_index);
}

static time_t g_start_time = 0;

bool Transcoder::process_packet(AVPacket* packet) {
    int stream_index = packet->stream_index;

    if (stream_index == v_stream_idx_ && v_enc_ctx_) {
        // Decode video
        int ret = avcodec_send_packet(v_dec_ctx_, packet);
        if (ret < 0) return true; // skip bad packets

        AVFrame* dec_frame = av_frame_alloc();
        AVFrame* enc_frame = av_frame_alloc();
        enc_frame->format = v_enc_ctx_->pix_fmt;
        enc_frame->width = v_enc_ctx_->width;
        enc_frame->height = v_enc_ctx_->height;
        av_frame_get_buffer(enc_frame, 0);

        while (ret >= 0) {
            ret = avcodec_receive_frame(v_dec_ctx_, dec_frame);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
            if (ret < 0) break;

            if (sws_ctx_) {
                sws_scale(sws_ctx_, dec_frame->data, dec_frame->linesize, 0,
                          dec_frame->height, enc_frame->data, enc_frame->linesize);
            } else {
                av_frame_copy(enc_frame, dec_frame);
                av_frame_copy_props(enc_frame, dec_frame);
            }

            // Rescale PTS from input stream time_base to encoder time_base
            AVRational in_tb = in_fmt_ctx_->streams[v_stream_idx_]->time_base;
            enc_frame->pts = av_rescale_q(dec_frame->pts, in_tb, v_enc_ctx_->time_base);
            enc_frame->pict_type = AV_PICTURE_TYPE_NONE;
            encode_write_frame(enc_frame, v_out_idx_);
            encoded_frames_++;
            print_progress(encoded_frames_, total_frames_, g_start_time);

            av_frame_unref(dec_frame);
            av_frame_unref(enc_frame);
        }
        av_frame_free(&dec_frame);
        av_frame_free(&enc_frame);

    } else if (stream_index == a_stream_idx_ && a_enc_ctx_) {
        // Decode audio
        int ret = avcodec_send_packet(a_dec_ctx_, packet);
        if (ret < 0) return true;

        AVFrame* dec_frame = av_frame_alloc();
        AVFrame* resampled = av_frame_alloc();
        resampled->format = a_enc_ctx_->sample_fmt;
        resampled->sample_rate = a_enc_ctx_->sample_rate;
        av_channel_layout_copy(&resampled->ch_layout, &a_enc_ctx_->ch_layout);

        while (ret >= 0) {
            ret = avcodec_receive_frame(a_dec_ctx_, dec_frame);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
            if (ret < 0) break;

            AVRational a_in_tb = in_fmt_ctx_->streams[a_stream_idx_]->time_base;

            // Resample if needed
            AVFrame* out_frame = dec_frame;
            if (swr_ctx_) {
                int dst_samples = swr_get_out_samples(swr_ctx_, dec_frame->nb_samples);
                resampled->nb_samples = dst_samples;
                av_frame_get_buffer(resampled, 0);

                int converted = swr_convert(swr_ctx_, resampled->data, dst_samples,
                                   (const uint8_t**)dec_frame->data, dec_frame->nb_samples);
                if (converted < 0) break;
                resampled->nb_samples = converted;
                out_frame = resampled;
            }
            out_frame->pts = av_rescale_q(dec_frame->pts, a_in_tb, a_enc_ctx_->time_base);
            encode_write_frame(out_frame, a_out_idx_);
            av_frame_unref(dec_frame);
            av_frame_unref(resampled);
        }
        av_frame_free(&dec_frame);
        av_frame_free(&resampled);
    }
    return true;
}

bool Transcoder::transcode(const TranscodeConfig& config) {
    g_start_time = time(nullptr);

    if (!open_input(config.input_file)) return false;
    if (!open_output(config.output_file, config)) return false;

    // Seek to start if needed
    if (config.start_time >= 0) {
        int64_t seek_ts = (int64_t)(config.start_time * AV_TIME_BASE);
        av_seek_frame(in_fmt_ctx_, -1, seek_ts, AVSEEK_FLAG_BACKWARD);
    }

    fprintf(stderr, "Transcoding: %s -> %s\n", config.input_file.c_str(), config.output_file.c_str());

    AVPacket* pkt = av_packet_alloc();
    while (av_read_frame(in_fmt_ctx_, pkt) >= 0) {
        // Check duration limit
        if (config.duration > 0 && pkt->pts != AV_NOPTS_VALUE) {
            AVStream* stream = in_fmt_ctx_->streams[pkt->stream_index];
            double pts_sec = pkt->pts * av_q2d(stream->time_base);
            if (pts_sec > config.start_time + config.duration + 1.0) {
                av_packet_unref(pkt);
                break;
            }
        }
        process_packet(pkt);
        av_packet_unref(pkt);
    }
    av_packet_free(&pkt);

    // Flush encoders
    if (v_enc_ctx_) flush_encoder(v_out_idx_);
    if (a_enc_ctx_) flush_encoder(a_out_idx_);

    av_write_trailer(out_fmt_ctx_);
    fprintf(stderr, "\nDone.\n");
    cleanup();
    return true;
}

void Transcoder::cleanup() {
    if (swr_ctx_) swr_free(&swr_ctx_);
    if (sws_ctx_) sws_freeContext(sws_ctx_);
    if (a_enc_ctx_) avcodec_free_context(&a_enc_ctx_);
    if (v_enc_ctx_) avcodec_free_context(&v_enc_ctx_);
    if (a_dec_ctx_) avcodec_free_context(&a_dec_ctx_);
    if (v_dec_ctx_) avcodec_free_context(&v_dec_ctx_);
    if (out_fmt_ctx_) {
        if (out_fmt_ctx_->pb) avio_closep(&out_fmt_ctx_->pb);
        avformat_free_context(out_fmt_ctx_);
    }
    if (in_fmt_ctx_) avformat_close_input(&in_fmt_ctx_);
}
