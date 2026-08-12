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

--- Reads a whole file, or nil if it could not be read.
--
-- Every file we read here is one the player rewrites underneath us: it replaces
-- the status file via rename() about once a second, which is exactly how often
-- the player widget reads it. On the Kindle these live on /mnt/us, which is
-- vfat and has no unlink-while-open semantics, so the handle we are holding can
-- stop being readable partway through.
--
-- That matters because Lua's line iterator turns a read error into a *raised*
-- error ("if (ferror(f)) luaL_error(strerror(errno))"), and the status poll runs
-- inside a UIManager task - an error there is not caught anywhere and takes the
-- whole of KOReader down. So: one read instead of an iteration, wrapped, and a
-- failure is just "no data this time".
local function read_file(path)
    local ok, content = pcall(function()
        local f = io.open(path, "r")
        if not f then return nil end
        local data = f:read("*a")
        f:close()
        return data
    end)
    if not ok then
        Backend.log("read failed for " .. tostring(path) .. ": " .. tostring(content))
        return nil
    end
    return content
end

--- Iterates the non-empty lines of a string, tolerating CRLF.
local function each_line(content)
    return (content or ""):gmatch("[^\r\n]+")
end

--=============================================================================
-- .kinamp.conf
--
-- The players' shared settings file. KinAMP-minimal reads it at startup and
-- writes only current_index back, so this is how a setting reaches a player
-- that is not running yet - and how one survives the player exiting.
--=============================================================================

--- Sets one numeric key, leaving the rest of the file alone.
local function write_config(key, value)
    local content = read_file(Config.conf_file) or ""
    local entry = key .. "=" .. tostring(value)

    if content:match(key .. "=%-?%d+") then
        content = content:gsub(key .. "=%-?%d+", entry)
    else
        -- Exactly one newline before the new entry, whether or not the file
        -- ended with one - the GTK player reads this file too.
        content = (content ~= "" and (content:gsub("\n+$", "") .. "\n") or "") .. entry .. "\n"
    end

    local f = io.open(Config.conf_file, "w")
    if not f then
        Backend.log("Error writing config: " .. Config.conf_file)
        return false
    end
    f:write(content)
    f:close()
    Backend.log("Updated config: " .. entry)
    return true
end

local function read_config(key)
    local content = read_file(Config.conf_file)
    if not content then return nil end
    return tonumber(content:match(key .. "=(%-?%d+)"))
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
    -- One retry: the window where the file is being replaced is a few
    -- microseconds wide, so a second attempt costs nothing and keeps a transient
    -- miss from flashing the widget back to "Not playing" for a tick.
    local content = read_file(Config.status_file)
    if not content or content == "" then
        content = read_file(Config.status_file)
    end
    if not content or content == "" then return nil end

    local status = {}
    for line in each_line(content) do
        local key, value = line:match("^([%w_]+)=(.*)$")
        if key then status[key] = value end
    end

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
            status.cover = Config.runtime_dir .. status.cover
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
--
-- The Bluetooth keepalive is armed first, and the daemon only started once that
-- is done. It has to be this way round: btfd reads ensureBTconnection when the
-- radio comes up and never again, so arming it means cycling the radio, and the
-- moment before a player exists is the one moment that cycle interrupts no
-- audio. This is what startkinamp.sh does for the GTK player (BTenable 0:1,
-- then the player sets the flag and raises the radio); the KOReader launcher
-- has no equivalent, so it happens here.
--
-- Nothing waits on it succeeding. A device with no btfd, or one whose radio will
-- not come up, still gets its player - the worst case is the twenty minute
-- disconnect that was there before any of this.
local pending_cmd

local function launch(args)
    local cmd = string.format("nohup %s %s > %s 2>&1 &",
        Backend.shell_escape(Config.bin_path),
        args or "",
        Backend.shell_escape(Config.log_file))

    -- Asking for something else while the radio is still cycling replaces what
    -- is queued rather than starting a second player. There is no daemon yet to
    -- hand the new arguments to over the FIFO, and two launches racing means the
    -- launcher script killing the first player moments after it appeared. Last
    -- request wins, which is also what the list that sent it has already said.
    pending_cmd = cmd
    if Backend.launch_pending then
        Backend.log("launch already waiting on Bluetooth, superseded by: " .. cmd)
        return
    end

    local UIManager = require("ui/uimanager")
    local BT = require("kinamp_bt")
    local waiting

    Backend.launch_pending = true
    local immediate = BT.armKeepalive(function(ok, err)
        Backend.launch_pending = false
        if waiting then UIManager:close(waiting) end
        if not ok and err ~= "unavailable" then
            Backend.log("BT: keepalive not armed (" .. tostring(err) .. "), starting anyway")
        end
        Backend.log("exec: " .. pending_cmd)
        os.execute(pending_cmd)
        pending_cmd = nil
    end)

    -- Only when the radio really is being cycled, which is the once-per-session
    -- case: several seconds pass with nothing on screen moving, and pressing
    -- play into silence reads as a player that has failed. When there is nothing
    -- to arm the callback has already run above and this never shows.
    if not immediate then
        local InfoMessage = require("ui/widget/infomessage")
        local _ = require("gettext")
        waiting = InfoMessage:new{ text = _("Preparing Bluetooth…") }
        UIManager:show(waiting)
    end
end

