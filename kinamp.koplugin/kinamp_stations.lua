-- Radio station manager.
--
-- The list the KinAMP players read from .kinamp_radio.txt, editable in place:
-- tap a station to select it, tap it again (or hold it) to play, and the
-- buttons along the bottom act on whatever is selected. Stations are added
-- either from the bundled database or by typing a name and a URL.
--
-- Tapping selects rather than plays because everything else in the window needs
-- to know which station you mean, and a list where the only way to point at a
-- station is to start listening to it makes editing one an accident away from a
-- burst of noise.
--
-- This replaces radio_cli, the text-mode editor that had to be launched from
-- KUAL with the reader closed.
--
-- Adding a station is the one slow operation: a good third of the links in the
-- database are .pls/.m3u playlists and the player has no playlist reader, so
-- those have to be fetched and unwrapped down to a stream first (see
-- kinamp_stationdb.lua). Anything that can be decided without the network is,
-- so an add that needs no lookup never asks for Wi-Fi.

local ButtonDialog = require("ui/widget/buttondialog")
local ConfirmBox = require("ui/widget/confirmbox")
local Device = require("device")
local InfoMessage = require("ui/widget/infomessage")
local InputDialog = require("ui/widget/inputdialog")
local Menu = require("ui/widget/menu")
local MultiInputDialog = require("ui/widget/multiinputdialog")
local NetworkMgr = require("ui/network/manager")
local UIManager = require("ui/uimanager")
local Backend = require("kinamp_backend")
local ButtonMenu = require("kinamp_menu")
local StationDB = require("kinamp_stationdb")
local ffiUtil = require("ffi/util")
local logger = require("logger")
local _ = require("gettext")
local T = ffiUtil.template
local Screen = Device.screen

-- How many playlists deep to keep unwrapping. Shoutcast's tunein-station.pls
-- legitimately points at another playlist; past this it's a loop.
local MAX_RESOLVE_DEPTH = 3

local PLAYING_MARK = "\u{25B6} "  -- the player widget marks the current track the same way
local SELECTED_MARK = "\u{2022} "
local ADDED_MARK = "\u{2713} "

-- Host part of a URL, for telling same-named stations apart in a result list.
local function url_host(url)
    return url:match("^%w+://([^/:]+)") or ""
end

local function notify(text, timeout)
    UIManager:show(InfoMessage:new{ text = text, timeout = timeout or 2 })
end

-- Runs `work` with a message on screen. Searches and playlist lookups both
-- block the UI thread for a second or two, which is long enough that something
-- has to say why.
local function with_message(text, work)
    local info = InfoMessage:new{ text = text }
    UIManager:show(info)
    -- Only worth showing if it gets painted before the work starts rather than
    -- after it has finished.
    UIManager:forceRePaint()
    local ok, result, extra = pcall(work)
    UIManager:close(info)
    if not ok then
        logger.warn("KinAMP:", result)
        return nil, "internal error"
    end
    return result, extra
end

local StationManager = ButtonMenu:extend{
    title = _("Radio stations"),
    -- Kept alongside the buttons: it's what the Menu key maps to on the
    -- keyboard Kindles, which have no way to tap a button.
    title_bar_left_icon = "plus",
    -- Which station the bottom buttons act on, if any.
    selected_idx = nil,
}

function StationManager:init()
    self.stations = Backend.get_stations()
    ButtonMenu.init(self)
end

