#pragma once
#include <string>
#include <cstdint>

struct TranscodeConfig {
    std::string input_file;
    std::string output_file;
    std::string video_codec = "libx264";
    std::string preset = "medium";
    int crf = 23;
    int64_t video_bitrate = 0;    // 0 = use CRF mode
    int width = 0;                 // 0 = keep original
    int height = 0;                // 0 = keep original
    int fps = 0;                   // 0 = keep original
    int64_t audio_bitrate = 128000;
    bool no_audio = false;
    double start_time = -1;        // seconds, -1 = no trim
    double duration = 0;           // seconds
};

class Transcoder {
public:
    bool transcode(const TranscodeConfig& config);

private:
    bool open_input(const std::string& filename);
    bool open_output(const std::string& filename, const TranscodeConfig& config);
    bool init_video_decoder(int stream_index);
    bool init_audio_decoder(int stream_index);
    bool init_video_encoder(const TranscodeConfig& config, int input_stream_index);
    bool init_audio_encoder(const TranscodeConfig& config, int input_stream_index);
    bool init_video_scaler(int input_stream_index);
    bool init_audio_resampler(int input_stream_index);
    bool encode_write_frame(struct AVFrame* frame, int stream_index);
    bool flush_encoder(int stream_index);
    bool process_packet(struct AVPacket* packet);
    void cleanup();
    void print_error(int ret, const char* context);

    struct AVFormatContext* in_fmt_ctx_ = nullptr;
    struct AVFormatContext* out_fmt_ctx_ = nullptr;
    struct AVCodecContext* v_dec_ctx_ = nullptr;
    struct AVCodecContext* a_dec_ctx_ = nullptr;
    struct AVCodecContext* v_enc_ctx_ = nullptr;
    struct AVCodecContext* a_enc_ctx_ = nullptr;
    struct SwsContext* sws_ctx_ = nullptr;
    struct SwrContext* swr_ctx_ = nullptr;
    int v_stream_idx_ = -1;
    int a_stream_idx_ = -1;
    int v_out_idx_ = -1;
    int a_out_idx_ = -1;
    int64_t total_frames_ = 0;
    int64_t encoded_frames_ = 0;
};