--- Polls for the player's FIFO to appear, without blocking the UI thread.
-- @param callback called with true once the player answers, false on timeout
function Backend.wait_until_running(callback)
    local UIManager = require("ui/uimanager")
    local deadline = os.time() + Config.startup_timeout

    local function poll()
        if Backend.is_running() then
            callback(true)
        elseif Backend.launch_pending then
            -- The daemon has not been started yet: the Bluetooth keepalive is
            -- being armed first and that cycles the radio, which takes longer
            -- than a player takes to come up. Not this timeout's to spend, so
            -- it only starts running once the process is actually on its way.
            deadline = os.time() + Config.startup_timeout
            UIManager:scheduleIn(0.25, poll)
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

--- @param strategy 0 in order, 1 repeat all, 2 shuffle
-- Stored as well as sent: the player takes the command straight away but never
-- writes the setting back, so without this the choice would be forgotten the
-- next time the daemon is started - and would not reach one that is not up.
function Backend.set_strategy(strategy)
    write_config("playback_strategy", strategy)
    return Backend.send(string.format("strategy %d", strategy))
end

--- The strategy in force: what the running player reports, or failing that
-- what the next one will read out of the config.
function Backend.get_strategy()
    local status = Backend.get_status()
    if status and status.strategy then return status.strategy end
    return read_config("playback_strategy") or 0
end

--=============================================================================
-- Playlists and stations
--=============================================================================

-- Read Radio Stations
-- Returns: { {name="Jazz", url="http://..."}, ... }
function Backend.get_stations()
    local stations = {}
    local content = read_file(Config.radio_file)
    if not content then
        Backend.log("Radio file not found: " .. Config.radio_file)
        return stations
    end
    for line in each_line(content) do
        -- Split by pipe '|'
        local name, url = line:match("^(.*)|(.*)$")
        if name and url then
            table.insert(stations, { name = name, url = url })
        end
    end
    return stations
end

--- Writes the station list back, replacing whatever was there.
-- The file is one "name|url" record per line and has no quoting of any kind
-- (see load_radio_stations() in cli_player.cpp), so a name carrying a
-- separator or a newline would silently become a different station - or two.
-- Neither is worth rejecting an otherwise fine station over, so they are
-- flattened instead.
function Backend.save_stations(stations)
    local f = io.open(Config.radio_file, "w")
    if not f then
        Backend.log("Error: cannot write station file " .. Config.radio_file)
        return false
    end
    for _, s in ipairs(stations) do
        local name = tostring(s.name or ""):gsub("[|\r\n]", " ")
        local url = tostring(s.url or ""):gsub("%s+", "")
        if name ~= "" and url ~= "" then
            f:write(name .. "|" .. url .. "\n")
        end
    end
    f:close()
    Backend.log(string.format("Saved %d stations to %s", #stations, Config.radio_file))
    return true
end

-- Load Internal Playlist
function Backend.load_internal_playlist()
    local items = {}
    local content = read_file(Config.playlist_file)
    if not content then
        Backend.log("Playlist file not found: " .. Config.playlist_file)
        return items
    end
    for line in each_line(content) do
        table.insert(items, line)
    end
    return items
end

--- Writes a list of paths as a playlist file.
-- Deliberately a bare list of paths rather than an #EXTM3U: that is what the
-- player reads back (load_playlist in cli_player.cpp skips # lines anyway) and
-- what every other m3u reader accepts.
function Backend.write_m3u(path, items)
    local f = io.open(path, "w")
    if not f then
        Backend.log("Error: cannot write playlist " .. tostring(path))
        return false
    end
    for _, entry in ipairs(items) do
        f:write(entry .. "\n")
    end
    f:close()
    Backend.log(string.format("Wrote %d entries to %s", #items, tostring(path)))
    return true
end

-- Helper to save internal playlist
function Backend.save_internal_playlist(items)
    Backend.log("Saving internal queue to " .. Config.playlist_file)
    return Backend.write_m3u(Config.playlist_file, items)
end

--- True if the name ends in one of the extensions the player can decode.
function Backend.is_playable(name)
    local ext = tostring(name):match("%.([^.]+)$")
    return ext ~= nil and known_ext[ext:lower()] == true
end

--- Sets current_index in .kinamp.conf.
-- Only needed to pick the starting track for a player that is not running yet;
-- once it is up, the player maintains this itself.
function Backend.update_config(index)
    return write_config("current_index", index)
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

--- Reads an m3u/m3u8 file into a list of playable paths.
-- Mirrors the player's own parsing (load_playlist in cli_player.cpp):
-- #EXTM3U/#EXTINF directives are not paths, CRLF endings would otherwise leave
-- a stray CR inside the file name, and relative entries are relative to the
-- playlist's own directory - so they are resolved here, while we still know
-- where the playlist came from. Callers store the result as the internal queue,
-- which lives elsewhere and would no longer be a valid base for them.
function Backend.read_m3u(path)
    local entries = {}
    local content = read_file(path)
    if not content then
        Backend.log("Cannot open playlist: " .. tostring(path))
        return entries
    end

    local base = path:match("^(.*/)") or ""

    for line in each_line(content) do
        if line:sub(1, 1) ~= "#" then
            local is_absolute = line:sub(1, 1) == "/"
            local is_url = line:find("://", 1, true) ~= nil
            if not is_absolute and not is_url and base ~= "" then
                line = base .. line
            end
            table.insert(entries, line)
        end
    end

    Backend.log(string.format("read_m3u: %d entries from %s", #entries, tostring(path)))
    return entries
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
        if Backend.is_playable(name) then
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
