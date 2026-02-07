#include "music_backend.h"
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

#include <fstream>
#include <vector>
#include <mutex>
#include <algorithm>

extern "C" {
#include <faad/neaacdec.h>
#include "mpeg4/mp4read.h"
}

#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio/miniaudio.h"

// Global mutex to protect the non-reentrant mp4read library
static std::mutex mp4_mutex;

const char* PIPE_PATH = "/tmp/kinamp_audio_pipe";

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
// Stream VFS Implementation (wget wrapper)
// =================================================================================

struct StreamVFS {
    ma_vfs_callbacks cb;
    int fd;
    pid_t pid;
    Decoder* decoder;
};

static ma_result StreamVFS_onOpen(ma_vfs* pVFS, const char* pFilePath, ma_uint32 openMode, ma_vfs_file* pFile) {
    StreamVFS* self = (StreamVFS*)pVFS;
    if (openMode & MA_OPEN_MODE_WRITE) return MA_ACCESS_DENIED;
    
    int pipefd[2];
    if (pipe(pipefd) == -1) {
        perror("StreamVFS: pipe failed");
        return MA_ERROR;
    }

    pid_t pid = fork();
    if (pid == -1) {
        perror("StreamVFS: fork failed");
        close(pipefd[0]);
        close(pipefd[1]);
        return MA_ERROR;
    }

    if (pid == 0) { // Child
        close(pipefd[0]); // Close read end
        dup2(pipefd[1], STDOUT_FILENO); // Redirect stdout to pipe
        close(pipefd[1]); // Close write end

        // Close unused file descriptors
        int max_fd = sysconf(_SC_OPEN_MAX);
        for (int i = 3; i < max_fd; i++) close(i);

        execlp("wget", "wget", "-q", "-T", "3", "--no-check-certificate", "-O", "-", pFilePath, (char*)NULL);
        perror("StreamVFS: exec failed");
        _exit(1);
    } else { // Parent
        close(pipefd[1]); // Close write end
        self->fd = pipefd[0];
        self->pid = pid;
        
        if (self->decoder) {
            self->decoder->set_stream_pid(pid);
        }

        *pFile = (ma_vfs_file)(intptr_t)self->fd;
        return MA_SUCCESS;
    }
}

static ma_result StreamVFS_onOpenW(ma_vfs* pVFS, const wchar_t* pFilePath, ma_uint32 openMode, ma_vfs_file* pFile) {
    (void)pVFS; (void)pFilePath; (void)openMode; (void)pFile;
    return MA_NOT_IMPLEMENTED;
}

static ma_result StreamVFS_onClose(ma_vfs* pVFS, ma_vfs_file file) {
    StreamVFS* self = (StreamVFS*)pVFS;
    int fd = (int)(intptr_t)file;
    
    if (fd >= 0) {
        close(fd);
    }
    
    if (self->pid > 0) {
        if (self->decoder) {
            self->decoder->set_stream_pid(0);
        }
        kill(self->pid, SIGTERM);
        waitpid(self->pid, NULL, 0);
        self->pid = 0;
    }

    return MA_SUCCESS;
}

static ma_result StreamVFS_onRead(ma_vfs* pVFS, ma_vfs_file file, void* pDst, size_t sizeInBytes, size_t* pBytesRead) {
    (void)pVFS;
    int fd = (int)(intptr_t)file;
    
    ssize_t bytesRead = 0;
    while (true) {
        bytesRead = read(fd, pDst, sizeInBytes);
        if (bytesRead < 0 && errno == EINTR) continue;
        break;
    }

    if (pBytesRead) *pBytesRead = (bytesRead > 0) ? bytesRead : 0;
    
    if (bytesRead == 0) return MA_AT_END;
    if (bytesRead < 0) return MA_IO_ERROR;
    
    return MA_SUCCESS;
}

