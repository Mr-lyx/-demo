#pragma once
#include <cstdio>
#include <ctime>
#include <string>

inline void print_progress(int64_t current, int64_t total, time_t start_time) {
    if (total <= 0) {
        fprintf(stderr, "\rframe=%lld  ", (long long)current);
        return;
    }
    double pct = (double)current / total;
    int bar_width = 30;
    int filled = (int)(pct * bar_width);
    time_t now = time(nullptr);
    double elapsed = difftime(now, start_time);
    double fps = elapsed > 0 ? current / elapsed : 0;
    double eta = fps > 0 ? (total - current) / fps : 0;

    fprintf(stderr, "\r[");
    for (int i = 0; i < bar_width; i++) {
        fputc(i < filled ? '#' : ' ', stderr);
    }
    fprintf(stderr, "] %3d%%  frame=%lld/%lld  fps=%.0f  eta=%02d:%02d:%02d",
            (int)(pct * 100), (long long)current, (long long)total,
            fps, (int)(eta / 3600), (int)(eta / 60) % 60, (int)eta % 60);
    fflush(stderr);
}
