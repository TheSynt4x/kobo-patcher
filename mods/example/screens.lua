-- What the example adds to Kobo's own screens: a key on Kobo's keyboard, an entry in My Books'
-- filter menu, and a section on Kobo's More page. Each is set up once from main.lua, which only
-- records it, and each does nothing until it's switched on here, so the mod changes Kobo's screens
-- only when asked. A switch takes effect straight away, with no restart.
local screens = {}

local function on(name)
  return nk.pref(name, "off") == "on"
end

function screens.start()
  -- A key left of the space bar, on every keyboard. An empty label leaves it off.
  nk.keyboard.key{
    label = function(language) return on("key") and "Ex" or "" end,
    tapped = function() nk.toast("The example's key", "nk.keyboard.key added it, left of the space bar.", 3000) end,
  }

  -- An entry after Kobo's own in My Books' filter menu, for books in English.
  nk.books.filters(function()
    if not on("filter") then return {} end
    return { { name = "English (the example's)", short_name = "English", languages = { "en", "en-US", "en-GB", "eng" } } }
  end)

  -- A section at the end of Kobo's More page each time Kobo builds it, with a row of the mod's own.
  nk.on_screen("MoreView", function(view)
    if not on("more") then return end
    view:add_section("FROM THE EXAMPLE MOD")
    view:add_button("nk.on_screen added this", "Hello", function()
      nk.toast("Hello", "This row is on Kobo's own More page.", 3000)
    end)
  end)

  -- Each change to Kobo's settings, shown on the This Kobo page.
  nk.device.on_setting(function(key, value) require("kobo").setting_changed(key, value) end)
end

function screens.build(page)
  page:section("THE KEYBOARD")
  page:toggle("A key of the example's, left of the space bar", on("key"), function(now)
    nk.set_pref("key", now and "on" or "off")
    nk.keyboard.reload()
  end)
  page:text("Tap a search box to see it. nk.keyboard.key adds it, and nk.keyboard.reload makes the keyboard again.")

  page:section("MY BOOKS")
  page:toggle("An entry in My Books' filter menu", on("filter"), function(now)
    nk.set_pref("filter", now and "on" or "off")
  end)
  page:text("nk.books.filters asks for its entries each time Kobo builds the menu, so it shows the next time it opens.")

  page:section("THE MORE PAGE")
  page:toggle("A section at the end of Kobo's More page", on("more"), function(now)
    nk.set_pref("more", now and "on" or "off")
    -- Kobo builds the More page once, so it's built again, with the rest of the main window, the
    -- way Kobo does after USB. That closes this screen too.
    nk.main_window.rebuild()
  end)
  page:text("nk.on_screen runs each time Kobo makes its More page, so switching this builds Kobo's main window again, and this screen closes.")
end

return screens
