local WidgetContainer = require("ui/widget/container/widgetcontainer")
local UIManager = require("ui/uimanager")
local Dispatcher = require("dispatcher")
local PathChooser = require("ui/widget/pathchooser")
local InfoMessage = require("ui/widget/infomessage")
local KinAMPPlayer = require("kinamp_player")
local Backend = require("kinamp_backend")
local Config = require("kinamp_config")
local logger = require("logger")
local _ = require("gettext")

local KinAMP = WidgetContainer:extend{
    name = "kinamp",
}

-- Registered as a Dispatcher action rather than a hardcoded gesture, so the
-- player can be bound to whatever the user prefers in Gesture Manager - a tap
-- on the bottom right corner being the obvious one.
function KinAMP:onDispatcherRegisterActions()
    Dispatcher:registerAction("kinamp_show_player",
        { category = "none", event = "ShowKinAMPPlayer", title = _("KinAMP: show player"), general = true })
end

function KinAMP:init()
    logger.info("KinAMP: Plugin Loaded")
    self:onDispatcherRegisterActions()
    if self.ui.menu then
        self.ui.menu:registerToMainMenu(self)
    end
    if self.ui.reader_menu then
        self.ui.reader_menu:registerToMainMenu(self)
    end
    self.current_playlist = Backend.load_internal_playlist()
end

function KinAMP:onShowKinAMPPlayer()
    -- Close only a player that is genuinely still on screen: closing one the
    -- user already dismissed would re-fire CloseWidget on a freed widget.
    if self.player and UIManager:isWidgetShown(self.player) then
        UIManager:close(self.player)
        self.player = nil
        return true -- treat the gesture as a toggle
    end
    self.player = KinAMPPlayer:new{}
    UIManager:show(self.player)
    return true
end

-- Verification helper
-- Only meaningful when a new player had to be launched: if a running player took
-- the command over the FIFO there is nothing to wait for. Launching goes through
-- startkinamp_koreader.sh, which stops any previous instance first, so this can
-- legitimately take a few seconds.
function KinAMP:verifyPlayback(command_was_sent)
    if command_was_sent then return end
    Backend.wait_until_running(function(ok)
        if not ok then
            UIManager:show(InfoMessage:new{text=_("Error: KinAMP failed to start.\nCheck logs.")})
        end
    end)
end

function KinAMP:addToMainMenu(menu_items)
    menu_items.kinamp = {
        text = _("KinAMP Player"),
        sorting_hint = "tools",
        sub_item_table = {
            {
                text = _("Show Player"),
                callback = function() self:onShowKinAMPPlayer() end,
                separator = true,
            },
            {
                text = _("Radio Stations"),
                sub_item_table_func = function() return self:getRadioSubmenu() end,
            },
            {
                text = _("Play M3U Playlist…"),
                callback = function() self:chooseM3U() end
            },
            {
                text = _("Music Library"),
                sub_item_table_func = function() return self:getLibrarySubmenu() end,
            },
            {
                text = _("Stop Playback"),
                callback = function()
                    Backend.stop()
                    UIManager:show(InfoMessage:new{text=_("Playback Stopped"),timeout=2})
                end
            },
            {
                -- Stopping now leaves the player resident so the next track
                -- starts instantly; this is how you get rid of it entirely.
                text = _("Quit KinAMP Player"),
                callback = function()
                    if Backend.quit() then
                        UIManager:show(InfoMessage:new{text=_("Player closed"),timeout=2})
                    else
                        UIManager:show(InfoMessage:new{text=_("Player is not running"),timeout=2})
                    end
                end
            }
        }
    }
end

function KinAMP:getRadioSubmenu()
    local stations = Backend.get_stations()
    local items = {}
    if #stations == 0 then
        table.insert(items, {text = _("No stations found"), enabled = false})
    else
        for idx, s in ipairs(stations) do
            table.insert(items, {
                text = s.name,
                callback = function() 
                    local sent = Backend.play_radio(s.url)
                    UIManager:show(InfoMessage:new{text=_("Playing: ") .. s.name,timeout=2})
                    self:verifyPlayback(sent)
                end
            })
        end
    end
    return items
