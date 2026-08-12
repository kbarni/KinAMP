--[[--
Floating media player for KinAMP.

A movable dialog with cover art, now-playing text, a progress bar and transport
controls. It renders whatever the player daemon publishes in its status file and
drives it over the command FIFO; it never launches or kills anything itself
except through Backend.

E-ink shapes the design: the widget polls once a second while visible, but only
repaints the region that actually changed. Position ticks touch the progress
row alone, and the frame as a whole is only redrawn when the track or the
playback state changes.
--]]

local Blitbuffer = require("ffi/blitbuffer")
local CenterContainer = require("ui/widget/container/centercontainer")
local Device = require("device")
local FocusManager = require("ui/widget/focusmanager")
local Font = require("ui/font")
local FrameContainer = require("ui/widget/container/framecontainer")
local Geom = require("ui/geometry")
local GestureRange = require("ui/gesturerange")
local HorizontalGroup = require("ui/widget/horizontalgroup")
local HorizontalSpan = require("ui/widget/horizontalspan")
local IconButton = require("ui/widget/iconbutton")
local IconWidget = require("ui/widget/iconwidget")
local ImageWidget = require("ui/widget/imagewidget")
local MovableContainer = require("ui/widget/container/movablecontainer")
local ProgressWidget = require("ui/widget/progresswidget")
local Size = require("ui/size")
local TextWidget = require("ui/widget/textwidget")
local TitleBar = require("ui/widget/titlebar")
local UIManager = require("ui/uimanager")
local VerticalGroup = require("ui/widget/verticalgroup")
local VerticalSpan = require("ui/widget/verticalspan")
local VerticalSlider = require("kinamp_slider")
local Backend = require("kinamp_backend")
local logger = require("logger")
local _ = require("gettext")
local Screen = Device.screen

local ICON_DIR = debug.getinfo(1, "S").source:match("@?(.*/)") .. "icons/"

--- Button showing one of our own icons.
-- IconButton resolves names against KOReader's icon set, which has no media
-- controls; everything else it does (tap flash, hold, geometry) is reused as is.
local PlayerButton = IconButton:extend{
    icon_file = nil,
}

function PlayerButton:init()
    self.image = IconWidget:new{
        file = ICON_DIR .. self.icon_file,
        width = self.width,
        height = self.height,
        alpha = true,
    }

    self.show_parent = self.show_parent or self

    self.horizontal_group = HorizontalGroup:new{}
    table.insert(self.horizontal_group, HorizontalSpan:new{})
    table.insert(self.horizontal_group, self.image)
    table.insert(self.horizontal_group, HorizontalSpan:new{})

    self.button = VerticalGroup:new{}
    table.insert(self.button, VerticalSpan:new{})
    table.insert(self.button, self.horizontal_group)
    table.insert(self.button, VerticalSpan:new{})

    self[1] = self.button
    self:update()
end

--- Swaps the glyph, e.g. play <-> pause.
function PlayerButton:setIconFile(icon_file)
    if self.icon_file == icon_file then return false end
    self.icon_file = icon_file
    self.image:free()
    self.image.file = ICON_DIR .. icon_file
    self.image._bb = nil
    return true
end

local KinAMPPlayer = FocusManager:extend{
    -- How often to re-read the player's status while we are on screen.
    poll_interval = 1,
    -- Last status we rendered, to decide what needs repainting.
    last = nil,
}

local function format_time(seconds)
    if not seconds or seconds < 0 then seconds = 0 end
    return string.format("%d:%02d", math.floor(seconds / 60), seconds % 60)
end

