#include "tags.h"

#include <glib.h>
#include <stdio.h>
#include <string.h>
#include <vector>

// Tag parsing runs against whatever happens to sit on the Kindle's storage, so
// every length field read from a file is treated as hostile: sizes are clamped
// and every access is bounds checked. A corrupt tag must degrade to "no tags",
// never to a huge allocation or a crash.

namespace {

const size_t MAX_TAG_BYTES = 1024u * 1024;  // upper bound for any single read
const size_t MAX_OGG_SCAN  = 512u * 1024;   // enough for the first few Ogg pages
const size_t MAX_LIST_SCAN = 64u * 1024;    // RIFF LIST/INFO chunk
const int    MAX_BLOCKS    = 64;            // metadata blocks / RIFF chunks walked

typedef std::vector<unsigned char> Buffer;

unsigned be24(const unsigned char* p) {
    return ((unsigned)p[0] << 16) | ((unsigned)p[1] << 8) | (unsigned)p[2];
}

unsigned be32(const unsigned char* p) {
    return ((unsigned)p[0] << 24) | ((unsigned)p[1] << 16) |
           ((unsigned)p[2] << 8)  |  (unsigned)p[3];
}

unsigned le32(const unsigned char* p) {
    return ((unsigned)p[3] << 24) | ((unsigned)p[2] << 16) |
           ((unsigned)p[1] << 8)  |  (unsigned)p[0];
}

// ID3 stores sizes with the high bit of every byte cleared, so that a size can
// never look like an MPEG sync word.
unsigned syncsafe32(const unsigned char* p) {
    return ((unsigned)(p[0] & 0x7F) << 21) | ((unsigned)(p[1] & 0x7F) << 14) |
           ((unsigned)(p[2] & 0x7F) << 7)  |  (unsigned)(p[3] & 0x7F);
}

// Reads up to len bytes at offset. The buffer is shrunk to what was actually
// read, so a truncated file yields a short buffer rather than a failure.
bool read_at(FILE* f, long offset, size_t len, Buffer* out) {
    if (len == 0 || len > MAX_TAG_BYTES) return false;
    if (fseek(f, offset, SEEK_SET) != 0) return false;
    out->resize(len);
    size_t got = fread(&(*out)[0], 1, len, f);
    out->resize(got);
    return got > 0;
}

// Cuts a single byte string at its first NUL. ID3 strings are NUL terminated,
// ID3v1 fields are NUL padded, and an ID3v2.4 frame packs its extra values
// behind the terminator; we only ever want the first one.
size_t nul_trim(const char* data, size_t len) {
    const void* nul = memchr(data, '\0', len);
    return (nul != NULL) ? (size_t)((const char*)nul - data) : len;
}

// Converts raw tag bytes to UTF-8 and trims surrounding whitespace. Anything
// that cannot be interpreted comes back empty, so a garbled tag falls through
// to the next source instead of putting mojibake on the label.
//
// A NULL charset means the bytes are already UTF-8. The two byte encodings keep
// their embedded NULs through the conversion and are cut by building the
// std::string from a C string, which stops at the first one.
std::string to_utf8(const char* data, size_t len, const char* from_charset) {
    if (data == NULL || len == 0) return "";

    gchar* conv = NULL;
    if (from_charset == NULL) {
        len = nul_trim(data, len);
        if (len == 0) return "";
        if (!g_utf8_validate(data, (gssize)len, NULL)) return "";
        conv = g_strndup(data, len);
    } else {
        conv = g_convert(data, (gssize)len, "UTF-8", from_charset, NULL, NULL, NULL);
    }
    if (conv == NULL) return "";

    std::string result(g_strstrip(conv));
    g_free(conv);
    return result;
}

// For the fields whose spec says latin1 (ID3v1, ID3v2 encoding 0, RIFF INFO)
// but which taggers routinely fill with UTF-8 anyway. Latin1 accented bytes are
// almost never valid UTF-8, so preferring UTF-8 when it validates picks the
// right one in practice.
std::string to_utf8_lenient(const char* data, size_t len) {
    if (data == NULL || len == 0) return "";

    size_t trimmed = nul_trim(data, len);
    std::string as_utf8 = to_utf8(data, trimmed, NULL);
    if (!as_utf8.empty()) return as_utf8;
    return to_utf8(data, trimmed, "ISO-8859-1");
}

bool key_equals(const char* key, size_t len, const char* name) {
    return strlen(name) == len && g_ascii_strncasecmp(key, name, len) == 0;
}

bool needs_more(const AudioTags& tags) {
    return tags.title.empty() || tags.artist.empty() || tags.album.empty();
}

// ---------------------------------------------------------------------------
// Vorbis comments (FLAC and Ogg share this format verbatim)
// ---------------------------------------------------------------------------

// Stops cleanly on a truncated buffer, keeping whatever was found so far. That
// matters for Ogg, where the comment packet may run past our scan window.
void parse_vorbis_comment(const unsigned char* p, size_t len, AudioTags* out) {
    if (len < 8) return;

    size_t pos = 4;
    unsigned vendor_len = le32(p);
    if (vendor_len > len - pos) return;
    pos += vendor_len;

    if (pos + 4 > len) return;
    unsigned count = le32(p + pos);
    pos += 4;
    if (count > 4096) count = 4096;

    for (unsigned i = 0; i < count && pos + 4 <= len; ++i) {
        unsigned clen = le32(p + pos);
        pos += 4;
        if (clen > len - pos) return;

        const char* comment = (const char*)p + pos;
        const char* eq = (const char*)memchr(comment, '=', clen);
        if (eq != NULL) {
            size_t klen = (size_t)(eq - comment);
            const char* value = eq + 1;
            size_t vlen = clen - klen - 1;

            std::string* field = NULL;
            if (key_equals(comment, klen, "TITLE"))       field = &out->title;
            else if (key_equals(comment, klen, "ARTIST")) field = &out->artist;
            else if (key_equals(comment, klen, "ALBUM"))  field = &out->album;

            if (field != NULL && field->empty()) {
                *field = to_utf8(value, vlen, NULL);
            }
        }
        pos += clen;
    }
}

// ---------------------------------------------------------------------------
// ID3v2 / ID3v1 (MP3)
// ---------------------------------------------------------------------------

struct Id3Header {
    unsigned major;
    unsigned flags;
    size_t body_size;   // bytes following the 10 byte header, footer excluded
    size_t total_size;  // header + body + footer
};

bool read_id3_header(FILE* f, Id3Header* h) {
    unsigned char hdr[10];
    if (fseek(f, 0, SEEK_SET) != 0) return false;
    if (fread(hdr, 1, sizeof(hdr), f) != sizeof(hdr)) return false;
    if (memcmp(hdr, "ID3", 3) != 0) return false;
    if (hdr[3] == 0xFF || hdr[4] == 0xFF) return false;

    h->major = hdr[3];
    h->flags = hdr[5];
    h->body_size = syncsafe32(hdr + 6);
    // The footer flag only exists in 2.4; earlier versions leave the bit clear.
    h->total_size = 10 + h->body_size + ((h->flags & 0x10) ? 10 : 0);
    return true;
}

// Undoes the unsynchronisation scheme: every 0xFF 0x00 pair becomes 0xFF.
void remove_unsync(Buffer* buf) {
    size_t w = 0;
    for (size_t r = 0; r < buf->size(); ++r) {
        (*buf)[w++] = (*buf)[r];
        if ((*buf)[r] == 0xFF && r + 1 < buf->size() && (*buf)[r + 1] == 0x00) ++r;
    }
    buf->resize(w);
}

std::string decode_text_frame(const unsigned char* p, size_t len) {
    if (len < 1) return "";

    const char* data = (const char*)p + 1;
    size_t dlen = len - 1;
    switch (p[0]) {
        case 0:  return to_utf8_lenient(data, dlen);      // nominally latin1
        case 1:  return to_utf8(data, dlen, "UTF-16");    // BOM prefixed
        case 2:  return to_utf8(data, dlen, "UTF-16BE");
        case 3:  return to_utf8(data, dlen, NULL);        // already UTF-8
        default: return "";
    }
}

void parse_id3v2(FILE* f, AudioTags* out) {
    Id3Header h;
    if (!read_id3_header(f, &h)) return;
    if (h.major < 2 || h.major > 4) return;

    Buffer body;
    if (!read_at(f, 10, h.body_size, &body)) return;

    // In 2.2 and 2.3 unsynchronisation applies to the whole tag; 2.4 moved it to
    // a per frame flag.
    if (h.major < 4 && (h.flags & 0x80)) remove_unsync(&body);

    size_t pos = 0;
    if (h.flags & 0x40) {
        if (body.size() < 4) return;
        // 2.3 stores the extended header size excluding itself, 2.4 includes it
        // and uses a syncsafe integer.
        size_t ext = (h.major >= 4) ? syncsafe32(&body[0]) : (size_t)be32(&body[0]) + 4;
        if (ext > body.size()) return;
        pos = ext;
    }

    const size_t id_len  = (h.major == 2) ? 3 : 4;
    const size_t hdr_len = (h.major == 2) ? 6 : 10;

    while (pos + hdr_len <= body.size()) {
        const unsigned char* fh = &body[pos];
        if (fh[0] == 0) break;  // padding starts here

        size_t fsize;
        unsigned fflags = 0;
        if (h.major == 2) {
            fsize = be24(fh + 3);
        } else if (h.major == 3) {
            fsize = be32(fh + 4);
            fflags = fh[9];
        } else {
            // Plenty of taggers write 2.4 frame sizes as plain big endian. A byte
            // with the high bit set proves the field is not syncsafe.
            bool syncsafe = (fh[4] | fh[5] | fh[6] | fh[7]) < 0x80;
            fsize = syncsafe ? syncsafe32(fh + 4) : be32(fh + 4);
            fflags = fh[9];
        }
        pos += hdr_len;
        if (fsize == 0 || fsize > body.size() - pos) break;

        std::string id((const char*)fh, id_len);
        std::string* field = NULL;
        if (id == "TIT2" || id == "TT2")      field = &out->title;
        else if (id == "TPE1" || id == "TP1") field = &out->artist;
        else if (id == "TALB" || id == "TAL") field = &out->album;

        // Compressed or encrypted frames would need zlib and a key respectively.
        bool opaque = (h.major == 3 && (fflags & 0xC0)) ||
                      (h.major == 4 && (fflags & 0x0C));

        if (field != NULL && field->empty() && !opaque) {
            Buffer frame(body.begin() + pos, body.begin() + pos + fsize);
            if (h.major == 4 && (fflags & 0x02)) remove_unsync(&frame);

            size_t skip = 0;
            if (h.major == 3 && (fflags & 0x20)) skip += 1;  // group identity
            if (h.major == 4 && (fflags & 0x40)) skip += 1;
            if (h.major == 4 && (fflags & 0x01)) skip += 4;  // data length indicator

            if (skip < frame.size()) {
                *field = decode_text_frame(&frame[skip], frame.size() - skip);
            }
        }
        pos += fsize;
    }
}

void parse_id3v1(FILE* f, AudioTags* out) {
    unsigned char tag[128];
    if (fseek(f, -128L, SEEK_END) != 0) return;
    if (fread(tag, 1, sizeof(tag), f) != sizeof(tag)) return;
    if (memcmp(tag, "TAG", 3) != 0) return;

    if (out->title.empty())  out->title  = to_utf8_lenient((const char*)tag + 3,  30);
    if (out->artist.empty()) out->artist = to_utf8_lenient((const char*)tag + 33, 30);
    if (out->album.empty())  out->album  = to_utf8_lenient((const char*)tag + 63, 30);
}

// ---------------------------------------------------------------------------
// FLAC
// ---------------------------------------------------------------------------

void parse_flac(FILE* f, long offset, AudioTags* out) {
    long pos = offset + 4;  // caller already matched the "fLaC" marker

    for (int i = 0; i < MAX_BLOCKS; ++i) {
        unsigned char bh[4];
        if (fseek(f, pos, SEEK_SET) != 0) return;
        if (fread(bh, 1, sizeof(bh), f) != sizeof(bh)) return;

        unsigned type = bh[0] & 0x7F;
        bool last = (bh[0] & 0x80) != 0;
        size_t blen = be24(bh + 1);
        pos += 4;

        if (type == 4) {  // VORBIS_COMMENT
            Buffer block;
            if (read_at(f, pos, blen, &block) && !block.empty()) {
                parse_vorbis_comment(&block[0], block.size(), out);
            }
            return;
        }
        if (last) return;
        pos += (long)blen;
    }
}

// ---------------------------------------------------------------------------
// Ogg (Vorbis, Opus and FLAC-in-Ogg)
// ---------------------------------------------------------------------------

void parse_ogg_comment_packet(const Buffer& packet, bool ogg_flac, AudioTags* out) {
    if (ogg_flac) {
        // The packet carries a FLAC metadata block, header included.
        if (packet.size() > 4 && (packet[0] & 0x7F) == 4) {
            parse_vorbis_comment(&packet[4], packet.size() - 4, out);
        }
    } else if (packet.size() > 7 && packet[0] == 0x03 &&
               memcmp(&packet[1], "vorbis", 6) == 0) {
        parse_vorbis_comment(&packet[7], packet.size() - 7, out);
    } else if (packet.size() > 8 && memcmp(&packet[0], "OpusTags", 8) == 0) {
        parse_vorbis_comment(&packet[8], packet.size() - 8, out);
    }
}

// Walks Ogg pages looking for the second packet of the first logical stream,
// which is where every codec we care about puts its comment header.
void parse_ogg(FILE* f, long offset, AudioTags* out) {
    Buffer buf;
    if (!read_at(f, offset, MAX_OGG_SCAN, &buf)) return;

    unsigned serial = 0;
    bool have_serial = false;
    bool ogg_flac = false;
    int packet_index = 0;
    Buffer packet;
    size_t pos = 0;

    while (pos + 27 <= buf.size()) {
        if (memcmp(&buf[pos], "OggS", 4) != 0) return;

        unsigned page_serial = le32(&buf[pos + 14]);
        unsigned nsegs = buf[pos + 26];
        size_t seg_table = pos + 27;
        if (seg_table + nsegs > buf.size()) return;

        if (!have_serial) {
            serial = page_serial;
            have_serial = true;
        }

        size_t off = seg_table + nsegs;
        for (unsigned i = 0; i < nsegs; ++i) {
            unsigned seg = buf[seg_table + i];
            if (off + seg > buf.size()) return;

            if (page_serial == serial) {
                packet.insert(packet.end(), buf.begin() + off,
                              buf.begin() + off + seg);
            }
            off += seg;

            // A segment shorter than 255 bytes terminates the packet.
            if (seg < 255 && page_serial == serial) {
                if (packet_index == 0) {
                    ogg_flac = packet.size() > 5 && packet[0] == 0x7F &&
                               memcmp(&packet[1], "FLAC", 4) == 0;
                } else if (packet_index == 1) {
                    parse_ogg_comment_packet(packet, ogg_flac, out);
                    return;
                }
                ++packet_index;
                packet.clear();
            }
        }
        pos = off;
    }
}

// ---------------------------------------------------------------------------
// MP4 / M4A / M4B (iTunes style metadata in moov.udta.meta.ilst)
// ---------------------------------------------------------------------------
//
// The player decodes these with mp4read, which has richer metadata, but that
// library keeps its state in one global struct behind a mutex the decoder holds
// for as long as a track plays. Walking the atoms here keeps tag reading
// independent of playback.

const int MAX_ATOMS = 256;

// Reads the atom header at pos, yielding the range of its payload and the
// position of the next sibling.
bool read_atom(FILE* f, long pos, long limit, char type[4], long* body, long* end) {
    if (pos < 0 || limit - pos < 8) return false;

    unsigned char hdr[8];
    if (fseek(f, pos, SEEK_SET) != 0) return false;
    if (fread(hdr, 1, sizeof(hdr), f) != sizeof(hdr)) return false;

    unsigned long long size = be32(hdr);
    memcpy(type, hdr + 4, 4);

    long header = 8;
    if (size == 1) {
        // A size of 1 means the real, 64 bit size follows the type.
        unsigned char ext[8];
        if (fread(ext, 1, sizeof(ext), f) != sizeof(ext)) return false;
        size = ((unsigned long long)be32(ext) << 32) | be32(ext + 4);
        header = 16;
    } else if (size == 0) {
        // A size of 0 means the atom runs to the end of its parent.
        size = (unsigned long long)(limit - pos);
    }

    if (size < (unsigned long long)header) return false;
    if (size > (unsigned long long)(limit - pos)) return false;

    *body = pos + header;
    *end = pos + (long)size;
    return true;
}

bool find_atom(FILE* f, long start, long limit, const char* want,
               long* body, long* end) {
    long pos = start;
    for (int i = 0; i < MAX_ATOMS && pos < limit; ++i) {
        char type[4];
        long b = 0, e = 0;
        if (!read_atom(f, pos, limit, type, &b, &e)) return false;
        if (memcmp(type, want, 4) == 0) {
            *body = b;
            *end = e;
            return true;
        }
        if (e <= pos) return false;  // malformed: no forward progress
        pos = e;
    }
    return false;
}

// Every ilst entry wraps its value in a 'data' atom: a 4 byte type indicator
// (1 means UTF-8 text), a 4 byte locale, then the payload.
std::string read_ilst_value(FILE* f, long start, long limit) {
    long body = 0, end = 0;
    if (!find_atom(f, start, limit, "data", &body, &end)) return "";
    if (end - body < 8) return "";

    Buffer d;
    if (!read_at(f, body, (size_t)(end - body), &d) || d.size() < 8) return "";
    if ((be32(&d[0]) & 0x00FFFFFF) != 1) return "";

    return to_utf8((const char*)&d[8], d.size() - 8, NULL);
}

void parse_mp4(FILE* f, long start, long file_end, AudioTags* out) {
    long moov_b = 0, moov_e = 0;
    if (!find_atom(f, start, file_end, "moov", &moov_b, &moov_e)) return;

    long udta_b = 0, udta_e = 0;
    if (!find_atom(f, moov_b, moov_e, "udta", &udta_b, &udta_e)) return;

    long meta_b = 0, meta_e = 0;
    if (!find_atom(f, udta_b, udta_e, "meta", &meta_b, &meta_e)) return;

    // 'meta' is a full box, so a version/flags word precedes its children.
    // A few writers leave it out, hence the second attempt.
    long ilst_b = 0, ilst_e = 0;
    if (!find_atom(f, meta_b + 4, meta_e, "ilst", &ilst_b, &ilst_e) &&
        !find_atom(f, meta_b, meta_e, "ilst", &ilst_b, &ilst_e)) {
        return;
    }

    // The tag names start with the 0xA9 copyright sign, written in octal so the
    // escape cannot swallow the letter that follows it.
    long b = 0, e = 0;
    if (find_atom(f, ilst_b, ilst_e, "\251nam", &b, &e)) out->title = read_ilst_value(f, b, e);
    if (find_atom(f, ilst_b, ilst_e, "\251ART", &b, &e)) out->artist = read_ilst_value(f, b, e);
    if (out->artist.empty() &&
        find_atom(f, ilst_b, ilst_e, "aART", &b, &e))    out->artist = read_ilst_value(f, b, e);
    if (find_atom(f, ilst_b, ilst_e, "\251alb", &b, &e)) out->album = read_ilst_value(f, b, e);
}

// ---------------------------------------------------------------------------
// WAV (RIFF LIST/INFO)
// ---------------------------------------------------------------------------

void parse_riff_info(const unsigned char* p, size_t len, AudioTags* out) {
    if (len < 4 || memcmp(p, "INFO", 4) != 0) return;

    size_t pos = 4;
    while (pos + 8 <= len) {
        size_t clen = le32(p + pos + 4);
        const char* id = (const char*)p + pos;
        pos += 8;
        if (clen > len - pos) return;

        std::string* field = NULL;
        if (memcmp(id, "INAM", 4) == 0)      field = &out->title;
        else if (memcmp(id, "IART", 4) == 0) field = &out->artist;
        else if (memcmp(id, "IPRD", 4) == 0) field = &out->album;

        if (field != NULL && field->empty()) {
            *field = to_utf8_lenient((const char*)p + pos, clen);
        }
        pos += clen + (clen & 1);  // chunks are padded to an even length
    }
}

void parse_wav(FILE* f, long offset, AudioTags* out) {
    unsigned char hdr[12];
    if (fseek(f, offset, SEEK_SET) != 0) return;
    if (fread(hdr, 1, sizeof(hdr), f) != sizeof(hdr)) return;
    if (memcmp(hdr + 8, "WAVE", 4) != 0) return;

    long pos = offset + 12;
    for (int i = 0; i < MAX_BLOCKS; ++i) {
        unsigned char ch[8];
        if (fseek(f, pos, SEEK_SET) != 0) return;
        if (fread(ch, 1, sizeof(ch), f) != sizeof(ch)) return;

        size_t clen = le32(ch + 4);
        pos += 8;

        if (memcmp(ch, "LIST", 4) == 0) {
            Buffer list;
            size_t want = (clen < MAX_LIST_SCAN) ? clen : MAX_LIST_SCAN;
            if (read_at(f, pos, want, &list) && !list.empty()) {
                parse_riff_info(&list[0], list.size(), out);
            }
            if (!needs_more(*out)) return;
        }
        pos += (long)(clen + (clen & 1));
    }
}

}  // namespace

