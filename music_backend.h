#ifndef MUSIC_BACKEND_H
#define MUSIC_BACKEND_H

#include <gst/gst.h>
#include <string>
#include <vector>
#include <atomic>
#include <pthread.h>
#include <memory>
#include <mutex>
#include <sys/types.h>

// Callback type for End of Stream (song finished)
typedef void (*EosCallback)(void* user_data);

// Callback type for Errors
typedef void (*ErrorCallback)(const char* msg, void* user_data);

struct Chapter {
    uint64_t timestamp; // 100ns units
    std::string title;
};

enum class AudioFormat {
    UNKNOWN,
    M4B_AAC,    // M4A/M4B MP4 Container (FAAD + mp4read)
    MINIAUDIO,  // MP3, FLAC, WAV (miniaudio)
    AAC_ADTS    // Raw AAC stream (FAAD) - Future support
};

enum class InputType {
    FILE,
    STREAM
};

// --- Decoder Class ---
class Decoder {
public:
    Decoder();
    ~Decoder();

    // Start decoding the specified file in a separate thread.
    // Returns true if thread started successfully.
    bool start(const char* filepath, int start_time = 0);

    // Stop the decoding thread.
    // This sets the stop flag and waits for the thread to join.
    void stop();

    // Check if the decoder thread is currently running.
    bool is_running() const;

    void set_error_callback(ErrorCallback callback, void* user_data);
    
    // Internal use for stream killing
    void set_stream_pid(pid_t pid);

private:
    std::atomic<bool> stop_flag;
    std::atomic<bool> running;
    pthread_t thread_id;
    std::string current_filepath;
    int start_time;

    ErrorCallback on_error_callback;
    void* error_user_data;

    std::mutex pid_mutex;
    pid_t current_stream_pid;

    static void* thread_func(void* arg);
    void decode_loop();

    // Decoding Strategies
    void decode_mp4_file(const char* filepath, int start_time);
    void decode_miniaudio(const char* filepath, int start_time); // For files
    void decode_stream(const char* url); // For HTTP streams

    // Helpers
    AudioFormat detect_format(const char* resource, InputType type);
    InputType detect_input_type(const char* resource);
};

// --- MusicBackend Class ---
class MusicBackend {
public:
    // Public state for GUI
    bool is_playing;
    bool is_paused;

    MusicBackend();
    ~MusicBackend();

    // --- Public API ---
    void play_file(const char* filepath, int start_time = 0);
    void pause();
    void stop();
    
    // Returns true if the backend is currently performing a stop operation
    // (used to prevent UI race conditions)
    bool is_shutting_down() const;

    gint64 get_duration();
    gint64 get_position();
    const char* get_current_filepath();

    void set_eos_callback(EosCallback callback, void* user_data);
    void set_error_callback(ErrorCallback callback, void* user_data);

    void read_metadata(const char* filepath);
    
    std::string meta_title;
    std::string meta_artist;
    std::string meta_album;
    std::vector<unsigned char> cover_art;
    std::vector<Chapter> chapters;
    int current_samplerate;
    gint64 total_duration;

private:
    std::unique_ptr<Decoder> decoder;
    
    GstElement *pipeline;
    GstBus *bus;
    guint bus_watch_id;

    std::string current_filepath_str;
    std::atomic<bool> stopping; // Flag to indicate stop in progress

    EosCallback on_eos_callback;
    void* eos_user_data;

    ErrorCallback on_error_callback;
    void* error_user_data;
    
    gint64 last_position;

    // Helper to cleanup GStreamer resources
    void cleanup_pipeline();

    // GStreamer bus callback
    static gboolean bus_callback_func(GstBus *bus, GstMessage *msg, gpointer data);

    // Internal error callback to bridge Decoder -> MusicBackend -> UI
    static void internal_decoder_error_callback(const char* msg, void* user_data);
};

#endif // MUSIC_BACKEND_H