function KinAMPPlayer:init()
    local screen_w, screen_h = Screen:getWidth(), Screen:getHeight()

    local border_size = Size.border.window
    local padding = Size.padding.large
    self.frame_width = math.min(
        math.floor(math.min(screen_w, screen_h) * 0.84),
        screen_w - 2 * Size.margin.default)
    local inner_width = self.frame_width - 2 * (border_size + padding)

    -- The volume column runs down the right edge; everything else lives in what
    -- is left of the dialog, so the two never overlap and the cover stays
    -- centred on its own column rather than on the frame.
    local volume_width = Screen:scaleBySize(32)
    local volume_gap = Size.padding.large
    local content_width = inner_width - volume_width - volume_gap

    -- Icon sizes: the play button is deliberately larger, it is the one control
    -- you aim for without looking.
    local main_button = Screen:scaleBySize(60)
    local side_button = Screen:scaleBySize(38)
    local cover_size = math.floor(content_width * 0.60)

    if Device:hasKeys() then
        self.key_events.Close = { { Device.input.group.Back } }
    end
    if Device:isTouchDevice() then
        self.ges_events.Tap = {
            GestureRange:new{
                ges = "tap",
                range = Geom:new{ x = 0, y = 0, w = screen_w, h = screen_h },
            },
        }
    end

    -- --- Title bar -------------------------------------------------------
    self.title_bar = TitleBar:new{
        width = inner_width,
        align = "center",
        title = _("KinAMP"),
        with_bottom_line = true,
        -- Everything that has no button of its own lives behind this.
        left_icon = "appbar.menu",
        left_icon_tap_callback = function() self:showMenu() end,
        close_callback = function() self:onClose() end,
        show_parent = self,
    }

    -- --- Cover art -------------------------------------------------------
    -- Kept in its own container so a new track can swap the image without
    -- disturbing anything around it.
    self.cover_container = CenterContainer:new{
        dimen = Geom:new{ w = content_width, h = cover_size },
        self:buildCover(nil, cover_size),
    }
    self.cover_size = cover_size
    self.cover_frame = FrameContainer:new{
        bordersize = 0,
        padding = 0,
        margin = 0,
        self.cover_container,
    }

    -- --- Now playing -----------------------------------------------------
    self.artist_text = TextWidget:new{
        text = "",
        face = Font:getFace("cfont", 17),
        max_width = content_width,
    }
    self.title_text = TextWidget:new{
        text = _("Not playing"),
        face = Font:getFace("tfont", 21),
        bold = true,
        max_width = content_width,
    }
    self.info_frame = FrameContainer:new{
        bordersize = 0,
        padding = 0,
        margin = 0,
        VerticalGroup:new{
            align = "center",
            CenterContainer:new{
                dimen = Geom:new{ w = content_width, h = self.artist_text:getSize().h },
                self.artist_text,
            },
            VerticalSpan:new{ width = Size.padding.small },
            CenterContainer:new{
                dimen = Geom:new{ w = content_width, h = self.title_text:getSize().h },
                self.title_text,
            },
        },
    }

    -- --- Progress --------------------------------------------------------
    self.progress_bar = ProgressWidget:new{
        width = content_width,
        height = Screen:scaleBySize(10),
        percentage = 0,
        margin_h = 0,
        margin_v = 0,
    }
    -- Radio has no length to show, so the bar is swapped for a blank of the
    -- same height rather than left sitting at zero.
    self.progress_blank = VerticalSpan:new{ width = self.progress_bar.height }
    self.elapsed_text = TextWidget:new{
        text = "0:00",
        face = Font:getFace("xx_smallinfofont"),
    }
    self.total_text = TextWidget:new{
        text = "0:00",
        face = Font:getFace("xx_smallinfofont"),
    }
    self.progress_group = VerticalGroup:new{
        align = "left",
        self.progress_bar,
        VerticalSpan:new{ width = Size.padding.small },
        HorizontalGroup:new{
            self.elapsed_text,
            HorizontalSpan:new{ width = content_width - self.elapsed_text:getSize().w
                                        - self.total_text:getSize().w },
            self.total_text,
        },
    }
    self.progress_frame = FrameContainer:new{
        bordersize = 0,
        padding = 0,
        margin = 0,
        self.progress_group,
    }

    -- --- Controls --------------------------------------------------------
    self.playlist_button = PlayerButton:new{
        icon_file = "playlist.svg",
        width = side_button, height = side_button,
        show_parent = self,
        callback = function() self:showPlaylist() end,
    }
    self.prev_button = PlayerButton:new{
        icon_file = "prev.svg",
        width = side_button, height = side_button,
        show_parent = self,
        callback = function() self:onPrevious() end,
    }
    self.play_button = PlayerButton:new{
        icon_file = "play.svg",
        width = main_button, height = main_button,
        show_parent = self,
        callback = function() self:onPlayPause() end,
    }
    self.next_button = PlayerButton:new{
        icon_file = "next.svg",
        width = side_button, height = side_button,
        show_parent = self,
        callback = function() self:onNext() end,
    }
    self.radio_button = PlayerButton:new{
        icon_file = "radio.svg",
        width = side_button, height = side_button,
        show_parent = self,
        callback = function() self:showStations() end,
    }

    local buttons = {
        self.playlist_button, self.prev_button, self.play_button,
        self.next_button, self.radio_button,
    }
    local buttons_width = 0
    for _, b in ipairs(buttons) do
        buttons_width = buttons_width + b:getSize().w
    end
    local gap = math.max(0, math.floor((content_width - buttons_width) / (#buttons - 1)))

    local controls = HorizontalGroup:new{ align = "center" }
    for i, b in ipairs(buttons) do
        if i > 1 then
            table.insert(controls, HorizontalSpan:new{ width = gap })
        end
        table.insert(controls, b)
    end
    self.controls_frame = FrameContainer:new{
        bordersize = 0,
        padding = 0,
        margin = 0,
        controls,
    }

    -- --- Volume ----------------------------------------------------------
    -- Sits beside the cover: high enough to be a comfortable target, short
    -- enough to leave the artwork the widest thing in the dialog.
    self.volume_label = TextWidget:new{
        text = _("VOL"),
        face = Font:getFace("xx_smallinfofont"),
    }
    local label_height = self.volume_label:getSize().h
    self.volume_slider = VerticalSlider:new{
        width = volume_width,
        height = cover_size - label_height - Size.padding.small,
    }
    self.volume_frame = FrameContainer:new{
        bordersize = 0,
        padding = 0,
        margin = 0,
        VerticalGroup:new{
            align = "center",
            CenterContainer:new{
                dimen = Geom:new{ w = volume_width, h = label_height },
                self.volume_label,
            },
            VerticalSpan:new{ width = Size.padding.small },
            self.volume_slider,
        },
    }

    -- --- Assembly --------------------------------------------------------
    local body = HorizontalGroup:new{
        align = "top",
        VerticalGroup:new{
            align = "center",
            self.cover_frame,
            VerticalSpan:new{ width = Size.padding.large },
            self.info_frame,
            VerticalSpan:new{ width = Size.padding.large },
            self.progress_frame,
            VerticalSpan:new{ width = Size.padding.large },
            self.controls_frame,
        },
        HorizontalSpan:new{ width = volume_gap },
        self.volume_frame,
    }

    self.frame = FrameContainer:new{
        background = Blitbuffer.COLOR_WHITE,
        bordersize = border_size,
        radius = Size.radius.window,
        padding = padding,
        margin = 0,
        VerticalGroup:new{
            align = "center",
            self.title_bar,
            VerticalSpan:new{ width = Size.padding.large },
            body,
        },
    }

    self.movable = MovableContainer:new{ self.frame }
    self[1] = CenterContainer:new{
        dimen = Geom:new{ w = screen_w, h = screen_h },
        self.movable,
    }

    self.layout = { { self.playlist_button, self.prev_button, self.play_button,
                      self.next_button, self.radio_button } }

    self:refresh(true)
end

--- Builds the cover image: embedded artwork if the player extracted any,
-- otherwise the KinAMP mark.
function KinAMPPlayer:buildCover(path, size)
    if path then
        local lfs = require("libs/libkoreader-lfs")
        if lfs.attributes(path, "mode") == "file" then
            return ImageWidget:new{
                file = path,
                width = size,
                height = size,
                scale_factor = 0, -- fit, keeping aspect ratio
                -- Every track writes to the same filename, so caching by path
                -- would keep showing the previous track's artwork.
                file_do_cache = false,
            }
        end
    end
    return ImageWidget:new{
        file = ICON_DIR .. "kinamp-icon.png",
        width = size,
        height = size,
        scale_factor = 0, -- fit, keeping aspect ratio
        -- The mark is transparent outside the diamond; without this its
        -- background blits as a grey box instead of the dialog's white.
        alpha = true,
    }
end

--=============================================================================
-- State rendering
--=============================================================================

--- Re-reads the player status and repaints whatever changed.
--
-- Guarded, because every path into it runs somewhere that catches nothing: the
-- 1 Hz poll is a UIManager task, and the rest are widget construction and button
-- callbacks. An error in any of them propagates out of the event loop and takes
-- KOReader down - losing someone's place in a book because a status file could
-- not be read is not a trade worth making. A failed refresh leaves the widget
-- showing whatever it had.
-- @param force redraw the whole frame regardless
function KinAMPPlayer:refresh(force)
    local ok, err = pcall(self.doRefresh, self, force)
    if not ok then
        logger.warn("KinAMP: status refresh failed:", err)
    end
end

function KinAMPPlayer:doRefresh(force)
    local status = Backend.get_status()
    local last = self.last

    local repaint_all = force or false
    local repaint_info = false
    local repaint_progress = false
    local repaint_play = false
    local repaint_volume = false

    -- Track identity: path covers both files and stream URLs.
    local track_changed = not last or last.path ~= (status and status.path)
        or (last.state == nil) ~= (status == nil)

    if not status then
        if track_changed then
            self.artist_text:setText("")
            self.title_text:setText(_("Not playing"))
            self:setCover(nil)
            self.progress_group[1] = self.progress_blank
            self.elapsed_text:setText("0:00")
            self.total_text:setText("0:00")
            -- A player that is not running has no volume to show; the next one
            -- to start comes up at full, which is what the knob now says.
            self.volume_slider:setPercentage(1)
            repaint_all = true
        end
        if self.play_button:setIconFile("play.svg") then repaint_all = true end
        self.last = nil
    else
        if track_changed or (last and last.title ~= status.title)
                or (last and last.artist ~= status.artist) then
            -- Radio streams carry the whole "Artist - Title" in one ICY field
            -- and have no artist of their own; fall back to the station name.
            local title = status.title
            if not title or title == "" then
                title = status.path and (status.path:match("([^/]+)$") or status.path) or _("Unknown")
            end
            self.title_text:setText(title)
            self.artist_text:setText(status.artist ~= "" and status.artist
                                     or (status.is_radio and status.station or ""))
            repaint_info = true
        end

        if track_changed then
            self:setCover(status.cover)
            repaint_all = true
        end

        -- Progress
        local elapsed = format_time(status.pos)
        local total, percentage
        if status.is_radio or not status.dur or status.dur <= 0 then
            total = status.is_radio and _("LIVE") or "0:00"
            percentage = nil
        else
            total = format_time(status.dur)
            percentage = math.min(1, status.pos / status.dur)
        end

        if percentage then
            if self.progress_group[1] ~= self.progress_bar then
                self.progress_group[1] = self.progress_bar
                repaint_all = true
            end
            if self.progress_bar.percentage ~= percentage then
                self.progress_bar.percentage = percentage
                repaint_progress = true
            end
        elseif self.progress_group[1] ~= self.progress_blank then
            self.progress_group[1] = self.progress_blank
            repaint_all = true
        end

        if self.elapsed_text.text ~= elapsed then
            self.elapsed_text:setText(elapsed)
            repaint_progress = true
        end
        if self.total_text.text ~= total then
            self.total_text:setText(total)
            repaint_progress = true
        end

        -- Volume: the knob follows the player, but only when what the player
        -- reports actually changes. A tap moves the knob immediately and the
        -- next poll or two can still carry the old value; adopting every report
        -- would snap it back before the player has caught up.
        if status.vol and (not last or last.vol ~= status.vol) then
            repaint_volume = self.volume_slider:setPercentage(status.vol / 100)
        end

        repaint_play = self.play_button:setIconFile(
            status.is_playing and "pause.svg" or "play.svg")

        self.last = status
    end

    if repaint_all then
        self:refreshRegion(self.frame)
    else
        if repaint_info then self:refreshRegion(self.info_frame) end
        if repaint_play then self:refreshRegion(self.controls_frame) end
        if repaint_volume then self:refreshRegion(self.volume_slider) end
        -- The position tick is the only thing that happens every second, so it
        -- gets the cheapest refresh we have.
        if repaint_progress then self:refreshRegion(self.progress_frame, "fast") end
    end
end

function KinAMPPlayer:refreshRegion(widget, refresh_type)
    if not widget or not widget.dimen then return end
    UIManager:setDirty(self, function()
        return refresh_type or "ui", widget.dimen
    end)
end

function KinAMPPlayer:setCover(path)
    if self.cover_path == path then return end
    self.cover_path = path
    local old = self.cover_container[1]
    if old and old.free then old:free() end
    self.cover_container[1] = self:buildCover(path, self.cover_size)
end

--=============================================================================
-- Polling
--=============================================================================

function KinAMPPlayer:startPolling()
    if not self._poll_callback then
        self._poll_callback = function()
            -- refresh() is guarded, so a failed tick cannot break the loop.
            self:refresh()
            UIManager:scheduleIn(self.poll_interval, self._poll_callback)
        end
    end
    UIManager:scheduleIn(self.poll_interval, self._poll_callback)
end

function KinAMPPlayer:stopPolling()
    if self._poll_callback then
        UIManager:unschedule(self._poll_callback)
    end
end

--=============================================================================
-- Actions
--=============================================================================

--- Applies a change straight away instead of waiting for the next poll, so the
-- button you pressed reacts immediately rather than up to a second later.
function KinAMPPlayer:actAndRefresh(fn)
    fn()
    UIManager:scheduleIn(0.2, function() self:refresh() end)
end

function KinAMPPlayer:onPlayPause()
    local status = Backend.get_status()
    if not status then
        -- Nothing running: start the saved queue.
        local playlist = Backend.load_internal_playlist()
        if #playlist == 0 then
            local InfoMessage = require("ui/widget/infomessage")
            UIManager:show(InfoMessage:new{ text = _("Playlist is empty!"), timeout = 2 })
            return
        end
        self:actAndRefresh(function() Backend.play_internal_queue(playlist) end)
        -- A cold start goes through the launcher script, which can take a few
        -- seconds; keep checking until it answers.
        Backend.wait_until_running(function() self:refresh(true) end)
        return
    end
    self:actAndRefresh(function() Backend.pause() end)
end

function KinAMPPlayer:onNext()
    self:actAndRefresh(function() Backend.next_track() end)
end

function KinAMPPlayer:onPrevious()
    self:actAndRefresh(function() Backend.previous_track() end)
end

--- What the buttons around the cover cannot say: how the queue is played, and
-- how to stop or get rid of the player.
--
-- Stop and quit are different requests and both are worth having. Stopping
-- leaves the daemon resident, costing nothing while idle (it sits in poll() on
-- the command FIFO) and starting the next track instantly; quitting reclaims
-- the several megabytes of resident GStreamer, at the price of a couple of
-- seconds the next time something plays.
function KinAMPPlayer:showMenu()
    local ButtonDialog = require("ui/widget/buttondialog")
    local InfoMessage = require("ui/widget/infomessage")
    local dialog
    local function act(fn)
        return function()
            UIManager:close(dialog)
            fn()
        end
    end

    -- Read once, when the dialog is built: the checkmarks are drawn from it and
    -- picking one closes the dialog, so it never has to be kept up to date.
    local strategy = Backend.get_strategy()
    local function strategy_button(text, value)
        return {
            text = text,
            checked_func = function() return strategy == value end,
            callback = act(function()
                Backend.set_strategy(value)
                UIManager:show(InfoMessage:new{ text = text, timeout = 2 })
            end),
        }
    end

    dialog = ButtonDialog:new{
        title = _("KinAMP"),
        title_align = "center",
        buttons = {
            {
                -- Matches the player's own enum: 0 normal, 1 repeat, 2 random.
                strategy_button(_("In order"), 0),
                strategy_button(_("Repeat"), 1),
                strategy_button(_("Shuffle"), 2),
            },
            {
                {
                    text = _("Stop playback"),
                    callback = act(function()
                        self:actAndRefresh(function() Backend.stop() end)
                    end),
                },
            },
            {
                {
                    text = _("Quit player"),
                    callback = act(function()
                        local was_running = Backend.quit()
                        UIManager:show(InfoMessage:new{
                            text = was_running and _("Player closed")
                                                or _("Player is not running"),
                            timeout = 2,
                        })
                        self:actAndRefresh(function() end)
                    end),
                },
            },
        },
    }
    UIManager:show(dialog)
end

--- Opens the queue, which is also where it is edited: tracks are added,
-- reordered and removed there, and it plays what you tap and closes on its way
-- out, so all that is left for us is to pick up whatever it started.
function KinAMPPlayer:showPlaylist()
    local PlaylistManager = require("kinamp_playlist")
    PlaylistManager.open{
        on_close = function()
            self:refresh(true)
        end,
    }
end

--- Opens the station list, which is also where stations are added and removed.
-- It plays the station itself and closes on its way out, so all that is left
-- for us is to pick up whatever it started.
function KinAMPPlayer:showStations()
    local StationManager = require("kinamp_stations")
    StationManager.open{
        on_close = function()
            self:refresh(true)
        end,
    }
end

--=============================================================================
-- Events
--=============================================================================

--- Moves the volume to wherever the slider was tapped.
-- The knob only follows if the player took the command: with nothing listening
-- there is no volume to change, and a knob that moved anyway would be claiming
-- otherwise.
function KinAMPPlayer:setVolumeAt(y)
    local pct = self.volume_slider:percentageAt(y)
    -- Rounded, not truncated: the slider works in whole percent, and 0.29 * 100
    -- is 28.999... in binary, which would send a value one below the knob and
    -- have the next poll drag it back down.
    if not Backend.set_volume(math.floor(pct * 100 + 0.5)) then return end
    if self.volume_slider:setPercentage(pct) then
        self:refreshRegion(self.volume_slider, "fast")
    end
end

function KinAMPPlayer:onTap(arg, ges)
    -- Volume first: its column overlaps nothing else, and it is checked before
    -- the tap-outside-to-close rule below.
    local slider = self.volume_slider.dimen
    if slider and ges.pos:intersectWith(slider) then
        self:setVolumeAt(ges.pos.y)
        return true
    end

    -- Tapping the progress bar seeks, which is the whole reason the player
    -- publishes a position at all. Only when the bar is the widget currently in
    -- the layout: on radio it is swapped out, and its dimen would be a stale
    -- leftover from the last track that had one.
    local bar = self.progress_bar.dimen
    if bar and self.progress_group[1] == self.progress_bar and ges.pos:intersectWith(bar) then
        local status = self.last
        if status and not status.is_radio and status.dur and status.dur > 0 then
            local ratio = math.max(0, math.min(1, (ges.pos.x - bar.x) / bar.w))
            self:actAndRefresh(function() Backend.seek(ratio * status.dur) end)
        end
        return true
    end

    -- Anywhere outside the dialog closes it.
    if self.frame.dimen and ges.pos:notIntersectWith(self.frame.dimen) then
        self:onClose()
        return true
    end
    return true
end

function KinAMPPlayer:onShow()
    self:refresh(true)
    self:startPolling()
    UIManager:setDirty(self, function()
        return "ui", self.frame.dimen
    end)
    return true
end

function KinAMPPlayer:onClose()
    UIManager:close(self)
    return true
end

function KinAMPPlayer:onCloseWidget()
    self:stopPolling()
    UIManager:setDirty(nil, function()
        return "ui", self.frame.dimen
    end)
    self:free()
end

function KinAMPPlayer:onAnyKeyPressed()
    self:onClose()
    return true
end

return KinAMPPlayer
