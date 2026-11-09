-- nk.updates: a mod's own updates over Wi-Fi, checked when Check is tapped on the mod's page, the
-- way Kobo Patcher checks for its own. build.sh builds this into every Lua mod.
--
-- Nothing downloaded is trusted until it's checked: the manifest has to carry an Ed25519 signature
-- from the mod's key, and the new build has to match the SHA-512 and size the manifest gives it.
-- That's why plain HTTP is tried when TLS doesn't work.
--
-- tools/sign_mod.mjs writes the three files to put at the mod's address: manifest.txt,
-- manifest.sig, and lib<mod>.so. The manifest is, fields separated by tabs:
--
--   kobo-mod-manifest	1
--   mod	<sha512>	<size>	<model-firmware,...>	<version>	<release or dev>
--
-- which is the same mod line as Kobo Patcher's manifest, so its manifest reads this way too.
local updates = {}

-- For testing: a file in the mod's .adds folder with another address, like
-- http://192.168.1.10:8765/. What's there still has to be signed.
local URL_FILE = "update-url"

local source -- {url = , key = , name = }, from nk.updates{...}
local busy = false

local function fields_of(line)
  local fields = {}
  for field in line:gmatch("[^\t]+") do fields[#fields + 1] = field end
  return fields
end

-- The manifest's mod, {hash, size, targets, version, release}, or nil. The first mod line counts,
-- and lines that can't be read are left out.
function updates.parse(text)
  for line in text:gmatch("[^\n]+") do
    local f = fields_of(line)
    if #f >= 5 and f[1] == "mod" and #f[2] == 128 then
      return {hash = f[2], size = tonumber(f[3]), targets = f[4], version = tonumber(f[5]), release = f[6] == "release"}
    end
  end
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
  if mod == nil or mod.version == nil then return false end
  if mod.version > nk.version then return true end
  return nk.build ~= "release" and mod.release and mod.version == nk.version
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
  local mod = updates.parse(text)
  if not mod or not updates.for_this_kobo(mod) or not updates.newer(mod) then
    return ("%s is up to date."):format(name)
  end

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
  if not nk.replace_self(data, mod.hash) then return ("The new %s couldn't be installed. Nothing was changed."):format(name) end
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
  local status
  status = page:button(title, "Check", function()
    updates.check(function(text)
      pcall(status.set, status, text) -- the screen may have closed
    end)
  end)
  return status
end

-- nk.updates{url = , key = , name = }: where this mod's updates come from. Only records it, so it
-- can be called from main.lua's top level.
setmetatable(updates, {__call = function(_, options)
  if type(options) ~= "table" then error("nk.updates takes a table: {url = , key = }", 2) end
  if not nk.version then error("nk.updates needs the mod's version, in a file named version in its folder", 2) end
  if type(options.url) ~= "string" or not options.url:find("^https?://") then error("nk.updates needs a url, http:// or https://", 2) end
  if type(options.key) ~= "string" or not options.key:find("^%x+$") or #options.key ~= 64 then
    error("nk.updates needs a key: the public half of the signing key, 64 hex digits", 2)
  end
  source = {url = options.url, key = options.key:lower(), name = options.name or nk.name}
end})

return updates
