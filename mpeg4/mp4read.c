/****************************************************************************
    MP4 input module

    Copyright (C) 2017 Krzysztof Nikiel

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
****************************************************************************/

#define _CRT_SECURE_NO_WARNINGS

#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <limits.h>

#include "unicode_support.h"
#include "mp4read.h"

enum ATOM_TYPE
{
    ATOM_STOP = 0 /* end of atoms */ ,
    ATOM_NAME /* plain atom */ ,
    ATOM_DESCENT,               /* starts group of children */
    ATOM_ASCENT,                /* ends group */
    ATOM_DATA,
    ATOM_F_OPTIONAL = 0x100
};

typedef int (*parse_t)(int);

typedef struct
{
    uint16_t opcode;
    const char *name;
    parse_t parse;
} creator_t;

#define STOP() {ATOM_STOP, NULL, NULL}
#define NAME(N) {ATOM_NAME, N, NULL}
#define DESCENT() {ATOM_DESCENT, NULL, NULL}
#define ASCENT() {ATOM_ASCENT, NULL, NULL}
#define DATA(N, F) {ATOM_NAME, N, NULL}, {ATOM_DATA, NULL, F}

#define OPTIONAL_NAME(N) {ATOM_NAME | ATOM_F_OPTIONAL, N, NULL}
#define OPTIONAL_DESCENT() {ATOM_DESCENT | ATOM_F_OPTIONAL, NULL, NULL}
#define OPTIONAL_DATA(N, F) {ATOM_NAME | ATOM_F_OPTIONAL, N, NULL}, {ATOM_DATA | ATOM_F_OPTIONAL, NULL, F}


mp4config_t mp4config = { 0 };

static FILE *g_fin = NULL;
static uint32_t g_current_track_id = 0;
static uint32_t g_temp_chapter_track_id = 0;

enum {ERR_OK = 0, ERR_FAIL = -1, ERR_UNSUPPORTED = -2};

#define freeMem(A) if (*(A)) {free(*(A)); *(A) = NULL;}

static size_t datain(void *data, size_t size)
{
    return fread(data, 1, size, g_fin);
}

static int stringin(char *txt, int sizemax)
{
    int size;
    for (size = 0; size < sizemax; size++)
    {
        if (fread(txt + size, 1, 1, g_fin) != 1)
            return ERR_FAIL;
        if (!txt[size])
            break;
    }
    txt[sizemax-1] = '\0';

    return size;
}

static uint32_t u32in(void)
{
    uint8_t u8[4];
    datain(&u8, 4);
    return (uint32_t)u8[3] | ((uint32_t)u8[2] << 8) | ((uint32_t)u8[1] << 16) | ((uint32_t)u8[0] << 24);
}

static uint16_t u16in(void)
{
    uint8_t u8[2];
    datain(&u8, 2);
    return (uint16_t)u8[1] | ((uint16_t)u8[0] << 8);
}

static int u8in(void)
{
    uint8_t u8;
    datain(&u8, 1);
    return u8;
}

static int ftypin(int size)
{
    enum {BUFSIZE = 40};
    char buf[BUFSIZE];
    uint32_t u32;

    buf[4] = 0;
    datain(buf, 4);
    u32 = u32in();

    if (mp4config.verbose.header)
        fprintf(stderr, "Brand:\t\t\t%s(version %d)\n", buf, u32);

    stringin(buf, BUFSIZE);

    if (mp4config.verbose.header)
        fprintf(stderr, "Compatible brands:\t%s\n", buf);

    return size;
}

enum
{ SECSINDAY = 24 * 60 * 60 };
static char *mp4time(time_t t)
{
    int y;

    // subtract some seconds from the start of 1904 to the start of 1970
    for (y = 1904; y < 1970; y++)
    {
        t -= 365 * SECSINDAY;
        if (!(y & 3))
            t -= SECSINDAY;
    }
    return ctime(&t);
}

static int tkhdin(int size)
{
    uint8_t version = u8in();
    u8in(); u8in(); u8in(); // flags
    if (version == 1) {
        u32in(); u32in(); // ctime
        u32in(); u32in(); // mtime
    } else {
        u32in(); // ctime
        u32in(); // mtime
    }
    g_current_track_id = u32in();
    g_temp_chapter_track_id = 0;
    return size;
}

static int chapin(int size)
{
    g_temp_chapter_track_id = u32in();
    return size;
}

static int mdhdin(int size)
{
    // version/flags
    u32in();
    // Creation time
    mp4config.ctime = u32in();
    // Modification time
    mp4config.mtime = u32in();
    // Time scale
    mp4config.samplerate = u32in();
    // Duration
    mp4config.samples = u32in();
    // Language
    u16in();
    // pre_defined
    u16in();

    return size;
}

static int hdlr1in(int size)
{
    uint8_t buf[5];

    buf[4] = 0;
    // version/flags
    u32in();
    // pre_defined
    u32in();
    // Component subtype
    datain(buf, 4);
    if (mp4config.verbose.header)
        fprintf(stderr, "*track media type: '%s': ", buf);
    if (memcmp("soun", buf, 4))
    {
        if (mp4config.verbose.header)
            fprintf(stderr, "unsupported, skipping\n");
        return ERR_UNSUPPORTED;
    }
    else
    {
        if (mp4config.verbose.header)
            fprintf(stderr, "OK\n");
        mp4config.chapter_track_id = g_temp_chapter_track_id;
    }
    // reserved
    u32in();
    u32in();
    u32in();
    // name
    // null terminate
    u8in();

    return size;
}

static int stsdin(int size)
{
    // version/flags
    u32in();
    // Number of entries(one 'mp4a')
    if (u32in() != 1) //fixme: error handling
        return ERR_FAIL;

    return size;
}

