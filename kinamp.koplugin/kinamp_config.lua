local lfs = require("libs/libkoreader-lfs")
local Device = require("device")

-- Which device we are on. Everything platform-specific in this plugin is a
-- path or a launcher name, and both are decided here so nothing downstream has
-- to ask again.
--
-- KOReader itself is the same on both, which is the whole reason the plugin
-- ports: the player binary is the only native part, and it is reached through
-- the same command FIFO either way.
local function detect_platform()
    local forced = os.getenv("KINAMP_PLATFORM")
    if forced and forced ~= "" then return forced end
    if Device:isKindle() then return "kindle" end
    if Device:isKobo() then return "kobo" end
    return "other"
end

local platform = detect_platform()

-- Install directory. The Kobo build puts the player inside KOReader's own
-- folder, as kinamp/ next to plugins/ - so the first candidate is derived from
-- KOReader rather than hardcoded: DataStorage:getDataDir() is
-- /mnt/onboard/.adds/koreader on a Kobo, /mnt/us/koreader on a Kindle, and the
-- profile dir in the emulator. That one entry covers an install sitting beside
-- the plugin on any of the three, however the storage happens to be mounted.
--
-- The fixed paths after it are the older layouts, kept so an existing install
-- keeps working: /mnt/us/KinAMP is where a jailbroken Kindle has always put it,
-- and /mnt/onboard/.adds/kinamp is where the Kobo build used to unpack.
--
-- Every candidate is tried regardless of the detected platform: a device whose
-- storage is mounted somewhere unusual is better served by finding an install
-- that is actually there than by insisting on the path its platform normally
-- uses. KINAMP_DIR overrides the lot.
local INSTALL_DIRS = {
    kindle = { "/mnt/us/KinAMP" },
    kobo   = { "/mnt/onboard/.adds/kinamp", "/mnt/onboard/KinAMP" },
}

local function koreader_dir()
    local ok, DataStorage = pcall(require, "datastorage")
    if not ok then return nil end
    return DataStorage:getDataDir()
end

local function find_bin_folder()
    local env_dir = os.getenv("KINAMP_DIR")
    if env_dir and env_dir ~= "" then
        return env_dir:gsub("/?$", "/")
    end
    local data_dir = koreader_dir()
    if data_dir and lfs.attributes(data_dir .. "/kinamp", "mode") == "directory" then
        return data_dir .. "/kinamp/"
    end
    local order = { platform, "kindle", "kobo" }
    for _, name in ipairs(order) do
        for _, dir in ipairs(INSTALL_DIRS[name] or {}) do
            if lfs.attributes(dir, "mode") == "directory" then
                return dir .. "/"
            end
        end
    end
    -- Nothing installed anywhere we know of. Create the KOReader-relative dir
    -- so the rest of the plugin has somewhere to write its config and log,
    -- even though there is no binary in it to launch.
    local dir = (data_dir or ".") .. "/kinamp/"
    lfs.mkdir(dir)
    return dir
end

local bin_folder = find_bin_folder()

-- The launcher, which is what differs most between the two: the Kindle script
-- picks between the GStreamer 0.10 and 1.0 builds and the armel binary, while
-- the Kobo has exactly one flavour and needs the bundled libfaad on
-- LD_LIBRARY_PATH. Whichever script is actually installed wins, so a folder
-- holding only one of them works even if the platform was guessed wrong.
local function find_launcher()
    local candidates = platform == "kobo"
        and { "startkinamp_kobo.sh", "startkinamp_koreader.sh" }
        or { "startkinamp_koreader.sh", "startkinamp_kobo.sh" }
    for _, name in ipairs(candidates) do
        if lfs.attributes(bin_folder .. name, "mode") == "file" then
            return bin_folder .. name
        end
    end
    return bin_folder .. candidates[1]
end

-- Not the install dir: on both devices that's vfat and can't hold a FIFO.
-- Keep in sync with get_runtime_path() in cli_player.cpp.
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
local MUSIC_DIRS = { "/mnt/us/music", "/mnt/onboard/music", "/mnt/onboard/Music" }

local function find_music_dir()
    local env_dir = os.getenv("KINAMP_MUSIC_FOLDER")
    if env_dir and lfs.attributes(env_dir, "mode") == "directory" then
        return env_dir
    end
    for _, dir in ipairs(MUSIC_DIRS) do
        if lfs.attributes(dir, "mode") == "directory" then
            return dir
        end
    end
    return os.getenv("HOME") or "/"
end

return {
    -- Bump together with project(KinAMP VERSION ...) in CMakeLists.txt.
    version = "2.9",
    github_url = "https://www.github.com/kbarni/KinAMP",

    platform = platform,

    -- Paths
    bin_folder = bin_folder,
    bin_path = find_launcher(),
    conf_file = bin_folder .. ".kinamp.conf",
    radio_file = bin_folder .. ".kinamp_radio.txt",
    playlist_file = bin_folder .. ".kinamp_playlist.m3u",

    -- Station dump shipped with the binaries, the same one radio_cli reads.
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
    -- The launcher kills the previous instance first, which takes a second or
    -- two.
    startup_timeout = 6,

    debug_mode = true,
    log_file = bin_folder .. "kinamp.log",
}
