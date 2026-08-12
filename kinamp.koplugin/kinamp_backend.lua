-- Client for the KinAMP-minimal player daemon.
--
-- The player is a separate process that owns the audio pipeline; this module
-- only talks to it, over two channels the player creates and removes itself:
--
--   .kinamp_cmd     FIFO we write newline-terminated commands to
--   .kinamp_status  key=value file the player rewrites on every state change
--                   and once a second while playing
--
-- Nothing here blocks the UI thread. The FIFO is opened non-blocking, so with
-- no player running the open fails immediately instead of hanging waiting for a
-- reader, and that failure doubles as the liveness check.

local ffi = require("ffi")
local bit = require("bit")
local lfs = require("libs/libkoreader-lfs")
local logger = require("logger")
local Config = require("kinamp_config")

require("ffi/posix_h")
local C = ffi.C

local Backend = {}

local known_ext = {}
for _, ext in ipairs(Config.extensions) do
    known_ext[ext:lower()] = true
end

function Backend.shell_escape(s)
    if not s then return "''" end
    return "'" .. string.gsub(tostring(s), "'", "'\\''") .. "'"
end

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

-- One read instead of a line iterator, wrapped, and a failure is just "no data
-- this time".
--
-- Every file read here is one the player rewrites underneath us - the status
-- file is replaced via rename() about once a second, which is also how often we
-- read it. On the Kindle these sit on /mnt/us, which is vfat and has no
-- unlink-while-open semantics, so an open handle can stop being readable
-- partway through. Lua's line iterator turns a read error into a *raised* error
-- (luaL_error(strerror(errno)) on ferror), and the status poll runs inside a
-- UIManager task, where nothing catches it and it takes KOReader down with it.
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

-- Non-empty lines, tolerating CRLF.
local function each_line(content)
    return (content or ""):gmatch("[^\r\n]+")
end

-- .kinamp.conf
--
-- The players' shared settings file, and all "carry on where it stopped" is
-- made of: which list was playing (is_radio_mode), where in it (current_index),
-- how (playback_strategy) and how loud (volume). Every player reads it at
-- startup, so it's how a setting reaches one that isn't running yet, and how it
-- survives the last one exiting.
--
-- What the others write back is uneven: the GTK player rewrites the file when
-- it quits, KinAMP-minimal only keeps current_index up to date and only in
-- music mode. So anything this plugin changes it has to store itself, or the
-- next cold start reads a stale value.

-- Order new keys are appended in, so a file we create from nothing looks like
-- one the GTK player wrote.
local CONF_KEYS = { "current_index", "playback_strategy", "is_radio_mode", "volume" }

