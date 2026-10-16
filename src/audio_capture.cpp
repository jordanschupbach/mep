// In-house microphone capture -- see audio_capture.h. This file holds the
// platform-neutral parts (resampler, segment writer, RecordSegments) and
// the Linux (ALSA), Windows (WASAPI) and Emscripten capture backends; the
// macOS backend is audio_capture_macos.mm.

#include "audio_capture.h"

#include "wav_doc.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <filesystem>
#include <limits>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#include <mmsystem.h>
#include <mmreg.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <future>
#elif !defined(__EMSCRIPTEN__) && !defined(__APPLE__)
#include <alsa/asoundlib.h>

#include <cerrno>
#endif

namespace mep::audio {

// ---------------------------------------------------------------------------
// Resampler

namespace {

constexpr int kZeroCrossings = 32;     // kernel half length, in sinc zero crossings
constexpr int kTableResolution = 512;  // table entries per zero crossing
constexpr double kKaiserBeta = 9.0;    // ~ -90 dB sidelobes
constexpr double kPassband = 0.9;      // cutoff as a fraction of the lower Nyquist
constexpr double kPi = 3.14159265358979323846;

// Zeroth-order modified Bessel function of the first kind (power series).
double BesselI0(double x) {
    double sum = 1.0, term = 1.0;
    const double q = x * x / 4.0;
    for (int k = 1; k < 64; ++k) {
        term *= q / (static_cast<double>(k) * static_cast<double>(k));
        sum += term;
        if (term < sum * 1e-17) break;
    }
    return sum;
}

}  // namespace

Resampler::Resampler(int in_rate, int out_rate)
    : in_rate_(in_rate > 0 ? in_rate : 1), out_rate_(out_rate > 0 ? out_rate : 1) {
    step_ = static_cast<double>(in_rate_) / static_cast<double>(out_rate_);
    const double ratio = std::min(1.0, static_cast<double>(out_rate_) / static_cast<double>(in_rate_));
    cutoff_ = kPassband * ratio;
    half_width_ = static_cast<int>(std::ceil(static_cast<double>(kZeroCrossings) / cutoff_));
    // table_[i] = sinc(u) * kaiser(u / Z) at u = i / kTableResolution zero
    // crossings; two guard entries keep the interpolation in bounds.
    const size_t n = static_cast<size_t>(kZeroCrossings * kTableResolution) + 2;
    table_.resize(n);
    const double i0_beta = BesselI0(kKaiserBeta);
    for (size_t i = 0; i < n; ++i) {
        const double u = static_cast<double>(i) / kTableResolution;
        if (u >= kZeroCrossings) {
            table_[i] = 0.0f;
            continue;
        }
        const double sinc = u == 0.0 ? 1.0 : std::sin(kPi * u) / (kPi * u);
        const double r = u / kZeroCrossings;
        const double window = BesselI0(kKaiserBeta * std::sqrt(1.0 - r * r)) / i0_beta;
        table_[i] = static_cast<float>(sinc * window);
    }
    // Lead-in: half_width_ zeros before input sample 0, so the first output
    // (centred on input 0) has a full history and the filter delay is
    // compensated.
    base_ = -static_cast<long long>(half_width_);
    buf_.assign(static_cast<size_t>(half_width_), 0.0f);
}

void Resampler::Produce(std::vector<float> &out, uint64_t limit) {
    const uint64_t in = static_cast<uint64_t>(in_rate_), outr = static_cast<uint64_t>(out_rate_);
    const double scale = static_cast<double>(kTableResolution) * cutoff_;
    const size_t table_last = static_cast<size_t>(kZeroCrossings * kTableResolution);
    while (total_out_ < limit) {
        // Exact rational position of this output on the input timeline:
        // no accumulated drift however long the recording runs.
        const uint64_t num = total_out_ * in;
        const long long whole = static_cast<long long>(num / outr);
        const double frac = static_cast<double>(num % outr) / static_cast<double>(outr);
        const long long rel = whole - base_;
        if (rel + half_width_ >= static_cast<long long>(buf_.size())) break;
        double acc = 0.0;
        for (long long j = rel - half_width_ + 1; j <= rel + half_width_; ++j) {
            const double d = std::fabs(static_cast<double>(j - rel) - frac);
            const double x = d * scale;
            const size_t i = static_cast<size_t>(x);
            if (i >= table_last) continue;
            const double t = x - static_cast<double>(i);
            const double h = static_cast<double>(table_[i]) * (1.0 - t) + static_cast<double>(table_[i + 1]) * t;
            acc += h * static_cast<double>(buf_[static_cast<size_t>(j)]);
        }
        out.push_back(static_cast<float>(acc * cutoff_));
        ++total_out_;
    }
    // Drop history the next output no longer reaches (in batches, so the
    // erase is amortised).
    const uint64_t next = total_out_ * in / outr;
    const long long keep_from = static_cast<long long>(next) - base_ - half_width_;
    if (keep_from > 8192) {
        buf_.erase(buf_.begin(), buf_.begin() + static_cast<std::ptrdiff_t>(keep_from));
        base_ += keep_from;
    }
}

void Resampler::Process(const float *in, size_t count, std::vector<float> &out) {
    if (in_rate_ == out_rate_) {
        out.insert(out.end(), in, in + count);
        total_in_ += count;
        total_out_ += count;
        return;
    }
    buf_.insert(buf_.end(), in, in + count);
    total_in_ += count;
    Produce(out, std::numeric_limits<uint64_t>::max());
}

void Resampler::Flush(std::vector<float> &out) {
    if (in_rate_ == out_rate_) return;
    const uint64_t in = static_cast<uint64_t>(in_rate_), outr = static_cast<uint64_t>(out_rate_);
    const uint64_t target = (total_in_ * outr + in / 2) / in;
    buf_.insert(buf_.end(), static_cast<size_t>(half_width_) + 2, 0.0f);
    Produce(out, target);
}

int16_t FloatToPcm16(float v) {
    if (!(v == v)) return 0;  // NaN
    const float c = std::clamp(v, -1.0f, 1.0f);
    return static_cast<int16_t>(std::lround(static_cast<double>(c) * 32767.0));
}

// ---------------------------------------------------------------------------
// SegmentWriter

SegmentWriter::SegmentWriter(SegmentOptions options, int in_rate, SegmentReady ready)
    : opt_(std::move(options)), resampler_(in_rate, opt_.out_rate > 0 ? opt_.out_rate : 16000),
      ready_(std::move(ready)) {
    if (opt_.out_rate <= 0) opt_.out_rate = 16000;
    if (opt_.prefix.empty()) opt_.prefix = "chunk";
    const double samples = std::round(std::max(0.1, opt_.chunk_seconds) * opt_.out_rate);
    chunk_samples_ = static_cast<size_t>(samples);
}

std::string SegmentWriter::SegmentPath(int index) const {
    char num[32];
    std::snprintf(num, sizeof num, "_%03d.wav", index);
    std::string dir = opt_.dir.empty() ? std::string(".") : opt_.dir;
    if (dir.back() != '/' && dir.back() != '\\') dir += '/';
    return dir + opt_.prefix + num;
}

bool SegmentWriter::WriteSegment(const int16_t *samples, size_t count, std::string *error) {
    if (index_ == 0 && !opt_.dir.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(opt_.dir, ec);
    }
    const std::string path = SegmentPath(index_);
    if (!WriteWavPcm16(path, samples, count, opt_.out_rate, 1, error)) return false;
    ++index_;
    if (ready_) ready_(path);
    return true;
}

bool SegmentWriter::Push(const float *mono, size_t count, std::string *error) {
    scratch_.clear();
    resampler_.Process(mono, count, scratch_);
    for (float v : scratch_) pending_.push_back(FloatToPcm16(v));
    size_t done = 0;
    while (pending_.size() - done >= chunk_samples_) {
        if (!WriteSegment(pending_.data() + done, chunk_samples_, error)) return false;
        done += chunk_samples_;
    }
    if (done) pending_.erase(pending_.begin(), pending_.begin() + static_cast<std::ptrdiff_t>(done));
    return true;
}

bool SegmentWriter::Finish(std::string *error) {
    scratch_.clear();
    resampler_.Flush(scratch_);
    for (float v : scratch_) pending_.push_back(FloatToPcm16(v));
    size_t done = 0;
    while (pending_.size() - done >= chunk_samples_) {
        if (!WriteSegment(pending_.data() + done, chunk_samples_, error)) return false;
        done += chunk_samples_;
    }
    const size_t rest = pending_.size() - done;
    const double min_final = std::max(0.0, opt_.min_final_seconds) * opt_.out_rate;
    bool ok = true;
    if (rest > 0 && static_cast<double>(rest) >= min_final) ok = WriteSegment(pending_.data() + done, rest, error);
    pending_.clear();
    return ok;
}

// ---------------------------------------------------------------------------
// RecordSegments

int RecordSegments(const SegmentOptions &options, const RecordEvents &events, const std::atomic<bool> &cancelled,
                   std::string *error) {
    struct Shared {
        std::mutex mu;
        std::condition_variable cv;
        std::vector<float> mono;
    };
    Shared shared;
    Capture capture;
    /**
     * @brief Device callback: downmixes to mono and queues for the recording thread.
     */
    auto on_frames = [&shared, &capture](const float *in, size_t frames) {
        const int ch = std::max(1, capture.Channels());
        std::lock_guard<std::mutex> lk(shared.mu);
        if (ch == 1) {
            shared.mono.insert(shared.mono.end(), in, in + frames);
        } else {
            const size_t c = static_cast<size_t>(ch);
            for (size_t f = 0; f < frames; ++f) {
                float sum = 0.0f;
                for (size_t k = 0; k < c; ++k) sum += in[f * c + k];
                shared.mono.push_back(sum / static_cast<float>(ch));
            }
        }
        shared.cv.notify_one();
    };
    if (!capture.Start(on_frames, error)) return 1;

    std::string write_error;
    SegmentWriter writer(options, capture.SampleRate(), events.on_segment);
    const auto started = std::chrono::steady_clock::now();
    const size_t silence_window = static_cast<size_t>(capture.SampleRate()) * 2;
    size_t seen = 0;
    bool any_nonzero = false, warned = false;
    std::vector<float> batch;
    while (!cancelled.load()) {
        batch.clear();
        {
            std::unique_lock<std::mutex> lk(shared.mu);
            shared.cv.wait_for(lk, std::chrono::milliseconds(100), [&shared] { return !shared.mono.empty(); });
            batch.swap(shared.mono);
        }
        std::string failure;
        if (capture.Failed(&failure)) {
            capture.Stop();
            if (error) *error = failure;
            return 1;
        }
        if (batch.empty()) {
            if (seen == 0 && std::chrono::steady_clock::now() - started > std::chrono::seconds(5)) {
                capture.Stop();
                if (error) *error = "no audio arrived from the input device within 5 seconds";
                return 1;
            }
            continue;
        }
        if (!any_nonzero)
            any_nonzero = std::any_of(batch.begin(), batch.end(), [](float v) { return v != 0.0f; });
        seen += batch.size();
        if (!any_nonzero && !warned && seen >= silence_window) {
            warned = true;
            if (events.on_warning)
                events.on_warning("the microphone delivers pure digital silence -- the input may be muted, or "
                                  "microphone access may have been refused");
        }
        if (!writer.Push(batch.data(), batch.size(), &write_error)) {
            capture.Stop();
            if (error) *error = write_error;
            return 1;
        }
    }
    capture.Stop();
    {
        std::lock_guard<std::mutex> lk(shared.mu);
        batch.swap(shared.mono);
    }
    if ((!batch.empty() && !writer.Push(batch.data(), batch.size(), &write_error)) || !writer.Finish(&write_error)) {
        if (error) *error = write_error;
        return 1;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Capture backends (macOS: audio_capture_macos.mm)

#if defined(__EMSCRIPTEN__)

struct Capture::Impl {};
Capture::Capture() : impl_(std::make_unique<Impl>()) {}
Capture::~Capture() = default;
bool Capture::Start(FrameCallback, std::string *error) {
    if (error) *error = "microphone capture is not available in the web build";
    return false;
}
void Capture::Stop() {}
int Capture::SampleRate() const { return 0; }
int Capture::Channels() const { return 0; }
bool Capture::Failed(std::string *) const { return false; }

#elif defined(_WIN32)

// WASAPI shared-mode capture of the default communications-or-console
// input (eCapture/eConsole), polled every 10 ms on a thread of our own.
// Everything COM is created and released on that thread.
struct Capture::Impl {
    FrameCallback callback;
    std::thread thread;
    std::atomic<bool> running{false};
    mutable std::mutex mu;
    std::string failure;
    int rate = 0, channels = 0;

    void Fail(const std::string &msg) {
        std::lock_guard<std::mutex> lk(mu);
        if (failure.empty()) failure = msg;
    }
};

namespace {

std::string HrMessage(const char *what, HRESULT hr) {
    char buf[96];
    std::snprintf(buf, sizeof buf, " (HRESULT 0x%08lX)", static_cast<unsigned long>(hr));
    if (hr == E_ACCESSDENIED)
        return std::string(what) + ": microphone access denied -- allow desktop apps to use the microphone in "
                                   "Settings > Privacy & security > Microphone" + buf;
    return std::string(what) + " failed" + buf;
}

template <typename T>
void SafeRelease(T *&p) {
    if (p) {
        p->Release();
        p = nullptr;
    }
}

// The capture thread: opens the device, reports the outcome through
// `ready` (empty string = success), then pumps packets until `running`
// turns false.
void WasapiThread(Capture::Impl *impl, std::promise<std::string> ready) {
    const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IMMDeviceEnumerator *enumerator = nullptr;
    IMMDevice *device = nullptr;
    IAudioClient *client = nullptr;
    IAudioCaptureClient *capture = nullptr;
    WAVEFORMATEX *mix = nullptr;
    std::string err;
    bool is_float = false;
    int bits = 0;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                  reinterpret_cast<void **>(&enumerator));
    if (FAILED(hr)) err = HrMessage("creating the audio device enumerator", hr);
    if (err.empty()) {
        hr = enumerator->GetDefaultAudioEndpoint(eCapture, eConsole, &device);
        if (hr == static_cast<HRESULT>(E_NOTFOUND))
            err = "no microphone / input device found";
        else if (FAILED(hr))
            err = HrMessage("opening the default input device", hr);
    }
    if (err.empty()) {
        hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void **>(&client));
        if (FAILED(hr)) err = HrMessage("activating the input device", hr);
    }
    if (err.empty()) {
        hr = client->GetMixFormat(&mix);
        if (FAILED(hr)) err = HrMessage("reading the input format", hr);
    }
    if (err.empty()) {
        bits = mix->wBitsPerSample;
        if (mix->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
            is_float = true;
        } else if (mix->wFormatTag == WAVE_FORMAT_EXTENSIBLE && mix->cbSize >= 22) {
            // The KSDATAFORMAT_SUBTYPE_* GUIDs are {0000000X-0000-0010-8000-
            // 00aa00389b71} with X the plain format tag, so Data1 tells them
            // apart without linking ksuser for the GUID constants.
            const WAVEFORMATEXTENSIBLE *ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE *>(mix);
            is_float = ext->SubFormat.Data1 == static_cast<unsigned long>(WAVE_FORMAT_IEEE_FLOAT);
            if (!is_float && ext->SubFormat.Data1 != static_cast<unsigned long>(WAVE_FORMAT_PCM)) err = "unsupported input sample format";
        } else if (mix->wFormatTag != WAVE_FORMAT_PCM) {
            err = "unsupported input sample format";
        }
        if (err.empty() && !(is_float ? bits == 32 : (bits == 16 || bits == 32)))
            err = "unsupported input sample size";
    }
    if (err.empty()) {
        const REFERENCE_TIME buffer_100ns = 2000000;  // 200 ms
        hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, buffer_100ns, 0, mix, nullptr);
        if (FAILED(hr)) err = HrMessage("initializing the input stream", hr);
    }
    if (err.empty()) {
        hr = client->GetService(__uuidof(IAudioCaptureClient), reinterpret_cast<void **>(&capture));
        if (FAILED(hr)) err = HrMessage("getting the capture service", hr);
    }
    if (err.empty()) {
        impl->rate = static_cast<int>(mix->nSamplesPerSec);
        impl->channels = static_cast<int>(mix->nChannels);
        hr = client->Start();
        if (FAILED(hr)) err = HrMessage("starting the input stream", hr);
    }
    const bool ok = err.empty();
    if (ok) impl->running = true;
    ready.set_value(err);

    std::vector<float> conv;
    const size_t ch = ok ? static_cast<size_t>(impl->channels) : 0;
    while (ok && impl->running.load()) {
        Sleep(10);
        UINT32 packet = 0;
        hr = capture->GetNextPacketSize(&packet);
        while (SUCCEEDED(hr) && packet > 0) {
            BYTE *data = nullptr;
            UINT32 frames = 0;
            DWORD flags = 0;
            hr = capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr);
            if (FAILED(hr)) break;
            const size_t n = static_cast<size_t>(frames) * ch;
            conv.assign(n, 0.0f);
            if (!(flags & AUDCLNT_BUFFERFLAGS_SILENT) && data) {
                if (is_float) {
                    const float *src = reinterpret_cast<const float *>(data);
                    std::copy(src, src + n, conv.begin());
                } else if (bits == 16) {
                    const int16_t *src = reinterpret_cast<const int16_t *>(data);
                    for (size_t i = 0; i < n; ++i) conv[i] = static_cast<float>(src[i]) / 32768.0f;
                } else {
                    const int32_t *src = reinterpret_cast<const int32_t *>(data);
                    for (size_t i = 0; i < n; ++i) conv[i] = static_cast<float>(static_cast<double>(src[i]) / 2147483648.0);
                }
            }
            capture->ReleaseBuffer(frames);
            if (frames && impl->callback) impl->callback(conv.data(), frames);
            hr = capture->GetNextPacketSize(&packet);
        }
        if (FAILED(hr)) {
            impl->Fail(hr == AUDCLNT_E_DEVICE_INVALIDATED ? std::string("the input device was disconnected")
                                                          : HrMessage("reading from the input device", hr));
            break;
        }
    }
    if (client && ok) client->Stop();
    if (mix) CoTaskMemFree(mix);
    SafeRelease(capture);
    SafeRelease(client);
    SafeRelease(device);
    SafeRelease(enumerator);
    if (SUCCEEDED(co)) CoUninitialize();
}

}  // namespace

