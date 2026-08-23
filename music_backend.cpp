#include "music_backend.h"
#include "tags.h"
#include "icy_client.h"
#include "adts.h"
#include <glib.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <signal.h>
#include <errno.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netdb.h>

#include <fstream>
#include <vector>
#include <mutex>
#include <algorithm>

extern "C" {
#include "faad_compat.h"
#include "mpeg4/mp4read.h"
}

#define STB_VORBIS_HEADER_ONLY
#include "miniaudio/extras/stb_vorbis.c"
#undef STB_VORBIS_HEADER_ONLY

#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio/miniaudio.h"

#include "miniaudio/extras/stb_vorbis.c"

// Global mutex to protect the non-reentrant mp4read library
static std::mutex mp4_mutex;

#ifdef GST10
static inline gboolean kinamp_query_duration(GstElement *pipeline, gint64 *duration) {
    return gst_element_query_duration(pipeline, GST_FORMAT_TIME, duration);
}
#else
static inline gboolean kinamp_query_duration(GstElement *pipeline, gint64 *duration) {
    GstFormat format = GST_FORMAT_TIME;
    return gst_element_query_duration(pipeline, &format, duration);
}
#endif

const char* PIPE_PATH = "/tmp/kinamp_audio_pipe";

// How much of a stream may be buffered for format sniffing before decoding.
#define STREAM_PEEK_MAX 8192

static const int STREAM_OUTPUT_RATE = 44100;
static const int STREAM_OUTPUT_CHANNELS = 2;

// =================================================================================
// Helper Functions
// =================================================================================

static std::string get_extension(const std::string& filename) {
    size_t pos = filename.find_last_of(".");
    if (pos == std::string::npos) return "";
    std::string ext = filename.substr(pos);
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    return ext;
}

static InputType detect_input_type_helper(const char* resource) {
    if (strncmp(resource, "http://", 7) == 0 || strncmp(resource, "https://", 8) == 0) {
        return InputType::STREAM;
    }
    return InputType::FILE;
}

static AudioFormat detect_format_helper(const char* resource, InputType type) {
    if (type == InputType::STREAM) {
        return AudioFormat::UNKNOWN; 
    }

    std::string ext = get_extension(resource);
    if (ext == ".m4b" || ext == ".m4a" || ext == ".mp4") {
        return AudioFormat::M4B_AAC;
    } else if (ext == ".mp3" || ext == ".flac" || ext == ".wav" || ext == ".ogg") {
        return AudioFormat::MINIAUDIO;
    }
    
    return AudioFormat::UNKNOWN;
}


// =================================================================================
// Stream VFS Implementation
//
// Plain http:// is opened directly (see icy_client) so the response headers are
// visible and ICY now-playing titles can be stripped out of the audio. https://
// falls back to wrapping wget, as no TLS library is linked - those streams carry
// no metadata.
// =================================================================================

// Opened once, up front, so the format can be sniffed from the response before
// picking a decoder. Both transports (direct socket, wget pipe) look the same
// to the reader.
struct StreamSource {
    int fd;
    pid_t pid;          // wget child, 0 when connected directly
    IcyStream icy;
    bool use_icy;
    std::string content_type;

    // Bytes read for format sniffing, replayed before the socket is touched again.
    unsigned char peek[STREAM_PEEK_MAX];
    size_t peek_len;
    size_t peek_pos;
};

static void icy_title_trampoline(const std::string& title, void* user_data) {
    Decoder* dec = (Decoder*)user_data;
    if (dec) dec->emit_metadata(title);
}

static void stream_source_init(StreamSource* src) {
    src->fd = -1;
    src->pid = 0;
    icy_stream_init(&src->icy);
    src->use_icy = false;
    src->content_type.clear();
    src->peek_len = 0;
    src->peek_pos = 0;
}

static bool stream_source_open(StreamSource* src, const char* url, Decoder* decoder) {
    stream_source_init(src);

    // Plain HTTP: connect directly so the response headers (icy-metaint,
    // content-type) are visible. wget hides them.
    if (strncmp(url, "http://", 7) == 0) {
        src->icy.on_title = icy_title_trampoline;
        src->icy.user_data = decoder;

        if (icy_open(url, &src->icy)) {
            src->use_icy = true;
            src->fd = src->icy.fd;
            src->content_type = src->icy.content_type;

            if (decoder) decoder->set_stream_socket(src->icy.fd);

            if (!src->icy.name.empty()) {
                g_print("Decoder: station '%s'\n", src->icy.name.c_str());
            }
            if (!src->content_type.empty()) {
                g_print("Decoder: content-type %s\n", src->content_type.c_str());
            }
            if (src->icy.metaint > 0) {
                g_print("Decoder: ICY metadata every %d bytes\n", src->icy.metaint);
            } else {
                g_print("Decoder: no ICY metadata offered\n");
            }
            return true;
        }
        g_print("Decoder: direct connect failed, falling back to wget\n");
    }

    int pipefd[2];
    if (pipe(pipefd) == -1) {
        perror("StreamSource: pipe failed");
        return false;
    }

    pid_t pid = fork();
    if (pid == -1) {
        perror("StreamSource: fork failed");
        close(pipefd[0]);
        close(pipefd[1]);
        return false;
    }

    if (pid == 0) { // Child
        close(pipefd[0]); // Close read end
        dup2(pipefd[1], STDOUT_FILENO); // Redirect stdout to pipe
        close(pipefd[1]); // Close write end

        // Close unused file descriptors
        int max_fd = sysconf(_SC_OPEN_MAX);
        for (int i = 3; i < max_fd; i++) close(i);

        execlp("wget", "wget", "-q", "-T", "3", "--no-check-certificate", "-O", "-", url, (char*)NULL);
        perror("StreamSource: exec failed");
        _exit(1);
    }

    close(pipefd[1]); // Close write end
    src->fd = pipefd[0];
    src->pid = pid;
    if (decoder) decoder->set_stream_pid(pid);
    return true;
}

