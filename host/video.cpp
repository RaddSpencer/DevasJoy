// Video recording of every presented frame (ASURA_VIDEO=<file.mp4>): for showing rendering problems from a whole play session without
// re-running it. Windows' own H.264 encoder (Media Foundation sink writer, nothing to download) writes an MP4 at the game's frame
// rate; each presented frame is one video frame, timed by its index, so the video plays at game speed. Next to it, <file>.frames.txt
// lists for each video frame the game frame number and the input clock in ms (the clock ASURA_INPUTREC and the [sync] lines use),
// to find a moment in the logs from a time in the video. Frames come from the render thread as raw back buffer rows (xenos.cpp
// video_capture: staging copies read two frames late, so the GPU is never waited on) and are converted and encoded on a thread here.
// Files: <file>.mp4, then <file>_2.mp4, _3 ... every 5 minutes, each with its own .frames.txt (the video frame numbers restart at 0
// in each; the game frame column runs on). ASURA_VIDEOMBPS sets the bit rate (default 12 Mbit/s at 1280x720, scaled by area, at most 40). If the encoder falls behind by more than
// 90 frames, frames are left out and counted in the log (the video then skips; the timestamps stay right).
#define NOMINMAX
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")

int settings_target_fps();                         // options.cpp
uint32_t input_clock_ms();                         // xam.cpp

namespace {
struct Frame { std::vector<uint8_t> px; uint32_t w, h, pitch, fmt; uint64_t gameFrame; uint32_t ms; };
std::mutex g_lock;
std::condition_variable g_cv;
std::deque<Frame> g_queue;
std::atomic<bool> g_started{false}, g_done{false};
uint64_t g_dropped = 0;

// Back buffer formats as DXGI numbers: 24 R10G10B10A2, 87 B8G8R8A8, 28 R8G8B8A8. Output: B, G, R, A bytes (MFVideoFormat_RGB32).
void to_bgra(const Frame& f, std::vector<uint8_t>& out) {
    out.resize((size_t)f.w * f.h * 4);
    for (uint32_t y = 0; y < f.h; y++) {
        const uint8_t* s = f.px.data() + (size_t)y * f.pitch;
        uint8_t* d = out.data() + (size_t)y * f.w * 4;
        for (uint32_t x = 0; x < f.w; x++, s += 4, d += 4) {
            if (f.fmt == 24) { uint32_t v; memcpy(&v, s, 4); d[2] = (uint8_t)((v >> 2) & 255); d[1] = (uint8_t)((v >> 12) & 255); d[0] = (uint8_t)((v >> 22) & 255); }
            else if (f.fmt == 87 || f.fmt == 91) { d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; }
            else { d[0] = s[2]; d[1] = s[1]; d[2] = s[0]; }
            d[3] = 255;
        }
    }
}

struct Writer {
    IMFSinkWriter* sw = nullptr; DWORD stream = 0; uint32_t w = 0, h = 0; uint64_t index = 0; FILE* side = nullptr;
    bool open(const std::string& path, uint32_t width, uint32_t height, uint32_t fps) {
        std::wstring wp(path.begin(), path.end());
        IMFAttributes* at = nullptr;
        MFCreateAttributes(&at, 2);
        at->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
        at->SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING, TRUE);
        HRESULT hr = MFCreateSinkWriterFromURL(wp.c_str(), nullptr, at, &sw);
        at->Release();
        if (FAILED(hr)) { fprintf(stderr, "[video] can't create %s (%08lX)\n", path.c_str(), hr); return false; }
        const char* e = getenv("ASURA_VIDEOMBPS");
        const double mbps = e ? atof(e) : std::min(40.0, 12.0 * (double)width * height / (1280.0 * 720.0));   // 12 at 720p, 27 at 1080p, at most 40
        IMFMediaType* out = nullptr; MFCreateMediaType(&out);
        out->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video); out->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
        out->SetUINT32(MF_MT_AVG_BITRATE, (UINT32)(mbps * 1e6)); out->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        MFSetAttributeSize(out, MF_MT_FRAME_SIZE, width, height); MFSetAttributeRatio(out, MF_MT_FRAME_RATE, fps, 1); MFSetAttributeRatio(out, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        hr = sw->AddStream(out, &stream); out->Release();
        IMFMediaType* in = nullptr; MFCreateMediaType(&in);
        in->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video); in->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
        in->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive); in->SetUINT32(MF_MT_DEFAULT_STRIDE, width * 4);   // top-down rows
        MFSetAttributeSize(in, MF_MT_FRAME_SIZE, width, height); MFSetAttributeRatio(in, MF_MT_FRAME_RATE, fps, 1); MFSetAttributeRatio(in, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        if (SUCCEEDED(hr)) hr = sw->SetInputMediaType(stream, in, nullptr);
        in->Release();
        if (SUCCEEDED(hr)) hr = sw->BeginWriting();
        if (FAILED(hr)) { fprintf(stderr, "[video] encoder setup failed for %ux%u (%08lX)\n", width, height, hr); sw->Release(); sw = nullptr; return false; }
        w = width; h = height; index = 0;
        side = fopen((path + ".frames.txt").c_str(), "w");
        if (side) fprintf(side, "# video_frame game_frame input_ms  (video time = video_frame / %u s)\n", fps);
        fprintf(stderr, "[video] recording %ux%u at %u fps, %.1f Mbit/s to %s\n", width, height, fps, mbps, path.c_str());
        return true;
    }
    void write(const std::vector<uint8_t>& bgra, const Frame& f, uint32_t fps) {
        IMFMediaBuffer* buf = nullptr;
        if (FAILED(MFCreateMemoryBuffer((DWORD)bgra.size(), &buf))) return;
        BYTE* p = nullptr; buf->Lock(&p, nullptr, nullptr); memcpy(p, bgra.data(), bgra.size()); buf->Unlock(); buf->SetCurrentLength((DWORD)bgra.size());
        IMFSample* s = nullptr; MFCreateSample(&s); s->AddBuffer(buf);
        const LONGLONG dur = 10000000LL / fps;
        s->SetSampleTime((LONGLONG)index * dur); s->SetSampleDuration(dur);
        const HRESULT hr = sw->WriteSample(stream, s);
        s->Release(); buf->Release();
        if (FAILED(hr)) { static int n = 0; if (n++ < 5) fprintf(stderr, "[video] WriteSample failed (%08lX)\n", hr); }
        if (side) fprintf(side, "%llu %llu %u\n", (unsigned long long)index, (unsigned long long)f.gameFrame, f.ms);
        index++;
    }
    void close() {
        if (sw) { sw->Finalize(); sw->Release(); sw = nullptr; }
        if (side) { fclose(side); side = nullptr; }
    }
};