static int mp4ain(int size)
{
    // Reserved (6 bytes)
    u32in();
    u16in();
    // Data reference index
    u16in();
    // Version
    u16in();
    // Revision level
    u16in();
    // Vendor
    u32in();
    // Number of channels
    mp4config.channels = u16in();
    // Sample size (bits)
    mp4config.bits = u16in();
    // Compression ID
    u16in();
    // Packet size
    u16in();
    // Sample rate (16.16)
    // fractional framerate, probably not for audio
    // rate integer part
    u16in();
    // rate reminder part
    u16in();

    return size;
}


static uint32_t getsize(void)
{
    int cnt;
    uint32_t size = 0;
    for (cnt = 0; cnt < 4; cnt++)
    {
        int tmp = u8in();

        size <<= 7;
        size |= (tmp & 0x7f);
        if (!(tmp & 0x80))
            break;
    }
    return size;
}

static int esdsin(int size)
{
    // descriptor tree:
    // MP4ES_Descriptor
    //   MP4DecoderConfigDescriptor
    //      MP4DecSpecificInfoDescriptor
    //   MP4SLConfigDescriptor
    enum
    {
        TAG_ES = 3,
        TAG_DC = 4,
        TAG_DSI = 5,
        TAG_SLC = 6
    };

    // version/flags
    u32in();
    if (u8in() != TAG_ES)
        return ERR_FAIL;
    getsize();
    // ESID
    u16in();
    // flags(url(bit 6); ocr(5); streamPriority (0-4)):
    u8in();

    if (u8in() != TAG_DC)
        return ERR_FAIL;
    getsize();
    if (u8in() != 0x40) /* not MPEG-4 audio */
        return ERR_FAIL;
    // flags
    u8in();
    // buffer size (24 bits)
    mp4config.buffersize = u16in() << 8;
    mp4config.buffersize |= u8in();
    // bitrate
    mp4config.bitratemax = u32in();
    mp4config.bitrateavg = u32in();

    if (u8in() != TAG_DSI)
        return ERR_FAIL;
    mp4config.asc.size = getsize();
    if (mp4config.asc.size > sizeof(mp4config.asc.buf))
        return ERR_FAIL;
    // get AudioSpecificConfig
    datain(mp4config.asc.buf, mp4config.asc.size);

    if (u8in() != TAG_SLC)
        return ERR_FAIL;
    getsize();
    // "predefined" (no idea)
    u8in();

    return size;
}

/* stbl "Sample Table" layout: 
 *  - stts "Time-to-Sample" - useless
 *  - stsc "Sample-to-Chunk" - condensed table chunk-to-num-samples
 *  - stsz "Sample Size" - size table
 *  - stco "Chunk Offset" - chunk starts
 * 
 * When receiving stco we can combine stsc and stsz tables to produce final
 * sample offsets.
 */

static int sttsin(int size)
{
    uint32_t ntts;

    if (size < 8)
        return ERR_FAIL;

    // version/flags
    u32in();
    ntts = u32in();

    if (ntts < 1)
        return ERR_FAIL;

    /* 2 x uint32_t per entry */
    if (((size - 8u) / 8u) < ntts)
        return ERR_FAIL;

    return size;
}

static int stscin(int size)
{
    uint32_t i, tmp, firstchunk, prevfirstchunk, samplesperchunk;

    if (size < 8)
        return ERR_FAIL;

    // version/flags
    u32in();

    mp4config.frame.nsclices = u32in();

    if (!mp4config.frame.nsclices)
        return ERR_FAIL;

    tmp = sizeof(slice_info_t) * mp4config.frame.nsclices;
    if (tmp < mp4config.frame.nsclices)
        return ERR_FAIL;
    mp4config.frame.map = malloc(tmp);
    if (!mp4config.frame.map)
        return ERR_FAIL;

    /* 3 x uint32_t per entry */
    if (((size - 8u) / 12u) < mp4config.frame.nsclices)
        return ERR_FAIL;

    prevfirstchunk = 0;
    for (i = 0; i < mp4config.frame.nsclices; ++i) {
      firstchunk = u32in();
      samplesperchunk = u32in();
      // id - unused
      u32in();
      if (firstchunk <= prevfirstchunk)
        return ERR_FAIL;
      if (samplesperchunk < 1)
        return ERR_FAIL;
      mp4config.frame.map[i].firstchunk = firstchunk;
      mp4config.frame.map[i].samplesperchunk = samplesperchunk;
      prevfirstchunk = firstchunk;
    }

    return size;
}

static int stszin(int size)
{
    uint32_t i, tmp;

    if (size < 12)
        return ERR_FAIL;

    // version/flags
    u32in();
    // (uniform) Sample size
    // TODO(eustas): add uniform sample size support?
    u32in();
    mp4config.frame.nsamples = u32in();

    if (!mp4config.frame.nsamples)
        return ERR_FAIL;

    tmp = sizeof(frame_info_t) * mp4config.frame.nsamples;
    if (tmp < mp4config.frame.nsamples)
        return ERR_FAIL;
    mp4config.frame.info = malloc(tmp);
    if (!mp4config.frame.info)
        return ERR_FAIL;

    if ((size - 12u) / 4u < mp4config.frame.nsamples)
        return ERR_FAIL;

    for (i = 0; i < mp4config.frame.nsamples; i++)
    {
        mp4config.frame.info[i].len = u32in();
        mp4config.frame.info[i].offset = 0;
        if (mp4config.frame.maxsize < mp4config.frame.info[i].len)
            mp4config.frame.maxsize = mp4config.frame.info[i].len;
    }

    return size;
}

