#include "icy_client.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netdb.h>
#include <algorithm>

static const int ICY_CONNECT_TIMEOUT = 5;
static const int ICY_READ_TIMEOUT    = 15;
static const int ICY_MAX_REDIRECTS   = 5;

void icy_stream_init(IcyStream* s) {
    s->fd = -1;
    s->metaint = 0;
    s->bytes_to_meta = 0;
    s->name.clear();
    s->content_type.clear();
    s->on_title = NULL;
    s->user_data = NULL;
}

bool icy_parse_url(const std::string& url, std::string& host, int& port, std::string& path) {
    if (url.size() < 7 || url.compare(0, 7, "http://") != 0) return false;
    std::string rest = url.substr(7);

    size_t slash = rest.find('/');
    std::string authority = (slash == std::string::npos) ? rest : rest.substr(0, slash);
    path = (slash == std::string::npos) ? "/" : rest.substr(slash);
    if (authority.empty()) return false;

    // Strip any userinfo@ prefix.
    size_t at = authority.find('@');
    if (at != std::string::npos) authority = authority.substr(at + 1);

    port = 80;
    size_t colon = authority.find_last_of(':');
    if (colon != std::string::npos && authority.find(']') == std::string::npos) {
        port = atoi(authority.c_str() + colon + 1);
        if (port <= 0 || port > 65535) return false;
        host = authority.substr(0, colon);
    } else {
        host = authority;
    }
    return !host.empty();
}

static int tcp_connect(const std::string& host, int port, int timeout_sec) {
    char portstr[16];
    snprintf(portstr, sizeof(portstr), "%d", port);

    struct addrinfo hints;
    struct addrinfo* res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    if (getaddrinfo(host.c_str(), portstr, &hints, &res) != 0) return -1;

    int sock = -1;
    for (struct addrinfo* ai = res; ai != NULL; ai = ai->ai_next) {
        sock = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (sock < 0) continue;

        int flags = fcntl(sock, F_GETFL, 0);
        fcntl(sock, F_SETFL, flags | O_NONBLOCK);

        if (connect(sock, ai->ai_addr, ai->ai_addrlen) == 0) {
            fcntl(sock, F_SETFL, flags);
            break;
        }
        if (errno != EINPROGRESS) {
            close(sock);
            sock = -1;
            continue;
        }

        fd_set wfds;
        FD_ZERO(&wfds);
        FD_SET(sock, &wfds);
        struct timeval tv;
        tv.tv_sec = timeout_sec;
        tv.tv_usec = 0;

        int err = 1;
        if (select(sock + 1, NULL, &wfds, NULL, &tv) > 0) {
            socklen_t len = sizeof(err);
            if (getsockopt(sock, SOL_SOCKET, SO_ERROR, &err, &len) != 0) err = 1;
        }
        if (err == 0) {
            fcntl(sock, F_SETFL, flags);
            break;
        }
        close(sock);
        sock = -1;
    }
    freeaddrinfo(res);

    if (sock >= 0) {
        struct timeval tv;
        tv.tv_sec = ICY_READ_TIMEOUT;
        tv.tv_usec = 0;
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    }
    return sock;
}

// Reads the response head one byte at a time. Slower than block reads, but it
// stops exactly at the terminator, so no audio is ever swallowed into a buffer
// the decoder cannot see.
static bool read_response_head(int sock, std::string& head) {
    head.clear();
    while (head.size() < 16384) {
        char c;
        ssize_t n = read(sock, &c, 1);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        head.push_back(c);

        size_t s = head.size();
        if (s >= 4 && head.compare(s - 4, 4, "\r\n\r\n") == 0) return true;
        if (s >= 2 && head.compare(s - 2, 2, "\n\n") == 0) return true;
    }
    return false;
}

std::string icy_header_value(const std::string& head, const char* name) {
    std::string lower_head = head;
    std::transform(lower_head.begin(), lower_head.end(), lower_head.begin(), ::tolower);
    std::string key = name;
    std::transform(key.begin(), key.end(), key.begin(), ::tolower);

    size_t pos = 0;
    while (pos < lower_head.size()) {
        size_t eol = lower_head.find('\n', pos);
        if (eol == std::string::npos) eol = lower_head.size();

        if (lower_head.compare(pos, key.size(), key) == 0) {
            size_t colon = lower_head.find(':', pos);
            if (colon != std::string::npos && colon < eol) {
                std::string v = head.substr(colon + 1, eol - colon - 1);
                size_t b = v.find_first_not_of(" \t\r\n");
                size_t e = v.find_last_not_of(" \t\r\n");
                if (b == std::string::npos) return "";
                return v.substr(b, e - b + 1);
            }
        }
        pos = eol + 1;
    }
    return "";
}

// Legacy Shoutcast answers "ICY 200 OK" instead of a real HTTP status line.
static int response_status(const std::string& head) {
    size_t sp = head.find(' ');
    if (sp == std::string::npos) return -1;
    return atoi(head.c_str() + sp + 1);
}