static void stream_source_close(StreamSource* src, Decoder* decoder) {
    if (src->use_icy) {
        // Unregister before closing, or stop() could shutdown() a stale fd.
        if (decoder) decoder->set_stream_socket(-1);
        src->use_icy = false;
    }

    if (src->fd >= 0) {
        close(src->fd);
        src->fd = -1;
        src->icy.fd = -1;
    }

    if (src->pid > 0) {
        if (decoder) decoder->set_stream_pid(0);
        kill(src->pid, SIGTERM);
        waitpid(src->pid, NULL, 0);
        src->pid = 0;
    }
}

// Audio only: interleaved ICY metadata is consumed on the way past.
static ssize_t stream_source_read(StreamSource* src, void* dst, size_t n) {
    if (n == 0) return 0;

    // Replay whatever the format sniffer already pulled off the wire.
    if (src->peek_pos < src->peek_len) {
        size_t avail = src->peek_len - src->peek_pos;
        size_t take = (n < avail) ? n : avail;
        memcpy(dst, src->peek + src->peek_pos, take);
        src->peek_pos += take;
        return (ssize_t)take;
    }

    if (src->use_icy) return icy_read(&src->icy, dst, n);

    ssize_t r;
    while (true) {
        r = read(src->fd, dst, n);
        if (r < 0 && errno == EINTR) continue;
        break;
    }
    return r;
}

// Buffers up to want bytes so they can be examined and still decoded later.
static size_t stream_source_peek(StreamSource* src, size_t want) {
    if (want > STREAM_PEEK_MAX) want = STREAM_PEEK_MAX;

    while (src->peek_len < want) {
        size_t space = want - src->peek_len;
        ssize_t r;
        if (src->use_icy) {
            r = icy_read(&src->icy, src->peek + src->peek_len, space);
        } else {
            r = read(src->fd, src->peek + src->peek_len, space);
            if (r < 0 && errno == EINTR) continue;
        }
        if (r <= 0) break;
        src->peek_len += (size_t)r;
    }
    return src->peek_len;
}

// ---------------------------------------------------------------- format sniff

static bool ct_has(const std::string& ct, const char* needle) {
    std::string lower = ct;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    return lower.find(needle) != std::string::npos;
}

static AudioFormat sniff_stream_format(StreamSource* src, const char* url) {
    const std::string& ct = src->content_type;
    if (!ct.empty()) {
        if (ct_has(ct, "aac")) return AudioFormat::AAC_ADTS;   // audio/aac, audio/aacp
        if (ct_has(ct, "mpeg") || ct_has(ct, "mp3")) return AudioFormat::MINIAUDIO;
        if (ct_has(ct, "ogg") || ct_has(ct, "vorbis")) return AudioFormat::MINIAUDIO;
        if (ct_has(ct, "flac") || ct_has(ct, "wav")) return AudioFormat::MINIAUDIO;
    }

    // No usable content-type: look at the bytes themselves.
    size_t got = stream_source_peek(src, 8192);
    if (got >= 4) {
        const unsigned char* p = src->peek;
        if (memcmp(p, "OggS", 4) == 0) return AudioFormat::MINIAUDIO;
        if (memcmp(p, "fLaC", 4) == 0) return AudioFormat::MINIAUDIO;
        if (memcmp(p, "RIFF", 4) == 0) return AudioFormat::MINIAUDIO;
        if (memcmp(p, "ID3", 3) == 0)  return AudioFormat::MINIAUDIO;
        if (adts_find_sync(p, got) >= 0) return AudioFormat::AAC_ADTS;
        if (p[0] == 0xFF && (p[1] & 0xE0) == 0xE0) return AudioFormat::MINIAUDIO; // MPEG sync
    }

    // Last resort: the URL.
    std::string ext = get_extension(url);
    if (ext == ".aac") return AudioFormat::AAC_ADTS;
    return AudioFormat::MINIAUDIO;
}

// ---------------------------------------------------------------- miniaudio VFS

// The stream is already open by the time miniaudio asks for it, so the VFS just
// wraps the live StreamSource instead of opening anything.
struct StreamVFS {
    ma_vfs_callbacks cb;
    StreamSource* src;
};

static ma_result StreamVFS_onOpen(ma_vfs* pVFS, const char* pFilePath, ma_uint32 openMode, ma_vfs_file* pFile) {
    StreamVFS* self = (StreamVFS*)pVFS;
    (void)pFilePath;
    if (openMode & MA_OPEN_MODE_WRITE) return MA_ACCESS_DENIED;
    if (!self->src || self->src->fd < 0) return MA_ERROR;

    *pFile = (ma_vfs_file)(intptr_t)self->src->fd;
    return MA_SUCCESS;
}

static ma_result StreamVFS_onOpenW(ma_vfs* pVFS, const wchar_t* pFilePath, ma_uint32 openMode, ma_vfs_file* pFile) {
    (void)pVFS; (void)pFilePath; (void)openMode; (void)pFile;
    return MA_NOT_IMPLEMENTED;
}

static ma_result StreamVFS_onClose(ma_vfs* pVFS, ma_vfs_file file) {
    (void)pVFS; (void)file;
    return MA_SUCCESS; // decode_stream owns the StreamSource lifetime
}

static ma_result StreamVFS_onRead(ma_vfs* pVFS, ma_vfs_file file, void* pDst, size_t sizeInBytes, size_t* pBytesRead) {
    StreamVFS* self = (StreamVFS*)pVFS;
    (void)file;

    ssize_t n = stream_source_read(self->src, pDst, sizeInBytes);
    if (pBytesRead) *pBytesRead = (n > 0) ? (size_t)n : 0;

    if (n < 0) return MA_IO_ERROR;
    if (n == 0) return MA_AT_END;
    return MA_SUCCESS;
}

static ma_result StreamVFS_onWrite(ma_vfs* pVFS, ma_vfs_file file, const void* pSrc, size_t sizeInBytes, size_t* pBytesWritten) {
    (void)pVFS; (void)file; (void)pSrc; (void)sizeInBytes; (void)pBytesWritten;
    return MA_ACCESS_DENIED;
}

