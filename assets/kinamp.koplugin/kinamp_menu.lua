--[[--
A Menu with a row of buttons along the bottom.

KOReader's Menu has no footer beyond its page counter, so the buttons are a
ButtonTable stacked underneath the menu's own content inside the same frame:
the list is built one button table shorter, and the window as a whole comes to
the height that was asked for. That keeps one border, one radius and one
tap-outside-to-close region around the two.

Subclasses provide `genItemTable`, `genTitle` and `genButtons`, load whatever
they are listing, and then call `ButtonMenu.init(self)`.
--]]

local ButtonTable = require("ui/widget/buttontable")
local Device = require("device")
local Menu = require("ui/widget/menu")
local Size = require("ui/size")
local VerticalGroup = require("ui/widget/verticalgroup")
local Screen = Device.screen

local ButtonMenu = Menu:extend{
    is_popout = true,
    is_borderless = false,
}

function ButtonMenu:init()
    self.width = self.width or math.floor(Screen:getWidth() * 0.9)
    -- The height asked for is for the whole window; Menu only gets what the
    -- button table leaves it, and self.dimen is put back together afterwards.
    local total_height = self.height or math.floor(Screen:getHeight() * 0.9)
    local border_size = self.is_borderless and 0 or Size.border.window

    self.button_table = ButtonTable:new{
        width = self.width - 2 * border_size,
        buttons = self:genButtons(),
        -- Draws a line above the first row, which is what separates the
        -- buttons from the page counter sitting right on top of them.
        zero_sep = true,
        show_parent = self,
    }
    self.height = total_height - self.button_table:getSize().h

    self.item_table = self:genItemTable()
    self.title = self:genTitle()
    Menu.init(self)

    -- Menu leaves us FrameContainer{ OverlapGroup }, the overlap group being
    -- the list with its title bar and page counter. Stack the buttons under it
    -- inside that same frame.
    local frame = self[1]
    frame[1] = VerticalGroup:new{
        align = "left",
        frame[1],
        self.button_table,
    }

    -- Menu sized self.dimen for the list alone, and it is what the swipe and
    -- pan ranges and the tap-outside-to-close test all read. Mutated in place
    -- because those gesture ranges hold a reference to this very table.
    self.dimen.h = total_height
    self.height = total_height
end

--- Rebuilds the list after an edit.
-- @param keep_idx item to stay on, so the page does not jump back to 1
function ButtonMenu:updateList(keep_idx)
    if keep_idx and keep_idx < 1 then keep_idx = nil end
    self.item_table = self:genItemTable()
    self:switchItemTable(self:genTitle(), self.item_table, keep_idx)
end

--- Menu rebuilds its focus layout from the visible items on every redraw, so
-- the buttons have to be put back each time for key navigation to reach them.
function ButtonMenu:updateItems(select_number, no_recalculate_dimen)
    Menu.updateItems(self, select_number, no_recalculate_dimen)
    if self.button_table then
        for _i, row in ipairs(self.button_table.buttons_layout) do
            self.layout[#self.layout + 1] = row
        end
    end
end

return ButtonMenu