static int stcoin(int size)
{
    uint32_t numchunks, chunkn, slicen, samplesleft, i, offset;
    uint32_t nextoffset;

    if (size < 8)
        return ERR_FAIL;

    // version/flags
    u32in();

    // Number of entries
    numchunks = u32in();
    if ((numchunks < 1) || ((numchunks + 1) == 0))
        return ERR_FAIL;

    if ((size - 8u) / 4u < numchunks)
        return ERR_FAIL;

    chunkn = 0;
    samplesleft = 0;
    slicen = 0;
    offset = 0;

    for (i = 0; i < mp4config.frame.nsamples; ++i) {
        if (samplesleft == 0)
        {
            chunkn++;
            if (chunkn > numchunks)
                return ERR_FAIL;
            if (slicen < mp4config.frame.nsclices &&
                (slicen + 1) < mp4config.frame.nsclices) {
                if (chunkn == mp4config.frame.map[slicen + 1].firstchunk)
                    slicen++;
            }
            samplesleft = mp4config.frame.map[slicen].samplesperchunk;
            offset = u32in();
        }
        mp4config.frame.info[i].offset = offset;
        nextoffset = offset + mp4config.frame.info[i].len;
        if (nextoffset < offset)
            return ERR_FAIL;
        offset = nextoffset;
        samplesleft--;
    }

    freeMem(&mp4config.frame.map);

    return size;
}

#if 0
static int tagtxt(char *tagname, const char *tagtxt)
{
    //int txtsize = strlen(tagtxt);
    int size = 0;
    //int datasize = txtsize + 16;

#if 0
    size += u32out(datasize + 8);
    size += dataout(tagname, 4);
    size += u32out(datasize);
    size += dataout("data", 4);
    size += u32out(1);
    size += u32out(0);
    size += dataout(tagtxt, txtsize);
#endif

    return size;
}

static int tagu32(char *tagname, int n /*number of stored fields*/)
{
    //int numsize = n * 4;
    int size = 0;
    //int datasize = numsize + 16;

#if 0
    size += u32out(datasize + 8);
    size += dataout(tagname, 4);
    size += u32out(datasize);
    size += dataout("data", 4);
    size += u32out(0);
    size += u32out(0);
#endif

    return size;
}
#endif

static int chplin(int size)
{
    uint32_t count, i;
    
    // Version (1) + Flags (3)
    u32in();
    // Reserved (4)
    u32in();
    
    count = u8in();
    printf("Reading %u chapters:\n", count);
    if (count > 0) {
        mp4config.chapters = (mp4chapter_t*)malloc(sizeof(mp4chapter_t) * count);
        if (mp4config.chapters) {
            mp4config.chapter_count = count;
            
            for (i = 0; i < count; i++) {
                uint64_t time = (uint64_t)u32in() << 32 | u32in();
                int len = u8in();
                char *title = (char*)malloc(len + 1);
                if (title) {
                    datain(title, len);
                    title[len] = 0;
                    mp4config.chapters[i].title = title;
                    printf("Chapter %d: %s at %lu\n", i+1, title, (unsigned long)(time/10000000));
                } else {
                    // Skip title data if malloc fails
                    while(len--) u8in();
                    mp4config.chapters[i].title = NULL;
                    printf("Chapter %d: <memory allocation failed> at %lu\n", i+1, (unsigned long)(time/10000000));
                }
                mp4config.chapters[i].timestamp = time;
                
                if (mp4config.verbose.tags) {
                    fprintf(stderr, "Chapter %d: %s at %lu\n", i+1, title ? title : "NULL", (unsigned long)(time/10000000));
                }
            }
        }
    }
    
    // consume remaining bytes if any? chpl atom size should match
    
    return size;
}

static int metain(int size)
{
    (void)size;  /* why not used? */
    // version/flags
    u32in();

    return ERR_OK;
}

static int hdlr2in(int size)
{
    uint8_t buf[4];

    // version/flags
    u32in();
    // Predefined
    u32in();
    // Handler type
    datain(buf, 4);
    if (memcmp(buf, "mdir", 4))
        return ERR_FAIL;
    datain(buf, 4);
    if (memcmp(buf, "appl", 4))
        return ERR_FAIL;
    // Reserved
    u32in();
    u32in();
    // null terminator
    u8in();

    return size;
}