static ma_result StreamVFS_onSeek(ma_vfs* pVFS, ma_vfs_file file, ma_int64 offset, ma_seek_origin origin) {
    (void)pVFS; (void)file;
    if (offset == 0 && origin == ma_seek_origin_start) return MA_SUCCESS;
    return MA_IO_ERROR;
}

static ma_result StreamVFS_onTell(ma_vfs* pVFS, ma_vfs_file file, ma_int64* pCursor) {
    (void)pVFS; (void)file;
    if (pCursor) *pCursor = 0;
    return MA_SUCCESS;
}

static ma_result StreamVFS_onInfo(ma_vfs* pVFS, ma_vfs_file file, ma_file_info* pInfo) {
    (void)pVFS; (void)file;
    if (pInfo) {
        pInfo->sizeInBytes = 0;
    }
    return MA_SUCCESS;
}

// =================================================================================
// Decoder Implementation
// =================================================================================

Decoder::Decoder() : stop_flag(false), running(false), thread_id(0), volume(1.0), on_error_callback(NULL), error_user_data(NULL), on_metadata_callback(NULL), metadata_user_data(NULL), last_stream_title(""), current_stream_pid(0), current_stream_fd(-1) {
    unlink(PIPE_PATH);
    if (mkfifo(PIPE_PATH, 0666) == -1) {
        perror("Decoder: Failed to create named pipe");
    }
}

Decoder::~Decoder() {
    stop();
    unlink(PIPE_PATH);
}

void Decoder::set_volume(double vol) {
    volume = vol;
}

bool Decoder::start(const char* filepath, int start_time) {
    if (running) {
        stop();
    }

    current_filepath = filepath;
    this->start_time = start_time;
    stop_flag = false;
    running = true;
    last_stream_title.clear();

    if (pthread_create(&thread_id, NULL, thread_func, this) != 0) {
        perror("Decoder: Failed to create thread");
        running = false;
        return false;
    }
    return true;
}

void Decoder::set_error_callback(ErrorCallback callback, void* user_data) {
    on_error_callback = callback;
    error_user_data = user_data;
}

void Decoder::set_metadata_callback(MetadataCallback callback, void* user_data) {
    on_metadata_callback = callback;
    metadata_user_data = user_data;
}

void Decoder::set_stream_pid(pid_t pid) {
    std::lock_guard<std::mutex> lock(pid_mutex);
    current_stream_pid = pid;
}

void Decoder::set_stream_socket(int fd) {
    std::lock_guard<std::mutex> lock(pid_mutex);
    current_stream_fd = fd;
}

void Decoder::emit_metadata(const std::string& raw_title) {
    // ICY declares no encoding; in practice it's UTF-8 or Latin-1. Invalid UTF-8
    // makes GTK labels render empty, so convert before handing it up.
    std::string title = raw_title;
    if (!g_utf8_validate(title.c_str(), -1, NULL)) {
        gchar* converted = g_convert(title.c_str(), -1, "UTF-8", "ISO-8859-1", NULL, NULL, NULL);
        if (!converted) return;
        title = converted;
        g_free(converted);
    }

    // Servers repeat the current title on many intervals.
    if (title.empty() || title == last_stream_title) return;
    last_stream_title = title;

    g_print("Decoder: ICY title: %s\n", title.c_str());
    if (on_metadata_callback) {
        on_metadata_callback(title.c_str(), metadata_user_data);
    }
}

void Decoder::stop() {
    if (!running) return;

    stop_flag = true;

    // Force kill the stream process if active to unblock fread/read
    {
        std::lock_guard<std::mutex> lock(pid_mutex);
        if (current_stream_pid > 0) {
            kill(current_stream_pid, SIGTERM);
        }
        // A directly-connected socket has no process to kill. shutdown() makes
        // the blocking read return at once instead of waiting out the receive
        // timeout.
        if (current_stream_fd >= 0) {
            shutdown(current_stream_fd, SHUT_RDWR);
        }
    }

    int fd = open(PIPE_PATH, O_RDONLY | O_NONBLOCK);
    if (fd >= 0) close(fd);

    if (thread_id != 0) {
        pthread_join(thread_id, NULL);
        thread_id = 0;
    }

    running = false;
}

bool Decoder::is_running() const {
    return running;
}

void* Decoder::thread_func(void* arg) {
    Decoder* self = static_cast<Decoder*>(arg);
    self->decode_loop();
    return NULL;
}

void Decoder::decode_loop() {
    g_print("Decoder: Starting for %s\n", current_filepath.c_str());

    InputType inputType = detect_input_type(current_filepath.c_str());
    AudioFormat format = detect_format(current_filepath.c_str(), inputType);

    if (inputType == InputType::STREAM) {
        decode_stream(current_filepath.c_str());
    } else if (format == AudioFormat::M4B_AAC) {
        decode_mp4_file(current_filepath.c_str(), start_time);
    } else if (format == AudioFormat::MINIAUDIO) {
        decode_miniaudio(current_filepath.c_str(), start_time);
    } else {
        g_printerr("Decoder: Unsupported format or input type for %s\n", current_filepath.c_str());
        running = false;
    }
}