void encoder_thread(std::string path) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    MFStartup(MF_VERSION);
    const uint32_t fps = (uint32_t)settings_target_fps();
    Writer wr; int segment = 1;
    std::vector<uint8_t> bgra;
    for (;;) {
        Frame f;
        {
            std::unique_lock<std::mutex> lk(g_lock);
            g_cv.wait(lk, [] { return !g_queue.empty(); });
            f = std::move(g_queue.front()); g_queue.pop_front();
        }
        if (f.w == 0) break;                                               // the stop marker
        if (wr.sw && ((f.w & ~1u) != wr.w || (f.h & ~1u) != wr.h || wr.index >= 300ull * fps)) {   // a new file every 5 minutes (a crash loses
                                                                                                         // at most the open one) or when the window changes size
            wr.close();
            const size_t dot = path.rfind('.');
            const std::string next = (dot == std::string::npos ? path : path.substr(0, dot)) + "_" + std::to_string(++segment) + (dot == std::string::npos ? "" : path.substr(dot));
            if (!wr.open(next, f.w & ~1u, f.h & ~1u, fps)) break;
        }
        if (!wr.sw && !wr.open(path, f.w & ~1u, f.h & ~1u, fps)) break;    // H.264 needs even sizes: the last odd row/column is left out
        if ((f.w & 1) || (f.h & 1)) {                                      // crop to the even size the encoder was opened with
            Frame c = f; c.w = wr.w; c.h = wr.h;
            to_bgra(c, bgra);
        } else to_bgra(f, bgra);
        wr.write(bgra, f, fps);
    }
    wr.close();
    g_done = true;
    fprintf(stderr, "[video] finished (%llu frames left out)\n", (unsigned long long)g_dropped);
    MFShutdown();
}
}  // namespace

// Close the video before the process ends (the window's close button, the game's own exit, a crash report): an MP4 is unplayable
// until Finalize writes its index. Waits for the frames already queued, up to about 10 s.
static std::atomic<bool> g_finished{false};
void video_finish() {
    if (!g_started.load() || g_finished.exchange(true)) return;
    { std::lock_guard<std::mutex> lk(g_lock); Frame stop{}; stop.w = 0; g_queue.push_back(std::move(stop)); }
    g_cv.notify_one();
    for (int i = 0; i < 500 && !g_done.load(); i++) Sleep(20);
}
bool video_wanted() { static const bool on = getenv("ASURA_VIDEO") != nullptr; return on; }

// One presented frame's back buffer rows (render thread). pitch is the mapped row pitch; fmt the DXGI format number.
void video_submit(const uint8_t* rows, uint32_t w, uint32_t h, uint32_t pitch, uint32_t fmt, uint64_t gameFrame) {
    if (!video_wanted() || !w || !h) return;
    if (!g_started.exchange(true)) {
        std::thread(encoder_thread, std::string(getenv("ASURA_VIDEO"))).detach();
    }
    Frame f; f.w = w; f.h = h; f.pitch = w * 4; f.fmt = fmt; f.gameFrame = gameFrame; f.ms = input_clock_ms();
    f.px.resize((size_t)f.pitch * h);
    for (uint32_t y = 0; y < h; y++) memcpy(f.px.data() + (size_t)y * f.pitch, rows + (size_t)y * pitch, f.pitch);
    {
        std::lock_guard<std::mutex> lk(g_lock);
        if (g_queue.size() > 90) { if (g_dropped++ % 30 == 0) fprintf(stderr, "[video] encoder behind: %llu frames left out so far\n", (unsigned long long)g_dropped); return; }
        g_queue.push_back(std::move(f));
    }
    g_cv.notify_one();
}
