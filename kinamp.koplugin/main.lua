-- KinAMP plugin entry point.
--
-- Everything (stations, playlist, transport, volume, stop, quit) is reachable
-- from the floating player, so the menu is a single entry that opens it, plus
-- the same thing as a Dispatcher action for a gesture.
--
-- The rest of this file is the one job the player can't do itself: noticing
-- that KOReader is going away, and taking an idle daemon with it.

local WidgetContainer = require("ui/widget/container/widgetcontainer")
local UIManager = require("ui/uimanager")
local Dispatcher = require("dispatcher")
local KinAMPPlayer = require("kinamp_player")
local Backend = require("kinamp_backend")
local logger = require("logger")
local _ = require("gettext")

local KinAMP = WidgetContainer:extend{
    name = "kinamp",
}

-- A Dispatcher action rather than a hardcoded gesture, so it can be bound to
-- whatever the user prefers in Gesture Manager.
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
end

function KinAMP:onShowKinAMPPlayer()
    -- Only close a player that is still on screen; closing one the user already
    -- dismissed would re-fire CloseWidget on a freed widget.
    if self.player and UIManager:isWidgetShown(self.player) then
        UIManager:close(self.player)
        self.player = nil
        return true -- the gesture acts as a toggle
    end
    self.player = KinAMPPlayer:new{}
    UIManager:show(self.player)
    return true
end

-- Leaving KOReader.
--
-- The player is a separate process on purpose: it outlives the plugin so that
-- playback survives whatever the reader is doing, and on the device it can be
-- picked up again from KUAL or the GTK player. So anything that is actually
-- playing is left alone - quitting KOReader isn't a request to stop the music.
--
-- A player with nothing to play does go. Stopped it costs no CPU (it sits in
-- poll() on the command FIFO with no timer armed), but it's still several MB of
-- resident GStreamer with nobody left to command it, against a couple of
-- seconds to cold-start the next one.
function KinAMP:quitIdlePlayer()
    local status = Backend.get_status()
    if not status then return end
    if status.is_playing then
        Backend.log("KOReader is exiting, leaving the player running")
        return
    end
    Backend.log("KOReader is exiting, quitting the idle player")
    Backend.quit()
end

-- Drops our lipc handle if the Bluetooth menu ever opened one. Only the handle:
-- the radio is the device's, not ours, and ensureBTconnection belongs to
-- whoever is playing. KinAMP-minimal raises it for its own lifetime and lowers
-- it on exit, and that's the process that outlives us here.
function KinAMP:closeBluetooth()
    pcall(function() require("kinamp_bt").close() end)
end

function KinAMP:leaving()
    self:quitIdlePlayer()
    self:closeBluetooth()
end

-- Exit from the menu, or the Dispatcher exit action. Broadcast before anything
-- is torn down, which is the tidiest moment to write to the FIFO.
function KinAMP:onExit()
    self:leaving()
end

-- Power off and reboot broadcast Close rather than Exit.
function KinAMP:onClose()
    self:leaving()
end

-- Catch-all, because not every way out announces itself: "Exit KOReader?" from
-- the back button in the file manager just closes the widget stack. Every route
-- does end in the host being torn down, so that's what we watch.
--
-- Switching between file manager and reader tears the plugin down too, and that
-- is not an exit. Only the switch sets tearing_down (see ReaderUI:onShowingReader
-- and FileManager:onShowingReader), which is how we tell the two apart.
function KinAMP:onCloseWidget()
    if self.ui and self.ui.tearing_down then return end
    self:leaving()
end

function KinAMP:addToMainMenu(menu_items)
    menu_items.kinamp = {
        text = _("KinAMP Player"),
        sorting_hint = "tools",
        callback = function() self:onShowKinAMPPlayer() end,
    }
end

return KinAMP
