-- The Bluetooth device list.
--
-- The stock interface has a pairing wizard, but it's drawn by pillow, which
-- KOReader disables outright while it's in the foreground (koreader.sh sets
-- disableEnablePillow), so none of it is reachable from here. This is the
-- replacement, and a smaller thing than the original: it lists the devices btfd
-- already knows about and connects to one of them.
--
-- Pairing is out of scope. It needs confirmation dialogs that only pillow
-- provides, so a device has to be paired once from the Kindle's own settings;
-- after that it lives in ListPaired forever and this is enough to use it.
--
-- The list follows the station list: first tap points the buttons at a device,
-- a second tap on the same one acts, holding it skips the pointing.

local InfoMessage = require("ui/widget/infomessage")
local UIManager = require("ui/uimanager")
local ButtonMenu = require("kinamp_menu")
local BT = require("kinamp_bt")
local ffiUtil = require("ffi/util")
local _ = require("gettext")
local T = ffiUtil.template

local CONNECTED_MARK = "\u{25B6} "  -- the station list marks what is on the air the same way
local SELECTED_MARK = "\u{2022} "

local function notify(text, timeout)
    UIManager:show(InfoMessage:new{ text = text, timeout = timeout or 2 })
end

-- Puts a message on screen for the length of an async operation and returns a
-- function that takes it back down. Radio transitions and connections take
-- seconds with nothing else moving on screen, so something has to say what is
-- being waited for.
local function progress(text)
    local info = InfoMessage:new{ text = text }
    UIManager:show(info)
    -- Only worth showing if it's painted before the wait, not after it.
    UIManager:forceRePaint()
    return function() UIManager:close(info) end
end

-- Turns the radio on or off, with the waiting and the reporting. Exported
-- because the player's Bluetooth checkbox is the same operation and the outcome
-- should read the same from either place.
local function toggleRadio(on, done_cb)
    local dismiss = progress(on and _("Turning Bluetooth on…")
                                or _("Turning Bluetooth off…"))
    BT.setEnabled(on, function(ok, err)
        dismiss()
        if ok then
            local name = on and BT.getConnectedName()
            if name then
                notify(T(_("Bluetooth on, connected to %1"), name))
            else
                notify(on and _("Bluetooth on") or _("Bluetooth off"))
            end
        elseif err == "busy" then
            notify(_("Bluetooth is still changing state."), 3)
        elseif err == "unavailable" then
            notify(_("Bluetooth is not available on this device."), 3)
        else
            notify(on and _("Could not turn Bluetooth on.")
                      or _("Could not turn Bluetooth off."), 3)
        end
        if done_cb then done_cb(ok) end
    end)
end

local BTDevices = ButtonMenu:extend{
    title = _("Bluetooth"),
    -- Which device the bottom buttons act on, if any.
    selected_idx = nil,
}

function BTDevices:init()
    self:load()
    ButtonMenu.init(self)
end

function BTDevices:load()
    self.radio_on = BT.isOn()
    -- Reading the paired list with the radio down isn't known to work on every
    -- firmware, so an empty answer there is not an error.
    self.devices = BT.getPaired() or {}
    self.connected = BT.getConnectedSet()
    self.connected_name = BT.getConnectedName()
    if self.selected_idx and self.selected_idx > #self.devices then
        self.selected_idx = nil
    end
end

-- Reloads and repaints, unless the list has been closed underneath us: the
-- connect and radio callbacks land seconds later, by which time the user may
-- well have dismissed it, and updating a freed widget would take KOReader down.
function BTDevices:reload(keep_idx)
    if not UIManager:isWidgetShown(self) then return end
    self:load()
    self:updateList(keep_idx or self.selected_idx)
end