-- Rewrites the file with `updates` (key -> value) applied. Line based rather
-- than one gsub over the whole text: values aren't all integers any more
-- (volume is a fraction), and a pattern loose enough to match those also
-- matches the tail of another key's name.
local function write_config(updates)
    local content = read_file(Config.conf_file) or ""
    local pending = {}
    for key, value in pairs(updates) do pending[key] = tostring(value) end

    local lines = {}
    for line in each_line(content) do
        local key = line:match("^([%w_]+)=")
        if key and pending[key] then
            lines[#lines + 1] = key .. "=" .. pending[key]
            pending[key] = nil -- a duplicate key later in the file just goes
        else
            lines[#lines + 1] = line
        end
    end
    for _, key in ipairs(CONF_KEYS) do
        if pending[key] then
            lines[#lines + 1] = key .. "=" .. pending[key]
            pending[key] = nil
        end
    end
    for key, value in pairs(pending) do
        lines[#lines + 1] = key .. "=" .. value
    end

    local f = io.open(Config.conf_file, "w")
    if not f then
        Backend.log("Error writing config: " .. Config.conf_file)
        return false
    end
    -- Every line newline terminated whether or not the file was: the C++
    -- players use getline() and would lose the last entry otherwise.
    f:write(table.concat(lines, "\n") .. "\n")
    f:close()
    Backend.log("Updated config: " .. table.concat(lines, "; "))
    return true
end

-- One key as a number, or nil when absent or not a number. Pass `content` to
-- read several keys out of one snapshot of the file.
local function read_config(key, content)
    content = content or read_file(Config.conf_file)
    if not content then return nil end
    for line in each_line(content) do
        local name, value = line:match("^([%w_]+)=(.*)$")
        if name == key then return tonumber(value) end
    end
    return nil
end

-- Volume is stored as the GTK player stores it, a 0..1 fraction. Formatted by
-- hand rather than with %.2f, which goes through sprintf and would write "0,75"
-- wherever the C locale uses a comma; the players parse in the classic locale
-- and would read that as zero.
local function volume_fraction(percent)
    percent = math.max(0, math.min(100, math.floor(percent + 0.5)))
    return string.format("%d.%02d", math.floor(percent / 100), percent % 100)
end

-- Everything the next cold start will resume from.
function Backend.get_saved_state()
    local content = read_file(Config.conf_file)
    local index = read_config("current_index", content)
    local volume = read_config("volume", content)

    return {
        -- The file counts from 0 like the players do, -1 meaning "nothing".
        index = (index and index >= 0) and (index + 1) or nil,
        is_radio = read_config("is_radio_mode", content) == 1,
        volume = volume and math.max(0, math.min(100, math.floor(volume * 100 + 0.5))) or nil,
        strategy = read_config("playback_strategy", content) or 0,
    }
end

-- Control channel

local O_WRONLY_NONBLOCK = bit.bor(C.O_WRONLY, C.O_NONBLOCK)

-- Returns a fd, or nil if no player is listening. A FIFO left behind by a
-- killed player fails here with ENXIO, so a stale file never looks alive.
local function open_fifo()
    local fd = C.open(Config.cmd_fifo, O_WRONLY_NONBLOCK)
    if fd < 0 then return nil end
    return fd
end

-- Sends one or more commands. Pass several to have them applied back to back
-- with no state visible in between, e.g. send("load /path/to.m3u", "index 3").
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
    -- player never sees half a command interleaved with another client's.
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

function Backend.is_running()
    local fd = open_fifo()
    if not fd then return false end
    C.close(fd)
    return true
end

-- The player's published state, or nil if nothing is running. The player writes
-- the file with rename(), so a read sees either the previous snapshot or the
-- new one, never a partial line.
function Backend.get_status()
    -- One retry: the window where the file is being replaced is a few
    -- microseconds wide, and a transient miss would otherwise flash the widget
    -- back to "Not playing" for a tick.
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
    -- install dir), which is not ours.
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

-- Launching

-- Starts the daemon. The launcher script kills any previous instance first, so
-- this is also how a stuck player gets recycled. It returns as soon as the
-- shell has forked; the FIFO appears a moment later, hence wait_until_running().
--
-- The Bluetooth keepalive is armed first and the daemon only started once that
-- is done. It has to be this way round: btfd reads ensureBTconnection when the
-- radio comes up and never again, so arming it means cycling the radio, and the
-- moment before a player exists is the one moment that cycle interrupts no
-- audio. startkinamp.sh does the same for the GTK player (BTenable 0:1, then
-- the player sets the flag and raises the radio); the KOReader launcher has no
-- equivalent, so it happens here.
--
-- Nothing waits on it succeeding. A device with no btfd, or one whose radio
-- won't come up, still gets its player; worst case is the twenty minute
-- disconnect that was there before any of this.
local pending_cmd

local function launch(args)
    local cmd = string.format("nohup %s %s > %s 2>&1 &",
        Backend.shell_escape(Config.bin_path),
        args or "",
        Backend.shell_escape(Config.log_file))

    -- Asking for something else while the radio is cycling replaces what's
    -- queued rather than starting a second player: there's no daemon yet to
    -- hand new arguments to over the FIFO, and two launches racing means the
    -- launcher killing the first player moments after it appeared. Last request
    -- wins, which is what the list that sent it has already said.
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

    -- Only when the radio really is being cycled, which is once per session:
    -- several seconds pass with nothing moving on screen, and pressing play
    -- into silence reads as a player that has failed. With nothing to arm the
    -- callback has already run above and this never shows.
    if not immediate then
        local InfoMessage = require("ui/widget/infomessage")
        local _ = require("gettext")
        waiting = InfoMessage:new{ text = _("Preparing Bluetooth…") }
        UIManager:show(waiting)
    end
end

-- Polls for the FIFO to appear without blocking the UI thread. The callback
-- gets true once the player answers, false on timeout.
function Backend.wait_until_running(callback)
    local UIManager = require("ui/uimanager")
    local deadline = os.time() + Config.startup_timeout

    local function poll()
        if Backend.is_running() then
            callback(true)
        elseif Backend.launch_pending then
            -- Not started yet: the keepalive is being armed first and that
            -- cycles the radio, which takes longer than a player takes to come
            -- up. Not this timeout's to spend, so only start counting once the
            -- process is actually on its way.
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

-- Transport. All no-ops when no player is running.

function Backend.play()            return Backend.send("play")    end
function Backend.pause()           return Backend.send("pause")   end
function Backend.next_track()      return Backend.send("next")    end
function Backend.previous_track()  return Backend.send("prev")    end

-- Stops playback but leaves the daemon resident, so the next play is instant.
function Backend.stop()            return Backend.send("stop")    end

-- Terminates the player process.
function Backend.quit()            return Backend.send("quit")    end

function Backend.seek(seconds)
    return Backend.send(string.format("seek %d", math.floor(seconds)))
end

-- percent is 0-100. Stored as well as sent, for the same reason as the strategy
-- below: neither player writes the volume back while it runs (the GTK one only
-- does so on the way out), so without this the setting wouldn't survive the
-- daemon quitting and wouldn't reach one that isn't up yet. Always returns true
-- - the value is kept whether or not anything was listening.
function Backend.set_volume(percent)
    percent = math.max(0, math.min(100, math.floor(percent)))
    -- Sent first: the file write is the slow half and not what the ear is
    -- waiting for.
    Backend.send(string.format("vol %d", percent))
    write_config({ volume = volume_fraction(percent) })
    return true
end

-- 0 in order, 1 repeat all, 2 shuffle. Stored as well as sent: the player takes
-- the command straight away but never writes the setting back.
function Backend.set_strategy(strategy)
    write_config({ playback_strategy = strategy })
    return Backend.send(string.format("strategy %d", strategy))
end

-- What the running player reports, or failing that what the next one will read
-- out of the config.
function Backend.get_strategy()
    local status = Backend.get_status()
    if status and status.strategy then return status.strategy end
    return read_config("playback_strategy") or 0
end

-- Playlists and stations

-- Returns { {name="Jazz", url="http://..."}, ... }
function Backend.get_stations()
    local stations = {}
    local content = read_file(Config.radio_file)
    if not content then
        Backend.log("Radio file not found: " .. Config.radio_file)
        return stations
    end
    for line in each_line(content) do
        local name, url = line:match("^(.*)|(.*)$")
        if name and url then
            table.insert(stations, { name = name, url = url })
        end
    end
    return stations
end

-- Replaces the station file. It's one "name|url" record per line with no
-- quoting at all (see load_radio_stations() in cli_player.cpp), so a name
-- carrying a separator or a newline would silently become a different station,
-- or two. Not worth rejecting an otherwise fine station over, so flatten them.
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

-- A bare list of paths rather than an #EXTM3U: that's what the player reads
-- back (load_playlist in cli_player.cpp skips # lines anyway) and what every
-- other m3u reader accepts.
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

function Backend.save_internal_playlist(items)
    Backend.log("Saving internal queue to " .. Config.playlist_file)
    return Backend.write_m3u(Config.playlist_file, items)
end

function Backend.is_playable(name)
    local ext = tostring(name):match("%.([^.]+)$")
    return ext ~= nil and known_ext[ext:lower()] == true
end

-- Records what is being played in .kinamp.conf so the next cold start resumes
-- here. `index` is 0-based, the way the players count it.
--
-- Written on every start, not only when no player is running: KinAMP-minimal
-- writes current_index back itself, but only for music, so a station picked over
-- the FIFO would leave the file pointing at whatever was playing before it.
function Backend.update_config(index, is_radio)
    return write_config({
        current_index = index,
        is_radio_mode = is_radio and 1 or 0,
    })
end

-- Starting playback.
--
-- Each of these drives a running player over the FIFO and only falls back to
-- launching a process when none is up. That's the point of the daemon: changing
-- track used to cost a kill, a 2 second wait and a GStreamer restart.
--
-- They return true when a running player took the command (playback has already
-- changed) and false when a new player had to be launched, in which case audio
-- starts once it comes up - see wait_until_running().

-- `index` is the station's 1-based place in the list, when it has one. Stored,
-- so a cold start comes back to this station rather than the first.
function Backend.play_radio(url, index)
    Backend.update_config((index or 1) - 1, true)
    if Backend.send("radio " .. url) then return true end
    launch("--radio " .. Backend.shell_escape(url))
    return false
end

-- Reads an m3u/m3u8 into a list of paths, mirroring the player's own parsing
-- (load_playlist in cli_player.cpp): #EXTM3U/#EXTINF directives aren't paths,
-- CRLF endings would leave a stray CR inside the file name, and relative entries
-- are relative to the playlist's own directory - resolved here, while we still
-- know where the playlist came from. Callers store the result as the internal
-- queue, which lives elsewhere and is no longer a valid base.
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

-- Plays the internal queue starting at `index` (1-based).
function Backend.play_from_index(index, items)
    if items and not Backend.save_internal_playlist(items) then return false end

    -- Recorded before either route: for a player that isn't running this is how
    -- the start position is handed over at all (a cold KinAMP-minimal reads it
    -- from there), and for one that is, it's what the next cold start after this
    -- session resumes from.
    Backend.update_config(index - 1, false)

    if Backend.send("load " .. Config.playlist_file, "index " .. tostring(index - 1)) then
        return true
    end

    launch("--music")
    return false
end

-- Starts a player on the saved track or station. Nothing is passed on the
-- command line: KinAMP-minimal launched bare reads mode, list, position,
-- strategy and volume out of .kinamp.conf itself. All this does first is make
-- sure the position it will read is one the list still has, since a shortened
-- queue (or one that never had a track selected) would leave it with nothing to
-- play. `entry` is what saved_entry() returns.
function Backend.resume(entry)
    Backend.update_config(entry.index - 1, entry.is_radio)
    launch("")
    return false
end

local function basename(path)
    return tostring(path):match("([^/]+)$") or tostring(path)
end

-- What a cold start would resume: the saved position resolved against the list
-- it belongs to. Returns { name, index (1-based, clamped), is_radio } plus the
-- saved state, or nil plus the saved state when that list is empty.
function Backend.saved_entry()
    local saved = Backend.get_saved_state()
    local items = saved.is_radio and Backend.get_stations()
                  or Backend.load_internal_playlist()
    if #items == 0 then return nil, saved end

    local index = math.max(1, math.min(saved.index or 1, #items))
    return {
        name = saved.is_radio and items[index].name or basename(items[index]),
        index = index,
        is_radio = saved.is_radio,
    }, saved
end

-- Playable files directly inside `path`, non-recursive. lfs rather than
-- shelling out to find(1): io.popen would block the whole UI for as long as the
-- scan takes, and find isn't guaranteed off-device.
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
