--[[--
Thin client for the KinAMP-minimal player daemon.

The player is a separate process that owns the audio pipeline; this module only
talks to it. There are two channels, both created and removed by the player:

  * `.kinamp_cmd`    - a FIFO we write newline-terminated commands to.
  * `.kinamp_status` - a key=value file the player rewrites on every state
                       change and once a second while playing.

Nothing here blocks the UI thread. The FIFO is opened non-blocking, so if no
player is running the open fails immediately instead of hanging forever waiting
for a reader, and that failure doubles as the liveness check.
--]]

local ffi = require("ffi")
local bit = require("bit")
local lfs = require("libs/libkoreader-lfs")
local logger = require("logger")
local Config = require("kinamp_config")

require("ffi/posix_h")
local C = ffi.C

local Backend = {}

-- Extension lookup built once from the configured list.
local known_ext = {}
for _, ext in ipairs(Config.extensions) do
    known_ext[ext:lower()] = true
end

-- Helper: Escape strings for shell
function Backend.shell_escape(s)
    if not s then return "''" end
    return "'" .. string.gsub(tostring(s), "'", "'\\''") .. "'"
end

-- Helper: Log to file
function Backend.log(msg)
    logger.dbg("KinAMP:", msg)
    if Config.debug_mode then
        local f = io.open(Config.log_file, "a")
        if f then
            f:write(os.date() .. ": " .. tostring(msg) .. "\n")
            f:close()
        end
    end
end

--=============================================================================
-- Control channel
--=============================================================================

local O_WRONLY_NONBLOCK = bit.bor(C.O_WRONLY, C.O_NONBLOCK)

--- Opens the command FIFO without blocking.
-- Returns a file descriptor, or nil if no player is listening. A FIFO left
-- behind by a killed player fails here with ENXIO, so a stale file never makes
-- us think the player is alive.
local function open_fifo()
    local fd = C.open(Config.cmd_fifo, O_WRONLY_NONBLOCK)
    if fd < 0 then return nil end
    return fd
end