bool read_audio_tags(const char* filepath, AudioTags* out) {
    if (filepath == NULL || out == NULL) return false;

    out->title.clear();
    out->artist.clear();
    out->album.clear();

    FILE* f = fopen(filepath, "rb");
    if (f == NULL) return false;

    // An ID3v2 tag can sit in front of any of these containers, so find where the
    // container actually starts before sniffing its magic.
    Id3Header id3;
    bool has_id3 = read_id3_header(f, &id3);
    long start = has_id3 ? (long)id3.total_size : 0;

    unsigned char magic[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
    if (fseek(f, start, SEEK_SET) == 0) {
        if (fread(magic, 1, sizeof(magic), f) != sizeof(magic)) {
            memset(magic, 0, sizeof(magic));
        }
    }

    long file_end = 0;
    if (fseek(f, 0, SEEK_END) == 0) {
        long size = ftell(f);
        if (size > 0) file_end = size;
    }

    // Native container tags win over a prepended ID3 tag, which is only ever a
    // compatibility bolt-on when it appears on FLAC, Ogg or WAV.
    bool known_container = true;
    if (memcmp(magic, "fLaC", 4) == 0) {
        parse_flac(f, start, out);
    } else if (memcmp(magic, "OggS", 4) == 0) {
        parse_ogg(f, start, out);
    } else if (memcmp(magic, "RIFF", 4) == 0) {
        parse_wav(f, start, out);
    } else if (memcmp(magic + 4, "ftyp", 4) == 0) {
        parse_mp4(f, start, file_end, out);
    } else {
        known_container = false;
    }

    if (has_id3 && needs_more(*out)) {
        parse_id3v2(f, out);
    }
    // Only MP3 and raw AAC reach for ID3v1: on a real container the last 128
    // bytes are audio, and could match "TAG" by chance.
    if (!known_container && needs_more(*out)) {
        parse_id3v1(f, out);
    }

    fclose(f);
    return !out->title.empty() || !out->artist.empty() || !out->album.empty();
}