Capture::Capture() : impl_(std::make_unique<Impl>()) {}
Capture::~Capture() { Stop(); }

bool Capture::Start(FrameCallback callback, std::string *error) {
    Stop();
    impl_->callback = std::move(callback);
    {
        std::lock_guard<std::mutex> lk(impl_->mu);
        impl_->failure.clear();
    }
    std::promise<std::string> ready;
    std::future<std::string> result = ready.get_future();
    impl_->thread = std::thread(WasapiThread, impl_.get(), std::move(ready));
    const std::string err = result.get();
    if (!err.empty()) {
        if (impl_->thread.joinable()) impl_->thread.join();
        if (error) *error = err;
        return false;
    }
    return true;
}

void Capture::Stop() {
    impl_->running = false;
    if (impl_->thread.joinable()) impl_->thread.join();
}

int Capture::SampleRate() const { return impl_->rate; }
int Capture::Channels() const { return impl_->channels; }
bool Capture::Failed(std::string *error) const {
    std::lock_guard<std::mutex> lk(impl_->mu);
    if (impl_->failure.empty()) return false;
    if (error) *error = impl_->failure;
    return true;
}

#elif !defined(__APPLE__)

// ALSA capture through the "default" PCM (PulseAudio/PipeWire's plugin on
// a desktop, dmix/dsnoop otherwise), mono S16 at 48 kHz -- the plug layer
// converts from whatever the hardware does, our Resampler takes it to
// 16 kHz. A reader thread waits on the PCM (100 ms timeout, so Stop()
// never hangs on a stalled device) and reads what is available.
struct Capture::Impl {
    FrameCallback callback;
    snd_pcm_t *pcm = nullptr;
    std::thread thread;
    std::atomic<bool> running{false};
    mutable std::mutex mu;
    std::string failure;
    int rate = 48000, channels = 1;