static int ilstin(int size)
{
    enum {NUMSET = 1, GENRE, EXTAG};
    int read = 0;

    static struct {
        char *name;
        char *id;
        int flag;
    } tags[] = {
        {"Album       ", "\xa9" "alb", 0},
        {"Album Artist", "aART", 0},
        {"Artist      ", "\xa9" "ART", 0},
        {"Comment     ", "\xa9" "cmt", 0},
        {"Cover image ", "covr", 0},
        {"Compilation ", "cpil", 0},
        {"Copyright   ", "cprt", 0},
        {"Date        ", "\xa9" "day", 0},
        {"Disc#       ", "disk", NUMSET},
        {"Genre       ", "gnre", GENRE},
        {"Grouping    ", "\xa9" "grp", 0},
        {"Lyrics      ", "\xa9" "lyr", 0},
        {"Title       ", "\xa9" "nam", 0},
        {"Rating      ", "rtng", 0},
        {"BPM         ", "tmpo", 0},
        {"Encoder     ", "\xa9" "too", 0},
        {"Track       ", "trkn", NUMSET},
        {"Composer    ", "\xa9" "wrt", 0},
        {0, "----", EXTAG},
        {0, 0, 0},
    };

    static const char *genres[] = {
        "Blues", "Classic Rock", "Country", "Dance",
        "Disco", "Funk", "Grunge", "Hip-Hop",
        "Jazz", "Metal", "New Age", "Oldies",
        "Other", "Pop", "R&B", "Rap",
        "Reggae", "Rock", "Techno", "Industrial",
        "Alternative", "Ska", "Death Metal", "Pranks",
        "Soundtrack", "Euro-Techno", "Ambient", "Trip-Hop",
        "Vocal", "Jazz+Funk", "Fusion", "Trance",
        "Classical", "Instrumental", "Acid", "House",
        "Game", "Sound Clip", "Gospel", "Noise",
        "Alternative Rock", "Bass", "Soul", "Punk",
        "Space", "Meditative", "Instrumental Pop", "Instrumental Rock",
        "Ethnic", "Gothic", "Darkwave", "Techno-Industrial",
        "Electronic", "Pop-Folk", "Eurodance", "Dream",
        "Southern Rock", "Comedy", "Cult", "Gangsta",
        "Top 40", "Christian Rap", "Pop/Funk", "Jungle",
        "Native US", "Cabaret", "New Wave", "Psychadelic",
        "Rave", "Showtunes", "Trailer", "Lo-Fi",
        "Tribal", "Acid Punk", "Acid Jazz", "Polka",
        "Retro", "Musical", "Rock & Roll", "Hard Rock",
        "Folk", "Folk-Rock", "National Folk", "Swing",
        "Fast Fusion", "Bebob", "Latin", "Revival",
        "Celtic", "Bluegrass", "Avantgarde", "Gothic Rock",
        "Progressive Rock", "Psychedelic Rock", "Symphonic Rock", "Slow Rock",
        "Big Band", "Chorus", "Easy Listening", "Acoustic",
        "Humour", "Speech", "Chanson", "Opera",
        "Chamber Music", "Sonata", "Symphony", "Booty Bass",
        "Primus", "Porn Groove", "Satire", "Slow Jam",
        "Club", "Tango", "Samba", "Folklore",
        "Ballad", "Power Ballad", "Rhythmic Soul", "Freestyle",
        "Duet", "Punk Rock", "Drum Solo", "Acapella",
        "Euro-House", "Dance Hall", "Goa", "Drum & Bass",
        "Club - House", "Hardcore", "Terror", "Indie",
        "BritPop", "Negerpunk", "Polsk Punk", "Beat",
        "Christian Gangsta Rap", "Heavy Metal", "Black Metal", "Crossover",
        "Contemporary Christian", "Christian Rock", "Merengue", "Salsa",
        "Thrash Metal", "Anime", "JPop", "Synthpop",
        "Unknown",
    };

    fprintf(stderr, "----------tag list-------------\n");
    while(read < size)
    {
        int asize, dsize;
        uint8_t id[5];
        uint8_t tagid[5];
        int cnt;
        uint32_t type;

        id[4] = 0;

        asize = u32in();
        read += asize;
        asize -= 4;
        if (datain(tagid, 4) < 4)
            return ERR_FAIL;
        tagid[4] = 0;
        asize -= 4;

        for (cnt = 0; tags[cnt].id; cnt++)
        {
            if (!memcmp(tagid, tags[cnt].id, 4))
                break;
        }

        if (tags[cnt].name)
            fprintf(stderr, "%s :   ", tags[cnt].name);
        else
        {
            if (tags[cnt].flag != EXTAG)
                fprintf(stderr, "'%s'       :   ", tagid);
        }

        dsize = u32in();
        asize -= 4;
        if (datain(id, 4) < 4)
            return ERR_FAIL;
        asize -= 4;

        if (tags[cnt].flag != EXTAG)
        {
            if (memcmp(id, "data", 4))
                return ERR_FAIL;
        }
        else
        {
            int spc;

            if (memcmp(id, "mean", 4))
                goto skip;
            dsize -= 8;
            while (dsize > 0)
            {
                u8in();
                asize--;
                dsize--;
            }
            if (asize >= 8)
            {
                dsize = u32in() - 8;
                asize -= 4;
                if (datain(id, 4) < 4)
                    return ERR_FAIL;
                asize -= 4;
                if (memcmp(id, "name", 4))
                    goto skip;
                u32in();
                asize -= 4;
                dsize -= 4;
            }
            spc = 13 - dsize;
            if (spc < 0) spc = 0;
            while (dsize > 0)
            {
                fprintf(stderr, "%c",u8in());
                asize--;
                dsize--;
            }
            while (spc--)
                fprintf(stderr, " ");
            fprintf(stderr, ":   ");
            if (asize >= 8)
            {
                dsize = u32in() - 8;
                asize -= 4;
                if (datain(id, 4) < 4)
                    return ERR_FAIL;
                asize -= 4;
                if (memcmp(id, "data", 4))
                    goto skip;
                u32in();
                asize -= 4;
                dsize -= 4;
            }
            while (dsize > 0)
            {
                fprintf(stderr, "%c",u8in());
                asize--;
                dsize--;
            }
            fprintf(stderr, "\n");

            goto skip;
        }
        type = u32in();
        asize -= 4;
        u32in();
        asize -= 4;
        fprintf(stderr, "[type %02x] ", type);
        switch(type)
        {
        case 1:
            if (!memcmp(tagid, tags[0].id, 4) || !memcmp(tagid, tags[2].id, 4) || !memcmp(tagid, tags[12].id, 4))
            {
                char *val = (char*)malloc(asize + 1);
                if (val)
                {
                    int k;
                    for (k = 0; k < asize; k++) val[k] = (char)u8in();
                    val[asize] = 0;

                    if (!memcmp(tagid, tags[12].id, 4)) {
                        freeMem(&mp4config.meta_title);
                        fprintf(stderr, "Title %s\n", val);
                        mp4config.meta_title = val;
                    } else if (!memcmp(tagid, tags[2].id, 4)) {
                        freeMem(&mp4config.meta_artist);
                        fprintf(stderr, "Artist %s\n", val);
                        mp4config.meta_artist = val;
                    } else if (!memcmp(tagid, tags[0].id, 4)) {
                        freeMem(&mp4config.meta_album);
                        fprintf(stderr, "Album %s\n", val);
                        mp4config.meta_album = val;
                    }
                    asize = 0;
                }
                else
                {
                     while (asize > 0)
                    {
                        fprintf(stderr, "%c",u8in());
                        asize--;
                    }
                }
            }
            else
            {
                while (asize > 0)
                {
                    fprintf(stderr, "%c",u8in());
                    asize--;
                }
            }
            break;
        case 0:
            switch(tags[cnt].flag)
            {
            case NUMSET:
                u16in();
                asize -= 2;

                fprintf(stderr, "%d", u16in());
                asize -= 2;
                fprintf(stderr, "/%d", u16in());
                asize -= 2;
                break;
            case GENRE:
                {
                    uint16_t gnum = u16in();
                    asize -= 2;
                    if (!gnum)
                       goto skip;
                    gnum--;
                    if (gnum >= 147)
                        gnum = 147;
                    fprintf(stderr, "%s", genres[gnum]);
                }
                break;
            default:
                 if (!memcmp(tagid, "covr", 4))
                {
                    freeMem(&mp4config.cover_art.data);
                    mp4config.cover_art.data = (uint8_t*)malloc(asize);
                    if (mp4config.cover_art.data)
                    {
                        mp4config.cover_art.size = asize;
                        datain(mp4config.cover_art.data, asize);
                        asize = 0;
                    }
                }
                else
                {
                    while(asize > 0)
                    {
                        fprintf(stderr, "%d/", u16in());
                        asize-=2;
                    }
                }
            }
            break;
        case 0x15:
            //fprintf(stderr, "(8bit data)");
            while(asize > 0)
            {
                fprintf(stderr, "%d", u8in());
                asize--;
                if (asize)
                    fprintf(stderr, "/");
            }
            break;
        default:
            fprintf(stderr, "(unknown data type)");
            break;
        }
        fprintf(stderr, "\n");

    skip:
        // skip to the end of atom
        while (asize > 0)
        {
            u8in();
            asize--;
        }
    }
    fprintf(stderr, "-------------------------------\n");

    return size;
}

