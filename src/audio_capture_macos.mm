// macOS backend of mep::audio::Capture (audio_capture.h): an AudioQueue
// input on the default input device, mono float32 at 48 kHz (the queue
// converts from the device's own format), with buffers delivered on the
// queue's internal thread (no run loop needed).
//
// Microphone access is a TCC permission. It is checked up front with
// AVCaptureDevice so a refusal is a clear error instead of a recording
// of silence (which is what an AudioQueue on a denied microphone
// delivers). The permission belongs to the *responsible* app: mep.app
// (whose Info.plist carries NSMicrophoneUsageDescription, see the
// justfile's install-app) or, for a mep started from a shell, the
// terminal.

#include "audio_capture.h"

#import <AVFoundation/AVFoundation.h>
#include <AudioToolbox/AudioToolbox.h>

#include <dlfcn.h>
#include <libproc.h>
#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

namespace mep::audio {

struct Capture::Impl {
    FrameCallback callback;
    AudioQueueRef queue = nullptr;
    std::atomic<bool> running{false};
    mutable std::mutex mu;
    std::string failure;
    int rate = 48000, channels = 1;
};

namespace {

constexpr UInt32 kBufferFrames = 2048;  // ~43 ms at 48 kHz
constexpr int kBufferCount = 4;

void InputCallback(void *user, AudioQueueRef queue, AudioQueueBufferRef buffer, const AudioTimeStamp *, UInt32,
                   const AudioStreamPacketDescription *) {
    auto *impl = static_cast<Capture::Impl *>(user);
    if (!impl->running.load()) return;
    const size_t frames = buffer->mAudioDataByteSize / (sizeof(float) * static_cast<size_t>(impl->channels));
    if (frames > 0 && impl->callback) impl->callback(static_cast<const float *>(buffer->mAudioData), frames);
    if (impl->running.load()) AudioQueueEnqueueBuffer(queue, buffer, 0, nullptr);
}

// The queue stopping on its own while we still want it running means the
// device went away (unplugged, switched off).
void RunningChanged(void *user, AudioQueueRef queue, AudioQueuePropertyID) {
    auto *impl = static_cast<Capture::Impl *>(user);
    UInt32 is_running = 0, size = sizeof is_running;
    if (AudioQueueGetProperty(queue, kAudioQueueProperty_IsRunning, &is_running, &size) != noErr) return;
    if (!is_running && impl->running.load()) {
        std::lock_guard<std::mutex> lk(impl->mu);
        if (impl->failure.empty()) impl->failure = "the audio input stopped (device disconnected?)";
    }
}

std::string StatusText(const char *what, OSStatus status) {
    char buf[160];
    std::snprintf(buf, sizeof buf, "%s failed (OSStatus %d)", what, static_cast<int>(status));
    return buf;
}

// TCC *kills* a process whose responsible app has no
// NSMicrophoneUsageDescription the moment it would prompt (an "app has
// crashed because it attempted to access privacy-sensitive data" report),
// so before anything triggers the prompt, find the responsible app and
// check its Info.plist. Returns false with a message when the key is
// missing; true when present or when this cannot be determined (the
// lookup goes through libsystem's responsibility_get_pid_responsible_for_pid,
// looked up at runtime since it is not in the public headers).
bool ResponsibleAppCanPrompt(std::string *error) {
    using ResponsibleFn = pid_t (*)(pid_t);
    void *sym = dlsym(RTLD_DEFAULT, "responsibility_get_pid_responsible_for_pid");
    if (!sym) return true;
    const pid_t responsible = reinterpret_cast<ResponsibleFn>(sym)(getpid());
    if (responsible <= 0) return true;
    char path_buf[PROC_PIDPATHINFO_MAXSIZE];
    if (proc_pidpath(responsible, path_buf, sizeof path_buf) <= 0) return true;
    const std::string exe = path_buf;
    // Inside a bundle: <bundle>.app/Contents/MacOS/<exe> -- read the bundle's
    // Info.plist. A bare executable may carry an embedded __info_plist, which
    // CFBundleCopyInfoDictionaryForURL reads from the file itself.
    const size_t macos = exe.rfind("/Contents/MacOS/");
    const std::string target = macos == std::string::npos ? exe : exe.substr(0, macos);
    NSURL *url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:target.c_str()]];
    NSDictionary *info = CFBridgingRelease(CFBundleCopyInfoDictionaryForURL((__bridge CFURLRef)url));
    if (info[@"NSMicrophoneUsageDescription"] != nil) return true;
    if (error)
        *error = "cannot ask for microphone access: mep's responsible app (" + target +
                 ") has no NSMicrophoneUsageDescription in its Info.plist, and macOS would kill mep for asking" +
                 (target.find("mep.app") != std::string::npos ? " -- reinstall it with `just install-app`" : "");
    return false;
}

