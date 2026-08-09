#ifndef ICY_CLIENT_H
#define ICY_CLIENT_H

#include <stddef.h>
#include <sys/types.h>
#include <string>

// Minimal Shoutcast/Icecast HTTP client.
//
// wget hides the response headers, so a stream opened through it can never
// learn its icy-metaint. This connects directly instead, which also yields
// content-type and lets redirects be followed. Plain http:// only - no TLS
// library is linked, so https:// still needs the wget fallback.

typedef void (*IcyTitleCallback)(const std::string& raw_title, void* user_data);

struct IcyStream {
    int fd;              // connected socket, or -1
    int metaint;         // ICY metadata interval in bytes; 0 = none negotiated
    int bytes_to_meta;   // audio bytes still to come before the next block
    std::string name;    // icy-name, when the server sent one
    std::string content_type;
    IcyTitleCallback on_title;
    void* user_data;
};

void icy_stream_init(IcyStream* s);

// Connects and performs the GET, requesting metadata. Returns true on success,
// leaving s->fd connected and positioned at the first audio byte.
bool icy_open(const std::string& url, IcyStream* s);

// Reads exactly n audio bytes (fewer only at end of stream), transparently
// consuming any interleaved metadata blocks. Returns bytes written, or -1.
ssize_t icy_read(IcyStream* s, void* dst, size_t n);

void icy_close(IcyStream* s);

// Exposed for testing.
bool icy_parse_url(const std::string& url, std::string& host, int& port, std::string& path);
std::string icy_header_value(const std::string& head, const char* name);
std::string icy_extract_title(const char* block);

#endif // ICY_CLIENT_H
