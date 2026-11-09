-- nk.updates: a mod's own updates over Wi-Fi, checked when Check is tapped on the mod's page, the
-- way Kobo Patcher checks for its own. build.sh builds this into every Lua mod.
--
-- Nothing downloaded is trusted until it's checked: the manifest has to carry an Ed25519 signature
-- from the mod's key, and the new build has to match the SHA-512 and size the manifest gives it.
-- That's why plain HTTP is tried when TLS doesn't work.
--
-- tools/sign_mod.mjs writes what goes at the mod's address: manifest.txt, manifest.sig,
-- lib<mod>.so, and any other files the mod keeps up to date, such as Kobo Patcher's patch lists.
-- The manifest is, fields separated by tabs:
--
--   kobo-mod-manifest	1
--   mod	<sha512>	<size>	<model-firmware,...>	<version>	<release or dev>
--   file	<path>	<sha512>	<size>
--
-- Lines of other kinds are left alone, so a manifest can carry more for older mods, as the site's
-- carries list lines for Kobo Patchers before 12.
local updates = {}

-- For testing: a file in the mod's .adds folder with another address, like
-- http://192.168.1.10:8765/. What's there still has to be signed.
local URL_FILE = "update-url"
-- Left by the crash guard (kit/nk.cpp) when a new build didn't start cleanly and the one before it
-- came back: the version not to offer again, and, until it's been said, which version went back.
local SKIP_FILE, ROLLED_BACK_FILE = "update-skip", "rolled-back"

local source -- {url = , key = , name = , files = , changed = }, from nk.updates{...}
local busy = false

