-- "Kobo Patcher" in Settings: every patch for this firmware, switched on and off straight away in
-- Nickel's memory. The firmware's files aren't changed. The patch list is a file in .adds, so a new
-- one arrives without reinstalling the mod, and it's read again whenever it changes. Choices are
-- saved on the Kobo and made again each time Nickel starts, before it has built anything.
--
-- And "KOReader" on the More page, when KOReader is in .adds, and the languages in languages.lua.
local list = require "list"
local update = require "update"
local languages = require "languages"

local CHOICES = "choices.txt"
local KOREADER = "/mnt/onboard/.adds/koreader/koreader.sh"

local model, version = nk.device.model(), nk.device.firmware()
if version == "" then version = "unknown" end
local ready = nk.patch.ready()

-- Kept when main.lua runs again in development, since the bytes in memory stay as they are.
local state = nk.keep({choices = {}, order = {}})

-- ---- saved choices: an "id on" or "id off" line for each patch switched here ----

-- Kept by id, apart from the list, so a new list that drops or reorders patches loses nothing.
local function read_choices()
  local text = nk.file.read(CHOICES) or ""
  state.choices_text, state.choices, state.order = text, {}, {}
  for id, value in text:gmatch("(%S+)%s+(%S+)") do
    if state.choices[id] == nil then state.order[#state.order + 1] = id end
    state.choices[id] = value == "on"
  end
end

local function choose(patch, on)
  if state.choices[patch.id] == nil then state.order[#state.order + 1] = patch.id end
  state.choices[patch.id] = on
end

local function save_choices()
  local lines = {}
  for _, id in ipairs(state.order) do lines[#lines + 1] = id .. (state.choices[id] and " on\n" or " off\n") end
  local text = table.concat(lines)
  if nk.file.write(CHOICES, text) then state.choices_text = text else nk.log("couldn't save choices") end
end

-- The page can change the choices over USB. They're read again, and made, when the file isn't what
-- this mod last read or wrote.
local function choices_changed()
  return (nk.file.read(CHOICES) or "") ~= state.choices_text
end

-- Patches turned off go first, because patches in a group can share bytes. A patch to be off whose
-- bytes read as changed is left alone: they belong to something else, often a patch in its group
-- that's on.
local function apply_choices()
  for _, on in ipairs({false, true}) do
    for _, patch in ipairs(state.firmware.patches) do
      local now = nk.patch.state(patch.edits)
      if state.choices[patch.id] == on and now ~= (on and "on" or "off") and (on or now ~= "changed") then
        local ok = nk.patch.set(patch.edits, on, patch)
        nk.log(patch.id .. ": " .. (not ok and "bytes aren't as expected, not changing it" or on and "on" or "off"))
      end
    end
  end
end

-- ---- the patch list ----

local function list_path() return ("lists/%s-%s.txt"):format(model, version) end

-- Reads the list again if the file has changed since it was last read, and makes the saved
-- choices for any patches that are new to it. A list that can't be read leaves the last one.
-- Packs' lists, which the page puts in lists/custom as <pack>-<model>-<firmware>.txt: {pack, text}
-- for each one for this firmware, by name.
local CUSTOM = "lists/custom"

local function pack_lists()
  local suffix, found = ("-%s-%s.txt"):format(model, version), {}
  for _, name in ipairs(nk.file.list(CUSTOM)) do
    if #name > #suffix and name:sub(-#suffix) == suffix then
      local text = nk.file.read(CUSTOM .. "/" .. name)
      if text then found[#found + 1] = {pack = name:sub(1, -#suffix - 1), text = text} end
    end
  end
  table.sort(found, function(a, b) return a.pack < b.pack end)
  return found
end

local function for_here(loaded, why)
  if loaded and (loaded.model ~= model or loaded.version ~= version) then
    return nil, "the patch list is for another Kobo or firmware"
  end
  return loaded, why
end

-- A pack's patches go after Kobo Patcher's own. Each has to be named <pack>.something, so a pack
-- can't stand in for another's patches or Kobo Patcher's. A pack's list that can't be read is left
-- out, and the rest still work.
local function add_packs(loaded, packs)
  local ids = {}
  for _, patch in ipairs(loaded.patches) do ids[patch.id] = true end
  for _, each in ipairs(packs) do
    local extra, why = for_here(list.parse(each.text))
    if not extra then
      nk.log(("%s's patches not read: %s"):format(each.pack, why))
    else
      for _, patch in ipairs(extra.patches) do
        if patch.id:sub(1, #each.pack + 1) ~= each.pack .. "." or ids[patch.id] then
          nk.log(("%s's patch %s left out: it isn't named %s.something, or is there twice"):format(each.pack, patch.id, each.pack))
        else
          ids[patch.id] = true
          patch.all = false
          loaded.patches[#loaded.patches + 1] = patch
        end
      end
    end
  end
end

local function load_list()
  local text = nk.file.read(list_path())
  if not text then
    if not state.firmware then state.list_error = "there's no patch list for this firmware" end
    return
  end
  local packs, all = pack_lists(), text
  for _, each in ipairs(packs) do all = all .. "\0" .. each.pack .. "\0" .. each.text end
  if state.firmware and all == state.list_text then return end
  state.list_text = all
  local loaded, why = for_here(list.parse(text))
  if not loaded then
    state.list_error = why
    return nk.log("patch list not read: " .. why)
  end
  add_packs(loaded, packs)
  state.firmware, state.list_error = loaded, nil
  nk.log(("read %d patches for %s %s"):format(#loaded.patches, model, version))
  if ready then apply_choices() end
end

-- ---- the screen ----

local function same_group(a, b) return a ~= b and a.group ~= nil and a.group == b.group end

-- "on", "off", "partly" or "changed" for each patch. One patch in a group being on can make the
-- others read as changed, when really they're off.
local function read_states(patches)
  local rows = {}
  for i, patch in ipairs(patches) do rows[i] = {state = nk.patch.state(patch.edits)} end
  for i in ipairs(patches) do
    for k in ipairs(patches) do
      local other = rows[k].state
      if rows[i].state == "changed" and (other == "on" or other == "partly") and same_group(patches[i], patches[k]) then
        rows[i].state = "off"
      end
    end
  end
  return rows
end

local quiet = 0

-- Ticks or unticks without that counting as a tap.
local function set_quietly(toggle, on)
  quiet = quiet + 1
  pcall(toggle.set, toggle, on) -- the screen may have closed
  quiet = quiet - 1
end

-- A patch's settings in Kobo eReader.conf: its values going on, and its when_off values going off.
-- They go through Kobo's own Settings, so Nickel follows straight away, and finish's refresh shows
-- them. Only from a tap, never as the mod loads, so not from apply_choices: the page writes them
-- over USB for patches it switches, and the file keeps them across a restart.
local function write_settings(patch, on)
  for _, setting in ipairs(patch.settings or {}) do
    local value = on and setting.value or setting.when_off
    if value and not nk.device.set_setting(setting.key, value) then nk.log("couldn't write " .. setting.key) end
  end
end

-- Switches one patch, and the others in its group off for one going on. Returns whether it did,
-- whether that needs a restart, and whether it needs the main window built again.
local function switch(patches, rows, i, on)
  local patch, restart, rebuild = patches[i], false, false
  if on then
    for k, other in ipairs(patches) do
      local row = rows[k]
      if (row.state == "on" or row.state == "partly") and same_group(patch, other) then
        local ok, again = nk.patch.set(other.edits, false, other)
        if ok then
          write_settings(other, false)
          restart = restart or again
          rebuild = rebuild or other.main_window
          row.state = "off"
          choose(other, false)
          if row.toggle then set_quietly(row.toggle, false) end
        end
      end
    end
  end
  local ok, again = nk.patch.set(patch.edits, on, patch)
  if not ok then
    if rows[i].toggle then set_quietly(rows[i].toggle, not on) end
    return false, restart, rebuild
  end
  nk.log(patch.id .. ": " .. (on and "on" or "off"))
  write_settings(patch, on)
  rows[i].state = on and "on" or "off"
  if rows[i].toggle then set_quietly(rows[i].toggle, on) end
  choose(patch, on)
  return true, restart or again, rebuild or patch.main_window
end

local function finish(ok, restart, rebuild)
  save_choices()
  nk.patch.refresh()
  if not ok then
    return nk.error("Not everything was changed", "A patch's bytes have changed since this page opened. Close it and open it again.")
  end
  if restart and nk.confirm("Restart to finish",
      "Your Kobo has already used what this patch changes, so it takes effect after a restart.", "Restart", "Later") then
    nk.reboot()
  elseif rebuild and not restart then
    nk.main_window.rebuild()
  end
end

-- The rows of patches with the "all" flag in a category, leaving out the changed ones.
local function all_rows(patches, rows, category)
  local found = {}
  for i, patch in ipairs(patches) do
    if patch.category == category and patch.all and rows[i].state ~= "changed" then found[#found + 1] = i end
  end
  return found
end

local function all_on(rows, found)
  for _, i in ipairs(found) do
    if rows[i].state ~= "on" then return false end
  end
  return true
end

local function build(page)
  load_list()
  if choices_changed() then
    read_choices()
    if state.firmware and ready then
      apply_choices()
      nk.patch.refresh()
    end
  end
  local firmware = state.firmware
  page:section("THIS KOBO")
  page:value(firmware and firmware.name or model ~= "" and model or "Unknown model", version)
  local status
  status = page:button("New patches and Kobo Patcher", "Check", function()
    update.check(status, model, version, load_list)
  end)
  if not firmware then
    return page:text(("No patches yet: %s. Connect to Wi-Fi and tap Check, or add them from kobo.thesynt4x.dev."):format(state.list_error))
  end
  if not ready then
    return page:text("This firmware is missing something these patches need, so they're left off.")
  end
  page:text("Changes take effect straight away, and stay after a restart. These patches aren't made by Kobo, so use them at your own risk.")
  local patches = firmware.patches
  local rows = read_states(patches)
  -- Each category once, in the order it first comes up, with all of its patches.
  local categories, seen = {}, {}
  for _, patch in ipairs(patches) do
    if not seen[patch.category] then
      seen[patch.category] = true
      categories[#categories + 1] = patch.category
    end
  end
  -- A patch in a group can switch one in another category, so each "All" switch is shown again
  -- after any change.
  local alls = {}
  local function show_alls()
    for _, show in ipairs(alls) do show() end
  end
  quiet = quiet + 1
  for _, category in ipairs(categories) do
    page:section(category)
    -- "All <category>" switches every patch below it, and shows on only when they all are.
    local found, all = all_rows(patches, rows, category), nil
    local function show_all() set_quietly(all, all_on(rows, found)) end
    if #found > 1 then
      all = page:toggle("All " .. category:lower(), all_on(rows, found), function(on)
        if quiet > 0 then return end
        local warnings = {}
        for _, i in ipairs(found) do
          if on and rows[i].state ~= "on" and patches[i].warning then warnings[#warnings + 1] = patches[i].warning end
        end
        if #warnings > 0 and not nk.confirm(category:sub(1, 1) .. category:sub(2):lower(), table.concat(warnings, "\n\n"), "Turn on", "Cancel") then
          return show_all()
        end
        local ok, restart, rebuild = true, false, false
        for _, i in ipairs(found) do
          if rows[i].state ~= (on and "on" or "off") then
            local done, again, window = switch(patches, rows, i, on)
            ok, restart, rebuild = ok and done, restart or again, rebuild or window
          end
        end
        show_alls()
        finish(ok, restart, rebuild)
      end)
      alls[#alls + 1] = show_all
    end
    for i, patch in ipairs(patches) do
      local row = rows[i]
      -- Bytes that match neither side belong to something else, so they're left alone. Partly on
      -- shows as off, so turning it on finishes it.
      if patch.category == category and row.state == "changed" then
        page:value(patch.title, "Changed")
      elseif patch.category == category then
        row.toggle = page:toggle(patch.title, row.state == "on", function(on)
          if quiet > 0 then return end
          if on and patch.warning and not nk.confirm(patch.title, patch.warning, "Turn on", "Cancel") then
            return set_quietly(row.toggle, false)
          end
          local ok, restart, rebuild = switch(patches, rows, i, on)
          show_alls()
          finish(ok, restart, rebuild)
        end)
      end
    end
  end
  quiet = quiet - 1
end

if not state.started then
  state.started = true
  read_choices()
  load_list()
end
nk.settings{name = "Kobo Patcher", build = build}

-- Keyboards in your languages, a filter for them in My Books, and dictionaries by them.
languages.start()

-- KOReader's own launcher stops Nickel, and starts it again when KOReader quits.
if nk.file.exists(KOREADER) then
  nk.more{name = "KOReader", icon = "koreader", open = function()
    if not nk.file.exists(KOREADER) then
      return nk.error("KOReader isn't installed", "Add it again from kobo.thesynt4x.dev, then restart your Kobo.")
    end
    nk.log("starting KOReader")
    nk.spawn("exec " .. KOREADER)
  end}
end