static ma_result StreamVFS_onWrite(ma_vfs* pVFS, ma_vfs_file file, const void* pSrc, size_t sizeInBytes, size_t* pBytesWritten) {
    (void)pVFS; (void)file; (void)pSrc; (void)sizeInBytes; (void)pBytesWritten;
    return MA_ACCESS_DENIED;
}

static ma_result StreamVFS_onSeek(ma_vfs* pVFS, ma_vfs_file file, ma_int64 offset, ma_seek_origin origin) {
    (void)pVFS; (void)file; (void)offset; (void)origin;
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

Decoder::Decoder() : stop_flag(false), running(false), thread_id(0), on_error_callback(NULL), error_user_data(NULL), current_stream_pid(0) {
    unlink(PIPE_PATH);
    if (mkfifo(PIPE_PATH, 0666) == -1) {
        perror("Decoder: Failed to create named pipe");
    }
}

Decoder::~Decoder() {
    stop();
    unlink(PIPE_PATH);
}

bool Decoder::start(const char* filepath, int start_time) {
    if (running) {
        stop();
    }

    current_filepath = filepath;
    this->start_time = start_time;
    stop_flag = false;
    running = true;

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

void Decoder::set_stream_pid(pid_t pid) {
    std::lock_guard<std::mutex> lock(pid_mutex);
    current_stream_pid = pid;
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
            ssize_t to_write = frameInfo.samples * 2; 
            ssize_t written = write(fd, sample_buffer, to_write);

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
    StreamVFS vfs;
    memset(&vfs, 0, sizeof(vfs));
    vfs.cb.onOpen = StreamVFS_onOpen;
    vfs.cb.onOpenW = StreamVFS_onOpenW;
    vfs.cb.onClose = StreamVFS_onClose;
    vfs.cb.onRead = StreamVFS_onRead;
    vfs.cb.onWrite = StreamVFS_onWrite;
    vfs.cb.onSeek = StreamVFS_onSeek;
    vfs.cb.onTell = StreamVFS_onTell;
    vfs.cb.onInfo = StreamVFS_onInfo;
    vfs.fd = -1;
    vfs.pid = 0;
    vfs.decoder = this;

    int fd = open(PIPE_PATH, O_WRONLY);
    if (fd == -1) {
        perror("Decoder: Failed to open pipe");
        return;
    }

    ma_decoder_config decoder_config = ma_decoder_config_init(ma_format_s16, 2, 0); 
    ma_decoder decoder;

    ma_result result = ma_decoder_init_vfs((ma_vfs*)&vfs, url, &decoder_config, &decoder);

    if (result != MA_SUCCESS) {
        g_printerr("Decoder: Failed to open stream: %s (Result: %d)\n", url, result);
        if (on_error_callback) {
             on_error_callback("Unable to play stream. Ensure it is a supported format (MP3/FLAC/WAV).", error_user_data);
        }
        close(fd);
        return;
    }

    g_print("Decoder: Stream Init %d Hz, %d channels\n", decoder.outputSampleRate, decoder.outputChannels);

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
                 g_printerr("Decoder: Stream read error: %d\n", result);
            }
            break;
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
    g_print("Decoder: Stream Thread exiting.\n");
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
      last_position(0)
{
    signal(SIGPIPE, SIG_IGN);
    
    decoder->set_error_callback(internal_decoder_error_callback, this);

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

gint64 MusicBackend::get_duration() {
    if (total_duration > 0) return total_duration;

    if (pipeline) {
        GstFormat format = GST_FORMAT_TIME;
        gint64 duration;
        if (gst_element_query_duration(pipeline, &format, &duration)) {
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

    gchar *pipeline_desc = g_strdup_printf(
        "filesrc location=\"%s\" ! audio/x-raw-int, endianness=1234, signed=true, width=16, depth=16, rate=%d, channels=2 ! queue ! mixersink",
        PIPE_PATH, rate
    );
    pipeline = gst_parse_launch(pipeline_desc, NULL);
    g_free(pipeline_desc);

    if (!pipeline) {
        g_printerr("Backend: Failed to create pipeline\n");
        is_playing = false;
        return;
    }

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