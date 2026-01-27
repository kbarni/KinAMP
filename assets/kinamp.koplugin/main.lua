local WidgetContainer = require("ui/widget/container/widgetcontainer")
local UIManager = require("ui/uimanager")
local FileChooser = require("ui/widget/filechooser")
local InfoMessage = require("ui/widget/infomessage")
local Backend = require("kinamp_backend")
local Config = require("kinamp_config")
local logger = require("logger")
local _ = require("gettext")

local KinAMP = WidgetContainer:extend{
    name = "kinamp",
}

function KinAMP:init()
    logger.info("KinAMP: Plugin Loaded")
    if self.ui.menu then
        self.ui.menu:registerToMainMenu(self)
    end
    if self.ui.reader_menu then
        self.ui.reader_menu:registerToMainMenu(self)
    end
    self.current_playlist = Backend.load_internal_playlist()
end

-- Verification helper
function KinAMP:verifyPlayback()
    UIManager:scheduleIn(2, function()
        if not Backend.is_running() then
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
                    Backend.play_radio(s.url)
                    UIManager:show(InfoMessage:new{text=_("Playing: ") .. s.name,timeout=2})
                    self:verifyPlayback()
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
                Backend.play_internal_queue(self.current_playlist) 
                UIManager:show(InfoMessage:new{text=_("Starting playback..."),timeout=2})
                self:verifyPlayback()
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
                    Backend.play_from_index(idx, self.current_playlist)
                    UIManager:show(InfoMessage:new{text=_("Playing track ") .. idx,timeout=2})
                    self:verifyPlayback()
                end
            })
        end
    end

    return items
end

function KinAMP:chooseFolder(touchmenu_instance)
    local PathChooser = require("ui/widget/pathchooser")
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
                for idx, f in ipairs(files) do
                    table.insert(self.current_playlist, f)
                end
                UIManager:show(InfoMessage:new{text=string.format(_("Added %d tracks."), #files),timeout=2})
                if touchmenu_instance then touchmenu_instance:updateItems() end
            end
        end,
    }
    UIManager:show(path_chooser)
end

function KinAMP:chooseM3U()
    local file_chooser
    file_chooser = FileChooser:new{
        path = Config.music_dir,
        select_dirs = false,
        select_files = true,
        title = _("Select Playlist"),
        filter_func = function(name)
            return name:lower():match("%.m3u$") or name:lower():match("%.m3u8$")
        end,
        on_confirm = function(path)
            Backend.play_playlist_file(path)
            UIManager:show(InfoMessage:new{text=_("Starting playlist..."),timeout=2})
            self:verifyPlayback()
            UIManager:close(file_chooser)
        end,
        on_cancel = function()
            UIManager:close(file_chooser)
        end
    }
    UIManager:show(file_chooser)
end

return KinAMP
