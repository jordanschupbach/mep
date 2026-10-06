# YouTube player pane

An in-pane YouTube player (`Mode::Youtube`, 2026-10-05): search, pick and
play videos without leaving mep, picture and sound drawn by mep itself.

## Shape

- `src/youtube_player.{h,cpp}` (mep_core, editor-free, unit-tested by
  `mep-youtube-player-test`): yt-dlp JSON parsing (`ParseSearchLine`,
  `ParseStreamInfo`), argv builders (`SearchArgv`, `ResolveArgv`,
  `VideoDecodeArgv`, `AudioDecodeArgv`), `FrameAssembler` (fixed-size
  frames out of arbitrary pipe chunks), formatting helpers.
- `YoutubeSession` (editor.h) + the `Youtube*` methods in editor.cpp:
  three JobManager stages, all callbacks on the main thread --
  1. search: `yt-dlp "ytsearchN:query" --flat-playlist -j`, one result per
     stdout line; thumbnails fetched one at a time with `curl` and decoded
     with `jpeg::Decode`;
  2. resolve: `yt-dlp -j -f <480p prefs> --extractor-args
     youtube:player_client=android URL` -> `yt::StreamInfo` (URLs plus the
     `http_headers` they are bound to);
  3. decode: two `ffmpeg` jobs, raw RGBA frames (`-pix_fmt rgba -f
     rawvideo`, cfr at the stream's fps) and PCM16 (`-f s16le`), each on its
     own raw-stdout pipe with `should_poll_raw` as backpressure (<= 6 frames
     / <= 2 s of audio buffered in the session; the Job's 8 MB cap and the
     kernel pipe do the rest, so ffmpeg never runs ahead).
- Streaming audio: `gfx::OpenAudioStream` / `PushAudioStream` /
  `AudioStreamPlayedSeconds` (new on `IAudioBackend`, ALSA thread in
  `backend_native_audio.cpp`). The played-seconds clock (frames written -
  device delay - starvation silence) paces the video: a frame is shown when
  its pts (`start_sec + n / fps`) is reached, older due frames are dropped.
  Without a device/track, wall time.
- Seek = restart both decoders at `-ss T` (input-side seek, an HTTP range
  request on the stream URL) and reopen the audio stream. Pause = stop
  feeding the device and stop draining the pipes.
- `DrawYoutubePane` (main.cpp): video (letterboxed, one reused texture
  updated in place), transport bar (prev/play/next, mute + volume slider,
  elapsed/total, title, click-to-seek progress), result list (thumbnail,
  title, channel / views, duration badge), empty-state hints. The tab bar
  has a YouTube button next to the activity buttons (`:MepYoutube`).
- Commands: `:youtube [query|url|id]`, `:yt`, `:MepYoutube` (toggle, also
  `<leader>yt` and the tab-bar button). The toggle *hides* an on-screen
  player (`Editor::HideBufferInActiveTab`, buffer and session kept) and
  `Editor::YoutubePollOffscreen` (main loop, after `JobManager::PollAll`)
  keeps feeding every player no pane draws, so it plays on unseen; the
  next toggle splits beside the current pane and reuses the buffer.
  Stopping is `q`/`x`/`:bd`. Lua: `mep.youtube_toggle/search/play/state`
  (`state().visible`). Help: `help/youtube.org`.

## Things learned live

- The default yt-dlp web clients hand out googlevideo URLs that 403 for
  anything but yt-dlp itself (PO-token bound) -- even with the exact
  `http_headers`. `player_client=android` URLs work with a plain ffmpeg GET
  (with the User-Agent header), including `-ss` range seeks. mweb also
  worked; web/ios/tv did not.
- A SIGTERM'd ffmpeg mid-stream can linger for seconds to minutes, still
  decoding: its graceful shutdown waits on the blocked pipe write. The
  decoders are killed with SIGKILL (`JobManager::KillHard`) -- nothing of
  ours to flush -- and spawned `die_with_parent` so a dead mep takes them
  along.
- On Xvfb the CPU is Mesa's llvmpipe software rasterizer (12 threads at
  ~30% each while anything animates), not the player; mep's own main thread
  sits around 50% there at full frame rate, ffmpeg at 10-15% for 360p.
- `ui.key_press` sends unshifted keysyms, so `H`/`L` (minute seeks) can't be
  exercised over the agent socket; `h`/`l` reach the handler fine.
- A raw-stdout job that finished while its consumer's `should_poll_raw`
  was false used to be erased by `JobManager::PollAll` with its tail
  unread: the audio ffmpeg for a 19 s video exits in under a second, the
  player holds ~2 s of PCM back, and the other ~15 s vanished (the clock
  then froze at ~3.5 s). PollAll now holds the exit report until
  `Job::HasPendingRaw()` is false -- this also fixes the end of every
  longer video (the last <= 8 MB of PCM was at risk the same way).
- Inside the pane the leader beats Space (unlike the video/music panes):
  `<leader>yt` must work where the user lands after opening or picking a
  video. Pause with the default Space leader is Enter on the playing
  result; `i` hides like YouTube's miniplayer key.
