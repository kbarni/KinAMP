--[[--
Bluetooth control through com.lab126.btfd.

The Kindle's Bluetooth daemon is reachable over lipc, and KOReader already
ships the binding that can talk to it: libopenlipclua, the same one
frontend/device/kindle/device.lua uses for the whole Wi-Fi stack. Its handles
read and write scalars *and* read hasharray properties, so a single
open_no_name() handle covers everything here - there is no service name to
register, and so nothing to collide with KOReader's own lipc clients or with a
second instance of ourselves. KinAMP-minimal opens its handle the same way
(LipcOpenNoName in cli_player.cpp).

Nothing in this file draws anything. Every operation that has to wait on the
radio takes a callback and polls on a UIManager timer: BTenable returns
immediately and the radio needs a second or two to follow, and a blocking wait
would stop KOReader's event loop dead.

btfd's properties are undocumented (see KOREADER-BT-ANALYSIS-AND-PLAN.md).
These are the semantics verified on-device:

  BTenable            "1:1" / "0:1"   turn the radio on / off
  BTstate             1 / 0           radio up / down
  ensureBTconnection  1 / 0           inhibit the 20 minute idle disconnect
  ListPaired          hasharray       paired devices
  ListConnected       hasharray       of those, whichever is connected now
  BTconnectedDevName  string          its name
  Connect / Disconnect  MAC           act on one device

The hash key names inside ListPaired are *not* known, and differ between
firmware versions as far as anyone can tell. They are therefore discovered at
read time rather than hardcoded - see normalise() - and the raw shape is logged
once per property so it can be read out of kinamp.log if the guesswork ever
misses.

On ownership of ensureBTconnection: KinAMP-minimal raises the flag for its own
lifetime whenever it plays something (cli_player.cpp), and it is not our place
to lower it under a daemon that is still playing. We only ever move the flag as
part of a radio transition the user asked for, where the radio is going away
anyway.
--]]

local UIManager = require("ui/uimanager")
local Backend = require("kinamp_backend")

local BTFD = "com.lab126.btfd"

-- Radio transitions: how often to look, and when to give up. Turning the radio
-- on takes a couple of seconds on a good day; the timeout is generous because
-- the alternative to waiting is telling the user it failed when it had not.
local STATE_POLL = 0.5
local STATE_TIMEOUT = 12

-- Connecting has to reach the device, so it gets longer and is looked at less
-- often - each check is a hasharray round trip.
local CONNECT_POLL = 1
local CONNECT_TIMEOUT = 15

local BT = {
    -- Set while a radio transition is in flight, so a second tap on the
    -- checkbox cannot start a competing sequence.
    busy = false,
}

--=============================================================================
-- The lipc handle
--=============================================================================

local handle
local handle_tried = false

--- The handle, opened on first use and held for the plugin's lifetime.
-- Held rather than opened per call because the device dialog refreshes, and
-- because this module stays loaded across the plugin teardown that a
-- file manager <-> reader switch causes.
-- @return handle, or nil off-device and on anything without libopenlipclua
local function get_handle()
    if handle then return handle end
    if handle_tried then return nil end
    handle_tried = true

    local ok, lipc = pcall(require, "libopenlipclua")
    if not ok or not lipc then
        Backend.log("BT: no libopenlipclua, Bluetooth control unavailable")
        return nil
    end

    local opened
    ok, opened = pcall(lipc.open_no_name)
    if not ok or not opened then
        Backend.log("BT: could not open a lipc handle: " .. tostring(opened))
        return nil
    end

    handle = opened
    return handle
end

--- Drops the handle. The properties we set are btfd's and outlive it.
function BT.close()
    if not handle then return end
    pcall(handle.close, handle)
    handle = nil
    -- Deliberately not clearing handle_tried: if opening worked once it will
    -- work again, and if it never did there is no point retrying on the way out.
end

--=============================================================================
-- Property access
--
-- The binding raises on a lipc error rather than returning a code, so every
-- call is wrapped: a property that btfd does not have, or does not have on this
-- firmware, must degrade to "unknown" and never take KOReader down from a
-- button callback or a poll timer.
--=============================================================================

