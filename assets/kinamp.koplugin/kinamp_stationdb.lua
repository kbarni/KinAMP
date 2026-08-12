--[[--
Station discovery for KinAMP.

Two jobs, both of them ports of what `radio_cli` does on the device:

  * searching `allStations.json`, the 3.4 MB radio-browser dump shipped next to
    the binaries, for stations whose name matches what the user typed;
  * turning the link the database gives us into something the player can
    actually stream, because a good third of those links are `.pls`/`.m3u`
    playlists rather than streams and the player has no playlist reader of its
    own (see `decode_stream()` in music_backend.cpp - it expects audio).

Nothing here draws anything; kinamp_stations.lua owns the UI.
--]]

local http = require("socket.http")
local https = require("ssl.https")
local lfs = require("libs/libkoreader-lfs")
local socketutil = require("socketutil")
local url_parser = require("socket.url")
local util = require("util")
local logger = require("logger")
local Config = require("kinamp_config")

local StationDB = {}

--=============================================================================
-- The bundled database
--=============================================================================

-- The file is a single 3.4 MB line - [["name","url"],["name","url"],...] - with
-- no newline anywhere in it, so it cannot be read a record at a time. Reading
-- all of it into one Lua string would work but wastes several megabytes on a
-- device that has a few hundred; instead it is read in chunks that are split on
-- record boundaries, so each piece parses on its own.
local CHUNK_SIZE = 192 * 1024
local RECORD_SEP = '"],["'

-- Enough to fill a good few pages of results without turning the picker into a
-- list nobody scrolls to the end of.
StationDB.MAX_RESULTS = 400

local PLUGIN_DIR = debug.getinfo(1, "S").source:match("@?(.*/)") or "./"

--- Locates allStations.json.
-- On the device it sits in the install directory next to the binaries. The
-- other candidates are for running the plugin off-device, where it is checked
-- out inside the source tree and the database is its parent directory's.
function StationDB.db_path()
    local candidates = {
        Config.stations_db,
        Config.bin_folder .. "assets/allStations.json",
        PLUGIN_DIR .. "allStations.json",
        PLUGIN_DIR .. "../allStations.json",
    }
    for _, path in ipairs(candidates) do
        if lfs.attributes(path, "mode") == "file" then return path end
    end
    return nil
end

local JSON_ESCAPES = {
    ['"'] = '"', ["\\"] = "\\", ["/"] = "/",
    b = "\b", f = "\f", n = "\n", r = "\r", t = "\t",
}

