bin_folder = "/mnt/us/KinAMP/"

return {
    -- Path definitions
    bin_folder,
    bin_path = bin_folder .. "startkinamp_koreader.sh",
    lib_path = bin_folder .. "libs_hf",
    conf_file = bin_folder .. ".kinamp.conf",
    radio_file = bin_folder .. ".kinamp_radio.txt",
    playlist_file = bin_folder .. ".kinamp_playlist.m3u",
    music_dir = "/mnt/us/music", -- Default start dir for browser
    
    -- Supported extensions (matched case-insensitively via find -iname)
    extensions = { "mp3", "flac", "wav", "ogg", "m4a", "m4b", "mp4" },

    -- Debugging
    debug_mode = true,
    log_file = "/mnt/us/kinamp.log"
}