end

function KinAMP:getLibrarySubmenu()
    local items = {}
    
    -- 1. Play Internal Library
    table.insert(items, {
        text = _("Play Internal Playlist"),
        callback = function() 
            if #self.current_playlist == 0 then
                UIManager:show(InfoMessage:new{text=_("Playlist is empty!"),timeout=2})
            else
                local sent = Backend.play_internal_queue(self.current_playlist)
                UIManager:show(InfoMessage:new{text=_("Starting playback..."),timeout=2})
                self:verifyPlayback(sent)
            end
        end
    })
    
    -- 2. Add Folder
    table.insert(items, {
        text = _("Add Folder Content…"),
        keep_menu_open = true,
        callback = function(touchmenu_instance) self:chooseFolder(touchmenu_instance) end
    })
    
    -- 3. Clear Playlist
    table.insert(items, {
        text = _("Clear Internal Playlist"),
        keep_menu_open = true,
        callback = function(touchmenu_instance)
            self.current_playlist = {}
            Backend.save_internal_playlist(self.current_playlist)
            UIManager:show(InfoMessage:new{text=_("Playlist Cleared"),timeout=2})
            if touchmenu_instance then touchmenu_instance:updateItems() end
        end
    })
    
    -- Separator
    table.insert(items, { text = _("--- Current Queue ---"), enabled = false })
    
    -- List Items
    if #self.current_playlist == 0 then
        table.insert(items, { text = _("(Empty)"), enabled = false })
    else
        for idx, path in ipairs(self.current_playlist) do
            local name = path:match("([^/]+)$") or path
            table.insert(items, {
                text = string.format("%d. %s", idx, name),
                callback = function()
                    local sent = Backend.play_from_index(idx, self.current_playlist)
                    UIManager:show(InfoMessage:new{text=_("Playing track ") .. idx,timeout=2})
                    self:verifyPlayback(sent)
                end
            })
        end
    end

    return items
end

function KinAMP:chooseFolder(touchmenu_instance)
    local path_chooser
    path_chooser = PathChooser:new{
        select_directory = true,
        select_file = false,
        show_files = false,
        path = Config.music_dir,
        onConfirm = function(path)
            local files = Backend.scan_folder(path)
            if #files == 0 then
                UIManager:show(InfoMessage:new{text=_("No music files found in folder."),timeout=2})
            else
                for _, f in ipairs(files) do
                    table.insert(self.current_playlist, f)
                end
                -- Persist immediately: the queue used to survive only if you
                -- happened to press Play before closing KOReader.
                Backend.save_internal_playlist(self.current_playlist)
                UIManager:show(InfoMessage:new{text=string.format(_("Added %d tracks."), #files),timeout=2})
                if touchmenu_instance then touchmenu_instance:updateItems() end
            end
        end,
    }
    UIManager:show(path_chooser)
end

function KinAMP:chooseM3U()
    UIManager:show(PathChooser:new{
        title = _("Long-press a playlist to choose it"),
        path = Config.music_dir,
        select_directory = false,
        select_file = true,
        show_files = true,
        file_filter = function(filename)
            local name = filename:lower()
            return name:match("%.m3u$") ~= nil or name:match("%.m3u8$") ~= nil
        end,
        -- PathChooser closes itself once this returns; closing it here as well
        -- would be a double close.
        onConfirm = function(path)
            local entries = Backend.read_m3u(path)
            if #entries == 0 then
                UIManager:show(InfoMessage:new{text=_("No playable entries in that playlist."),timeout=2})
                return
            end
            -- The chosen playlist becomes the internal queue, so the player's
            -- playlist view and the running daemon always describe the same
            -- list. Playing it without this leaves the two disagreeing.
            self.current_playlist = entries
            local sent = Backend.play_internal_queue(entries)
            UIManager:show(InfoMessage:new{
                text = string.format(_("Playing %d tracks."), #entries), timeout = 2})
            self:verifyPlayback(sent)
        end,
    })
end

return KinAMP