static creator_t *g_atom = 0;
static int parse(uint32_t *sizemax)
{
    long apos = 0;
    long start_pos = ftell(g_fin);
    long aposmax = start_pos + *sizemax;
    uint32_t size;

    if ((g_atom->opcode & 0xFF) != ATOM_NAME)
    {
        fprintf(stderr, "parse error: root is not a 'name' opcode\n");
        return ERR_FAIL;
    }
    //fprintf(stderr, "looking for '%s'\n", (char *)g_atom->name);

    // search for atom in the file
    while (1)
    {
        char name[4];
        uint32_t tmp;

        apos = ftell(g_fin);
        if (apos >= (aposmax - 8))
        {
            if (g_atom->opcode & ATOM_F_OPTIONAL) {
                 fseek(g_fin, start_pos, SEEK_SET);
                 // Advance g_atom past this optional atom's definition
                 g_atom++;
                 if (g_atom->opcode & ATOM_DATA) g_atom++;
                 else if ((g_atom->opcode & 0xFF) == ATOM_DESCENT) {
                     int depth = 1;
                     g_atom++;
                     while (depth > 0 && g_atom->opcode != ATOM_STOP) {
                         if ((g_atom->opcode & 0xFF) == ATOM_DESCENT) depth++;
                         if ((g_atom->opcode & 0xFF) == ATOM_ASCENT) depth--;
                         g_atom++;
                     }
                 }
                 return ERR_OK;
            }
            fprintf(stderr, "parse error: atom '%s' not found\n", g_atom->name);
            return ERR_FAIL;
        }
        if ((tmp = u32in()) < 8)
        {
            fprintf(stderr, "invalid atom size %x @%lx\n", tmp, ftell(g_fin));
            return ERR_FAIL;
        }

        size = tmp;
        if (datain(name, 4) != 4)
        {
            // EOF
            fprintf(stderr, "can't read atom name @%lx\n", ftell(g_fin));
            return ERR_FAIL;
        }

        //fprintf(stderr, "atom: '%c%c%c%c'(%x)", name[0],name[1],name[2],name[3], size);

        if (!memcmp(name, g_atom->name, 4))
        {
            //fprintf(stderr, "OK\n");
            break;
        }
        //fprintf(stderr, "\n");

        fseek(g_fin, apos + size, SEEK_SET);
    }
    *sizemax = size;
    g_atom++;
    if ((g_atom->opcode & 0xFF) == ATOM_DATA)
    {
        int err = g_atom->parse(size - 8);
        if (err < ERR_OK)
        {
            fseek(g_fin, apos + size, SEEK_SET);
            return err;
        }
        g_atom++;
    }
    if ((g_atom->opcode & 0xFF) == ATOM_DESCENT)
    {
        long apos2 = ftell(g_fin);

        //fprintf(stderr, "descent\n");
        g_atom++;
        while (g_atom->opcode != ATOM_STOP)
        {
            uint32_t subsize = size - 8;
            int ret;
            if ((g_atom->opcode & 0xFF) == ATOM_ASCENT)
            {
                g_atom++;
                break;
            }
            // TODO: does not feel well - we always return to the same point!
            fseek(g_fin, apos2, SEEK_SET);
            if ((ret = parse(&subsize)) < 0)
                return ret;
        }
        //fprintf(stderr, "ascent\n");
    }

    fseek(g_fin, apos + size, SEEK_SET);

    return ERR_OK;
}