function BTDevices:genTitle()
    if self.radio_on == nil then return _("Bluetooth") end
    if not self.radio_on then return _("Bluetooth is off") end
    if self.connected_name then return T(_("Connected: %1"), self.connected_name) end
    return T(_("Bluetooth devices (%1)"), #self.devices)
end

function BTDevices:isConnected(device)
    return device ~= nil and self.connected[device.mac:lower()] == true
end

function BTDevices:genItemTable()
    local items = {}
    for idx, device in ipairs(self.devices) do
        local mark = ""
        if self:isConnected(device) then
            mark = CONNECTED_MARK
        elseif idx == self.selected_idx then
            mark = SELECTED_MARK
        end
        items[#items + 1] = {
            -- The address is what tells two identically named earbuds apart,
            -- and the only thing to go on when a name comes back empty.
            text = mark .. device.name,
            mandatory = device.mac,
            device = device,
            idx = idx,
        }
    end
    -- Menu draws this one in bold, which is what carries the selection when the
    -- device is also the connected one and already wears the connected mark.
    items.current = self.selected_idx

    if #items == 0 then
        local text
        if self.radio_on == nil then
            text = _("Bluetooth is not available on this device")
        elseif not self.radio_on then
            text = _("Bluetooth is off - turn it on below")
        else
            text = _("No paired devices - pair one in the Kindle's settings")
        end
        items[1] = { text = text, select_enabled = false }
    end
    return items
end

-- Points the bottom buttons at a device, or at nothing when idx is nil.
function BTDevices:select(idx)
    if idx and (idx < 1 or idx > #self.devices) then idx = nil end
    self.selected_idx = idx
    self:updateList(idx)
end

function BTDevices:connectDevice(idx)
    local device = self.devices[idx]
    if not device then return end

    local dismiss = progress(T(_("Connecting to %1…"), device.name))
    BT.connect(device.mac, function(ok, name)
        dismiss()
        if ok then
            -- Remembered so turning the radio back on can go straight to it.
            BT.setLastDevice(device.mac, device.name)
            notify(T(_("Connected to %1"), name or device.name))
        else
            -- The write always succeeds; only the connection can fail, and a
            -- paired device that is switched off or out of range is by far the
            -- likeliest reason.
            notify(T(_("Could not connect to %1.\nIs it switched on and in range?"),
                     device.name), 3)
        end
        self:reload(idx)
    end)
end

function BTDevices:disconnectDevice(idx)
    local device = self.devices[idx]
    if not device then return end

    local dismiss = progress(T(_("Disconnecting %1…"), device.name))
    BT.disconnect(device.mac, function(ok)
        dismiss()
        if not ok then
            notify(T(_("Could not disconnect %1."), device.name), 3)
        end
        self:reload(idx)
    end)
end

function BTDevices:toggleDevice(idx)
    local device = self.devices[idx]
    if not device then return end
    if self:isConnected(device) then
        self:disconnectDevice(idx)
    else
        self:connectDevice(idx)
    end
end

-- First tap selects, a second one on the same device connects it. KOReader has
-- no double-tap on menu items and this is as close as it gets.
function BTDevices:onMenuSelect(item)
    if not item.device then return true end
    if self.selected_idx == item.idx then
        self:toggleDevice(item.idx)
    else
        self:select(item.idx)
    end
    return true
end

function BTDevices:onMenuHold(item)
    if item.device then self:toggleDevice(item.idx) end
    return true
end

function BTDevices:genButtons()
    local function selected() return self.devices[self.selected_idx] end

    -- Connect and Disconnect are two buttons rather than one that changes its
    -- label: Button only reads text_func when it's built, while enabled_func is
    -- re-checked on every paint, so this is the pair that keeps up with the list.
    return {
        {
            {
                text = _("Connect"),
                enabled_func = function()
                    local device = selected()
                    return device ~= nil and not self:isConnected(device)
                end,
                callback = function() self:connectDevice(self.selected_idx) end,
            },
            {
                text = _("Disconnect"),
                enabled_func = function() return self:isConnected(selected()) end,
                callback = function() self:disconnectDevice(self.selected_idx) end,
            },
            {
                text = _("Refresh"),
                callback = function() self:reload() end,
            },
        },
        {
            {
                text = _("Turn Bluetooth on"),
                enabled_func = function() return self.radio_on == false end,
                callback = function() self:setRadio(true) end,
            },
            {
                text = _("Turn Bluetooth off"),
                enabled_func = function() return self.radio_on == true end,
                callback = function() self:setRadio(false) end,
            },
        },
    }
end

function BTDevices:setRadio(on)
    toggleRadio(on, function()
        -- The paired list is only readable with the radio up, so this is also
        -- what fills the list in after turning it on.
        self:reload()
    end)
end

-- opts.on_close is called once the list is dismissed.
function BTDevices.open(opts)
    opts = opts or {}
    local menu
    menu = BTDevices:new{
        close_callback = opts.on_close,
    }
    UIManager:show(menu)
    return menu
end

-- The radio toggle on its own, for the player's Bluetooth checkbox.
BTDevices.toggleRadio = toggleRadio

return BTDevices
