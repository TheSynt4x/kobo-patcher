-- This Kobo: what it is, its books and settings, and the things Kobo does again on its own, like
-- importing books, building its main window, and restarting.
local kobo = {}

-- The last of Kobo's settings to change, from nk.device.on_setting in screens.lua.
local state = nk.keep { last_setting = nil }

function kobo.setting_changed(key, value)
  state.last_setting = key .. " = " .. tostring(value)
end

function kobo.build(page)
  page:section("THIS KOBO")
  page:value("nk.device.model", nk.device.model())
  page:value("nk.device.firmware", nk.device.firmware())

  page:section("THIS MOD")
  page:value("nk.name", nk.name)
  page:value("nk.version", tostring(nk.version))
  page:value("nk.build", nk.build)
  page:value("nk.self_sha512", (nk.self_sha512() or "?"):sub(1, 16) .. "...")
  page:button("nk.log: a line in Nickel's log", "Log", function()
    nk.log("the example says hello at", nk.time())
  end)

  page:section("KOBO'S SETTINGS")
  local timeout = nk.device.setting("PowerOptions/AutoOffMinutes")
  page:value("PowerOptions/AutoOffMinutes", timeout or "not set")
  page:text("nk.device.setting reads Kobo eReader.conf, the way Nickel does.")
  page:value("Changed last", state.last_setting or "nothing yet")
  page:text("nk.device.on_setting hears each change to a setting. Change one in Kobo's Settings, then come back.")

  page:section("BOOKS")
  local languages = nk.books.languages()
  page:value("nk.books.languages", #languages > 0 and table.concat(languages, ", ") or "none yet")
  page:button("nk.import: look for new books, as after USB", "Import", function()
    nk.import()
    nk.toast("Importing", "Kobo looks for new books, as it does after a USB connection.", 3000)
  end)

  page:section("BUILDING AGAIN")
  page:button("nk.main_window.rebuild: Home and the bottom bar", "Rebuild", function()
    if nk.confirm("Build Kobo's main window again?", "Kobo builds its bottom bar and Home again, as after USB, and this screen closes.", "Rebuild", "Cancel") then
      nk.main_window.rebuild()
    end
  end)
  page:button("nk.reboot: restart the Kobo", "Restart", function()
    if nk.confirm("Restart your Kobo?", "It restarts the way Kobo does, and comes back where it starts.", "Restart", "Cancel") then
      nk.reboot()
    end
  end)
end

return kobo
