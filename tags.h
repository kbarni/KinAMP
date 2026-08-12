#ifndef TAGS_H
#define TAGS_H

#include <string>

// Artist/title/album from a file's tags. An empty string means the tag was
// absent or unreadable, so fall back to the file name. duration_seconds is 0
// when the length couldn't be determined.
struct AudioTags {
    std::string title;
    std::string artist;
    std::string album;
    int duration_seconds;

    AudioTags() : duration_seconds(0) {}
};

// Reads tags from MP3 (ID3v2.2/2.3/2.4 and ID3v1), FLAC and Ogg (Vorbis
// comments, Opus included) and WAV (RIFF INFO). M4A/M4B go through mp4read, not
// here. Strings come back as valid UTF-8.
//
// duration_seconds is filled from the headers alone - no audio is decoded, so
// even a large file costs a handful of seeks. MP3s without a Xing/VBRI header
// fall back to a constant bitrate estimate: exact for CBR, approximate for VBR.
//
// True if at least one text tag was filled in. duration_seconds is set
// independently, so a file with no tags at all can still come back with a length.
bool read_audio_tags(const char* filepath, AudioTags* out);

#endif // TAGS_H
