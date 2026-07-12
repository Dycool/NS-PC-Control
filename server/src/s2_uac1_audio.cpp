#include "s2_uac1_audio.hpp"

#if defined(__linux__) && defined(NS_HAVE_ALSA)

#include "app_state.hpp"

#include <alsa/asoundlib.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cerrno>
#include <condition_variable>
#include <cctype>
#include <cstring>
#include <deque>
#include <filesystem>
#include <mutex>
#include <optional>
#include <print>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

namespace {

constexpr unsigned S2_AUDIO_RATE = 48000;
constexpr unsigned S2_AUDIO_CHANNELS = 2;
constexpr snd_pcm_uframes_t S2_AUDIO_PERIOD_FRAMES = 48; // 1 ms at 48 kHz
constexpr snd_pcm_uframes_t S2_AUDIO_BUFFER_FRAMES = 384; // 8 ms
constexpr size_t S2_AUDIO_BYTES_PER_FRAME = S2_AUDIO_CHANNELS * sizeof(int16_t);
static_assert(S2_AUDIO_PERIOD_FRAMES * S2_AUDIO_BYTES_PER_FRAME
              == ns::S2_AUDIO_USB_FRAME_BYTES);

struct S2Uac1AudioState {
    std::mutex lifecycle_mtx;
    snd_pcm_t* console_capture = nullptr; // USB OUT -> ALSA capture -> UDP client
    snd_pcm_t* microphone_playback = nullptr; // UDP client -> ALSA playback -> USB IN
    std::string pcm_device;

    std::atomic<bool> running{false};
    std::atomic<bool> ready{false};
    bool stopping = false; // protected by lifecycle_mtx
    std::atomic<bool> microphone_activated{false};
    std::thread capture_thread;
    std::thread playback_thread;

    std::mutex console_mtx;
    std::condition_variable console_cv;
    std::deque<std::array<uint8_t, ns::S2_AUDIO_USB_FRAME_BYTES>> console_frames;