local function get_int(prop)
    local h = get_handle()
    if not h then return nil end
    local ok, value = pcall(h.get_int_property, h, BTFD, prop)
    if not ok then
        Backend.log("BT: reading " .. prop .. " failed: " .. tostring(value))
        return nil
    end
    return value
end

local function get_str(prop)
    local h = get_handle()
    if not h then return nil end
    local ok, value = pcall(h.get_string_property, h, BTFD, prop)
    if not ok then
        Backend.log("BT: reading " .. prop .. " failed: " .. tostring(value))
        return nil
    end
    return value
end

local function set_int(prop, value)
    local h = get_handle()
    if not h then return false end
    local ok, err = pcall(h.set_int_property, h, BTFD, prop, value)
    if not ok then
        Backend.log(string.format("BT: %s = %s (int) failed: %s", prop, tostring(value), tostring(err)))
    end
    return ok
end

local function set_str(prop, value)
    local h = get_handle()
    if not h then return false end
    local ok, err = pcall(h.set_string_property, h, BTFD, prop, value)
    if not ok then
        Backend.log(string.format("BT: %s = %s (str) failed: %s", prop, tostring(value), tostring(err)))
    end
    return ok
end

--=============================================================================
-- Device lists
--=============================================================================

-- Anywhere inside a value, not anchored: some firmwares hand back
-- "AA:BB:CC:DD:EE:FF" on its own, others wrap it in something longer.
local MAC_PATTERN = "%x%x:%x%x:%x%x:%x%x:%x%x:%x%x"

-- Tried in order before falling back to scanning every value. These are
-- guesses; the fallback is what actually has to work.
local MAC_KEYS = { "mac", "address", "addr", "bdaddr", "bd_addr", "btaddr", "device" }
local NAME_KEYS = { "name", "devname", "dev_name", "device_name", "alias",
                    "friendly_name", "displayName", "title" }

--- Reads a hasharray property.
-- Both hasharrays are destroyed on every path, error paths included: they are
-- C allocations that nothing else will ever come back for.
-- @return list of key/value tables, or nil if the property could not be read
local function read_list(prop)
    local h = get_handle()
    if not h then return nil end

    local ok, ha_in = pcall(h.new_hasharray, h)
    if not ok or not ha_in then
        Backend.log("BT: could not allocate a hasharray for " .. prop)
        return nil
    end

    -- An empty hasharray goes in because we are only reading.
    local got, ha_out = pcall(h.access_hash_property, h, BTFD, prop, ha_in)
    if not got or not ha_out then
        Backend.log("BT: reading " .. prop .. " failed: " .. tostring(ha_out))
        pcall(ha_in.destroy, ha_in)
        return nil
    end

    local parsed, entries = pcall(ha_out.to_table, ha_out)
    -- Guarded against the binding handing back the very hasharray we passed in,
    -- which would make this a double free.
    if ha_out ~= ha_in then pcall(ha_out.destroy, ha_out) end
    pcall(ha_in.destroy, ha_in)

    if not parsed then
        Backend.log("BT: could not convert " .. prop .. ": " .. tostring(entries))
        return nil
    end
    return entries or {}
end

-- One log line per property per session: enough to read the real key names out
-- of kinamp.log, not enough to fill it.
local shape_logged = {}

