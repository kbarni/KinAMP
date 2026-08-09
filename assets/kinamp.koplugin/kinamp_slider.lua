--[[--
A vertical slider, used for the player's volume.

KOReader has no vertical slider of its own, and the horizontal ones it does have
are built for settings dialogs: they come with their own labels, buttons and
refresh behaviour. This is the whole widget instead - a track and a knob that
draw themselves and nothing else.

It is deliberately passive: it holds a level and paints it. Hit testing and the
decision to actually change anything belong to the dialog that owns it, which is
the only place that knows whether a player is listening.
--]]

local Blitbuffer = require("ffi/blitbuffer")
local Device = require("device")
local Geom = require("ui/geometry")
local Widget = require("ui/widget/widget")
local Screen = Device.screen

local VerticalSlider = Widget:extend{
    -- Full width of the column, knob included; the track is centred in it.
    width = nil,
    height = nil,
    -- 0 at the bottom, 1 at the top.
    percentage = 1,
}

function VerticalSlider:init()
    self.knob_height = self.knob_height or Screen:scaleBySize(20)
    self.knob_width = self.knob_width or Screen:scaleBySize(16)
    self.track_width = self.track_width or Screen:scaleBySize(4)
    self.rail_width = self.rail_width or Screen:scaleBySize(1)
    self.border_size = self.border_size or Screen:scaleBySize(1)
    self.radius = self.radius or Screen:scaleBySize(3)
end

function VerticalSlider:getSize()
    return Geom:new{ w = self.width, h = self.height }
end

--- Distance the knob's top edge can travel.
function VerticalSlider:travel()
    return math.max(1, self.height - self.knob_height)
end

--- Level for a screen y, so a tap anywhere on the column lands where you aimed.
-- Rounded to whole percent: the player reports its volume as an integer, and a
-- knob sitting on a value it can never be told back would drift by a pixel on
-- the next poll.
function VerticalSlider:percentageAt(y)
    local top = (self.dimen and self.dimen.y or 0) + math.floor(self.knob_height / 2)
    local pct = 1 - (y - top) / self:travel()
    pct = math.max(0, math.min(1, pct))
    return math.floor(pct * 100 + 0.5) / 100
end

--- @return true if the knob moved, i.e. a repaint is worth it
function VerticalSlider:setPercentage(pct)
    pct = math.max(0, math.min(1, pct or 0))
    if self.percentage == pct then return false end
    self.percentage = pct
    return true
end

function VerticalSlider:paintTo(bb, x, y)
    self.dimen = Geom:new{ x = x, y = y, w = self.width, h = self.height }

    local knob_y = y + math.floor((1 - self.percentage) * self:travel())
    local middle = knob_y + math.floor(self.knob_height / 2)
    local centre_x = x + math.floor(self.width / 2)

    -- Below the knob the track is drawn solid and above it as a hairline, so the
    -- level reads without having to judge the knob's position against the ends.
    local rail_x = centre_x - math.floor(self.rail_width / 2)
    bb:paintRect(rail_x, y, self.rail_width, middle - y, Blitbuffer.COLOR_BLACK)
    local track_x = centre_x - math.floor(self.track_width / 2)
    bb:paintRect(track_x, middle, self.track_width, y + self.height - middle,
        Blitbuffer.COLOR_BLACK)

    -- The knob is filled rather than left transparent: it sits on top of the
    -- track, and an outline alone would have the line running through it.
    local knob_x = centre_x - math.floor(self.knob_width / 2)
    bb:paintRect(knob_x, knob_y, self.knob_width, self.knob_height,
        Blitbuffer.COLOR_WHITE)
    bb:paintBorder(knob_x, knob_y, self.knob_width, self.knob_height,
        self.border_size, Blitbuffer.COLOR_BLACK, self.radius)
end

return VerticalSlider