local function fields_of(line)
  local fields = {}
  for field in line:gmatch("[^\t]+") do fields[#fields + 1] = field end
  return fields
end

-- {mod = {hash, size, targets, version, release}, files = {[path] = {hash, size}}}. The first line
-- of each counts, and lines that can't be read are left out.
function updates.parse(text)
  local manifest = {files = {}}
  for line in text:gmatch("[^\n]+") do
    local f = fields_of(line)
    if #f >= 5 and f[1] == "mod" and #f[2] == 128 and not manifest.mod then
      manifest.mod = {hash = f[2], size = tonumber(f[3]), targets = f[4], version = tonumber(f[5]), release = f[6] == "release"}
    elseif #f >= 4 and f[1] == "file" and #f[3] == 128 and not manifest.files[f[2]] then
      manifest.files[f[2]] = {hash = f[3], size = tonumber(f[4])}
    end
  end
  return manifest
end

-- Whether a path can be one of the mod's files: relative, under its .adds folder, nothing hidden.
local function safe_path(path)
  return type(path) == "string" and path:find("^[%w_-][%w_./-]*$") ~= nil and not path:find("%.%.") and not path:find("//")
end

-- Whether data is the file the manifest describes.
function updates.matches(data, entry)
  return data ~= nil and entry ~= nil and #data == entry.size and nk.crypto.sha512(data) == entry.hash
end

-- Whether the manifest's mod is for this Kobo: its model and firmware, where it has been tried.
function updates.for_this_kobo(mod)
  local this = nk.device.model() .. "-" .. nk.device.firmware()
  for target in (mod.targets or ""):gmatch("[^,]+") do
    if target == this then return true end
  end
  return false
end

-- Whether the manifest's mod is a later version than this one. A dev build, made on a computer,
-- is offered the release of its own version too, which is the way back to what's published.
function updates.newer(mod)
  if mod == nil or mod.version == nil or updates.skipped(mod.version) then return false end
  if mod.version > nk.version then return true end
  return nk.build ~= "release" and mod.release and mod.version == nk.version
end

-- The highest version that didn't start cleanly on this Kobo, or nil.
function updates.skip()
  local text = nk.file.read(SKIP_FILE)
  return text and tonumber(text:match("%d+"))
end

-- Whether a later version than this one didn't start cleanly here, so it, and any before it, isn't
-- offered again.
function updates.skipped(version)
  local skip = updates.skip()
  return skip ~= nil and version > nk.version and version <= skip
end

local function base_url()
  local text = nk.file.read(URL_FILE)
  local line = text and text:match("^[^\r\n]*") or ""
  if #line <= 8 or #line >= 199 then line = source.url end
  return line:sub(-1) == "/" and line or line .. "/"
end

-- A check, from tapping Check until it's done: the text to show, and whether to restart.
local function run(say)
  local base, tried_http, name = base_url(), false, source.name

  -- A file from the mod's address, or nil and the status (0 when nothing answered).
  local function get(path)
    local code, body, why = nk.http.get(base .. path)
    if code == 0 and not tried_http and base:find("^https://") and why and why:find("SSL") then
      -- Everything is checked against the signature, so plain HTTP is as safe, just not private.
      base, tried_http = "http://" .. base:sub(9), true
      nk.log("TLS didn't work, trying " .. base)
      return get(path)
    end
    if code ~= 200 then
      nk.log(("download of %s failed: %s"):format(path, why or code))
      return nil, code
    end
    return body
  end

  local function unreachable(code)
    if code == 0 then return "Couldn't connect. Make sure Wi-Fi is on, then try again." end
    return "There's nothing to check for updates from yet. Try again later."
  end

  say("Checking for updates...")
  nk.log("checking " .. base)
  local text, code = get("manifest.txt")
  if not text then return unreachable(code) end
  local signature
  signature, code = get("manifest.sig")
  if not signature then return unreachable(code) end
  if not nk.crypto.ed25519_verify(signature:sub(1, 128), source.key, text) then
    return "The update didn't check out, so it wasn't used."
  end
  local manifest = updates.parse(text)

  -- The mod's files first, each saved where it's kept on the mod's address, once it matches.
  local result, changed = ("%s is up to date."):format(name), {}
  for _, path in ipairs(source.files and source.files() or {}) do
    local entry = manifest.files[path]
    if safe_path(path) and entry and not updates.matches(nk.file.read(path), entry) then
      say("Downloading what's new...")
      local data = get(path)
      if not data then return "What's new couldn't be downloaded. Try again later." end
      if not updates.matches(data, entry) then return "The download didn't match what was expected, so it wasn't used." end
      local folder = path:match("^(.*)/[^/]*$")
      if folder then nk.file.mkdir(folder) end
      if not nk.file.write(path, data) then return "What's new couldn't be saved. Check there's free space." end
      changed[#changed + 1] = path
    end
  end
  if #changed > 0 then
    result = source.changed and source.changed(changed) or ("%s has what's new."):format(name)
  end

  local mod = manifest.mod
  if mod and updates.for_this_kobo(mod) and mod.version and updates.skipped(mod.version) and #changed == 0 then
    return ("Version %d didn't work on this Kobo. A newer one will come."):format(mod.version)
  end
  if not mod or not updates.for_this_kobo(mod) or not updates.newer(mod) then return result end
  -- What's new is said while the question about the build waits, but "up to date" isn't.
  if #changed > 0 then say(result) end

  local question = ("Version %d of %s is ready. Your Kobo will restart to install it."):format(mod.version, name)
  if mod.version == nk.version then
    question = ("This Kobo has a dev build of %s. Install the release of version %d instead? Your Kobo will restart."):format(name, mod.version)
  end
  if not nk.confirm(("Update %s?"):format(name), question, "Update", "Later") then
    return ("Version %d is ready whenever you want it."):format(mod.version)
  end
  say(("Downloading %s..."):format(name))
  local data = get("lib" .. nk.name .. ".so")
  if not data then return ("The new %s couldn't be downloaded. Try again later."):format(name) end
  if not updates.matches(data, mod) then return "The download didn't match what was expected, so it wasn't used." end
  if not nk.replace_self(data, mod.hash, mod.version) then return ("The new %s couldn't be installed. Nothing was changed."):format(name) end
  return "Restarting to finish the update.", true
end

-- Checks for a newer build, asks before installing it, and restarts to finish. say(text) is told
-- how it's going. Does nothing while a check is already running.
function updates.check(say)
  if not source then error("this mod has no updates: call nk.updates{url = , key = } in main.lua", 2) end
  if busy then return end
  busy = true
  local ok, result, restart = pcall(run, say)
  busy = false
  if not ok then error(result, 0) end
  say(result)
  nk.log("update check: " .. result)
  if restart then nk.reboot() end
end

-- A row for the mod's page: its version, with Check, which shows how the check goes in its place.
-- Returns the row's title.
function updates.add_to(page)
  local title = ("Version %d"):format(nk.version)
  if nk.build ~= "release" then title = title .. ", dev build" end
  local skip = updates.skip()
  if skip and skip > nk.version then title = ("%s. Version %d didn't work on this Kobo"):format(title, skip) end
  local status
  status = page:button(title, "Check", function()
    updates.check(function(text)
      pcall(status.set, status, text) -- the screen may have closed
    end)
  end)
  return status
end

-- Once, after the build before an update came back: a message as soon as Kobo's Home is built, so
-- whoever has the Kobo knows, and can say which version broke. Kobo is running by then, so it never
-- shows while the mod loads. Without Home's class on this firmware, half a minute after the mod starts.
local told = false
local function tell_rolled_back()
  local text = not told and nk.file.read(ROLLED_BACK_FILE)
  local failed, back = (text or ""):match("(%d+)%s+(%d+)")
  if not failed then return end
  told = true
  local function tell()
    if not nk.file.exists(ROLLED_BACK_FILE) then return end
    nk.file.remove(ROLLED_BACK_FILE)
    nk.toast(source.name, ("Version %s didn't start on this Kobo, so version %s is back."):format(failed, back), 8000)
  end
  if not nk.on_screen("HomePageView", function() nk.after(1500, tell) end) then nk.after(30000, tell) end
end

-- nk.updates{url = , key = , name = , files = , changed = }: where this mod's updates come from.
-- files is a list of paths, or a function returning one, asked at each check; changed(paths) runs
-- after any of them is saved, and returns what Check says. Only records it all, so it can be
-- called from main.lua's top level.
setmetatable(updates, {__call = function(_, options)
  if type(options) ~= "table" then error("nk.updates takes a table: {url = , key = }", 2) end
  if not nk.version then error("nk.updates needs the mod's version, in a file named version in its folder", 2) end
  if type(options.url) ~= "string" or not options.url:find("^https?://") then error("nk.updates needs a url, http:// or https://", 2) end
  if type(options.key) ~= "string" or not options.key:find("^%x+$") or #options.key ~= 64 then
    error("nk.updates needs a key: the public half of the signing key, 64 hex digits", 2)
  end
  local files = options.files
  if type(files) == "table" then
    for _, path in ipairs(files) do
      if not safe_path(path) then error(("nk.updates can't keep %s: a file's path is relative, under the mod's folder"):format(tostring(path)), 2) end
    end
    local list = files
    files = function() return list end
  elseif files ~= nil and type(files) ~= "function" then
    error("nk.updates' files is a list of paths, or a function returning one", 2)
  end
  if options.changed ~= nil and type(options.changed) ~= "function" then error("nk.updates' changed is a function", 2) end
  source = {url = options.url, key = options.key:lower(), name = options.name or nk.name, files = files, changed = options.changed}
  tell_rolled_back()
end})

return updates
