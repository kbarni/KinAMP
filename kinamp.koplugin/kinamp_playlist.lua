-- Playlist manager.
--
-- The queue the KinAMP players read from .kinamp_playlist.m3u, editable in
-- place: tap a track to play it, hold one for the things you can do to it, and
-- the buttons along the bottom fill, empty and store the list. The player
-- widget opens this, so the same window both shows the queue and edits it - the
-- KOReader-side counterpart of the GTK player's playlist pane.
--
-- Edits are written to the playlist file straight away, and handed to a running
-- player as well - but the player's only way to take a new list is the `load`
-- command, which stops playback (see cli_player.cpp), so a *playing* one is left
-- alone: a queue edited mid-track applies from the next time playback starts,
-- which is what tapping a track here does. Clearing or loading a list is the
-- exception: what it is playing has just been thrown away, so it is stopped.

local ButtonDialog = require("ui/widget/buttondialog")
local ConfirmBox = require("ui/widget/confirmbox")
local InfoMessage = require("ui/widget/infomessage")
local InputDialog = require("ui/widget/inputdialog")
local PathChooser = require("ui/widget/pathchooser")
local UIManager = require("ui/uimanager")
local Backend = require("kinamp_backend")
local ButtonMenu = require("kinamp_menu")
local Config = require("kinamp_config")
local ffiUtil = require("ffi/util")
local lfs = require("libs/libkoreader-lfs")
local _ = require("gettext")
local T = ffiUtil.template

local PLAYING_MARK = "\u{25B6} "  -- the player widget marks the current track the same way

local function notify(text, timeout)
    UIManager:show(InfoMessage:new{ text = text, timeout = timeout or 2 })
end

local function basename(path)
    return path:match("([^/]+)$") or path
end

local function is_m3u(filename)
    local name = filename:lower()
    return name:match("%.m3u$") ~= nil or name:match("%.m3u8$") ~= nil
end

local PlaylistManager = ButtonMenu:extend{
    title = _("Playlist"),
    -- Where the file and folder choosers open. Per instance, so adding a second
    -- album doesn't start back at the top.
    last_dir = nil,
}

function PlaylistManager:init()
    self.playlist = Backend.load_internal_playlist()
    self.last_dir = self.last_dir or Config.music_dir
    ButtonMenu.init(self)
end

