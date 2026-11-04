-- The example mod: a row at the top of Kobo's Settings, opening a screen made of Kobo's own rows.
-- main.lua says where the mod goes. Its screens and logic are in the other files.
local count = require "count"

-- Kept when the scripts are reloaded during development.
local state = nk.keep { taps = 0 }

nk.settings("Example", function(page)
  page:section("FROM LUA")
  page:text("This screen is built by main.lua in the example mod. Tap the rows to try what a mod can do.")
  page:value("Taps so far", count.describe(state.taps))
  page:button("Count a tap", "Tap", function()
    state.taps = state.taps + 1
    page:refresh()
  end)

  page:section("SETTINGS")
  page:toggle("Remember a choice", nk.pref("choice", "off") == "on", function(on)
    nk.set_pref("choice", on and "on" or "off")
  end)

  page:section("WAITING")
  local status
  status = page:button("Nothing yet", "Clear", function() status:set("Nothing yet") end)
  page:button("Wait three seconds", "Start", function()
    for left = 3, 1, -1 do
      status:set(left .. "...")
      nk.sleep(1000)
    end
    status:set("Done waiting")
  end)
  page:button("Ask a question", "Ask", function()
    local yes = nk.confirm("A question", "Did this dialog come from Lua?", "Yes", "No")
    status:set(yes and "You said yes" or "You said no")
  end)
  page:button("Fail on purpose", "Fail", function()
    error("an error on purpose, to show the dialog")
  end)
end)

nk.log("example started")
