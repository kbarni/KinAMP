local lfs = require("libs/libkoreader-lfs")

-- KinAMP lives in /mnt/us/KinAMP on a jailbroken Kindle. That path doesn't
-- exist in the KOReader emulator, so fall back to a dir under the data dir.
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
    lfs.mkdir(dir)
    return dir
end

local bin_folder = find_bin_folder()

-- Not the install dir: on the device that's /mnt/us, which is vfat and can't
-- hold a FIFO. Keep in sync with get_runtime_path() in cli_player.cpp.
local function find_runtime_dir()
    local env_dir = os.getenv("KINAMP_RUNTIME_DIR")
    if env_dir and env_dir ~= "" then
        return (env_dir:gsub("/?$", "/"))
    end
    return "/tmp/"
end

local runtime_dir = find_runtime_dir()

-- KINAMP_MUSIC_FOLDER is shared with the GTK player, which uses it as the
-- start folder of its add file/folder dialogs.
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
    -- Bump together with project(KinAMP VERSION ...) in CMakeLists.txt.
    version = "2.9",
    github_url = "https://www.github.com/kbarni/KinAMP",

    -- Paths
    bin_folder = bin_folder,
    bin_path = bin_folder .. "startkinamp_koreader.sh",
    lib_path = bin_folder .. "libs_hf",
    conf_file = bin_folder .. ".kinamp.conf",
    radio_file = bin_folder .. ".kinamp_radio.txt",
    playlist_file = bin_folder .. ".kinamp_playlist.m3u",

    -- Station dump shipped with the binaries, the same one the player reads.
    -- First candidate only, see kinamp_stationdb.lua for the rest.
    stations_db = bin_folder .. "allStations.json",

    -- Control channel, created and removed by KinAMP-minimal. The FIFO doubles
    -- as the liveness check: only a running player holds its read end.
    runtime_dir = runtime_dir,
    cmd_fifo = runtime_dir .. "kinamp_cmd",
    status_file = runtime_dir .. "kinamp_status",

    music_dir = find_music_dir(), -- default start dir for the browser

    -- Matched case-insensitively.
    extensions = { "mp3", "flac", "wav", "ogg", "m4a", "m4b", "mp4" },

    -- How long to wait for a fresh player to create its FIFO.
    -- startkinamp_koreader.sh kills the previous instance first, which takes
    -- a second or two.
    startup_timeout = 6,

    debug_mode = true,
    log_file = bin_folder .. "kinamp.log",
}