static int moovin(int sizemax)
{
    long apos = ftell(g_fin);
    uint32_t atomsize;
    creator_t *old_atom = g_atom;
    int err, ret = sizemax;

    static creator_t mvhd[] = {
        NAME("mvhd"),
        STOP()
    };
    static creator_t trak[] = {
        NAME("trak"),
        DESCENT(),
        DATA("tkhd", tkhdin),
        OPTIONAL_NAME("tref"),
        DESCENT(),
            OPTIONAL_DATA("chap", chapin),
        ASCENT(),
        NAME("mdia"),
        DESCENT(),
        DATA("mdhd", mdhdin),
        DATA("hdlr", hdlr1in),
        NAME("minf"),
        DESCENT(),
        NAME("smhd"),
        NAME("dinf"),
        NAME("stbl"),
        DESCENT(),
        DATA("stsd", stsdin),
        DESCENT(),
        DATA("mp4a", mp4ain),
        DESCENT(),
        DATA("esds", esdsin),
        ASCENT(),
        ASCENT(),
        DATA("stts", sttsin),
        DATA("stsc", stscin),
        DATA("stsz", stszin),
        DATA("stco", stcoin),
        STOP()
    };

    g_atom = mvhd;
    atomsize = sizemax + apos - ftell(g_fin);
    if (parse(&atomsize) < 0) {
        g_atom = old_atom;
        return ERR_FAIL;
    }

    fseek(g_fin, apos, SEEK_SET);

    while (1)
    {
        //fprintf(stderr, "TRAK\n");
        g_atom = trak;
        atomsize = sizemax + apos - ftell(g_fin);
        if (atomsize < 8)
            break;
        //fprintf(stderr, "PARSE(%x)\n", atomsize);
        err = parse(&atomsize);
        //fprintf(stderr, "SIZE: %x/%x\n", atomsize, sizemax);
        if (err >= 0)
            break;
        if (err != ERR_UNSUPPORTED) {
            ret = err;
            break;
        }
        //fprintf(stderr, "UNSUPP\n");
    }

    g_atom = old_atom;
    return ret;
}


static creator_t g_head[] = {
    DATA("ftyp", ftypin),
    STOP()
};

static creator_t g_moov[] = {
    DATA("moov", moovin),
    //DESCENT(),
    //NAME("mvhd"),
    STOP()
};

static creator_t g_chapters[] = {
    NAME("moov"),
    DESCENT(),
    NAME("udta"),
    DESCENT(),
    DATA("chpl", chplin),
    ASCENT(),
    ASCENT(),
    STOP()
};

static creator_t g_meta1[] = {
    NAME("moov"),
    DESCENT(),
    NAME("udta"),
    DESCENT(),
    DATA("meta", metain),
    DESCENT(),
    DATA("hdlr", hdlr2in),
    DATA("ilst", ilstin),
    STOP()
};

static creator_t g_meta2[] = {
    DATA("meta", metain),
    DESCENT(),
    DATA("hdlr", hdlr2in),
    DATA("ilst", ilstin),
    STOP()
};


/* QuickTime Chapter Parsing Support */

typedef struct {
    uint32_t count;
    uint32_t duration;
} stts_entry_t;

typedef struct {
    uint32_t first_chunk;
    uint32_t samples_per_chunk;
    uint32_t id;
} stsc_entry_t;

static struct {
    stts_entry_t *stts;
    uint32_t stts_count;
    stsc_entry_t *stsc;
    uint32_t stsc_count;
    uint32_t *stsz;
    uint32_t stsz_count;
    uint32_t *stco;
    uint32_t stco_count;
    uint32_t timescale;
} qt_data = {0};

static void qt_reset(void) {
    freeMem(&qt_data.stts);
    freeMem(&qt_data.stsc);
    freeMem(&qt_data.stsz);
    freeMem(&qt_data.stco);
    qt_data.stts_count = 0;
    qt_data.stsc_count = 0;
    qt_data.stsz_count = 0;
    qt_data.stco_count = 0;
    qt_data.timescale = 0;
}

static int mdhdin_qt(int size) {
    uint8_t version = u8in();
    u8in(); u8in(); u8in(); // flags
    if (version == 1) {
        u32in(); u32in(); // ctime
        u32in(); u32in(); // mtime
    } else {
        u32in(); // ctime
        u32in(); // mtime
    }
    qt_data.timescale = u32in();
    return size;
}

static int sttsin_qt(int size) {
    uint32_t count, i;
    u32in(); // version/flags
    count = u32in();
    if (size < 0 || count > (uint32_t)size/8) return ERR_FAIL; // sanity
    qt_data.stts = (stts_entry_t*)calloc(count, sizeof(stts_entry_t));
    if (!qt_data.stts) return ERR_FAIL;
    qt_data.stts_count = count;
    for (i=0; i<count; i++) {
        qt_data.stts[i].count = u32in();
        qt_data.stts[i].duration = u32in();
    }
    return size;
}

static int stscin_qt(int size) {
    uint32_t count, i;
    u32in(); // version/flags
    count = u32in();
    if (size < 0 || count > (uint32_t)size/12) return ERR_FAIL;
    qt_data.stsc = (stsc_entry_t*)calloc(count, sizeof(stsc_entry_t));
    if (!qt_data.stsc) return ERR_FAIL;
    qt_data.stsc_count = count;
    for (i=0; i<count; i++) {
        qt_data.stsc[i].first_chunk = u32in();
        qt_data.stsc[i].samples_per_chunk = u32in();
        qt_data.stsc[i].id = u32in();
    }
    return size;
}