local function log_shape(prop, entries)
    if shape_logged[prop] then return end
    shape_logged[prop] = true
    if #entries == 0 then
        Backend.log("BT: " .. prop .. " is empty")
        return
    end
    for i, entry in ipairs(entries) do
        local parts = {}
        for key, value in pairs(entry) do
            parts[#parts + 1] = tostring(key) .. "=" .. tostring(value)
        end
        table.sort(parts)
        Backend.log(string.format("BT: %s[%d] %s", prop, i, table.concat(parts, " ")))
    end
end

--- Pulls a MAC and a display name out of one hash, whatever it calls them.
local function normalise(entry)
    if type(entry) ~= "table" then return nil end

    local mac
    for _, key in ipairs(MAC_KEYS) do
        local value = entry[key]
        if type(value) == "string" then
            mac = value:match(MAC_PATTERN)
            if mac then break end
        end
    end
    if not mac then
        -- No key we recognised: take the first value that looks like a MAC.
        for _, value in pairs(entry) do
            if type(value) == "string" then
                mac = value:match(MAC_PATTERN)
                if mac then break end
            end
        end
    end
    if not mac then return nil end

    local name
    for _, key in ipairs(NAME_KEYS) do
        local value = entry[key]
        if type(value) == "string" and value ~= "" and not value:match(MAC_PATTERN) then
            name = value
            break
        end
    end
    if not name then
        -- Longest string that is not the address: a device name is the only
        -- free text in these records, and the only long one.
        for _, value in pairs(entry) do
            if type(value) == "string" and value ~= "" and not value:match(MAC_PATTERN) then
                if not name or #value > #name then name = value end
            end
        end
    end

    return { mac = mac, name = name or mac, raw = entry }
end

local function device_list(prop)
    local entries = read_list(prop)
    if not entries then return nil end
    log_shape(prop, entries)

    local devices = {}
    for _, entry in ipairs(entries) do
        local device = normalise(entry)
        if device then devices[#devices + 1] = device end
    end
    return devices
end

--- Paired devices, whether or not they are in range.
function BT.getPaired() return device_list("ListPaired") end

--- Whichever paired devices are connected right now (usually one, often none).
function BT.getConnected() return device_list("ListConnected") end

--- The connected device's name, or nil. Cheaper than getConnected() and works
-- even if the hash key guessing above ever misses.
function BT.getConnectedName()
    local name = get_str("BTconnectedDevName")
    if name == "" then return nil end
    return name
end

--- Set of connected MACs, lowercased, for marking up a list of paired devices.
function BT.getConnectedSet()
    local connected = {}
    for _, device in ipairs(BT.getConnected() or {}) do
        connected[device.mac:lower()] = true
    end
    return connected
end

--=============================================================================
-- Radio state
--=============================================================================

--- BTstate: 1 with the radio up, 0 with it down (verified on-device).
function BT.getState() return get_int("BTstate") end

--- @return true/false, or nil when btfd will not say
function BT.isOn()
    local state = BT.getState()
    if state == nil then return nil end
    return state ~= 0
end

--- True when there is a btfd on this device that answers us.
-- False on the desktop, and on anything whose Bluetooth is not btfd's (the
-- 11th generation devices drive libace_bt instead) - which is what keeps the
-- Bluetooth entries out of the menu there rather than showing dead ones.
function BT.available()
    if not get_handle() then return false end
    return BT.getState() ~= nil
end

--- The 20 minute idle disconnect inhibitor.
-- Documented as a string property but the working stock sequence sets it with
-- lipc-set-prop -i, and so does the GTK player; int first, string as a fallback
-- in case a firmware ever disagrees.
function BT.setEnsure(on)
    local value = on and 1 or 0
    if set_int("ensureBTconnection", value) then return true end
    return set_str("ensureBTconnection", tostring(value))
end

--- The radio itself. The second field of the value is undocumented; the stock
-- interface always sends 1 and so do we.
function BT.setRadio(on)
    return set_str("BTenable", on and "1:1" or "0:1")
end

--- Polls BTstate until it reaches `target`, or gives up.
local function await_state(target, done_cb)
    local deadline = os.time() + STATE_TIMEOUT
    local function poll()
        if BT.getState() == target then
            done_cb(true)
        elseif os.time() >= deadline then
            Backend.log("BT: radio did not reach state " .. tostring(target))
            done_cb(false, "timeout")
        else
            UIManager:scheduleIn(STATE_POLL, poll)
        end
    end
    UIManager:scheduleIn(STATE_POLL, poll)
end

--- Turns the radio on or off, keepalive flag and all.
--
-- The order is the stock interface's, which is the one known to take: the flag
-- is written while the radio is *down* (BTenable 0:1 -> ensureBTconnection 1 ->
-- BTenable 1:1). Whether btfd latches it at radio-init or reads it live is not
-- known, so we do what is known to work. Turning the radio on is also the one
-- moment the flag can be raised for free: the radio is coming up anyway, so
-- there is no cycle and nothing audible - unlike the stock sequence, we never
-- take a working radio down just to set a flag.
--
-- @param on true to bring the radio up
-- @param done_cb called with (true) or (false, reason)
function BT.setEnabled(on, done_cb)
    done_cb = done_cb or function() end

    if not get_handle() then return done_cb(false, "unavailable") end
    if BT.busy then return done_cb(false, "busy") end

    local target = on and 1 or 0
    local state = BT.getState()
    if state == nil then return done_cb(false, "unavailable") end
    if state == target then
        -- Already there. Still worth making sure the flag agrees with the
        -- radio, which is cheap and fixes a flag stranded by a killed player.
        BT.setEnsure(on)
        return done_cb(true)
    end

    BT.busy = true
    BT.setEnsure(on)
    if not BT.setRadio(on) then
        BT.busy = false
        return done_cb(false, "write failed")
    end

    await_state(target, function(ok, err)
        BT.busy = false
        if ok and on then
            -- btfd reconnects the last device by itself most of the time; this
            -- is for when it does not, and costs nothing when it already has.
            local last = BT.getLastDevice()
            if last and last.mac and not BT.getConnectedName() then
                BT.connect(last.mac)
            end
        end
        done_cb(ok, err)
    end)
end

--=============================================================================
-- Connecting
--=============================================================================

--- Asks btfd to connect a paired device.
--
-- The write succeeds whether or not the device is switched on and in range -
-- it is a request, not a result - so the outcome has to be read back off
-- ListConnected. Without a callback this is fire and forget, which is what the
-- reconnect in setEnabled() wants.
-- @param done_cb called with (true, name) or (false, reason)
function BT.connect(mac, done_cb)
    if not mac then
        if done_cb then done_cb(false, "no address") end
        return
    end
    if not set_str("Connect", mac) then
        if done_cb then done_cb(false, "write failed") end
        return
    end
    if not done_cb then return end

    local wanted = mac:lower()
    local deadline = os.time() + CONNECT_TIMEOUT
    local function poll()
        if BT.getConnectedSet()[wanted] then
            done_cb(true, BT.getConnectedName())
        elseif os.time() >= deadline then
            Backend.log("BT: " .. mac .. " did not connect")
            done_cb(false, "timeout")
        else
            UIManager:scheduleIn(CONNECT_POLL, poll)
        end
    end
    UIManager:scheduleIn(CONNECT_POLL, poll)
end

--- Drops a connected device. Disconnecting is prompt, so this waits far less
-- patiently than connecting does.
function BT.disconnect(mac, done_cb)
    if not mac then
        if done_cb then done_cb(false, "no address") end
        return
    end
    if not set_str("Disconnect", mac) then
        if done_cb then done_cb(false, "write failed") end
        return
    end
    if not done_cb then return end

    local wanted = mac:lower()
    local deadline = os.time() + STATE_TIMEOUT
    local function poll()
        if not BT.getConnectedSet()[wanted] then
            done_cb(true)
        elseif os.time() >= deadline then
            done_cb(false, "timeout")
        else
            UIManager:scheduleIn(STATE_POLL, poll)
        end
    end
    UIManager:scheduleIn(STATE_POLL, poll)
end

--=============================================================================
-- The last device we were on
--
-- Kept in KOReader's settings rather than in .kinamp.conf: that file is shared
-- with the two C++ players, which parse it as integer keys only, and a MAC is
-- neither integer nor any of their business.
--=============================================================================

function BT.getLastDevice()
    if not G_reader_settings then return nil end
    local mac = G_reader_settings:readSetting("kinamp_bt_last_mac")
    if not mac then return nil end
    return { mac = mac, name = G_reader_settings:readSetting("kinamp_bt_last_name") or mac }
end

function BT.setLastDevice(mac, name)
    if not G_reader_settings or not mac then return end
    G_reader_settings:saveSetting("kinamp_bt_last_mac", mac)
    G_reader_settings:saveSetting("kinamp_bt_last_name", name)
end

return BT
