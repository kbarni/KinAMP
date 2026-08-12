#ifndef ICY_CLIENT_H
#define ICY_CLIENT_H

#include <stddef.h>
#include <sys/types.h>
#include <string>

// Minimal Shoutcast/Icecast HTTP client.
//
// wget hides the response headers, so a stream opened through it never learns
// its icy-metaint. This connects directly, which also gets us content-type and
// lets us follow redirects. Plain http:// only - no TLS library is linked, so
// https:// still goes through the wget fallback.

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

// Connects and does the GET, asking for metadata. On success s->fd is connected
// and positioned at the first audio byte.
bool icy_open(const std::string& url, IcyStream* s);

// Reads n audio bytes (fewer only at end of stream), eating any interleaved
// metadata blocks on the way. Returns bytes written, or -1.
ssize_t icy_read(IcyStream* s, void* dst, size_t n);

void icy_close(IcyStream* s);

// Exposed for testing.
bool icy_parse_url(const std::string& url, std::string& host, int& port, std::string& path);
std::string icy_header_value(const std::string& head, const char* name);
std::string icy_extract_title(const char* block);

#endif // ICY_CLIENT_H
