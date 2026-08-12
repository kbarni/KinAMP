--[[--
KinAMP plugin entry point.

The floating player is the whole interface - stations, playlist, transport,
volume, stop and quit are all reachable from it - so the menu is one entry that
opens it, and the same thing is registered as a Dispatcher action for a gesture.

What is left here is the part the player cannot do: noticing that KOReader is
going away, and taking an idle daemon with it.
--]]

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

--=============================================================================
-- Leaving KOReader
--
-- The player is a separate process on purpose: it outlives the plugin so that
-- playback survives whatever the reader is doing, and on the device it can be
-- picked up again from KUAL or the GTK player. So anything actually playing is
-- left alone here - quitting KOReader is not a request to stop the music.
--
-- What does go is a player with nothing to play. Stopped, it costs no CPU at
-- all (it sits in poll() on the command FIFO with no timer armed), but it is
-- still several megabytes of resident GStreamer with nobody left to send it a
-- command, and cold-starting the next one costs a couple of seconds once.
--=============================================================================

function KinAMP:quitIdlePlayer()
    local status = Backend.get_status()
    if not status then return end -- nothing running
    if status.is_playing then
        Backend.log("KOReader is exiting, leaving the player running")
        return
    end
    Backend.log("KOReader is exiting, quitting the idle player")
    Backend.quit()
end

--- Drops our lipc handle, if the Bluetooth menu ever opened one.
--
-- Only the handle: the Bluetooth state itself is deliberately left as it is.
-- The radio is the device's, not ours, and ensureBTconnection belongs to
-- whoever is playing - KinAMP-minimal raises it for its own lifetime and lowers
-- it when it exits, which is exactly the process that outlives us here.
function KinAMP:closeBluetooth()
    pcall(function() require("kinamp_bt").close() end)
end

--- Everything that has to happen on the way out, whichever way that is.
function KinAMP:leaving()
    self:quitIdlePlayer()
    self:closeBluetooth()
end

--- Exit from the menu, or the Dispatcher exit action.
-- Broadcast before anything is torn down, which is the tidiest moment to write
-- to the FIFO.
function KinAMP:onExit()
    self:leaving()
end

--- Power off and reboot: those broadcast Close rather than Exit.
function KinAMP:onClose()
    self:leaving()
end

--- The catch-all, because not every way out of KOReader announces itself:
-- "Exit KOReader?" from the back button in the file manager just closes the
-- widget stack and broadcasts nothing. Every route does end in the host being
-- torn down, so that is what we watch.
--
-- Switching between the file manager and the reader tears the plugin down too,
-- and that is not an exit. Only the switch sets tearing_down (see
-- ReaderUI:onShowingReader and FileManager:onShowingReader), so it is what
-- tells the two apart.
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