void Decoder::decode_mp4_file(const char* filepath, int start_time) {
    std::lock_guard<std::mutex> lock(mp4_mutex);

    if (mp4read_open(const_cast<char*>(filepath)) != 0) {
        g_printerr("Decoder: Failed to open file with mp4read: %s\n", filepath);
        return;
    }

    NeAACDecHandle hDecoder = NeAACDecOpen();
    if (!hDecoder) {
        g_printerr("Decoder: Failed to open FAAD2 decoder\n");
        mp4read_close();
        return;
    }

    NeAACDecConfigurationPtr config = NeAACDecGetCurrentConfiguration(hDecoder);
    config->outputFormat = FAAD_FMT_16BIT;
    config->downMatrix = 1;
    NeAACDecSetConfiguration(hDecoder, config);

    unsigned long samplerate;
    unsigned char channels;
    if ((int8_t)NeAACDecInit2(hDecoder, mp4config.asc.buf, mp4config.asc.size, &samplerate, &channels) < 0) {
        g_printerr("Decoder: Failed to initialize FAAD2 with ASC\n");
        NeAACDecClose(hDecoder);
        mp4read_close();
        return;
    }
    g_print("Decoder: M4B Init %lu Hz, %d channels\n", samplerate, channels);

    if (start_time > 0) {
        unsigned long samples_per_frame = 1024;
        if (mp4config.frame.nsamples > 0 && mp4config.samples > 0) {
             samples_per_frame = mp4config.samples / mp4config.frame.nsamples;
        }
        
        unsigned long target_frame = (unsigned long)((double)start_time * samplerate / samples_per_frame);
        
        if (target_frame < mp4config.frame.nsamples) {
             if (mp4read_seek(target_frame) == 0) {
                 g_print("Decoder: Seeked to %d seconds (frame %lu)\n", start_time, target_frame);
             } else {
                 g_printerr("Decoder: Failed to seek to frame %lu\n", target_frame);
             }
        }
    } else {
        mp4read_seek(0);
    }

    if (stop_flag) {
        NeAACDecClose(hDecoder);
        mp4read_close();
        return;
    }

    int fd = open(PIPE_PATH, O_WRONLY);
    if (fd == -1) {
        perror("Decoder: Failed to open pipe");
        NeAACDecClose(hDecoder);
        mp4read_close();
        return;
    }

    if (stop_flag) {
        close(fd);
        NeAACDecClose(hDecoder);
        mp4read_close();
        return;
    }

    while (!stop_flag) {
        if (mp4read_frame() != 0) {
            break;
        }

        NeAACDecFrameInfo frameInfo;
        void* sample_buffer = NeAACDecDecode(hDecoder, &frameInfo, 
                                             mp4config.bitbuf.data, 
                                             mp4config.bitbuf.size);

        if (frameInfo.error > 0) {
             g_printerr("Decoder: FAAD Warning: %s\n", NeAACDecGetErrorMessage(frameInfo.error));
             continue;
        }

        if (frameInfo.samples > 0) {
            int16_t* samples = (int16_t*)sample_buffer;
            double vol = volume.load();
            if (vol != 1.0) {
                for (unsigned int i = 0; i < frameInfo.samples; ++i) {
                    samples[i] = (int16_t)(samples[i] * vol);
                }
            }
            ssize_t to_write = frameInfo.samples * 2; 
            ssize_t written = write(fd, samples, to_write);

            if (written == -1) {
                if (errno == EPIPE) {
                    break;
                }
                perror("Decoder: write error");
                break;
            }
        }
    }

    close(fd);
    NeAACDecClose(hDecoder);
    mp4read_close();
    g_print("Decoder: M4B Thread exiting.\n");
}

void Decoder::decode_miniaudio(const char* filepath, int start_time) {
    ma_decoder_config decoder_config = ma_decoder_config_init(ma_format_s16, 2, 0); 
    ma_decoder decoder;
    
    ma_result result = ma_decoder_init_file(filepath, &decoder_config, &decoder);
    if (result != MA_SUCCESS) {
        g_printerr("Decoder: Failed to open file with miniaudio: %s (Result: %d)\n", filepath, result);
        return;
    }
    
    g_print("Decoder: Miniaudio Init %d Hz, %d channels\n", decoder.outputSampleRate, decoder.outputChannels);

    if (start_time > 0) {
        ma_uint64 target_frame = (ma_uint64)start_time * decoder.outputSampleRate;
        result = ma_decoder_seek_to_pcm_frame(&decoder, target_frame);
        if (result != MA_SUCCESS) {
            g_printerr("Decoder: Failed to seek to %d seconds\n", start_time);
        } else {
            g_print("Decoder: Seeked to %d seconds\n", start_time);
        }
    }

    int fd = open(PIPE_PATH, O_WRONLY);
    if (fd == -1) {
        perror("Decoder: Failed to open pipe");
        ma_decoder_uninit(&decoder);
        return;
    }
    
    if (stop_flag) {
        close(fd);
        ma_decoder_uninit(&decoder);
        return;
    }

    const size_t FRAMES_PER_READ = 1024;
    std::vector<int16_t> pcm_buffer(FRAMES_PER_READ * decoder.outputChannels);

    while (!stop_flag) {
        ma_uint64 frames_read = 0;
        result = ma_decoder_read_pcm_frames(&decoder, pcm_buffer.data(), FRAMES_PER_READ, &frames_read);
        
        if (frames_read == 0) {
            if (result != MA_SUCCESS && result != MA_AT_END) {
                 g_printerr("Decoder: Miniaudio read error: %d\n", result);
            }
            break;
        }

        double vol = volume.load();
        if (vol != 1.0) {
            for (size_t i = 0; i < frames_read * decoder.outputChannels; ++i) {
                pcm_buffer[i] = (int16_t)(pcm_buffer[i] * vol);
            }
        }

        ssize_t to_write = frames_read * decoder.outputChannels * sizeof(int16_t);
        ssize_t written = write(fd, pcm_buffer.data(), to_write);

        if (written == -1) {
            if (errno == EPIPE) {
                break;
            }
            perror("Decoder: write error");
            break;
        }
        
        if (result == MA_AT_END) break;
    }

    close(fd);
    ma_decoder_uninit(&decoder);
    g_print("Decoder: Miniaudio Thread exiting.\n");
}

void Decoder::decode_stream(const char* url) {
    StreamSource src;
    if (!stream_source_open(&src, url, this)) {
        g_printerr("Decoder: Failed to open stream: %s\n", url);
        if (on_error_callback) {
            on_error_callback("Unable to connect to the stream.", error_user_data);
        }
        return;
    }

    if (stop_flag) {
        stream_source_close(&src, this);
        return;
    }

    AudioFormat format = sniff_stream_format(&src, url);

    int fd = open(PIPE_PATH, O_WRONLY);
    if (fd == -1) {
        perror("Decoder: Failed to open pipe");
        stream_source_close(&src, this);
        return;
    }

    if (format == AudioFormat::AAC_ADTS) {
        g_print("Decoder: Stream format AAC (ADTS)\n");
        decode_stream_aac(&src, fd);
    } else {
        g_print("Decoder: Stream format MP3/OGG/FLAC (miniaudio)\n");
        decode_stream_miniaudio(&src, url, fd);
    }

    close(fd);
    stream_source_close(&src, this);
    g_print("Decoder: Stream Thread exiting.\n");
}