function PlaylistManager:genTitle()
    if #self.playlist == 0 then return _("Playlist") end
    return T(_("Playlist (%1)"), #self.playlist)
end

function PlaylistManager:genItemTable()
    local items = {}
    -- Marking the playing track makes the list double as a "where am I in the
    -- album" view, which is half of what it gets opened for.
    --
    -- By path rather than by index: the player reports a position in the list it
    -- last loaded, and whenever that isn't this one - a queue edited mid-track,
    -- a list replaced under a player that kept going - the same number points at
    -- an unrelated track here. The index is only used to pick between duplicates
    -- of the same path, and only when it names one of them.
    local status = Backend.get_status()
    local playing_path = nil
    if status and not status.is_radio and status.path and status.path ~= "" then
        playing_path = status.path
    end
    local playing_idx = playing_path and status.index or nil
    if playing_idx and self.playlist[playing_idx] ~= playing_path then
        playing_idx = nil
    end

    for idx, path in ipairs(self.playlist) do
        local marked = playing_path ~= nil and path == playing_path
                       and (playing_idx == nil or idx == playing_idx)
        local mark = marked and PLAYING_MARK or ""
        items[#items + 1] = {
            text = string.format("%s%d. %s", mark, idx, basename(path)),
            path = path,
            pl_idx = idx,
        }
    end
    if #items == 0 then
        items[1] = {
            text = _("Empty - add files or a folder below"),
            select_enabled = false,
        }
    end
    return items
end

-- keep_idx is the row to stay on. Pass `replaced` when the whole list was swapped
-- rather than edited, so a player still on the old queue is stopped rather than
-- left playing a track this list no longer has. Syncing before the redraw, so
-- the playing mark is drawn from what the player ends up holding.
function PlaylistManager:savePlaylist(keep_idx, replaced)
    if not Backend.save_internal_playlist(self.playlist) then
        notify(_("Could not save the playlist."), 3)
        return false
    end
    Backend.sync_queue(replaced)
    self:updateList(keep_idx)
    return true
end

function PlaylistManager:playIndex(idx)
    -- Hands the queue over as well: it's already on disk, but a running player
    -- has whatever list it last loaded, which isn't this one if it has just
    -- been edited.
    local sent = Backend.play_from_index(idx, self.playlist)
    notify(T(_("Playing: %1"), basename(self.playlist[idx])))
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

function PlaylistManager:onMenuSelect(item)
    if item.pl_idx then self:playIndex(item.pl_idx) end
    return true
end

function PlaylistManager:onMenuHold(item)
    if not item.pl_idx then return true end
    local idx = item.pl_idx
    local dialog
    local function act(fn)
        return function()
            UIManager:close(dialog)
            fn()
        end
    end

    dialog = ButtonDialog:new{
        title = basename(item.path) .. "\n" .. item.path,
        title_align = "center",
        buttons = {
            {
                {
                    text = _("Play"),
                    callback = act(function() self:playIndex(idx) end),
                },
            },
            {
                {
                    text = "▲ " .. _("Move up"),
                    enabled = idx > 1,
                    callback = act(function() self:moveTrack(idx, -1) end),
                },
                {
                    text = "▼ " .. _("Move down"),
                    enabled = idx < #self.playlist,
                    callback = act(function() self:moveTrack(idx, 1) end),
                },
            },
            {
                {
                    text = _("Remove"),
                    callback = act(function() self:removeTrack(idx) end),
                },
            },
        },
    }
    UIManager:show(dialog)
    return true
end

function PlaylistManager:moveTrack(idx, delta)
    local target = idx + delta
    if target < 1 or target > #self.playlist then return end
    self.playlist[idx], self.playlist[target] = self.playlist[target], self.playlist[idx]
    self:savePlaylist(target)
end

function PlaylistManager:removeTrack(idx)
    local name = basename(self.playlist[idx])
    table.remove(self.playlist, idx)
    if self:savePlaylist(math.min(idx, #self.playlist)) then
        notify(T(_("Removed: %1"), name))
    end
end

function PlaylistManager:genButtons()
    return {
        {
            {
                text = _("Add files"),
                callback = function() self:addFiles() end,
            },
            {
                text = _("Add folder"),
                callback = function() self:addFolder() end,
            },
            {
                text = _("Clear"),
                callback = function() self:confirmClear() end,
            },
        },
        {
            {
                text = _("Save as…"),
                callback = function() self:saveAs() end,
            },
            {
                text = _("Load…"),
                callback = function() self:loadPlaylist() end,
            },
        },
    }
end

function PlaylistManager:addPaths(paths)
    local first_added = #self.playlist + 1
    for _i, path in ipairs(paths) do
        self.playlist[#self.playlist + 1] = path
    end
    if self:savePlaylist(first_added) then
        notify(T(_("Added %1 tracks."), #paths))
    end
end

-- One file at a time, keeping the chooser open. PathChooser confirms a single
-- file, and picking a whole album through a chooser that closes after each tap
-- is no fun, so it reopens itself where it left off until dismissed.
function PlaylistManager:addFiles()
    local chooser
    chooser = PathChooser:new{
        title = _("Long-press a track to add it"),
        path = self.last_dir,
        select_directory = false,
        select_file = true,
        show_files = true,
        file_filter = function(filename) return Backend.is_playable(filename) end,
        onConfirm = function(path)
            self.last_dir = path:match("^(.*)/[^/]*$") or self.last_dir
            self.playlist[#self.playlist + 1] = path
            if self:savePlaylist(#self.playlist) then
                notify(T(_("Added: %1"), basename(path)))
                -- PathChooser closes itself once this returns, so open the next
                -- one after that rather than underneath it.
                UIManager:nextTick(function() self:addFiles() end)
            end
        end,
    }
    UIManager:show(chooser)
end

function PlaylistManager:addFolder()
    UIManager:show(PathChooser:new{
        title = _("Long-press a folder to add its tracks"),
        path = self.last_dir,
        select_directory = true,
        select_file = false,
        show_files = false,
        onConfirm = function(path)
            self.last_dir = path
            local files = Backend.scan_folder(path)
            if #files == 0 then
                notify(_("No music files in that folder."))
                return
            end
            self:addPaths(files)
        end,
    })
end

function PlaylistManager:confirmClear()
    if #self.playlist == 0 then
        notify(_("The playlist is already empty."))
        return
    end
    UIManager:show(ConfirmBox:new{
        text = T(_("Remove all %1 tracks from the playlist?"), #self.playlist),
        ok_text = _("Clear"),
        ok_callback = function()
            self.playlist = {}
            if self:savePlaylist(nil, true) then
                notify(_("Playlist cleared."))
            end
        end,
    })
end

function PlaylistManager:saveAs()
    if #self.playlist == 0 then
        notify(_("There is nothing to save."))
        return
    end

    local dialog
    dialog = InputDialog:new{
        title = _("Save playlist as"),
        description = T(_("Saved in %1"), self.last_dir),
        input = "playlist.m3u",
        buttons = {
            {
                {
                    text = _("Cancel"),
                    id = "close",
                    callback = function() UIManager:close(dialog) end,
                },
                {
                    text = _("Save"),
                    is_enter_default = true,
                    callback = function()
                        local name = dialog:getInputText()
                        UIManager:close(dialog)
                        self:writePlaylistFile(name)
                    end,
                },
            },
        },
    }
    UIManager:show(dialog)
    dialog:onShowKeyboard()
end

function PlaylistManager:writePlaylistFile(name)
    -- A name is a name, not a path: a stray slash would silently write into
    -- some other directory, or fail on one that doesn't exist.
    name = (name or ""):gsub("[/\r\n]", " "):gsub("^%s+", ""):gsub("%s+$", "")
    if name == "" then return end
    if not is_m3u(name) then name = name .. ".m3u" end

    local path = self.last_dir:gsub("/?$", "/") .. name
    local function write()
        if Backend.write_m3u(path, self.playlist) then
            notify(T(_("Saved: %1"), path), 3)
        else
            notify(T(_("Could not write %1"), path), 3)
        end
    end

    if lfs.attributes(path, "mode") then
        UIManager:show(ConfirmBox:new{
            text = T(_("%1 already exists.\n\nOverwrite it?"), path),
            ok_text = _("Overwrite"),
            ok_callback = write,
        })
    else
        write()
    end
end

-- Replaces the queue rather than appending to it: the players keep exactly one
-- queue, so loading a playlist is how you switch to it. Anything worth keeping
-- can be saved first with the button next to this one.
function PlaylistManager:loadPlaylist()
    UIManager:show(PathChooser:new{
        title = _("Long-press a playlist to load it"),
        path = self.last_dir,
        select_directory = false,
        select_file = true,
        show_files = true,
        file_filter = function(filename) return is_m3u(filename) end,
        onConfirm = function(path)
            self.last_dir = path:match("^(.*)/[^/]*$") or self.last_dir
            local entries = Backend.read_m3u(path)
            if #entries == 0 then
                notify(_("No playable entries in that playlist."))
                return
            end
            self.playlist = entries
            if self:savePlaylist(1, true) then
                notify(T(_("Loaded %1 tracks."), #entries))
            end
        end,
    })
end

-- opts.on_close is called once the list is dismissed, so a caller showing
-- playback state can pick up whatever changed.
function PlaylistManager.open(opts)
    opts = opts or {}
    local menu
    menu = PlaylistManager:new{
        close_callback = opts.on_close,
    }
    UIManager:show(menu)
    return menu
end

return PlaylistManager