// Returns false with a user-facing message when microphone access is
// refused. Asks (and waits for the user's answer) when it was never asked.
bool CheckMicrophonePermission(std::string *error) {
    if (@available(macOS 10.14, *)) {
        AVAuthorizationStatus status = [AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeAudio];
        if (status == AVAuthorizationStatusNotDetermined) {
            if (!ResponsibleAppCanPrompt(error)) return false;
            dispatch_semaphore_t done = dispatch_semaphore_create(0);
            __block BOOL granted = NO;
            [AVCaptureDevice requestAccessForMediaType:AVMediaTypeAudio
                                     completionHandler:^(BOOL ok) {
                                       granted = ok;
                                       dispatch_semaphore_signal(done);
                                     }];
            const long timed_out =
                dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, static_cast<int64_t>(120 * NSEC_PER_SEC)));
            if (timed_out != 0) {
                if (error) *error = "microphone permission prompt was not answered";
                return false;
            }
            status = granted ? AVAuthorizationStatusAuthorized : AVAuthorizationStatusDenied;
        }
        if (status == AVAuthorizationStatusDenied) {
            if (error)
                *error = "microphone access denied -- allow mep in System Settings > Privacy & Security > "
                         "Microphone (for a mep started from a terminal, allow the terminal app), then retry";
            return false;
        }
        if (status == AVAuthorizationStatusRestricted) {
            if (error) *error = "microphone access is restricted by a system policy (Screen Time / MDM)";
            return false;
        }
    }
    return true;
}

}  // namespace

Capture::Capture() : impl_(std::make_unique<Impl>()) {}
Capture::~Capture() { Stop(); }

bool Capture::Start(FrameCallback callback, std::string *error) {
    Stop();
    {
        std::lock_guard<std::mutex> lk(impl_->mu);
        impl_->failure.clear();
    }
    if (!CheckMicrophonePermission(error)) return false;
    impl_->callback = std::move(callback);

    AudioStreamBasicDescription f = {};
    f.mSampleRate = impl_->rate;
    f.mFormatID = kAudioFormatLinearPCM;
    f.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
    f.mChannelsPerFrame = static_cast<UInt32>(impl_->channels);
    f.mBitsPerChannel = 32;
    f.mBytesPerFrame = static_cast<UInt32>(sizeof(float)) * f.mChannelsPerFrame;
    f.mFramesPerPacket = 1;
    f.mBytesPerPacket = f.mBytesPerFrame;

    OSStatus st = AudioQueueNewInput(&f, InputCallback, impl_.get(), nullptr, nullptr, 0, &impl_->queue);
    if (st != noErr) {
        impl_->queue = nullptr;
        if (error) *error = StatusText("opening the default audio input (is there a microphone?)", st);
        return false;
    }
    AudioQueueAddPropertyListener(impl_->queue, kAudioQueueProperty_IsRunning, RunningChanged, impl_.get());
    for (int i = 0; i < kBufferCount; ++i) {
        AudioQueueBufferRef buf = nullptr;
        st = AudioQueueAllocateBuffer(impl_->queue, kBufferFrames * f.mBytesPerFrame, &buf);
        if (st == noErr) st = AudioQueueEnqueueBuffer(impl_->queue, buf, 0, nullptr);
        if (st != noErr) break;
    }
    impl_->running = true;
    if (st == noErr) st = AudioQueueStart(impl_->queue, nullptr);
    if (st != noErr) {
        impl_->running = false;
        AudioQueueDispose(impl_->queue, true);
        impl_->queue = nullptr;
        if (error) *error = StatusText("starting the audio input", st);
        return false;
    }
    return true;
}

void Capture::Stop() {
    impl_->running = false;
    if (impl_->queue) {
        // Both synchronous: once they return, no InputCallback is running
        // or will run.
        AudioQueueStop(impl_->queue, true);
        AudioQueueDispose(impl_->queue, true);
        impl_->queue = nullptr;
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

}  // namespace mep::audio
