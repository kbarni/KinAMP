#ifndef TAGS_H
#define TAGS_H

#include <string>

// Artist/title/album as stored in a file's tags. An empty string means the tag
// was absent or unreadable, in which case the caller should fall back to the
// file name. duration_seconds is 0 when the length could not be determined.
struct AudioTags {
    std::string title;
    std::string artist;
    std::string album;
    int duration_seconds;

    AudioTags() : duration_seconds(0) {}
};

// Reads tags from MP3 (ID3v2.2/2.3/2.4 and ID3v1), FLAC and Ogg (Vorbis
// comments, including Opus) and WAV (RIFF INFO). M4A/M4B are handled by
// mp4read instead, not here.
//
// Also fills in duration_seconds, from the file's headers alone: no audio is
// decoded, so the cost is a handful of seeks even on a large file. MP3s without
// a Xing/VBRI header fall back to a constant bitrate estimate, which is exact
// for CBR and approximate for VBR.
//
// All returned strings are valid UTF-8. Returns true if at least one text tag
// could be filled in; duration_seconds is set independently of that, so a file
// with no tags at all can still come back with a length.
bool read_audio_tags(const char* filepath, AudioTags* out);

#endif // TAGS_H