void Decoder::decode_stream_miniaudio(StreamSource* src, const char* url, int out_fd) {
    StreamVFS vfs;
    memset(&vfs.cb, 0, sizeof(vfs.cb));
    vfs.cb.onOpen = StreamVFS_onOpen;
    vfs.cb.onOpenW = StreamVFS_onOpenW;
    vfs.cb.onClose = StreamVFS_onClose;
    vfs.cb.onRead = StreamVFS_onRead;
    vfs.cb.onWrite = StreamVFS_onWrite;
    vfs.cb.onSeek = StreamVFS_onSeek;
    vfs.cb.onTell = StreamVFS_onTell;
    vfs.cb.onInfo = StreamVFS_onInfo;
    vfs.src = src;

    // The pipeline's rate, not the stream's: the caps are already fixed, so a
    // 48 kHz stream would play at the wrong pitch.
    ma_decoder_config decoder_config =
        ma_decoder_config_init(ma_format_s16, STREAM_OUTPUT_CHANNELS, STREAM_OUTPUT_RATE);
    ma_decoder decoder;

    ma_result result = ma_decoder_init_vfs((ma_vfs*)&vfs, url, &decoder_config, &decoder);

    if (result != MA_SUCCESS) {
        g_printerr("Decoder: Failed to open stream: %s (Result: %d)\n", url, result);
        if (on_error_callback) {
             on_error_callback("Unable to play stream. Ensure it is a supported format (MP3/AAC/OGG/FLAC/WAV).", error_user_data);
        }
        return;
    }

    g_print("Decoder: Stream Init %d Hz, %d channels\n", decoder.outputSampleRate, decoder.outputChannels);

    if (stop_flag) {
        ma_decoder_uninit(&decoder);
        return;
    }

    const size_t FRAMES_PER_READ = 1024;
    std::vector<int16_t> pcm_buffer(FRAMES_PER_READ * decoder.outputChannels);

    while (!stop_flag) {
        ma_uint64 frames_read = 0;
        result = ma_decoder_read_pcm_frames(&decoder, pcm_buffer.data(), FRAMES_PER_READ, &frames_read);

        if (frames_read == 0) {
            if (result != MA_SUCCESS && result != MA_AT_END) {
                 g_printerr("Decoder: Stream read error: %d\n", result);
            }
            break;
        }

        double vol = volume.load();
        if (vol != 1.0) {
            for (size_t i = 0; i < frames_read * decoder.outputChannels; ++i) {
                pcm_buffer[i] = (int16_t)(pcm_buffer[i] * vol);
            }
        }

        ssize_t to_write = frames_read * decoder.outputChannels * sizeof(int16_t);
        ssize_t written = write(out_fd, pcm_buffer.data(), to_write);

        if (written == -1) {
            if (errno == EPIPE) {
                break;
            }
            perror("Decoder: write error");
            break;
        }

        if (result == MA_AT_END) break;
    }

    ma_decoder_uninit(&decoder);
}