    std::mutex microphone_mtx;
    std::condition_variable microphone_cv;
    std::deque<std::array<uint8_t, ns::S2_AUDIO_USB_FRAME_BYTES>> microphone_frames;
};

S2Uac1AudioState g_audio;

std::string lower_ascii(std::string value) {
    for (char& ch : value) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return value;
}

std::string card_metadata(const snd_ctl_card_info_t* info) {
    const char* driver = snd_ctl_card_info_get_driver(info);
    const char* id = snd_ctl_card_info_get_id(info);
    const char* name = snd_ctl_card_info_get_name(info);
    const char* longname = snd_ctl_card_info_get_longname(info);
    return lower_ascii(
        std::string(driver ? driver : "") + " "
        + std::string(id ? id : "") + " "
        + std::string(name ? name : "") + " "
        + std::string(longname ? longname : ""));
}

bool card_sysfs_belongs_to_s2_uac1(int card) {
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path device = fs::path("/sys/class/sound") /
        ("card" + std::to_string(card)) / "device";
    const fs::path resolved = fs::weakly_canonical(device, ec);
    if (ec) return false;
    const std::string path = lower_ascii(resolved.string());
    return path.find("uac1.s2") != std::string::npos
        || (path.find("usb_gadget") != std::string::npos
            && path.find("uac1") != std::string::npos);
}

int card_uac1_score(int card, const snd_ctl_card_info_t* info) {
    const std::string text = card_metadata(info);
    int score = 0;
    if (card_sysfs_belongs_to_s2_uac1(card)) score += 100;
    if (text.find("switch 2 pro controller audio") != std::string::npos) score += 40;
    if (text.find("uac1") != std::string::npos) score += 20;
    if (text.find("gadget") != std::string::npos) score += 15;
    if (text.find("usb audio") != std::string::npos) score += 10;
    return score;
}

bool pcm_has_stream(snd_ctl_t* ctl, int device, snd_pcm_stream_t stream) {
    snd_pcm_info_t* info = nullptr;
    if (snd_pcm_info_malloc(&info) < 0 || !info) return false;
    snd_pcm_info_set_device(info, static_cast<unsigned>(device));
    snd_pcm_info_set_subdevice(info, 0);
    snd_pcm_info_set_stream(info, stream);
    const bool present = snd_ctl_pcm_info(ctl, info) >= 0;
    snd_pcm_info_free(info);
    return present;
}

std::optional<std::string> find_uac1_pcm_device() {
    struct Candidate { int score; int card; int device; std::string metadata; };
    std::optional<Candidate> best;

    int card = -1;
    while (snd_card_next(&card) >= 0 && card >= 0) {
        const std::string card_device = "hw:" + std::to_string(card);
        snd_ctl_t* ctl = nullptr;
        if (snd_ctl_open(&ctl, card_device.c_str(), 0) < 0) continue;

        snd_ctl_card_info_t* card_info = nullptr;
        if (snd_ctl_card_info_malloc(&card_info) < 0 || !card_info) {
            snd_ctl_close(ctl);
            continue;
        }
        if (snd_ctl_card_info(ctl, card_info) < 0) {
            snd_ctl_card_info_free(card_info);
            snd_ctl_close(ctl);
            continue;
        }

        const int score = card_uac1_score(card, card_info);
        const std::string metadata = card_metadata(card_info);
        snd_ctl_card_info_free(card_info);

        int device = -1;
        while (snd_ctl_pcm_next_device(ctl, &device) >= 0 && device >= 0) {
            if (!pcm_has_stream(ctl, device, SND_PCM_STREAM_CAPTURE)
                    || !pcm_has_stream(ctl, device, SND_PCM_STREAM_PLAYBACK)) {
                continue;
            }
            // Require positive UAC/gadget evidence. This avoids selecting an
            // unrelated duplex sound card while accepting kernel naming variants.
            if (score > 0 && (!best || score > best->score)) {
                best = Candidate{score, card, device, metadata};
            }
        }
        snd_ctl_close(ctl);
    }

    if (!best) return std::nullopt;
    if (g_ctx.verbose) {
        std::println("[s2][audio] selected ALSA card {}, device {} (score {}): {}",
                     best->card, best->device, best->score, best->metadata);
    }
    return "hw:" + std::to_string(best->card) + "," + std::to_string(best->device);
}

bool configure_pcm(snd_pcm_t* pcm, snd_pcm_stream_t stream) {
    snd_pcm_hw_params_t* hw = nullptr;
    snd_pcm_sw_params_t* sw = nullptr;
    if (snd_pcm_hw_params_malloc(&hw) < 0 || !hw) return false;

    int err = snd_pcm_hw_params_any(pcm, hw);
    if (err >= 0) err = snd_pcm_hw_params_set_access(pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED);
    if (err >= 0) err = snd_pcm_hw_params_set_format(pcm, hw, SND_PCM_FORMAT_S16_LE);
    if (err >= 0) err = snd_pcm_hw_params_set_channels(pcm, hw, S2_AUDIO_CHANNELS);

    unsigned rate = S2_AUDIO_RATE;
    int direction = 0;
    if (err >= 0) err = snd_pcm_hw_params_set_rate_near(pcm, hw, &rate, &direction);
    if (err >= 0 && rate != S2_AUDIO_RATE) err = -EINVAL;

    snd_pcm_uframes_t period = S2_AUDIO_PERIOD_FRAMES;
    direction = 0;
    if (err >= 0) err = snd_pcm_hw_params_set_period_size_near(pcm, hw, &period, &direction);

    snd_pcm_uframes_t buffer = std::max<snd_pcm_uframes_t>(S2_AUDIO_BUFFER_FRAMES, period * 4);
    if (err >= 0) err = snd_pcm_hw_params_set_buffer_size_near(pcm, hw, &buffer);
    if (err >= 0) err = snd_pcm_hw_params(pcm, hw);
    snd_pcm_hw_params_free(hw);
    if (err < 0) {
        if (g_ctx.verbose) {
            std::println(stderr, "[s2][audio] ALSA hardware setup failed: {}", snd_strerror(err));
        }
        return false;
    }

    if (snd_pcm_sw_params_malloc(&sw) < 0 || !sw) return false;
    err = snd_pcm_sw_params_current(pcm, sw);
    if (err >= 0) err = snd_pcm_sw_params_set_avail_min(pcm, sw, S2_AUDIO_PERIOD_FRAMES);
    if (err >= 0) {
        const snd_pcm_uframes_t threshold = stream == SND_PCM_STREAM_PLAYBACK ? period : 1;
        err = snd_pcm_sw_params_set_start_threshold(pcm, sw, threshold);
    }
    if (err >= 0) err = snd_pcm_sw_params(pcm, sw);
    snd_pcm_sw_params_free(sw);
    if (err < 0) {
        if (g_ctx.verbose) {
            std::println(stderr, "[s2][audio] ALSA software setup failed: {}", snd_strerror(err));
        }
        return false;
    }

    if (g_ctx.verbose) {
        std::println("[s2][audio] ALSA {} configured: rate={} Hz, period={} frames, buffer={} frames",
                     stream == SND_PCM_STREAM_CAPTURE ? "capture" : "playback",
                     rate, period, buffer);
    }
    err = snd_pcm_prepare(pcm);
    if (err < 0 && g_ctx.verbose) {
        std::println(stderr, "[s2][audio] ALSA prepare failed: {}", snd_strerror(err));
    }
    return err >= 0;
}

bool recover_pcm(snd_pcm_t* pcm, int err) {
    if (err == -EAGAIN) return true;
    return snd_pcm_recover(pcm, err, 1) >= 0;
}

void capture_loop() {
    std::array<uint8_t, ns::S2_AUDIO_USB_FRAME_BYTES> frame{};
    snd_pcm_uframes_t completed = 0;

    while (g_audio.running.load(std::memory_order_relaxed)) {
        snd_pcm_t* pcm = g_audio.console_capture;
        const snd_pcm_uframes_t requested = S2_AUDIO_PERIOD_FRAMES - completed;
        auto* destination = frame.data() + completed * S2_AUDIO_BYTES_PER_FRAME;
        const snd_pcm_sframes_t read_frames = snd_pcm_readi(pcm, destination, requested);

        if (read_frames > 0) {
            completed += static_cast<snd_pcm_uframes_t>(read_frames);
            if (completed < S2_AUDIO_PERIOD_FRAMES) continue;

            mark_switch2_usb_activity();
            {
                std::lock_guard<std::mutex> lock(g_audio.console_mtx);
                while (g_audio.console_frames.size() >= 8) g_audio.console_frames.pop_front();
                g_audio.console_frames.push_back(frame);
            }
            g_audio.console_cv.notify_one();
            completed = 0;
            continue;
        }

        if (read_frames == -EAGAIN) {
            (void)snd_pcm_wait(pcm, 2);
            continue;
        }
        completed = 0;
        if (!recover_pcm(pcm, static_cast<int>(read_frames))) {
            if (g_ctx.verbose) {
                std::println(stderr, "[s2][audio] ALSA capture error: {}",
                             snd_strerror(static_cast<int>(read_frames)));
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }
}

void playback_loop() {
    const std::array<uint8_t, ns::S2_AUDIO_USB_FRAME_BYTES> silence{};
    std::array<uint8_t, ns::S2_AUDIO_USB_FRAME_BYTES> frame{};
    snd_pcm_uframes_t completed = S2_AUDIO_PERIOD_FRAMES;

    while (g_audio.running.load(std::memory_order_relaxed)) {
        if (completed >= S2_AUDIO_PERIOD_FRAMES) {
            bool have_frame = false;
            {
                std::unique_lock<std::mutex> lock(g_audio.microphone_mtx);
                g_audio.microphone_cv.wait_for(lock, std::chrono::milliseconds(1), [&] {
                    return !g_audio.running.load(std::memory_order_relaxed)
                        || !g_audio.microphone_frames.empty();
                });
                if (!g_audio.running.load(std::memory_order_relaxed)) break;
                if (!g_audio.microphone_frames.empty()) {
                    frame = g_audio.microphone_frames.front();
                    g_audio.microphone_frames.pop_front();
                    have_frame = true;
                }
            }
            if (!have_frame) {
                if (!g_audio.microphone_activated.load(std::memory_order_acquire)) continue;
                frame = silence;
            }
            completed = 0;
        }

        snd_pcm_t* pcm = g_audio.microphone_playback;
        const snd_pcm_uframes_t requested = S2_AUDIO_PERIOD_FRAMES - completed;
        const auto* source = frame.data() + completed * S2_AUDIO_BYTES_PER_FRAME;
        const snd_pcm_sframes_t written_frames = snd_pcm_writei(pcm, source, requested);

        if (written_frames > 0) {
            completed += static_cast<snd_pcm_uframes_t>(written_frames);
            continue;
        }
        if (written_frames == -EAGAIN) {
            (void)snd_pcm_wait(pcm, 2);
            continue;
        }
        completed = S2_AUDIO_PERIOD_FRAMES; // discard stale partial data after recovery
        if (!recover_pcm(pcm, static_cast<int>(written_frames))) {
            if (g_ctx.verbose) {
                std::println(stderr, "[s2][audio] ALSA playback error: {}",
                             snd_strerror(static_cast<int>(written_frames)));
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }
}

void close_pcm_handles() {
    if (g_audio.console_capture) {
        snd_pcm_drop(g_audio.console_capture);
        snd_pcm_close(g_audio.console_capture);
        g_audio.console_capture = nullptr;
    }
    if (g_audio.microphone_playback) {
        snd_pcm_drop(g_audio.microphone_playback);
        snd_pcm_close(g_audio.microphone_playback);
        g_audio.microphone_playback = nullptr;
    }
}

} // namespace

bool s2_uac1_audio_compiled() { return true; }

bool s2_uac1_audio_start() {
    std::lock_guard<std::mutex> lifecycle_lock(g_audio.lifecycle_mtx);
    if (g_audio.ready.load(std::memory_order_acquire)) return true;
    if (g_audio.stopping) return false;

    const auto device = find_uac1_pcm_device();
    if (!device) return false;

    snd_pcm_t* capture = nullptr;
    snd_pcm_t* playback = nullptr;
    int err = snd_pcm_open(&capture, device->c_str(), SND_PCM_STREAM_CAPTURE, SND_PCM_NONBLOCK);
    if (err < 0) {
        std::println(stderr, "[s2][audio] failed to open ALSA capture {}: {}",
                     *device, snd_strerror(err));
        return false;
    }
    err = snd_pcm_open(&playback, device->c_str(), SND_PCM_STREAM_PLAYBACK, SND_PCM_NONBLOCK);
    if (err < 0) {
        std::println(stderr, "[s2][audio] failed to open ALSA playback {}: {}",
                     *device, snd_strerror(err));
        snd_pcm_close(capture);
        return false;
    }

    g_audio.console_capture = capture;
    g_audio.microphone_playback = playback;
    if (!configure_pcm(g_audio.console_capture, SND_PCM_STREAM_CAPTURE)
            || !configure_pcm(g_audio.microphone_playback, SND_PCM_STREAM_PLAYBACK)) {
        close_pcm_handles();
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(g_audio.console_mtx);
        g_audio.console_frames.clear();
    }
    {
        std::lock_guard<std::mutex> lock(g_audio.microphone_mtx);
        g_audio.microphone_frames.clear();
    }

    g_audio.pcm_device = *device;
    g_audio.microphone_activated.store(false, std::memory_order_release);
    g_audio.running.store(true, std::memory_order_release);
    try {
        g_audio.capture_thread = std::thread(capture_loop);
        g_audio.playback_thread = std::thread(playback_loop);
    } catch (const std::system_error& e) {
        std::println(stderr, "[s2][audio] failed to create audio worker thread: {}", e.what());
        g_audio.running.store(false, std::memory_order_release);
        g_audio.console_cv.notify_all();
        g_audio.microphone_cv.notify_all();
        if (g_audio.capture_thread.joinable()) g_audio.capture_thread.join();
        if (g_audio.playback_thread.joinable()) g_audio.playback_thread.join();
        close_pcm_handles();
        g_audio.pcm_device.clear();
        return false;
    }
    g_audio.ready.store(true, std::memory_order_release);
    if (g_ctx.verbose) {
        std::println("[s2][audio] stock UAC1 ALSA bridge active on {} (S16LE stereo 48 kHz)",
                     g_audio.pcm_device);
    }
    return true;
}

void s2_uac1_audio_stop() {
    std::unique_lock<std::mutex> lifecycle_lock(g_audio.lifecycle_mtx);
    g_audio.stopping = true;
    g_audio.ready.store(false, std::memory_order_release);
    g_audio.running.store(false, std::memory_order_release);
    g_audio.console_cv.notify_all();
    g_audio.microphone_cv.notify_all();

    std::thread capture = std::move(g_audio.capture_thread);
    std::thread playback = std::move(g_audio.playback_thread);
    lifecycle_lock.unlock();
    if (capture.joinable()) capture.join();
    if (playback.joinable()) playback.join();
    lifecycle_lock.lock();

    close_pcm_handles();
    g_audio.pcm_device.clear();
    g_audio.microphone_activated.store(false, std::memory_order_release);
    g_audio.stopping = false;
    {
        std::lock_guard<std::mutex> lock(g_audio.console_mtx);
        g_audio.console_frames.clear();
    }
    {
        std::lock_guard<std::mutex> lock(g_audio.microphone_mtx);
        g_audio.microphone_frames.clear();
    }
}

bool s2_uac1_audio_ready() {
    return g_audio.ready.load(std::memory_order_acquire);
}

bool s2_uac1_wait_console_audio(
        std::array<unsigned char, ns::S2_AUDIO_USB_FRAME_BYTES>& audio_frame,
        std::chrono::milliseconds timeout) {
    audio_frame.fill(0);
    if (!s2_uac1_audio_ready()) return false;

    std::unique_lock<std::mutex> lock(g_audio.console_mtx);
    if (!g_audio.console_cv.wait_for(lock, timeout, [&] {
            return !g_audio.console_frames.empty()
                || !g_audio.running.load(std::memory_order_relaxed);
        })) {
        return false;
    }
    if (g_audio.console_frames.empty()) return false;
    audio_frame = g_audio.console_frames.front();
    g_audio.console_frames.pop_front();
    return true;
}

bool s2_uac1_submit_microphone_audio(const uint8_t* data, size_t len) {
    if (!data || len == 0 || !s2_uac1_audio_ready()) return false;
    if (len % ns::S2_AUDIO_USB_FRAME_BYTES != 0) return false;

    g_audio.microphone_activated.store(true, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(g_audio.microphone_mtx);
        for (size_t offset = 0; offset < len; offset += ns::S2_AUDIO_USB_FRAME_BYTES) {
            while (g_audio.microphone_frames.size() >= 8) g_audio.microphone_frames.pop_front();
            std::array<uint8_t, ns::S2_AUDIO_USB_FRAME_BYTES> frame{};
            std::memcpy(frame.data(), data + offset, frame.size());
            g_audio.microphone_frames.push_back(frame);
        }
    }
    g_audio.microphone_cv.notify_one();
    return true;
}

#else

bool s2_uac1_audio_compiled() { return false; }
bool s2_uac1_audio_start() { return false; }
void s2_uac1_audio_stop() {}
bool s2_uac1_audio_ready() { return false; }

bool s2_uac1_wait_console_audio(
        std::array<unsigned char, ns::S2_AUDIO_USB_FRAME_BYTES>& audio_frame,
        std::chrono::milliseconds) {
    audio_frame.fill(0);
    return false;
}

bool s2_uac1_submit_microphone_audio(const uint8_t*, size_t) { return false; }

#endif