    void Fail(const std::string &msg) {
        std::lock_guard<std::mutex> lk(mu);
        if (failure.empty()) failure = msg;
    }
};

namespace {

void AlsaThread(Capture::Impl *impl) {
    constexpr snd_pcm_uframes_t kPeriod = 1024;
    std::vector<int16_t> raw(kPeriod * static_cast<size_t>(impl->channels));
    std::vector<float> conv(raw.size());
    while (impl->running.load()) {
        const int w = snd_pcm_wait(impl->pcm, 100);
        if (w == 0) continue;  // timeout: re-check running
        if (w < 0) {
            if (snd_pcm_recover(impl->pcm, w, 1) < 0) {
                impl->Fail(std::string("audio input error: ") + snd_strerror(w));
                return;
            }
            continue;
        }
        const snd_pcm_sframes_t n = snd_pcm_readi(impl->pcm, raw.data(), kPeriod);
        if (n == -EAGAIN) continue;
        if (n < 0) {
            const int rc = snd_pcm_recover(impl->pcm, static_cast<int>(n), 1);
            if (rc < 0) {
                impl->Fail(std::string("audio input error: ") + snd_strerror(rc));
                return;
            }
            continue;
        }
        const size_t samples = static_cast<size_t>(n) * static_cast<size_t>(impl->channels);
        for (size_t i = 0; i < samples; ++i) conv[i] = static_cast<float>(raw[i]) / 32768.0f;
        if (n > 0 && impl->callback) impl->callback(conv.data(), static_cast<size_t>(n));
    }
}

}  // namespace