// Raw AAC in ADTS framing, as served by Shoutcast/Icecast AAC and AAC+ stations.
// FAAD2 takes this through NeAACDecInit, which syncs to an ADTS header. The
// M4A/M4B path instead feeds it an AudioSpecificConfig from the MP4 container.
void Decoder::decode_stream_aac(StreamSource* src, int out_fd) {
    NeAACDecHandle hDecoder = NeAACDecOpen();
    if (!hDecoder) {
        g_printerr("Decoder: Failed to open FAAD2 decoder\n");
        if (on_error_callback) on_error_callback("Unable to start the AAC decoder.", error_user_data);
        return;
    }

    NeAACDecConfigurationPtr config = NeAACDecGetCurrentConfiguration(hDecoder);
    config->outputFormat = FAAD_FMT_16BIT;
    config->downMatrix = 1; // fold >2 channels down to stereo
    NeAACDecSetConfiguration(hDecoder, config);

    const size_t BUF_CAP = 65536;
    const size_t REFILL_BELOW = 16384; // comfortably more than one ADTS frame
    std::vector<unsigned char> buf(BUF_CAP);
    size_t len = 0;

    // Prime enough bytes for NeAACDecInit to find a sync word.
    while (len < REFILL_BELOW && !stop_flag) {
        ssize_t n = stream_source_read(src, buf.data() + len, BUF_CAP - len);
        if (n <= 0) break;
        len += (size_t)n;
    }

    if (stop_flag || len == 0) {
        NeAACDecClose(hDecoder);
        return;
    }

    // We join a live stream mid-frame, so align to a real frame boundary first.
    // NeAACDecInit doesn't search for one: given unaligned data it reports
    // 44100 Hz stereo defaults and then fails on every frame.
    long sync = adts_find_sync(buf.data(), len);
    if (sync < 0) {
        g_printerr("Decoder: No ADTS sync found in stream\n");
        if (on_error_callback) on_error_callback("Not a valid AAC stream.", error_user_data);
        NeAACDecClose(hDecoder);
        return;
    }
    if (sync > 0) {
        g_print("Decoder: skipped %ld bytes to ADTS frame boundary\n", sync);
        memmove(buf.data(), buf.data() + sync, len - (size_t)sync);
        len -= (size_t)sync;
    }

    unsigned long core_rate = 0;
    unsigned char core_channels = 0;
    long skip = NeAACDecInit(hDecoder, buf.data(), len, &core_rate, &core_channels);
    if (skip < 0) {
        g_printerr("Decoder: NeAACDecInit failed\n");
        if (on_error_callback) on_error_callback("Not a valid AAC stream.", error_user_data);
        NeAACDecClose(hDecoder);
        return;
    }
    if ((size_t)skip > len) skip = (long)len;
    if (skip > 0) {
        memmove(buf.data(), buf.data() + skip, len - (size_t)skip);
        len -= (size_t)skip;
    }
    g_print("Decoder: AAC core %lu Hz, %d channels\n", core_rate, (int)core_channels);

    // With SBR (HE-AAC) the decoded rate is double the core rate, and with
    // Parametric Stereo a mono core decodes to stereo. Neither is known until a
    // frame comes out, so set the converters up from the first good one.
    ma_resampler resampler;
    bool have_resampler = false;
    unsigned long out_rate_in = 0;
    unsigned int out_channels_in = 0;

    std::vector<int16_t> stereo;    // input frames folded to stereo
    std::vector<int16_t> resampled; // stereo at STREAM_OUTPUT_RATE

    int consecutive_errors = 0;
    bool eof = false;

    while (!stop_flag) {
        if (len < REFILL_BELOW && !eof) {
            ssize_t n = stream_source_read(src, buf.data() + len, BUF_CAP - len);
            if (n > 0) {
                len += (size_t)n;
            } else {
                eof = true;
            }
        }
        if (len == 0) break;

        NeAACDecFrameInfo frameInfo;
        memset(&frameInfo, 0, sizeof(frameInfo));
        void* sample_buffer = NeAACDecDecode(hDecoder, &frameInfo, buf.data(), len);

        if (frameInfo.error > 0) {
            g_printerr("Decoder: FAAD Warning: %s\n", NeAACDecGetErrorMessage(frameInfo.error));

            // Jump to the next frame boundary instead of giving up - a dropped
            // packet shouldn't end the station. Searching for the next sync
            // beats stepping a byte at a time through a corrupt frame.
            size_t advance;
            long next = (len > 1) ? adts_find_sync(buf.data() + 1, len - 1) : -1;
            if (next >= 0) {
                advance = (size_t)next + 1;
            } else {
                advance = (frameInfo.bytesconsumed > 0) ? frameInfo.bytesconsumed : 1;
            }
            if (advance > len) advance = len;
            memmove(buf.data(), buf.data() + advance, len - advance);
            len -= advance;

            if (++consecutive_errors > 64) {
                g_printerr("Decoder: Too many consecutive AAC errors, giving up\n");
                break;
            }
            continue;
        }
        consecutive_errors = 0;

        size_t advance = frameInfo.bytesconsumed;
        if (advance == 0 && frameInfo.samples == 0) {
            // No progress and no output: without this the loop would spin.
            if (eof) break;
            advance = 1;
        }
        if (advance > len) advance = len;
        if (advance > 0) {
            memmove(buf.data(), buf.data() + advance, len - advance);
            len -= advance;
        }

        if (frameInfo.samples == 0 || sample_buffer == NULL) {
            if (eof && len == 0) break;
            continue;
        }

        unsigned int in_channels = frameInfo.channels;
        unsigned long in_rate = frameInfo.samplerate;
        if (in_channels == 0 || in_rate == 0) continue;

        if (in_rate != out_rate_in || in_channels != out_channels_in) {
            g_print("Decoder: AAC output %lu Hz, %u channels\n", in_rate, in_channels);
            out_rate_in = in_rate;
            out_channels_in = in_channels;

            if (have_resampler) {
                ma_resampler_uninit(&resampler, NULL);
                have_resampler = false;
            }
            if (in_rate != (unsigned long)STREAM_OUTPUT_RATE) {
                ma_resampler_config rc = ma_resampler_config_init(
                    ma_format_s16, STREAM_OUTPUT_CHANNELS,
                    (ma_uint32)in_rate, (ma_uint32)STREAM_OUTPUT_RATE,
                    ma_resample_algorithm_linear);
                if (ma_resampler_init(&rc, NULL, &resampler) == MA_SUCCESS) {
                    have_resampler = true;
                } else {
                    g_printerr("Decoder: Failed to init resampler %lu -> %d Hz\n",
                               in_rate, STREAM_OUTPUT_RATE);
                }
            }
        }

        // FAAD reports total samples across all channels.
        size_t in_frames = frameInfo.samples / in_channels;
        if (in_frames == 0) continue;

        const int16_t* pcm = (const int16_t*)sample_buffer;
        stereo.resize(in_frames * STREAM_OUTPUT_CHANNELS);

        if (in_channels == 1) {
            for (size_t i = 0; i < in_frames; ++i) {
                stereo[i * 2] = pcm[i];
                stereo[i * 2 + 1] = pcm[i];
            }
        } else if (in_channels == 2) {
            memcpy(stereo.data(), pcm, in_frames * 2 * sizeof(int16_t));
        } else {
            // downMatrix should prevent this, but never read past the frame.
            for (size_t i = 0; i < in_frames; ++i) {
                stereo[i * 2] = pcm[i * in_channels];
                stereo[i * 2 + 1] = pcm[i * in_channels + 1];
            }
        }

        const int16_t* out_pcm = stereo.data();
        size_t out_frames = in_frames;

        if (have_resampler) {
            ma_uint64 frames_in = in_frames;
            ma_uint64 frames_out = 0;
            if (ma_resampler_get_expected_output_frame_count(&resampler, frames_in, &frames_out) != MA_SUCCESS) {
                frames_out = in_frames * STREAM_OUTPUT_RATE / out_rate_in + 1;
            }
            resampled.resize((size_t)frames_out * STREAM_OUTPUT_CHANNELS);

            ma_uint64 consumed = frames_in;
            ma_uint64 produced = frames_out;
            if (ma_resampler_process_pcm_frames(&resampler, stereo.data(), &consumed,
                                                resampled.data(), &produced) != MA_SUCCESS) {
                continue;
            }
            out_pcm = resampled.data();
            out_frames = (size_t)produced;
        }

        if (out_frames == 0) continue;

        double vol = volume.load();
        if (vol != 1.0) {
            int16_t* writable = (out_pcm == stereo.data()) ? stereo.data() : resampled.data();
            for (size_t i = 0; i < out_frames * STREAM_OUTPUT_CHANNELS; ++i) {
                writable[i] = (int16_t)(writable[i] * vol);
            }
        }

        ssize_t to_write = (ssize_t)(out_frames * STREAM_OUTPUT_CHANNELS * sizeof(int16_t));
        ssize_t written = write(out_fd, out_pcm, to_write);
        if (written == -1) {
            if (errno == EPIPE) break;
            perror("Decoder: write error");
            break;
        }
    }

    if (have_resampler) ma_resampler_uninit(&resampler, NULL);
    NeAACDecClose(hDecoder);
}