static int stszin_qt(int size) {
    uint32_t count, i, uniform;
    u32in(); // version/flags
    uniform = u32in();
    count = u32in();
    qt_data.stsz = (uint32_t*)calloc(count, sizeof(uint32_t));
    if (!qt_data.stsz) return ERR_FAIL;
    qt_data.stsz_count = count;
    if (uniform == 0) {
        if (size < 12 || count > (uint32_t)(size-12)/4) return ERR_FAIL;
        for (i=0; i<count; i++) qt_data.stsz[i] = u32in();
    } else {
        for (i=0; i<count; i++) qt_data.stsz[i] = uniform;
    }
    return size;
}

static int stcoin_qt(int size) {
    uint32_t count, i;
    u32in(); // version/flags
    count = u32in();
    if (size < 0 || count > (uint32_t)size/4) return ERR_FAIL;
    qt_data.stco = (uint32_t*)calloc(count, sizeof(uint32_t));
    if (!qt_data.stco) return ERR_FAIL;
    qt_data.stco_count = count;
    for (i=0; i<count; i++) qt_data.stco[i] = u32in();
    return size;
}

static int check_qt_id(int size) {
    uint8_t version = u8in();
    u8in(); u8in(); u8in();
    if (version == 1) { u32in(); u32in(); u32in(); u32in(); } 
    else { u32in(); u32in(); }
    uint32_t id = u32in();
    if (id != mp4config.chapter_track_id) return ERR_UNSUPPORTED; // Skip this track
    return size;
}

static void process_qt_chapters(void) {
    if (!qt_data.stco || !qt_data.stsz || !qt_data.timescale) return;

    // Expand STSC
    uint32_t *samples_in_chunk = (uint32_t*)calloc(qt_data.stco_count, sizeof(uint32_t));
    if (!samples_in_chunk) return;
    
    uint32_t i, k;
    for (i = 0; i < qt_data.stsc_count; ++i) {
        if (qt_data.stsc[i].first_chunk < 1) continue;
        uint32_t start = qt_data.stsc[i].first_chunk - 1;
        uint32_t end = (i + 1 < qt_data.stsc_count) ? (qt_data.stsc[i+1].first_chunk - 1) : qt_data.stco_count;
        for (k = start; k < end && k < qt_data.stco_count; ++k) {
            samples_in_chunk[k] = qt_data.stsc[i].samples_per_chunk;
        }
    }

    // Count total samples (chapters)
    uint32_t total_samples = qt_data.stsz_count;
    mp4config.chapters = (mp4chapter_t*)malloc(sizeof(mp4chapter_t) * total_samples);
    mp4config.chapter_count = 0;

    uint32_t current_sample = 0;
    uint64_t current_ticks = 0;
    uint32_t stts_idx = 0;
    uint32_t stts_sample_count = 0;

    for (i = 0; i < qt_data.stco_count; ++i) {
        uint32_t offset = qt_data.stco[i];
        uint32_t samples = samples_in_chunk[i];
        
        fseek(g_fin, offset, SEEK_SET);
        
        for (k = 0; k < samples; ++k) {
            if (current_sample >= total_samples) break;
            
            // Duration
            uint32_t dur = 0;
            if (stts_idx < qt_data.stts_count) {
                dur = qt_data.stts[stts_idx].duration;
                stts_sample_count++;
                if (stts_sample_count >= qt_data.stts[stts_idx].count) {
                    stts_idx++;
                    stts_sample_count = 0;
                }
            }
            
            uint64_t ms = (current_ticks * 1000) / qt_data.timescale;
            uint32_t len = qt_data.stsz[current_sample];
            
            if (len > 0) {
                char *buf = (char*)malloc(len + 1);
                if (buf) {
                    if (fread(buf, 1, len, g_fin) == len) {
                        buf[len] = 0;
                        // Handle pascal string length prefix if present (common in text tracks)
                        // If length > 2 and first 2 bytes as uint16 match length-2
                        uint16_t prefix = ((uint8_t)buf[0] << 8) | (uint8_t)buf[1];
                        char *title_ptr = buf+1;
                        if (len > 2 && prefix == len - 2) {
                            title_ptr += 2;
                        }
                        
                        mp4config.chapters[mp4config.chapter_count].timestamp = ms * 10000; // 100ns units
                        mp4config.chapters[mp4config.chapter_count].title = strdup(title_ptr);
                        mp4config.chapter_count++;
                    }
                    free(buf);
                }
            }
            
            current_ticks += dur;
            current_sample++;
        }
    }
    
    free(samples_in_chunk);
}

static int stblin_qt(int size) {
    long atom_end = ftell(g_fin) + size;
    
    while (ftell(g_fin) < atom_end) {
        long cur_pos = ftell(g_fin);
        if (atom_end - cur_pos < 8) break;

        uint32_t s = u32in();
        char n[4];
        if (datain(n, 4) != 4) break;
        
        if (s < 8) break;
        
        int ret = ERR_OK;
        if (!memcmp(n, "stts", 4)) ret = sttsin_qt(s);
        else if (!memcmp(n, "stsc", 4)) ret = stscin_qt(s);
        else if (!memcmp(n, "stsz", 4)) ret = stszin_qt(s);
        else if (!memcmp(n, "stco", 4)) ret = stcoin_qt(s);
        
        if (ret < 0) return ret;

        fseek(g_fin, cur_pos + s, SEEK_SET);
    }
    return size;
}

static creator_t g_qt_trak[] = {
    NAME("trak"),
    DESCENT(),
    DATA("tkhd", check_qt_id),
    NAME("mdia"),
    DESCENT(),
    DATA("mdhd", mdhdin_qt),
    NAME("minf"),
    DESCENT(),
    DATA("stbl", stblin_qt),
    ASCENT(),
    ASCENT(),
    ASCENT(),
    STOP()
};