--- Expands JSON escapes in a string that has already been cut out of the file.
-- Only about one name in twenty carries any, hence the early bail-out.
local function unescape(s)
    if not s:find("\\", 1, true) then return s end
    local out, i = {}, 1
    while true do
        local b = s:find("\\", i, true)
        if not b then
            out[#out + 1] = s:sub(i)
            break
        end
        out[#out + 1] = s:sub(i, b - 1)
        local c = s:sub(b + 1, b + 1)
        if c == "u" then
            local cp = tonumber(s:sub(b + 2, b + 5), 16)
            out[#out + 1] = cp and util.unicodeCodepointToUtf8(cp) or ""
            i = b + 6
        else
            out[#out + 1] = JSON_ESCAPES[c] or c
            i = b + 2
        end
    end
    return table.concat(out)
end

--- Reads the JSON string whose opening quote is at `i`.
-- @return its raw (still escaped) contents and the index just past the closing
--         quote, or nil if the string does not end within `s`
local function scan_string(s, i)
    if s:sub(i, i) ~= '"' then return nil end
    local j = i + 1
    while true do
        local k = s:find('[\\"]', j)
        if not k then return nil end
        if s:sub(k, k) == '"' then
            return s:sub(i + 1, k - 1), k + 1
        end
        j = k + 2 -- an escaped quote cannot end the string
    end
end

--- Index of the last occurrence of a plain substring.
local function rfind(s, needle)
    local last, init = nil, 1
    while true do
        local i = s:find(needle, init, true)
        if not i then return last end
        last, init = i, i + 1
    end
end

--- Matches one piece of the database, appending hits to `results`.
--
-- Records are never parsed up front: 45 000 of them would mean 90 000 throwaway
-- strings per search. Instead the piece is lowercased once and searched for the
-- probe term, and only around an actual hit is the enclosing record picked
-- apart. Record starts are walked forward alongside the hits, so a search that
-- matches nothing costs one lowercase and one failed find.
local function search_piece(text, probe, terms, phrase, results, seen, max_results)
    local lower = text:lower()
    local pos = 1
    local record_start, next_start = nil, text:find('["', 1, true)

    while #results < max_results do
        local hit = lower:find(probe, pos, true)
        if not hit then break end
        pos = hit + 1

        while next_start and next_start <= hit do
            record_start = next_start
            next_start = text:find('["', next_start + 2, true)
        end
        if record_start then
            local raw_name, after_name = scan_string(text, record_start + 1)
            -- A hit past the name is one inside the URL; the database is full
            -- of hostnames that read like a station name and matching them
            -- would drown the results the user meant.
            if raw_name and hit < after_name then
                local url_quote = text:find('"', after_name, true)
                local raw_url = url_quote and scan_string(text, url_quote)
                if raw_url and raw_url ~= "" then
                    local name = util.trim(unescape(raw_name))
                    local name_lower = name:lower()
                    local matched = true
                    for _, term in ipairs(terms) do
                        if not name_lower:find(term, 1, true) then
                            matched = false
                            break
                        end
                    end
                    if matched and name ~= "" then
                        local url = unescape(raw_url)
                        -- The dump lists the same station once per server it
                        -- was ever seen on, so duplicates are the rule.
                        local key = name_lower .. "|" .. url
                        if not seen[key] then
                            seen[key] = true
                            results[#results + 1] = {
                                name = name,
                                url = url,
                                phrase = name_lower:find(phrase, 1, true) ~= nil,
                            }
                        end
                    end
                end
            end
        end
    end
end

--- Searches the bundled database by station name.
-- Space-separated words all have to appear, in any order, case-insensitively.
-- @return list of {name=, url=} and whether the result was cut short at
--         MAX_RESULTS, or nil plus an error key
function StationDB.search(query, max_results)
    local path = StationDB.db_path()
    if not path then return nil, "nodb" end

    local phrase = util.trim(tostring(query or "")):lower()
    local terms = {}
    for word in phrase:gmatch("%S+") do
        terms[#terms + 1] = word
    end
    if #terms == 0 then return {}, false end

    -- Probe with the longest word: the rarer it is in the file, the fewer
    -- records have to be parsed to reject it.
    local probe = terms[1]
    for _, term in ipairs(terms) do
        if #term > #probe then probe = term end
    end

    local f = io.open(path, "rb")
    if not f then return nil, "nodb" end

    max_results = max_results or StationDB.MAX_RESULTS
    local results, seen, buf = {}, {}, ""

    local ok, err = pcall(function()
        while #results < max_results do
            local data = f:read(CHUNK_SIZE)
            if not data then break end
            buf = #buf > 0 and (buf .. data) or data
            -- Split on a record boundary so the piece we hand over contains
            -- only whole records and the leftover starts on the next one.
            local cut = rfind(buf, RECORD_SEP)
            if cut then
                search_piece(buf:sub(1, cut + 1), probe, terms, phrase, results, seen, max_results)
                buf = buf:sub(cut + 3)
            end
        end
        if #results < max_results and #buf > 0 then
            search_piece(buf, probe, terms, phrase, results, seen, max_results)
        end
    end)
    f:close()

    if not ok then
        logger.warn("KinAMP: station search failed:", err)
        return nil, "read"
    end

    -- Names that contain the whole query go first. Matching the words
    -- separately is what makes "radio jazz" find "Jazz Radio" at all, but on
    -- its own it also buries "Triple R" under every station with a "triple"
    -- and an "r" somewhere in it.
    if #terms > 1 then
        local exact, rest = {}, {}
        for _, station in ipairs(results) do
            local bucket = station.phrase and exact or rest
            bucket[#bucket + 1] = station
        end
        for _, station in ipairs(rest) do
            exact[#exact + 1] = station
        end
        results = exact
    end

    return results, #results >= max_results
end

--=============================================================================
-- Making a link playable
--=============================================================================

-- Station URLs routinely hide their extension behind a query string
-- (listen.pls?sid=25), so only the path is ever tested.
local function url_path(link)
    return (link:gsub("[?#].*$", ""))
end

local function has_ext(link, ext)
    return url_path(link):lower():sub(-#ext) == ext
end

local AUDIO_EXT = { ".mp3", ".aac", ".ogg", ".opus", ".flac", ".m3u8" }

--- What we can tell about a link before touching the network.
-- "playlist" - named .pls/.m3u, has to be fetched to get at the stream
-- "audio"    - a stream URL as it stands, nothing to resolve
-- "unknown"  - no extension to go on (listen.php?port=8000 and friends); worth
--              a small probe, but not worth failing over
function StationDB.link_kind(link)
    link = tostring(link or "")
    if has_ext(link, ".pls") or has_ext(link, ".m3u") then return "playlist" end
    for _, ext in ipairs(AUDIO_EXT) do
        if has_ext(link, ext) then return "audio" end
    end
    return "unknown"
end

--- True for streams the player cannot decode yet.
-- AAC/ADTS is handled by FAAD2, but HLS is a segmented transport rather than a
-- plain stream and there is nothing in music_backend.cpp that speaks it.
function StationDB.is_unsupported(link)
    return has_ext(tostring(link or ""), ".m3u8")
end

--- Sink that stops the transfer once `max_bytes` have arrived.
-- Playlists are small, but an entry that turns out to be a stream would
-- otherwise download until the battery ran out.
local function capped_sink(chunks, max_bytes)
    local received = 0
    return function(chunk)
        if not chunk then return 1 end
        chunks[#chunks + 1] = chunk
        received = received + #chunk
        if received >= max_bytes then return nil, "maxbytes" end
        return 1
    end
end

--- Fetches at most `max_bytes` of `link`.
-- Redirects are followed by hand because they cross schemes all the time here
-- (plain http station links redirecting to https), and LuaSocket only ever
-- follows a redirect with the module the request started in.
-- @return body, or nil plus an error string
local function http_fetch(link, max_bytes)
    for _ = 1, 4 do
        local parsed = url_parser.parse(link)
        local scheme = parsed and parsed.scheme
        local requester = (scheme == "https" and https) or (scheme == "http" and http)
        if not requester then return nil, "unsupported URL" end

        local chunks = {}
        socketutil:set_timeout(5, 10)
        local ok, code, headers = requester.request{
            url = link,
            method = "GET",
            headers = {
                ["Accept-Encoding"] = "identity",
                ["User-Agent"] = socketutil.USER_AGENT,
            },
            sink = capped_sink(chunks, max_bytes),
            redirect = false,
        }
        socketutil:reset_timeout()

        if not ok then
            -- Our own cap firing means the body arrived and we simply stopped
            -- reading it; anything else is a real failure. A response short
            -- enough to be a redirect never reaches the cap, so there is no
            -- redirect hiding behind this.
            if code == "maxbytes" then return table.concat(chunks) end
            return nil, tostring(code)
        end
        if code == 200 then return table.concat(chunks) end
        if code and code >= 300 and code < 400 and headers and headers.location then
            link = url_parser.absolute(link, headers.location)
        else
            return nil, "HTTP " .. tostring(code)
        end
    end
    return nil, "too many redirects"
end

--- Decides from the body rather than the name.
-- A .m3u can serve a [playlist], and plenty of extensionless endpoints are
-- playlists too - which is the whole reason "unknown" links get probed.
local function looks_like_playlist(body)
    local head = util.trim(body:sub(1, 512))
    if head == "" then return false end
    local start = head:sub(1, 32):lower()
    return start:find("[playlist]", 1, true) ~= nil
        or start:find("#extm3u", 1, true) ~= nil
        -- A bare list of stream URLs is a valid, extension-less M3U.
        or head:sub(1, 7) == "http://"
        or head:sub(1, 8) == "https://"
end

--- Pulls the stream URLs out of a .pls or .m3u body.
-- Entries may be relative to the playlist's own location, so they are resolved
-- here while we still know where the playlist came from.
local function parse_playlist(body, base_url)
    local is_pls = util.trim(body):sub(1, 32):lower():find("[playlist]", 1, true) ~= nil
    local urls, seen = {}, {}
    local first = true

    for line in body:gmatch("[^\r\n]+") do
        line = util.trim(line)
        if first then
            first = false
            line = util.trim((line:gsub("^\239\187\191", ""))) -- UTF-8 BOM
        end
        local entry
        if is_pls then
            -- FileN=url, skipping TitleN=/LengthN=/NumberOfEntries=
            if line:lower():sub(1, 4) == "file" then
                entry = util.trim(line:match("=(.*)$") or "")
            end
        elseif line ~= "" and line:sub(1, 1) ~= "#" then
            entry = line
        end
        if entry and entry ~= "" then
            if not entry:find("://", 1, true) then
                entry = url_parser.absolute(base_url, entry)
            end
            if not seen[entry] then
                seen[entry] = true
                urls[#urls + 1] = entry
            end
        end
    end
    return urls
end

--- Unwraps one level of playlist.
-- @return list of stream URLs when `link` turned out to be a playlist;
--         nil when it is already a stream (nothing to do);
--         nil plus an error string when a playlist could not be read.
function StationDB.resolve(link)
    local kind = StationDB.link_kind(link)
    if kind == "audio" then return nil end

    local named = kind == "playlist"
    local body, err = http_fetch(link, named and 65536 or 2048)
    if not body or body == "" then
        -- A link that only might have been a playlist is left alone: it is far
        -- more likely to be a stream that ignored us than a playlist we
        -- failed to read.
        if named then return nil, err or "empty response" end
        return nil
    end
    if not looks_like_playlist(body) then return nil end

    local streams = parse_playlist(body, link)
    if #streams == 0 then return nil, "no streams in playlist" end
    return streams
end

return StationDB