AudioFormat Decoder::detect_format(const char* resource, InputType type) {
    return detect_format_helper(resource, type);
}

InputType Decoder::detect_input_type(const char* resource) {
    return detect_input_type_helper(resource);
}


// =================================================================================
// MusicBackend Implementation
// =================================================================================

MusicBackend::MusicBackend() 
    : is_playing(false), is_paused(false),
      meta_title(""), meta_artist(""), meta_album(""), cover_art(), chapters(),
      current_samplerate(44100), total_duration(0),
      decoder(new Decoder()),
      pipeline(NULL), bus(NULL), bus_watch_id(0),
      current_filepath_str(""), stopping(false),
      on_eos_callback(NULL), eos_user_data(NULL),
      on_error_callback(NULL), error_user_data(NULL),
      on_metadata_callback(NULL), metadata_user_data(NULL),
      last_position(0), current_volume(1.0)
{
    signal(SIGPIPE, SIG_IGN);

    decoder->set_error_callback(internal_decoder_error_callback, this);
    decoder->set_metadata_callback(internal_decoder_metadata_callback, this);
    decoder->set_volume(current_volume);

    gst_init(NULL, NULL);
}

MusicBackend::~MusicBackend() {
    stop();
}

bool MusicBackend::is_shutting_down() const {
    return stopping;
}

const char* MusicBackend::get_current_filepath() {
    return current_filepath_str.c_str();
}

void MusicBackend::set_eos_callback(EosCallback callback, void* user_data) {
    on_eos_callback = callback;
    eos_user_data = user_data;
}

void MusicBackend::set_error_callback(ErrorCallback callback, void* user_data) {
    on_error_callback = callback;
    error_user_data = user_data;
}

void MusicBackend::internal_decoder_error_callback(const char* msg, void* user_data) {
    MusicBackend* self = static_cast<MusicBackend*>(user_data);
    if (self && self->on_error_callback) {
        self->on_error_callback(msg, self->error_user_data);
    }
}

void MusicBackend::internal_decoder_metadata_callback(const char* title, void* user_data) {
    MusicBackend* self = static_cast<MusicBackend*>(user_data);
    if (self && self->on_metadata_callback) {
        self->on_metadata_callback(title, self->metadata_user_data);
    }
}

void MusicBackend::set_metadata_callback(MetadataCallback callback, void* user_data) {
    on_metadata_callback = callback;
    metadata_user_data = user_data;
}

gint64 MusicBackend::get_duration() {
    if (total_duration > 0) return total_duration;

    if (pipeline) {
        gint64 duration;
        if (kinamp_query_duration(pipeline, &duration)) {
            return duration;
        }
    }
    return 0;
}

gint64 MusicBackend::get_position() {
    if (is_paused) {
        return last_position;
    }

    if (pipeline && is_playing) {
        GstClock *clock = gst_element_get_clock(pipeline);
        if (clock) {
            GstClockTime current_time = gst_clock_get_time(clock);
            GstClockTime base_time = gst_element_get_base_time(pipeline);
            gst_object_unref(clock);

            if (GST_CLOCK_TIME_IS_VALID(base_time) && current_time > base_time) {
                return (gint64)(current_time - base_time) + last_position;
            }
        }
    }
    return last_position;
}

void MusicBackend::read_metadata(const char* filepath) {
    meta_title.clear();
    meta_artist.clear();
    meta_album.clear();
    cover_art.clear();
    chapters.clear();
    current_samplerate = 44100; 
    total_duration = 0;

    if (filepath == nullptr) return;

    InputType type = detect_input_type_helper(filepath);
    AudioFormat format = detect_format_helper(filepath, type);

    if (type == InputType::STREAM) { 
        return; 
    }

    if (format == AudioFormat::M4B_AAC) {
        std::lock_guard<std::mutex> lock(mp4_mutex);
        mp4config.verbose.tags = 1;

        if (mp4read_open((char*)filepath) == 0) {
            if (mp4config.meta_title) meta_title = mp4config.meta_title;
            if (mp4config.meta_artist) meta_artist = mp4config.meta_artist;
            if (mp4config.meta_album) meta_album = mp4config.meta_album;
            if (mp4config.cover_art.data && mp4config.cover_art.size > 0) {
                cover_art.assign(mp4config.cover_art.data, mp4config.cover_art.data + mp4config.cover_art.size);
            }
            
            if (mp4config.chapters && mp4config.chapter_count > 0) {
                for (uint32_t i = 0; i < mp4config.chapter_count; ++i) {
                    Chapter ch;
                    ch.timestamp = mp4config.chapters[i].timestamp;
                    ch.title = mp4config.chapters[i].title ? mp4config.chapters[i].title : "";
                    chapters.push_back(ch);
                }
            }
            
            NeAACDecHandle hDecoder = NeAACDecOpen();
            if (hDecoder) {
                 NeAACDecConfigurationPtr config = NeAACDecGetCurrentConfiguration(hDecoder);
                 config->outputFormat = FAAD_FMT_16BIT;
                 NeAACDecSetConfiguration(hDecoder, config);

                 unsigned long rate = 0;
                 unsigned char channels = 0;
                 if ((int8_t)NeAACDecInit2(hDecoder, mp4config.asc.buf, mp4config.asc.size, &rate, &channels) >= 0) {
                     if (rate > 0) {
                         current_samplerate = (int)rate;
                     }
                 }
                 NeAACDecClose(hDecoder);
            }
            
            if (mp4config.samplerate > 0 && mp4config.samples > 0) {
                total_duration = (gint64)mp4config.samples * GST_SECOND / mp4config.samplerate;
            }

            mp4read_close();
        } else {
            g_printerr("Backend: Failed to read metadata for %s\n", filepath);
        }
        mp4config.verbose.tags = 0;
    } else if (format == AudioFormat::MINIAUDIO) {
        // miniaudio has no tag API, so ID3 and Vorbis comments are parsed
        // separately. Missing tags stay empty and the UI falls back to the file
        // name.
        AudioTags tags;
        if (read_audio_tags(filepath, &tags)) {
            meta_title = tags.title;
            meta_artist = tags.artist;
            meta_album = tags.album;
        }

        ma_decoder_config decoder_config = ma_decoder_config_init(ma_format_s16, 2, 0);
        ma_decoder temp_decoder;
        ma_result result = ma_decoder_init_file(filepath, &decoder_config, &temp_decoder);
        if (result == MA_SUCCESS) {
            current_samplerate = temp_decoder.outputSampleRate;
            ma_uint64 lengthInFrames;
            if (ma_decoder_get_length_in_pcm_frames(&temp_decoder, &lengthInFrames) == MA_SUCCESS) {
                total_duration = (gint64)lengthInFrames * GST_SECOND / current_samplerate;
            }
            ma_decoder_uninit(&temp_decoder);
            g_print("Backend: Miniaudio metadata %d Hz, %lld ns duration\n", current_samplerate, (long long)total_duration);
        } else {
             g_printerr("Backend: Miniaudio failed to probe %s\n", filepath);
        }
    }
}

