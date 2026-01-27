local Config = require("kinamp_config")
local Backend = {}

-- Helper: Escape strings for shell
function Backend.shell_escape(s)
    if not s then return "''" end
    return "'" .. string.gsub(s, "'", "'\\''") .. "'"
end

-- Helper: Log to file
function Backend.log(msg)
    if Config.debug_mode then
        local f = io.open(Config.log_file, "a")
        if f then
            f:write(os.date() .. ": " .. tostring(msg) .. "\n")
            f:close()
        end
    end
end

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
            table.insert(stations, {name=name, url=url})
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

-- Check if KinAMP is running
function Backend.is_running()
    -- pgrep returns 0 if process found, 1 if not
    return os.execute("pgrep -f KinAMP-minimal > /dev/null") == 0
end

-- Stop Playback
function Backend.stop()
    Backend.log("Stopping playback...")
    os.execute("pkill -f KinAMP-minimal")
end

-- Update .kinamp.conf with current_index
function Backend.update_config(index)
    local content = ""
    local f = io.open(Config.conf_file, "r")
    if f then
        content = f:read("*a")
        f:close()
    end

    -- Replace or append current_index
    if content:match("current_index=%d+") then
        content = content:gsub("current_index=%d+", "current_index=" .. tostring(index))
    else
        content = content .. "\ncurrent_index=" .. tostring(index) .. "\n"
    end

    f = io.open(Config.conf_file, "w")
    if f then
        f:write(content)
        f:close()
        Backend.log("Updated config: current_index=" .. tostring(index))
    else
        Backend.log("Error writing config: " .. Config.conf_file)
    end
end

-- Play Radio
function Backend.play_radio(url)
    Backend.stop()
    local cmd = string.format("nohup %s --radio %s > %s 2>&1 &", 
        Config.bin_path, 
        Backend.shell_escape(url),
        Config.log_file -- redirect output to log
    )
    Backend.log("Exec: " .. cmd)
    os.execute(cmd)
end

-- Play M3U Playlist (External)
function Backend.play_playlist_file(path)
    Backend.stop()
    local cmd = string.format("nohup %s --music %s > %s 2>&1 &", 
        Config.bin_path, 
        Backend.shell_escape(path),
        Config.log_file
    )
    Backend.log("Exec: " .. cmd)
    os.execute(cmd)
end

-- Helper to save internal playlist
function Backend.save_internal_playlist(items)
    Backend.log("Saving internal queue to " .. Config.playlist_file)
    local f = io.open(Config.playlist_file, "w")
    if not f then
        Backend.log("Error: Cannot write to playlist file")
        return false
    end
    for i, path in ipairs(items) do
        f:write(path .. "\n")
    end
    f:close()
    return true
end

-- Play Internal Queue (starts from beginning)
function Backend.play_internal_queue(items)
    if not Backend.save_internal_playlist(items) then return end
    
    Backend.update_config(0) -- Reset index to 0
    Backend.stop()
    
    -- Run with --music (uses .kinamp.conf and .kinamp_playlist.m3u)
    local cmd = string.format("nohup %s --music > %s 2>&1 &", 
        Config.bin_path, 
        Config.log_file
    )
    Backend.log("Exec: " .. cmd)
    os.execute(cmd)
end

-- Play from specific index in internal queue
function Backend.play_from_index(index, items)
    if not Backend.save_internal_playlist(items) then return end
    
    Backend.update_config(index - 1) -- 0-based index for backend? Assuming yes, Lua is 1-based.
    -- Wait, usually C apps use 0-based, Lua uses 1-based. 
    -- If .kinamp.conf expects 0-based: index-1. 
    -- I'll assume 0-based for the C app.
    
    Backend.stop()
    
    local cmd = string.format("nohup %s --music > %s 2>&1 &", 
        Config.bin_path, 
        Config.log_file
    )
    Backend.log("Exec: " .. cmd)
    os.execute(cmd)
end

-- Scan Folder for Music (Non-recursive)
function Backend.scan_folder(path)
    local ext_pattern = ""
    for i, ext in ipairs(Config.extensions) do
        if i > 1 then
            ext_pattern = ext_pattern .. " -o "
        end
        ext_pattern = ext_pattern .. "-iname '*." .. ext .. "'"
    end
    
    -- -maxdepth 1 for non-recursive
    local cmd = string.format("find %s -maxdepth 1 -type f \\( %s \\) | sort", 
        Backend.shell_escape(path), 
        ext_pattern
    )
    
    Backend.log("Scanning: " .. cmd)
    local f = io.popen(cmd)
    local files = {}
    if f then
        for line in f:lines() do
            table.insert(files, line)
        end
        f:close()
    else
        Backend.log("Error executing find")
    end
    return files
end

return Backend