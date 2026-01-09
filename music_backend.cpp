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

#include <fstream>
#include <vector>

#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio/miniaudio.h"

const char* PIPE_PATH = "/tmp/kinamp_audio_pipe";

Decoder::Decoder() : stop_flag(false), running(false), thread_id(0) {
    unlink(PIPE_PATH);
    if (mkfifo(PIPE_PATH, 0666) == -1) {
        perror("Decoder: Failed to create named pipe");
    }
}

Decoder::~Decoder() {
    stop();
    unlink(PIPE_PATH);
}

bool Decoder::start(const char* filepath) {
    if (running) {
        stop();
    }

    current_filepath = filepath;
    stop_flag = false;
    running = true;

    if (pthread_create(&thread_id, NULL, thread_func, this) != 0) {
        perror("Decoder: Failed to create thread");
        running = false;
        return false;
    }
    return true;
}

void Decoder::stop() {
    if (!running) return;

    stop_flag = true;

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

    ma_decoder_config decoder_config = ma_decoder_config_init(ma_format_s16, 2, 44100);
    ma_decoder decoder;
    ma_result result = ma_decoder_init_file(current_filepath.c_str(), &decoder_config, &decoder);
    if (result != MA_SUCCESS) {
        g_printerr("Decoder: Failed to open file with miniaudio: %s\n", current_filepath.c_str());
        return;
    }

    int fd = open(PIPE_PATH, O_WRONLY);
    if (fd == -1) {
        perror("Decoder: Failed to open pipe");
        ma_decoder_uninit(&decoder);
        return;
    }

    const size_t BUFFER_SIZE = 4096;
    std::vector<int16_t> pcm_buffer(BUFFER_SIZE);

    while (!stop_flag) {
        ma_uint64 frames_read = 0;
        result = ma_decoder_read_pcm_frames(&decoder, pcm_buffer.data(), BUFFER_SIZE / decoder.outputChannels, &frames_read);
        
        if (result != MA_SUCCESS || frames_read == 0) {
            break;
        }

        ssize_t to_write = frames_read * ma_get_bytes_per_frame(decoder.outputFormat, decoder.outputChannels);
        ssize_t written = write(fd, pcm_buffer.data(), to_write);

        if (written == -1) {
            if (errno == EPIPE) {
                break;
            }
            perror("Decoder: write error");
            break;
        }
    }

    close(fd);
    ma_decoder_uninit(&decoder);
    g_print("Decoder: Thread exiting.\n");
}


MusicBackend::MusicBackend() 
    : is_playing(false), is_paused(false), pipeline(NULL), bus(NULL), bus_watch_id(0),
      stopping(false), on_eos_callback(NULL), eos_user_data(NULL), last_position(0)
{
    signal(SIGPIPE, SIG_IGN);
    
    gst_init(NULL, NULL);
    decoder = std::unique_ptr<Decoder>(new Decoder());
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

gint64 MusicBackend::get_duration() {
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
                return (gint64)(current_time - base_time);
            }
        }
    }
    return 0;
}

void MusicBackend::play_file(const char* filepath) {
    if (stopping) return; 

    if (is_playing || is_paused) {
        stop();
    }

    g_print("Backend: Playing %s\n", filepath);
    current_filepath_str = filepath;
    is_playing = true;
    is_paused = false;
    last_position = 0;

    gchar *pipeline_desc = g_strdup_printf(
        "filesrc location=\"%s\" ! audio/x-raw-int, endianness=1234, signed=true, width=16, depth=16, rate=44100, channels=2 ! queue ! mixersink",
        PIPE_PATH
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

    if (!decoder->start(filepath)) {
        cleanup_pipeline();
        return;
    }

    gst_element_set_state(pipeline, GST_STATE_PLAYING);
}

void MusicBackend::pause() {
    if (!pipeline || !is_playing) return;

    if (is_paused) {
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