void MusicBackend::play_file(const char* filepath, int start_time) {
    if (stopping) return;

    if (is_playing || is_paused) {
        stop();
    }

    current_filepath_str = filepath;
    
    InputType type = detect_input_type_helper(filepath);
    if (type == InputType::STREAM) {
        current_samplerate = 44100; 
        total_duration = 0;
    } else {
        read_metadata(filepath);
    }

    g_print("Backend: Playing %s from %d\n", filepath, start_time);
    is_playing = true;
    is_paused = false;
    last_position = start_time * GST_SECOND;

    int rate = (current_samplerate > 0) ? current_samplerate : 44100;

    GError *pipeline_error = NULL;
#ifdef GST10
    gchar *pipeline_desc = g_strdup_printf(
        "filesrc location=\"%s\" ! audio/x-raw, format=S16LE, layout=interleaved, rate=%d, channels=2 ! queue ! mixersink",
        PIPE_PATH, rate
    );
#else
    gchar *pipeline_desc = g_strdup_printf(
        "filesrc location=\"%s\" ! audio/x-raw-int, endianness=1234, signed=true, width=16, depth=16, rate=%d, channels=2 ! queue ! mixersink",
        PIPE_PATH, rate
    );
#endif
    pipeline = gst_parse_launch(pipeline_desc, &pipeline_error);

    if (!pipeline) {
        g_printerr("Backend: Failed to create pipeline (%s): %s\n",
                   pipeline_desc,
                   pipeline_error ? pipeline_error->message : "unknown error");
        if (pipeline_error) g_error_free(pipeline_error);
        g_free(pipeline_desc);
        is_playing = false;
        return;
    }
    if (pipeline_error) g_error_free(pipeline_error);
    g_free(pipeline_desc);

    bus = gst_element_get_bus(pipeline);
    bus_watch_id = gst_bus_add_watch(bus, bus_callback_func, this);
    gst_object_unref(bus);

    if (!decoder->start(filepath, start_time)) {
        cleanup_pipeline();
        return;
    }

    gst_element_set_state(pipeline, GST_STATE_PLAYING);
}

void MusicBackend::pause() {
    if (!pipeline || !is_playing) return;

    if (is_paused) {
        GstClock *clock = gst_element_get_clock(pipeline);
        if (clock) {
            GstClockTime current_time = gst_clock_get_time(clock);
            GstClockTime base_time = gst_element_get_base_time(pipeline);
            gst_object_unref(clock);

            if (GST_CLOCK_TIME_IS_VALID(base_time) && current_time > base_time) {
                gint64 running_time = (gint64)(current_time - base_time);
                last_position -= running_time;
            }
        }
        
        gst_element_set_state(pipeline, GST_STATE_PLAYING);
        is_paused = false;
    } else {
        last_position = get_position();
        gst_element_set_state(pipeline, GST_STATE_PAUSED);
        is_paused = true;
    }
}

void MusicBackend::stop() {
    if (stopping) return;
    stopping = true;

    if (pipeline) {
        gst_element_set_state(pipeline, GST_STATE_NULL);
    }

    decoder->stop();

    cleanup_pipeline();
    
    stopping = false;
    is_playing = false;
    is_paused = false;
}

void MusicBackend::set_volume(double volume) {
    current_volume = volume;
    decoder->set_volume(volume);
}

double MusicBackend::get_volume() {
    return current_volume;
}

void MusicBackend::cleanup_pipeline() {
    if (bus_watch_id > 0) {
        g_source_remove(bus_watch_id);
        bus_watch_id = 0;
    }
    if (pipeline) {
        gst_element_set_state(pipeline, GST_STATE_NULL);
        gst_object_unref(pipeline);
        pipeline = NULL;
    }
}

gboolean MusicBackend::bus_callback_func(GstBus *bus, GstMessage *msg, gpointer data) {
    (void)bus;
    MusicBackend* self = static_cast<MusicBackend*>(data);

    switch (GST_MESSAGE_TYPE(msg)) {
        case GST_MESSAGE_EOS:
            g_print("Backend: EOS reached.\n");
            self->stop(); 

            if (self->on_eos_callback) {
                self->on_eos_callback(self->eos_user_data);
            }
            break;
        case GST_MESSAGE_ERROR: {
            GError *err;
            gchar *debug;
            gst_message_parse_error(msg, &err, &debug);
            g_printerr("Backend: Error: %s\n", err->message);
            g_error_free(err);
            g_free(debug);
            self->stop();
            break;
        }
        default:
            break;
    }
    return TRUE;
}