static void scan_qt_chapters(void) {
    creator_t *old = g_atom;
    uint32_t size = INT_MAX;
    int err;
    
    rewind(g_fin);
    // Find moov
    g_atom = g_moov; 
    
    // Manual scan for moov
    while(1) {
       uint32_t s = u32in();
       char n[4];
       if (datain(n, 4) != 4) break; // EOF
       
       if (!memcmp(n, "moov", 4)) {
           // Found moov.
           long moov_start = ftell(g_fin);
           long moov_end = moov_start + s - 8;
           
           while(ftell(g_fin) < moov_end) {
               // Scan for traks
               g_atom = g_qt_trak;
               size = moov_end - ftell(g_fin);
               err = parse(&size);
               if (err == ERR_OK) {
                   // Found the chapter track and parsed it!
                   process_qt_chapters();
                   break;
               }
               // if ERR_UNSUPPORTED, it was a trak but ID mismatch. loop continues (parse moved file ptr)
               // if ERR_FAIL, it wasn't a trak or error.
               if (err == ERR_FAIL) break;
           }
           break;
       }
       // skip atom
       fseek(g_fin, s - 8, SEEK_CUR);
    }
    
    g_atom = old;
    qt_reset();
}


int mp4read_frame(void)
{
    if (mp4config.frame.current >= mp4config.frame.nsamples)
        return ERR_FAIL;

    // TODO(eustas): avoid no-op seeks
    mp4read_seek(mp4config.frame.current);

    mp4config.bitbuf.size = mp4config.frame.info[mp4config.frame.current].len;

    if (fread(mp4config.bitbuf.data, 1, mp4config.bitbuf.size, g_fin)
        != mp4config.bitbuf.size)
    {
        fprintf(stderr, "can't read frame data(frame %d@0x%x)\n",
               mp4config.frame.current,
               mp4config.frame.info[mp4config.frame.current].offset);

        return ERR_FAIL;
    }

    mp4config.frame.current++;

    return ERR_OK;
}

int mp4read_seek(uint32_t framenum)
{
    if (framenum > mp4config.frame.nsamples)
        return ERR_FAIL;
    if (fseek(g_fin, mp4config.frame.info[framenum].offset, SEEK_SET))
        return ERR_FAIL;

    mp4config.frame.current = framenum;

    return ERR_OK;
}

static void mp4info(void)
{
    fprintf(stderr, "Modification Time:\t\t\t%s\n", mp4time(mp4config.mtime));
    fprintf(stderr, "Samplerate:\t\t%d\n", mp4config.samplerate);
    fprintf(stderr, "Total samples:\t\t%d\n", mp4config.samples);
    fprintf(stderr, "Total channels:\t\t%d\n", mp4config.channels);
    fprintf(stderr, "Bits per sample:\t%d\n", mp4config.bits);
    fprintf(stderr, "Buffer size:\t\t%d\n", mp4config.buffersize);
    fprintf(stderr, "Max bitrate:\t\t%d\n", mp4config.bitratemax);
    fprintf(stderr, "Average bitrate:\t%d\n", mp4config.bitrateavg);
    fprintf(stderr, "Frames:\t\t\t%d\n", mp4config.frame.nsamples);
    fprintf(stderr, "ASC size:\t\t%d\n", mp4config.asc.size);
    fprintf(stderr, "Duration:\t\t%.1f sec\n", (float)mp4config.samples/mp4config.samplerate);
    if (mp4config.frame.nsamples)
        fprintf(stderr, "Data offset:\t%x\n", mp4config.frame.info[0].offset);
}

int mp4read_close(void)
{
    freeMem(&mp4config.frame.info);
    freeMem(&mp4config.frame.map);
    freeMem(&mp4config.bitbuf.data);

    freeMem(&mp4config.meta_title);
    freeMem(&mp4config.meta_artist);
    freeMem(&mp4config.meta_album);
    freeMem(&mp4config.cover_art.data);
    mp4config.cover_art.size = 0;
    
    if (mp4config.chapters) {
        for (uint32_t i = 0; i < mp4config.chapter_count; i++) {
            freeMem(&mp4config.chapters[i].title);
        }
        freeMem(&mp4config.chapters);
    }
    mp4config.chapter_count = 0;
    mp4config.chapter_track_id = 0;

    return ERR_OK;
}

int mp4read_open(char *name)
{
    uint32_t atomsize;
    int ret;

    mp4read_close();

    g_fin = faad_fopen(name, "rb");
    if (!g_fin)
        return ERR_FAIL;

    if (mp4config.verbose.header)
        fprintf(stderr, "**** MP4 header ****\n");
    g_atom = g_head;
    atomsize = INT_MAX;
    if (parse(&atomsize) < 0)
        goto err;
    g_atom = g_moov;
    atomsize = INT_MAX;
    rewind(g_fin);
    if ((ret = parse(&atomsize)) < 0)
    {
        fprintf(stderr, "parse:%d\n", ret);
        goto err;
    }

    // alloc frame buffer
    mp4config.bitbuf.data = malloc(mp4config.frame.maxsize);

    if (!mp4config.bitbuf.data)
        goto err;

    if (mp4config.verbose.header)
    {
        mp4info();
        fprintf(stderr, "********************\n");
    }

    if (mp4config.verbose.tags)
    {
        rewind(g_fin);
        g_atom = g_chapters;
        atomsize = INT_MAX;
        parse(&atomsize); // Ignore error (chapters are optional)

        if (mp4config.chapter_count == 0 && mp4config.chapter_track_id != 0) {
            scan_qt_chapters();
        }

        rewind(g_fin);
        g_atom = g_meta1;
        atomsize = INT_MAX;
        ret = parse(&atomsize);
        if (ret < 0)
        {
            rewind(g_fin);
            g_atom = g_meta2;
            atomsize = INT_MAX;
            ret = parse(&atomsize);
        }
    }

    return ERR_OK;
err:
    mp4read_close();
    return ERR_FAIL;
}