--- Sends one or more commands to the player.
-- Pass several commands to have them applied back to back with no state visible
-- in between, e.g. send("load /path/to.m3u", "index 3").
-- @return true if the whole payload was written
function Backend.send(...)
    local commands = { ... }
    if #commands == 0 then return false end

    local payload = table.concat(commands, "\n") .. "\n"

    local fd = open_fifo()
    if not fd then
        Backend.log("send failed, no player listening: " .. table.concat(commands, "; "))
        return false
    end

    -- Commands are far below PIPE_BUF, so a successful write is atomic and the
    -- player never sees a half command interleaved with another client's.
    local written = tonumber(C.write(fd, payload, #payload))
    C.close(fd)

    if written ~= #payload then
        Backend.log(string.format("send wrote %s of %d bytes: %s",
            tostring(written), #payload, table.concat(commands, "; ")))
        return false
    end

    Backend.log("sent: " .. table.concat(commands, "; "))
    return true
end

--- True if a player process is listening on the command FIFO.
function Backend.is_running()
    local fd = open_fifo()
    if not fd then return false end
    C.close(fd)
    return true
end

--- Reads the player's published state.
-- The player writes the file with rename(), so a read either sees the previous
-- snapshot or the new one, never a partial line.
-- @return table of fields, or nil if no player is running
function Backend.get_status()
    local f = io.open(Config.status_file, "r")
    if not f then return nil end

    local status = {}
    for line in f:lines() do
        local key, value = line:match("^([%w_]+)=(.*)$")
        if key then status[key] = value end
    end
    f:close()

    for _, key in ipairs({ "pid", "index", "count", "pos", "dur", "vol", "strategy", "daemon" }) do
        status[key] = tonumber(status[key])
    end

    -- The file outlives a SIGKILLed player, so confirm against the FIFO.
    if not Backend.is_running() then return nil end

    -- The player names its files relative to its own working directory (the
    -- install dir), which is not ours. Resolve them so callers get a path they
    -- can actually open.
    if status.cover and status.cover ~= "" then
        if not status.cover:match("^/") then
            status.cover = Config.bin_folder .. status.cover
        end
    else
        status.cover = nil
    end

    status.is_playing = status.state == "playing"
    status.is_paused = status.state == "paused"
    status.is_stopped = status.state == "stopped" or status.state == nil
    status.is_radio = status.mode == "radio"
    -- Lua indexes from 1, the player from 0; -1 means nothing is loaded.
    if status.index and status.index >= 0 then
        status.index = status.index + 1
    else
        status.index = nil
    end

    return status
end

--=============================================================================
-- Launching
--=============================================================================

--- Starts the player daemon with the given arguments.
-- The launcher script stops any previous instance first, so this is also how a
-- stuck player gets recycled. It returns as soon as the shell has forked; the
-- FIFO appears a moment later, hence Backend.wait_until_running().
local function launch(args)
    local cmd = string.format("nohup %s %s > %s 2>&1 &",
        Backend.shell_escape(Config.bin_path),
        args or "",
        Backend.shell_escape(Config.log_file))
    Backend.log("exec: " .. cmd)
    os.execute(cmd)
end

--- Polls for the player's FIFO to appear, without blocking the UI thread.
-- @param callback called with true once the player answers, false on timeout
function Backend.wait_until_running(callback)
    local UIManager = require("ui/uimanager")
    local deadline = os.time() + Config.startup_timeout

    local function poll()
        if Backend.is_running() then
            callback(true)
        elseif os.time() >= deadline then
            Backend.log("player did not start within " .. Config.startup_timeout .. "s")
            callback(false)
        else
            UIManager:scheduleIn(0.25, poll)
        end
    end

    UIManager:scheduleIn(0.25, poll)
end

--=============================================================================
-- Transport (no-ops when no player is running)
--=============================================================================

function Backend.play()            return Backend.send("play")    end
function Backend.pause()           return Backend.send("pause")   end
function Backend.next_track()      return Backend.send("next")    end
function Backend.previous_track()  return Backend.send("prev")    end

--- Stops playback but leaves the daemon resident, so the next play is instant.
function Backend.stop()            return Backend.send("stop")    end

--- Terminates the player process entirely.
function Backend.quit()            return Backend.send("quit")    end

function Backend.seek(seconds)
    return Backend.send(string.format("seek %d", math.floor(seconds)))
end

--- @param percent 0-100
function Backend.set_volume(percent)
    percent = math.max(0, math.min(100, math.floor(percent)))
    return Backend.send(string.format("vol %d", percent))
end

--- @param strategy 0 normal, 1 repeat, 2 shuffle
function Backend.set_strategy(strategy)
    return Backend.send(string.format("strategy %d", strategy))
end

--=============================================================================
-- Playlists and stations
--=============================================================================

-- Read Radio Stations
-- Returns: { {name="Jazz", url="http://..."}, ... }
function Backend.get_stations()
    local stations = {}
    local f = io.open(Config.radio_file, "r")
    if not f then
        Backend.log("Radio file not found: " .. Config.radio_file)
        return stations
    end
    for line in f:lines() do
        -- Split by pipe '|'
        local name, url = line:match("^(.*)|(.*)$")
        if name and url then
            table.insert(stations, { name = name, url = url })
        end
    end
    f:close()
    return stations
end

-- Load Internal Playlist
function Backend.load_internal_playlist()
    local items = {}
    local f = io.open(Config.playlist_file, "r")
    if not f then
        Backend.log("Playlist file not found: " .. Config.playlist_file)
        return items
    end
    for line in f:lines() do
        if line and line ~= "" then
            table.insert(items, line)
        end
    end
    f:close()
    return items
end

-- Helper to save internal playlist
function Backend.save_internal_playlist(items)
    Backend.log("Saving internal queue to " .. Config.playlist_file)
    local f = io.open(Config.playlist_file, "w")
    if not f then
        Backend.log("Error: Cannot write to playlist file")
        return false
    end
    for _, path in ipairs(items) do
        f:write(path .. "\n")
    end
    f:close()
    return true
end

--- Sets current_index in .kinamp.conf.
-- Only needed to pick the starting track for a player that is not running yet;
-- once it is up, the player maintains this itself.
function Backend.update_config(index)
    local content = ""
    local f = io.open(Config.conf_file, "r")
    if f then
        content = f:read("*a")
        f:close()
    end

    if content:match("current_index=%-?%d+") then
        content = content:gsub("current_index=%-?%d+", "current_index=" .. tostring(index))
    else
        content = content .. "\ncurrent_index=" .. tostring(index) .. "\n"
    end

    f = io.open(Config.conf_file, "w")
    if f then
        f:write(content)
        f:close()
        Backend.log("Updated config: current_index=" .. tostring(index))
        return true
    end
    Backend.log("Error writing config: " .. Config.conf_file)
    return false
end

--=============================================================================
-- Starting playback
--
-- Each of these drives a running player over the FIFO, and only falls back to
-- launching a process when none is up. That is the whole point of the daemon:
-- changing track used to cost a kill, a 2 second wait and a GStreamer restart.
--
-- They all return true when a running player took the command (playback has
-- already changed), and false when a new player had to be launched instead, in
-- which case audio starts once it comes up - see Backend.wait_until_running().
--=============================================================================

-- Play Radio
function Backend.play_radio(url)
    if Backend.send("radio " .. url) then return true end
    launch("--radio " .. Backend.shell_escape(url))
    return false
end

-- Play M3U Playlist (External)
function Backend.play_playlist_file(path)
    if Backend.send("load " .. path, "index 0") then return true end
    launch("--music " .. Backend.shell_escape(path))
    return false
end

-- Play Internal Queue (starts from beginning)
function Backend.play_internal_queue(items)
    return Backend.play_from_index(1, items)
end

--- Plays the internal queue starting at `index` (1-based).
function Backend.play_from_index(index, items)
    if items and not Backend.save_internal_playlist(items) then return false end

    if Backend.send("load " .. Config.playlist_file, "index " .. tostring(index - 1)) then
        return true
    end

    -- No player yet: hand the start position over through the config file,
    -- which is where a cold KinAMP-minimal reads it from.
    Backend.update_config(index - 1)
    launch("--music")
    return false
end

--=============================================================================
-- Library
--=============================================================================

--- Lists playable files directly inside `path` (non-recursive).
-- Uses lfs rather than shelling out to find(1): io.popen would block the whole
-- UI for as long as the scan takes, and find is not guaranteed off-device.
function Backend.scan_folder(path)
    local files = {}
    path = path:gsub("/+$", "")

    local ok, iter, dir_obj = pcall(lfs.dir, path)
    if not ok then
        Backend.log("Cannot scan folder: " .. tostring(path))
        return files
    end

    for name in iter, dir_obj do
        local ext = name:match("%.([^.]+)$")
        if ext and known_ext[ext:lower()] then
            local full_path = path .. "/" .. name
            if lfs.attributes(full_path, "mode") == "file" then
                table.insert(files, full_path)
            end
        end
    end

    table.sort(files)
    return files
end

return Backend