function StationManager:genTitle()
    if #self.stations == 0 then return _("Radio stations") end
    return T(_("Radio stations (%1)"), #self.stations)
end

function StationManager:genItemTable()
    local items = {}
    -- Marking the station that's on the air makes the list double as a "what am
    -- I listening to" view, which is how most of these get opened.
    local status = Backend.get_status()
    local playing = status and status.is_radio and (status.station or status.path)

    for idx, station in ipairs(self.stations) do
        local mark = ""
        if playing and (station.name == playing or station.url == playing) then
            mark = PLAYING_MARK
        elseif idx == self.selected_idx then
            mark = SELECTED_MARK
        end
        items[#items + 1] = {
            text = mark .. station.name,
            mandatory = url_host(station.url),
            station = station,
            idx = idx,
        }
    end
    -- Menu draws this one in bold, which is what carries the selection when the
    -- station is also the one on the air and already wears the playing mark.
    items.current = self.selected_idx
    if #items == 0 then
        items[1] = {
            text = _("No stations yet - add one below"),
            select_enabled = false,
        }
    end
    return items
end

-- Points the bottom buttons at a station, or at nothing when idx is nil.
function StationManager:select(idx)
    if idx and (idx < 1 or idx > #self.stations) then idx = nil end
    self.selected_idx = idx
    self:updateList(idx)
end

function StationManager:saveStations(keep_idx)
    if not Backend.save_stations(self.stations) then
        notify(_("Could not save the station list."), 3)
        return false
    end
    -- Whatever was just added, moved or edited is the obvious thing to leave the
    -- buttons pointing at; a removal lands on whatever took its place.
    self:select(keep_idx)
    return true
end

-- The position goes along with the stream: it's what .kinamp.conf keeps, so a
-- player started later comes back to this station.
function StationManager:playStation(station, idx)
    local sent = Backend.play_radio(station.url, idx)
    notify(T(_("Playing: %1"), station.name))
    if not sent then
        -- Nothing was listening, so a player is starting up. Say so if it never
        -- answers, rather than leaving the user with a silent device.
        Backend.wait_until_running(function(ok)
            if not ok then
                notify(_("KinAMP failed to start."), 3)
            end
        end)
    end
    self:onClose()
end

-- First tap selects, a second one on the same station plays it. KOReader has no
-- double-tap on menu items and this is as close as it gets; holding a station
-- skips the selection step.
function StationManager:onMenuSelect(item)
    if not item.station then return true end
    if self.selected_idx == item.idx then
        self:playStation(item.station, item.idx)
    else
        self:select(item.idx)
    end
    return true
end

function StationManager:onMenuHold(item)
    if item.station then self:playStation(item.station, item.idx) end
    return true
end

function StationManager:genButtons()
    local function has_selection() return self.selected_idx ~= nil end
    local function on_selection(fn)
        return function()
            if not self.selected_idx then
                notify(_("Tap a station first."))
                return
            end
            fn(self.selected_idx)
        end
    end

    return {
        {
            {
                text = _("Play"),
                enabled_func = has_selection,
                callback = on_selection(function(idx)
                    self:playStation(self.stations[idx], idx)
                end),
            },
            {
                text = _("Add station"),
                callback = function() self:showAddDialog() end,
            },
            {
                text = _("Remove"),
                enabled_func = has_selection,
                callback = on_selection(function(idx) self:confirmRemove(idx) end),
            },
        },
        {
            {
                text = _("Edit"),
                enabled_func = has_selection,
                callback = on_selection(function(idx) self:showEditDialog(idx) end),
            },
            {
                text = "▲ " .. _("Up"),
                enabled_func = function() return self.selected_idx ~= nil and self.selected_idx > 1 end,
                callback = on_selection(function(idx) self:moveStation(idx, -1) end),
            },
            {
                text = "▼ " .. _("Down"),
                enabled_func = function()
                    return self.selected_idx ~= nil and self.selected_idx < #self.stations
                end,
                callback = on_selection(function(idx) self:moveStation(idx, 1) end),
            },
        },
    }
end

function StationManager:moveStation(idx, delta)
    local target = idx + delta
    if target < 1 or target > #self.stations then return end
    self.stations[idx], self.stations[target] = self.stations[target], self.stations[idx]
    self:saveStations(target)
end

function StationManager:confirmRemove(idx)
    local station = self.stations[idx]
    UIManager:show(ConfirmBox:new{
        text = T(_("Remove this station?\n\n%1"), station.name),
        ok_text = _("Remove"),
        ok_callback = function()
            table.remove(self.stations, idx)
            if self:saveStations(math.min(idx, #self.stations)) then
                notify(T(_("Removed: %1"), station.name))
            end
        end,
    })
end

-- The title bar's + and the "Add station" button are the same thing.
function StationManager:onLeftButtonTap()
    self:showAddDialog()
    return true
end

function StationManager:showAddDialog()
    local dialog
    local function act(fn)
        return function()
            UIManager:close(dialog)
            fn()
        end
    end

    dialog = ButtonDialog:new{
        title = _("Add a radio station"),
        title_align = "center",
        buttons = {
            {
                {
                    text = _("Search the station database"),
                    callback = act(function() self:showSearchDialog() end),
                },
            },
            {
                {
                    text = _("Enter name and URL"),
                    callback = act(function() self:showEditDialog(nil) end),
                },
            },
        },
    }
    UIManager:show(dialog)
end

-- Name/URL editor, for both a new station and an existing one. idx is nil to
-- append a new station.
function StationManager:showEditDialog(idx)
    local station = idx and self.stations[idx] or { name = "", url = "" }
    local dialog
    dialog = MultiInputDialog:new{
        title = idx and _("Edit station") or _("New station"),
        fields = {
            {
                description = _("Station name"),
                text = station.name,
                hint = _("Name"),
            },
            {
                description = _("Stream or playlist URL"),
                text = station.url,
                hint = "http://",
            },
        },
        buttons = {
            {
                {
                    text = _("Cancel"),
                    id = "close",
                    callback = function() UIManager:close(dialog) end,
                },
                {
                    text = idx and _("Save") or _("Add"),
                    is_enter_default = true,
                    callback = function()
                        local fields = dialog:getFields()
                        local name, url = fields[1], fields[2]
                        if not url or url:gsub("%s", "") == "" then
                            notify(_("A station needs a URL."))
                            return
                        end
                        UIManager:close(dialog)
                        self:acceptStation(name, url, idx)
                    end,
                },
            },
        },
    }
    UIManager:show(dialog)
    dialog:onShowKeyboard()
end

-- Takes a station from wherever it came from and works out what to store. idx
-- is the station to overwrite, or nil to append. on_done is called with the
-- stored station once it's in the list, which may be several dialogs later, or
-- never.
function StationManager:acceptStation(name, url, idx, on_done)
    url = url:gsub("%s", "")
    name = (name or ""):gsub("^%s+", ""):gsub("%s+$", "")
    if name == "" then name = url_host(url) end
    if name == "" then name = url end

    local station = { name = name, url = url }
    local kind = StationDB.link_kind(url)

    if kind == "audio" then
        return self:commitStation(station, idx, on_done)
    end
    if NetworkMgr:isOnline() then
        return self:resolveStation(station, 1, idx, on_done)
    end

    -- Offline. A named playlist is unplayable as it stands, so it's worth
    -- asking; an unknown link only *might* be one, and going online to find out
    -- isn't worth interrupting anybody for.
    if kind ~= "playlist" then
        return self:commitStation(station, idx, on_done)
    end
    UIManager:show(ConfirmBox:new{
        text = _("This is a playlist link. It has to be opened online before the player can use it.\n\nGo online now?"),
        ok_text = _("Go online"),
        ok_callback = function()
            NetworkMgr:runWhenOnline(function()
                self:resolveStation(station, 1, idx, on_done)
            end)
        end,
        -- Not the Cancel button: that one also fires when the box is dismissed
        -- with a tap outside, and adding a station off the back of "go away" is
        -- not what anybody meant.
        other_buttons = { { { text = _("Add as is"),
            callback = function() self:commitStation(station, idx, on_done) end } } },
    })
end

-- Unwraps playlists until a stream is left, asking when there's a choice.
function StationManager:resolveStation(station, depth, idx, on_done)
    if depth > MAX_RESOLVE_DEPTH then
        return self:commitStation(station, idx, on_done)
    end

    local streams, err = with_message(_("Opening playlist…"), function()
        return StationDB.resolve(station.url)
    end)

    if err then
        UIManager:show(ConfirmBox:new{
            text = T(_("Could not read the playlist:\n%1\n\nAdd the link as it is?"), err),
            ok_text = _("Add as is"),
            ok_callback = function() self:commitStation(station, idx, on_done) end,
        })
        return
    end
    if not streams then -- already a stream
        return self:commitStation(station, idx, on_done)
    end
    if #streams == 1 then
        station.url = streams[1]
        return self:resolveStation(station, depth + 1, idx, on_done)
    end

    -- Several streams: usually the same audio on different servers, so which
    -- one is picked matters and we can't pick it for them.
    local items = {}
    for _i, stream in ipairs(streams) do
        items[#items + 1] = {
            text = stream,
            mandatory = StationDB.is_unsupported(stream) and _("unsupported") or nil,
            stream = stream,
        }
    end

    local manager = self
    local picker
    local StreamMenu = Menu:extend{}

    function StreamMenu:onMenuSelect(item)
        UIManager:close(picker)
        station.url = item.stream
        manager:resolveStation(station, depth + 1, idx, on_done)
        return true
    end

    picker = StreamMenu:new{
        title = T(_("Choose a stream (%1)"), #streams),
        item_table = items,
        is_popout = true,
        is_borderless = false,
        multilines_show_more_text = true,
        width = math.floor(Screen:getWidth() * 0.9),
        height = math.floor(Screen:getHeight() * 0.8),
        close_callback = function() UIManager:close(picker) end,
    }
    UIManager:show(picker)
end

-- Puts the finished station into the list and saves it.
function StationManager:commitStation(station, idx, on_done)
    if StationDB.is_unsupported(station.url) then
        notify(_("HLS (.m3u8) streams are not supported yet."), 3)
        return
    end
    if not idx then
        for _i, existing in ipairs(self.stations) do
            if existing.url == station.url then
                notify(T(_("Already in your list as: %1"), existing.name), 3)
                return
            end
        end
    end

    if idx then
        self.stations[idx] = station
    else
        idx = #self.stations + 1
        self.stations[idx] = station
    end
    if not self:saveStations(idx) then return end

    notify(T(_("Added: %1"), station.name))
    if on_done then on_done(station) end
end

function StationManager:showSearchDialog()
    if not StationDB.db_path() then
        notify(_("The station database (allStations.json) was not found next to the KinAMP binaries."), 4)
        return
    end

    local dialog
    dialog = InputDialog:new{
        title = _("Search radio stations"),
        description = _("Words are matched against the station name, in any order."),
        input = self.last_query or "",
        input_hint = _("e.g. jazz radio"),
        buttons = {
            {
                {
                    text = _("Cancel"),
                    id = "close",
                    callback = function() UIManager:close(dialog) end,
                },
                {
                    text = _("Search"),
                    is_enter_default = true,
                    callback = function()
                        local query = dialog:getInputText()
                        UIManager:close(dialog)
                        self:runSearch(query)
                    end,
                },
            },
        },
    }
    UIManager:show(dialog)
    dialog:onShowKeyboard()
end

function StationManager:runSearch(query)
    if not query or query:gsub("%s", "") == "" then return end
    self.last_query = query

    local results, truncated = with_message(T(_("Searching for “%1”…"), query), function()
        return StationDB.search(query)
    end)

    if not results then
        notify(_("Could not read the station database."), 3)
        return
    end
    if #results == 0 then
        notify(T(_("No station matches “%1”."), query), 3)
        return
    end
    self:showResults(query, results, truncated)
end

function StationManager:showResults(query, results, truncated)
    local added = {}

    local function gen_items()
        local items = {}
        for _i, found in ipairs(results) do
            items[#items + 1] = {
                text = (added[found.url] and ADDED_MARK or "") .. found.name,
                mandatory = url_host(found.url),
                station = found,
            }
        end
        return items
    end

    local title = truncated
        and T(_("First %1 matches for “%2”"), #results, query)
        or T(_("%1 matches for “%2”"), #results, query)

    local manager = self
    local menu
    local ResultMenu = Menu:extend{}

    -- Tapping a result adds it and the list stays open with a tick against what
    -- went in, because looking for one station and adding three is the normal
    -- way this gets used.
    function ResultMenu:onMenuSelect(item)
        if added[item.station.url] then
            notify(T(_("Already added: %1"), item.station.name))
            return true
        end
        manager:acceptStation(item.station.name, item.station.url, nil, function(station)
            added[item.station.url] = true
            -- The URL that got stored is the resolved one; mark that too, so a
            -- second tap on the same row is caught.
            added[station.url] = true
            -- Redraw on the page it was tapped on, not back at the top.
            self:switchItemTable(nil, gen_items(), (self.page - 1) * self.perpage + 1)
        end)
        return true
    end

    -- The names in the database are what the uploader typed, so a hold showing
    -- the full URL is often the only way to tell two of them apart.
    function ResultMenu:onMenuHold(item)
        local dialog
        dialog = ButtonDialog:new{
            title = item.station.name .. "\n\n" .. item.station.url,
            title_align = "center",
            buttons = {
                {
                    {
                        text = _("Add"),
                        callback = function()
                            UIManager:close(dialog)
                            self:onMenuSelect(item)
                        end,
                    },
                },
            },
        }
        UIManager:show(dialog)
        return true
    end

    menu = ResultMenu:new{
        title = title,
        item_table = gen_items(),
        is_popout = true,
        is_borderless = false,
        width = math.floor(Screen:getWidth() * 0.9),
        height = math.floor(Screen:getHeight() * 0.9),
        close_callback = function() UIManager:close(menu) end,
    }
    UIManager:show(menu)
end

-- opts.on_close is called once the list is dismissed, so a caller showing
-- playback state can pick up whatever changed.
function StationManager.open(opts)
    opts = opts or {}
    local menu
    menu = StationManager:new{
        close_callback = opts.on_close,
    }
    UIManager:show(menu)
    return menu
end

return StationManager
