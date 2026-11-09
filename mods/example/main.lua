-- The example mod: a showcase of what a Lua mod can do, to read beside LUA.md and copy from.
--
-- main.lua runs while Qt is still loading the mod, before Kobo has built anything, so it only says
-- where the mod goes: a row on the More page, a tab, and what it adds to Kobo's own screens. Each
-- of those calls only records what it's asked for. Everything else runs later, in callbacks, from
-- the other files:
--
--   blocks.lua    every kind of block a page can hold
--   waiting.lua   waiting, timers and dialogs
--   tools.lua     JSON, patterns, text, hashes, files and the network
--   kobo.lua      this Kobo, its books and settings, and restarting
--   screens.lua   what the mod adds to Kobo's own screens, each with its switch
--   tab.lua       the tab in the bottom bar
--
-- What it leaves out, because it would change Kobo for everything, not just for the example:
-- nk.keyboard.choose, nk.books.language, nk.dictionaries.installed_first, nk.device.set_setting,
-- nk.spawn and nk.patch. Kobo Patcher (../patches) uses them. nk.settings, a row in Kobo's own
-- Settings, works too, but a mod's screens go on the More page.
local count = require "count"
local screens = require "screens"

-- Kept when the scripts are reloaded during development, as is every nk.keep table.
local state = nk.keep { taps = 0 }

-- Updates, when .adds/example/settings.ini says where from: update_url=... and update_key=...,
-- the public half of the key tools/sign_mod.mjs signs with. A mod of your own writes its address
-- and key here instead. The version is the one in the version file.
local update_url, update_key = nk.pref("update_url"), nk.pref("update_key")
if update_url and update_key then
  nk.updates{url = update_url, key = update_key, name = "Example"}
end

-- The first page: one row for each part of the showcase, each opening a page over this one.
local function build(page)
  page:heading("What a mod can do", "example", function()
    nk.toast("The example mod", "A heading's icon can be tapped, like Kobo's own.", 3000)
  end)
  page:text("Each row opens a page built by this mod, from Kobo's own widgets. The scripts are in the kit's example folder.")
  page:value("Taps so far", count.describe(state.taps))
  page:button("Count a tap", "Tap", function()
    state.taps = state.taps + 1
    page:refresh()
  end)

  page:section("THE SHOWCASE")
  page:button("Every kind of row", "Open", function() page:open(require("blocks").build, "Rows") end)
  page:button("Waiting and dialogs", "Open", function() page:open(require("waiting").build, "Waiting") end)
  page:button("Tools and the network", "Open", function() page:open(require("tools").build, "Tools") end)
  page:button("This Kobo", "Open", function() page:open(require("kobo").build, "This Kobo") end)
  page:button("Kobo's own screens", "Open", function() page:open(screens.build, "Kobo's screens") end)

  page:section("UPDATES")
  if update_url and update_key then
    nk.updates.add_to(page)
  else
    page:text("Put update_url= and update_key= in .adds/example/settings.ini, and Check looks there for a newer example signed with that key.")
  end
end

-- A row on Kobo's More page, with icons/example.png.
nk.more{name = "Example", icon = "example", open = function()
  nk.screen{title = "Example", small_title = "More", build = build}
end}

-- A tab in the bottom bar, before More: icons/tab.png, and icons/tab-active.png while it's open.
nk.tab{name = "Example", icon = "tab", build = function(page) require("tab").build(page) end}

-- Additions to Kobo's own screens: a key on its keyboard, an entry in My Books' filter menu, and a
-- section on the More page. Each is off until it's switched on in "Kobo's own screens".
screens.start()

nk.log("example started, version " .. tostring(nk.version) .. ", " .. nk.build)