static bool icy_open_depth(const std::string& url, IcyStream* s, int depth) {
    if (depth > ICY_MAX_REDIRECTS) return false;

    std::string host, path;
    int port = 80;
    if (!icy_parse_url(url, host, port, path)) return false;

    int sock = tcp_connect(host, port, ICY_CONNECT_TIMEOUT);
    if (sock < 0) return false;

    char request[2048];
    int len = snprintf(request, sizeof(request),
                       "GET %s HTTP/1.0\r\n"
                       "Host: %s:%d\r\n"
                       "User-Agent: KinAMP/1.0\r\n"
                       "Icy-MetaData: 1\r\n"
                       "Accept: */*\r\n"
                       "Connection: close\r\n"
                       "\r\n",
                       path.c_str(), host.c_str(), port);
    if (len <= 0 || len >= (int)sizeof(request)) {
        close(sock);
        return false;
    }

    ssize_t sent = 0;
    while (sent < len) {
        ssize_t n = write(sock, request + sent, len - sent);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) {
            close(sock);
            return false;
        }
        sent += n;
    }

    std::string head;
    if (!read_response_head(sock, head)) {
        close(sock);
        return false;
    }

    int status = response_status(head);
    if (status >= 300 && status < 400) {
        std::string location = icy_header_value(head, "location");
        close(sock);
        if (location.empty()) return false;
        if (location.compare(0, 7, "http://") != 0) {
            // Anything else (notably https://) needs the wget fallback.
            fprintf(stderr, "ICY: redirect to non-plain-HTTP target\n");
            return false;
        }
        return icy_open_depth(location, s, depth + 1);
    }

    // ICY 200 and HTTP 200 both land here; anything else is a failure.
    if (status != 200) {
        fprintf(stderr, "ICY: stream returned status %d\n", status);
        close(sock);
        return false;
    }

    std::string mi = icy_header_value(head, "icy-metaint");
    if (!mi.empty()) {
        int v = atoi(mi.c_str());
        if (v > 0 && v <= (1 << 22)) s->metaint = v;
    }

    s->fd = sock;
    s->bytes_to_meta = s->metaint;
    s->name = icy_header_value(head, "icy-name");
    s->content_type = icy_header_value(head, "content-type");
    return true;
}

bool icy_open(const std::string& url, IcyStream* s) {
    s->fd = -1;
    s->metaint = 0;
    s->bytes_to_meta = 0;
    s->name.clear();
    s->content_type.clear();
    return icy_open_depth(url, s, 0);
}

static bool read_exact(int fd, void* buf, size_t n) {
    size_t got = 0;
    while (got < n) {
        ssize_t r = read(fd, (char*)buf + got, n - got);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return false;
        got += r;
    }
    return true;
}

// StreamTitle='...';StreamUrl='...';  - quoting is unescaped in practice, so
// prefer the "';" terminator and only fall back to the last quote.
std::string icy_extract_title(const char* block) {
    const char* key = "StreamTitle='";
    const char* p = strstr(block, key);
    if (!p) return "";
    p += strlen(key);

    const char* end = strstr(p, "';");
    if (!end) {
        end = strrchr(p, '\'');
        if (!end || end < p) return "";
    }
    return std::string(p, end - p);
}

// Reads one metadata block. The length byte counts 16-byte units, and is zero
// on the vast majority of intervals because the title rarely changes.
static bool consume_metadata(IcyStream* s) {
    unsigned char len_byte = 0;
    if (!read_exact(s->fd, &len_byte, 1)) return false;
    if (len_byte == 0) return true;

    size_t n = (size_t)len_byte * 16; // max 255 * 16 = 4080
    char block[4080 + 1];
    if (!read_exact(s->fd, block, n)) return false;
    block[n] = '\0';

    std::string title = icy_extract_title(block);
    if (!title.empty() && s->on_title) {
        s->on_title(title, s->user_data);
    }
    return true;
}

ssize_t icy_read(IcyStream* s, void* dst, size_t n) {
    unsigned char* out = (unsigned char*)dst;
    size_t total = 0;

    // Return as soon as any audio is available rather than blocking until the
    // caller's buffer is full: on a live stream, filling a large buffer would
    // add seconds of latency. Looping only while total == 0 keeps the timing
    // behaviour of the original pipe-based reader, while still guaranteeing a
    // zero return means end of stream and never leaking a metadata block.
    while (total == 0) {
        if (s->metaint > 0 && s->bytes_to_meta == 0) {
            if (!consume_metadata(s)) break;
            s->bytes_to_meta = s->metaint;
        }

        size_t want = n;
        if (s->metaint > 0 && want > (size_t)s->bytes_to_meta) {
            want = (size_t)s->bytes_to_meta;
        }
        if (want == 0) break;

        ssize_t r = read(s->fd, out, want);
        if (r < 0 && errno == EINTR) continue;
        if (r < 0) return -1;
        if (r == 0) break; // stream ended

        total = (size_t)r;
        if (s->metaint > 0) s->bytes_to_meta -= (int)r;
    }

    return (ssize_t)total;
}

void icy_close(IcyStream* s) {
    if (s->fd >= 0) {
        close(s->fd);
        s->fd = -1;
    }
}
