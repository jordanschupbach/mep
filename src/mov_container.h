#pragma once

// mep's own in-house, minimal QuickTime (.mov) muxer/demuxer --
// ANIMATION_VIDEO_PLAN.md Phase 3. Writes/reads a single Motion-JPEG
// video track: fixed frame rate, one JPEG sample (frame, see
// jpeg_codec.h's Encode) per chunk -- the minimal legal ISO-base-media-
// family box structure for that (ftyp/mdat/moov -> mvhd/trak ->
// tkhd/mdia -> mdhd+hdlr/minf -> vmhd+dinf+stbl -> stsd('jpeg')+stts+
// stsc+stsz+stco). No audio track, no editing lists, no B-frames.
//
// WriteMovFile produces a real, standard, externally-playable file (any
// spec-compliant reader can open it, not just OpenMovFile below) --
// confirmed independently of mep's own demuxer, same verification
// approach as jpeg_codec.h's Encode. OpenMovFile/ReadMovFrameJpeg only
// need to handle what WriteMovFile itself produces (single video track,
// one sample per chunk) -- general arbitrary .mov/.mp4 import is out of
// scope (see ANIMATION_VIDEO_PLAN.md's Non-goals).

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

// Extension-based sniff (".mov", case-insensitive), matching every other
// format's own IsXPath convention (e.g. pdf_doc.h's IsPdfPath -- global
// scope like every one of those, not inside namespace mov below, so
// Editor::LoadFile's dispatch can call it unqualified the same way it
// calls IsPdfPath/IsModel3DPath) -- used to route a `.mov` open to
// Editor::OpenVideoInPlace.
bool IsMovPath(const std::string &path);

namespace mov {

// Muxes `jpeg_frames` (each already a complete JPEG bitstream, see
// jpeg::Encode) into a Motion-JPEG .mov file at `path`. Every frame
// lasts `fps_den`/`fps_num` seconds (constant frame rate); pass
// fps_den=1 for a plain integer fps. Returns false (with `*out_error`
// set) on an empty frame list or a write failure.
bool WriteMovFile(const std::string &path, int width, int height, int fps_num, int fps_den,
                   const std::vector<std::string> &jpeg_frames, std::string *out_error);

// Parsed handle to an opened .mov file: enough to know its dimensions/
// frame rate and where each frame's JPEG bytes live in the file, without
// having read any of `mdat` (the actual frame data) into memory --
// ReadMovFrameJpeg below does that lazily, one frame at a time, which is
// what lets VideoSession (ANIMATION_VIDEO_PLAN.md Phase 5) decode only
// the frames it's actually about to display.
struct MovFile {
    int width = 0;
    int height = 0;
    double fps = 0.0;
    // frame_index[i] = {absolute file byte offset, size in bytes} of
    // frame i's raw JPEG bytes.
    std::vector<std::pair<uint64_t, uint32_t>> frame_index;
};

// Parses `path`'s box structure (moov and its descendants) into `out`,
// without reading `mdat`'s (potentially large) frame payload. Returns
// false (with `*out_error` set) if the file isn't a .mov this demuxer
// understands (see the scope note above).
bool OpenMovFile(const std::string &path, MovFile *out, std::string *out_error);

// Reads one frame's raw JPEG bytes (ready to pass to jpeg::Decode)
// directly from `path` via a seek + bounded read -- does not require
// `mov` to have been fully loaded, and never touches any other frame's
// bytes. Returns an empty vector if `frame_index` is out of range or the
// read fails.
std::vector<unsigned char> ReadMovFrameJpeg(const std::string &path, const MovFile &mov, int frame_index);

}  // namespace mov