Capture::Capture() : impl_(std::make_unique<Impl>()) {}
Capture::~Capture() { Stop(); }

bool Capture::Start(FrameCallback callback, std::string *error) {
    Stop();
    impl_->callback = std::move(callback);
    {
        std::lock_guard<std::mutex> lk(impl_->mu);
        impl_->failure.clear();
    }
    int rc = snd_pcm_open(&impl_->pcm, "default", SND_PCM_STREAM_CAPTURE, SND_PCM_NONBLOCK);
    if (rc < 0) {
        impl_->pcm = nullptr;
        if (error) *error = std::string("cannot open the default audio input: ") + snd_strerror(rc);
        return false;
    }
    rc = snd_pcm_set_params(impl_->pcm, SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED,
                            static_cast<unsigned int>(impl_->channels), static_cast<unsigned int>(impl_->rate),
                            1 /* soft resample */, 200000 /* us latency */);
    if (rc >= 0) rc = snd_pcm_prepare(impl_->pcm);
    if (rc >= 0) rc = snd_pcm_start(impl_->pcm);
    if (rc < 0) {
        snd_pcm_close(impl_->pcm);
        impl_->pcm = nullptr;
        if (error) *error = std::string("cannot configure the default audio input: ") + snd_strerror(rc);
        return false;
    }
    impl_->running = true;
    impl_->thread = std::thread(AlsaThread, impl_.get());
    return true;
}

void Capture::Stop() {
    impl_->running = false;
    if (impl_->thread.joinable()) impl_->thread.join();
    if (impl_->pcm) {
        snd_pcm_drop(impl_->pcm);
        snd_pcm_close(impl_->pcm);
        impl_->pcm = nullptr;
    }
}

int Capture::SampleRate() const { return impl_->rate; }
int Capture::Channels() const { return impl_->channels; }
bool Capture::Failed(std::string *error) const {
    std::lock_guard<std::mutex> lk(impl_->mu);
    if (impl_->failure.empty()) return false;
    if (error) *error = impl_->failure;
    return true;
}

#endif

}  // namespace mep::audio
