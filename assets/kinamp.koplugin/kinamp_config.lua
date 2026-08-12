local lfs = require("libs/libkoreader-lfs")

-- KinAMP lives in /mnt/us/KinAMP on a jailbroken Kindle. Off-device (the
-- KOReader emulator, where the plugin UI is developed) that path does not
-- exist, so fall back to a directory under the KOReader data dir. KINAMP_DIR
-- overrides both.
local function find_bin_folder()
    local env_dir = os.getenv("KINAMP_DIR")
    if env_dir and env_dir ~= "" then
        return env_dir:gsub("/?$", "/")
    end
    if lfs.attributes("/mnt/us/KinAMP", "mode") == "directory" then
        return "/mnt/us/KinAMP/"
    end
    local DataStorage = require("datastorage")
    local dir = DataStorage:getDataDir() .. "/kinamp/"
    lfs.mkdir(dir) -- no-op if it already exists
    return dir
end

local bin_folder = find_bin_folder()

-- Where the player keeps its command FIFO and status file. Deliberately not the
-- install directory: on the device that is /mnt/us, which is vfat and cannot
-- hold a FIFO at all, so the control channel would silently never exist. Must
-- match get_runtime_path() in cli_player.cpp.
local function find_runtime_dir()
    local env_dir = os.getenv("KINAMP_RUNTIME_DIR")
    if env_dir and env_dir ~= "" then
        return (env_dir:gsub("/?$", "/"))
    end
    return "/tmp/"
end

local runtime_dir = find_runtime_dir()

-- KINAMP_MUSIC_FOLDER is shared with the GTK player, which uses it as the start
-- folder for its add file/folder dialogs.
local function find_music_dir()
    local env_dir = os.getenv("KINAMP_MUSIC_FOLDER")
    if env_dir and lfs.attributes(env_dir, "mode") == "directory" then
        return env_dir
    end
    if lfs.attributes("/mnt/us/music", "mode") == "directory" then
        return "/mnt/us/music"
    end
    return os.getenv("HOME") or "/"
end

return {
    -- Path definitions
    bin_folder = bin_folder,
    bin_path = bin_folder .. "startkinamp_koreader.sh",
    lib_path = bin_folder .. "libs_hf",
    conf_file = bin_folder .. ".kinamp.conf",
    radio_file = bin_folder .. ".kinamp_radio.txt",
    playlist_file = bin_folder .. ".kinamp_playlist.m3u",

    -- The bundled station database shipped alongside the binaries (the same
    -- file radio_cli searches). Several locations are tried at read time, see
    -- kinamp_stationdb.lua - this is only the first candidate.
    stations_db = bin_folder .. "allStations.json",

    -- Control channel, created and removed by KinAMP-minimal itself. The FIFO
    -- doubles as the liveness check: only a running player holds its read end.
    runtime_dir = runtime_dir,
    cmd_fifo = runtime_dir .. "kinamp_cmd",
    status_file = runtime_dir .. "kinamp_status",

    music_dir = find_music_dir(), -- Default start dir for browser

    -- Supported extensions (matched case-insensitively)
    extensions = { "mp3", "flac", "wav", "ogg", "m4a", "m4b", "mp4" },

    -- How long to wait for a freshly launched player to create its FIFO.
    -- startkinamp_koreader.sh stops any previous instance first, which can take
    -- a couple of seconds.
    startup_timeout = 6,

    -- Debugging
    debug_mode = true,
    log_file = bin_folder .. "kinamp.log",
}
