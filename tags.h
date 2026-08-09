#ifndef TAGS_H
#define TAGS_H

#include <string>

// Artist/title/album as stored in a file's tags. An empty string means the tag
// was absent or unreadable, in which case the caller should fall back to the
// file name.
struct AudioTags {
    std::string title;
    std::string artist;
    std::string album;
};

// Reads tags from MP3 (ID3v2.2/2.3/2.4 and ID3v1), FLAC and Ogg (Vorbis
// comments, including Opus) and WAV (RIFF INFO). M4A/M4B are handled by
// mp4read instead, not here.
//
// All returned strings are valid UTF-8. Returns true if at least one field
// could be filled in.
bool read_audio_tags(const char* filepath, AudioTags* out);

#endif // TAGS_H
