-- A Menu with a row of buttons along the bottom.
--
-- KOReader's Menu has no footer beyond the page counter, so the buttons go in
-- as a ButtonTable stacked under the menu content inside the same frame: build
-- the list one button table shorter, and the window ends up the height that was
-- asked for. One border, one radius, one tap-outside-to-close region for both.
--
-- Subclasses provide genItemTable, genTitle and genButtons, load whatever they
-- are listing, then call ButtonMenu.init(self).

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
    -- self.height is the whole window; Menu only gets what the button table
    -- leaves it. self.dimen is put back together at the end.
    local total_height = self.height or math.floor(Screen:getHeight() * 0.9)
    local border_size = self.is_borderless and 0 or Size.border.window

    self.button_table = ButtonTable:new{
        width = self.width - 2 * border_size,
        buttons = self:genButtons(),
        -- Line above the first row, separating the buttons from the page
        -- counter sitting right on top of them.
        zero_sep = true,
        show_parent = self,
    }
    self.height = total_height - self.button_table:getSize().h

    self.item_table = self:genItemTable()
    self.title = self:genTitle()
    Menu.init(self)

    -- Menu leaves us FrameContainer{ OverlapGroup }, the overlap group being the
    -- list with its title bar and page counter. Stack the buttons under it
    -- inside that same frame.
    local frame = self[1]
    frame[1] = VerticalGroup:new{
        align = "left",
        frame[1],
        self.button_table,
    }

    -- Menu sized self.dimen for the list alone, and that's what the swipe/pan
    -- ranges and the tap-outside test read. Mutate in place: those gesture
    -- ranges hold a reference to this very table.
    self.dimen.h = total_height
    self.height = total_height
end

-- keep_idx is the item to stay on, so the page doesn't jump back to 1.
function ButtonMenu:updateList(keep_idx)
    if keep_idx and keep_idx < 1 then keep_idx = nil end
    self.item_table = self:genItemTable()
    self:switchItemTable(self:genTitle(), self.item_table, keep_idx)
end

-- Menu rebuilds its focus layout from the visible items on every redraw, so the
-- buttons have to be appended again each time or key navigation can't reach them.
function ButtonMenu:updateItems(select_number, no_recalculate_dimen)
    Menu.updateItems(self, select_number, no_recalculate_dimen)
    if self.button_table then
        for _i, row in ipairs(self.button_table.buttons_layout) do
            self.layout[#self.layout + 1] = row
        end
    end
end

return ButtonMenu
