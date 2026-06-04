#include "transcoder.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>

static void print_help(const char* prog) {
    printf("Usage: %s <input> <output> [options]\n\n", prog);
    printf("Options:\n");
    printf("  --codec      <h264|h265|vp9>   Video encoder (default: h264)\n");
    printf("  --preset     <fast|medium|slow> Encoder preset (default: medium)\n");
    printf("  --crf        <0-51>            Constant quality (default: 23)\n");
    printf("  --bitrate    <N>M              Target video bitrate, e.g. 2M\n");
    printf("  --width      <N>               Output width\n");
    printf("  --height     <N>               Output height\n");
    printf("  --fps        <N>               Output framerate\n");
    printf("  --audio-bitrate <N>K           Audio bitrate (default: 128K)\n");
    printf("  --no-audio                     Remove audio\n");
    printf("  --start      <HH:MM:SS>        Start time\n");
    printf("  --duration   <HH:MM:SS>        Duration\n");
    printf("  -h, --help                     Show help\n");
    printf("\nExamples:\n");
    printf("  %s input.mp4 output.mp4 --width 1280 --height 720\n", prog);
    printf("  %s input.mp4 output.mp4 --codec h265 --crf 28\n", prog);
    printf("  %s input.mp4 output.mp4 --fps 30 --bitrate 2M\n", prog);
}

static int parse_time(const char* s) {
    int h = 0, m = 0, sec = 0;
    sscanf(s, "%d:%d:%d", &h, &m, &sec);
    return h * 3600 + m * 60 + sec;
}

static int64_t parse_bitrate(const char* s) {
    char* end;
    double val = strtod(s, &end);
    if (*end == 'M' || *end == 'm') return (int64_t)(val * 1000000);
    if (*end == 'K' || *end == 'k') return (int64_t)(val * 1000);
    return (int64_t)val;
}

int main(int argc, char* argv[]) {
    if (argc < 3) {
        print_help(argv[0]);
        return 1;
    }

    TranscodeConfig config;
    config.input_file = argv[1];
    config.output_file = argv[2];

    // Handle --help early, even when it looks like it might be input/output
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_help(argv[0]);
            return 0;
        }
    }

    for (int i = 3; i < argc; i++) {
        if (strcmp(argv[i], "--codec") == 0 && i + 1 < argc) {
            const char* c = argv[++i];
            if (strcmp(c, "h264") == 0) config.video_codec = "libx264";
            else if (strcmp(c, "h265") == 0) config.video_codec = "libx265";
            else if (strcmp(c, "vp9") == 0) config.video_codec = "libvpx-vp9";
            else { fprintf(stderr, "Unknown codec: %s\n", c); return 1; }
        } else if (strcmp(argv[i], "--preset") == 0 && i + 1 < argc) {
            config.preset = argv[++i];
        } else if (strcmp(argv[i], "--crf") == 0 && i + 1 < argc) {
            config.crf = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--bitrate") == 0 && i + 1 < argc) {
            config.video_bitrate = parse_bitrate(argv[++i]);
        } else if (strcmp(argv[i], "--width") == 0 && i + 1 < argc) {
            config.width = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--height") == 0 && i + 1 < argc) {
            config.height = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--fps") == 0 && i + 1 < argc) {
            config.fps = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--audio-bitrate") == 0 && i + 1 < argc) {
            config.audio_bitrate = parse_bitrate(argv[++i]);
        } else if (strcmp(argv[i], "--no-audio") == 0) {
            config.no_audio = true;
        } else if (strcmp(argv[i], "--start") == 0 && i + 1 < argc) {
            config.start_time = (double)parse_time(argv[++i]);
        } else if (strcmp(argv[i], "--duration") == 0 && i + 1 < argc) {
            config.duration = (double)parse_time(argv[++i]);
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            print_help(argv[0]);
            return 1;
        }
    }

    Transcoder t;
    if (!t.transcode(config)) {
        fprintf(stderr, "Transcoding failed.\n");
        return 1;
    }
    return 0;
}
