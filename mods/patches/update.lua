-- "New patches and Kobo Patcher": a newer patch list for this firmware, and a newer mod, over
-- Wi-Fi. Nothing downloaded is trusted until it's checked: the manifest has to carry an Ed25519
-- signature from UPDATE_KEY, and every other file has to match the SHA-512 and size the manifest
-- gives it. That's why plain HTTP is tried when TLS doesn't work.
--
-- The manifest, from tools/sign_mods.mjs:
--
--   kobo-patcher-manifest	1
--   mod	<sha512>	<size>	<model-firmware,...>	<version>     the Kobos the mod updates itself on
--   list	<model-firmware>	<sha512>	<size>
local update = {}

-- This mod's version. A mod is only offered when the manifest's is higher, so a newer build is
-- never replaced by an older one, as one built from a branch would be by the site's. Raise it
-- with each change to the mod that should reach Kobos: tools/sign_mods.mjs reads it from here.
-- The C mod before this one was 1, and its manifests had no version.
update.VERSION = 4

-- The public half of the key tools/sign_mods.mjs signs with.
local UPDATE_KEY = "f65e1ff82241944b3e2a8ddb433be5a710835feaca53db3c5fc4afdd01d567b8"

local DEFAULT_URL = "https://kobo.thesynt4x.dev/mods/"
-- For testing: a file with another address, like http://192.168.1.10:8765/mods/. What's there
-- still has to be signed.
local URL_FILE = "update-url"

local busy = false

local function fields_of(line)
  local fields = {}
  for field in line:gmatch("[^\t]+") do fields[#fields + 1] = field end
  return fields
end

-- {mod = {hash, size, targets}, lists = {[model-firmware] = {hash, size}}, lists2 = the same in format
-- 2}. Lines that can't be read are left out, and the first of each kind counts.
function update.parse_manifest(text)
  local manifest = {lists = {}, lists2 = {}}
  for line in text:gmatch("[^\n]+") do
    local f = fields_of(line)
    if #f >= 4 and f[1] == "mod" and #f[2] == 128 and not manifest.mod then
      manifest.mod = {hash = f[2], size = tonumber(f[3]), targets = f[4], version = tonumber(f[5] or "")}
    elseif #f >= 4 and f[1] == "list" and #f[3] == 128 and not manifest.lists[f[2]] then
      manifest.lists[f[2]] = {hash = f[3], size = tonumber(f[4])}
    elseif #f >= 4 and f[1] == "list2" and #f[3] == 128 and not manifest.lists2[f[2]] then
      manifest.lists2[f[2]] = {hash = f[3], size = tonumber(f[4]), path = "lists/2/" .. f[2] .. ".txt"}
    end
  end
  return manifest
end

-- Whether data is the file the manifest describes.
function update.matches(data, entry)
  return data ~= nil and entry ~= nil and #data == entry.size and nk.crypto.sha512(data) == entry.hash
end

-- Whether the manifest's mod is a later version than this one. One without a version isn't.
function update.newer(mod)
  return mod ~= nil and mod.version ~= nil and mod.version > update.VERSION
end

-- Whether the signature, in hex, is UPDATE_KEY's on the manifest.
function update.signed(manifest, signature)
  return nk.crypto.ed25519_verify(signature:sub(1, 128), UPDATE_KEY, manifest)
end

local function base_url()
  local text = nk.file.read(URL_FILE)
  local line = text and text:match("^[^\r\n]*") or ""
  if #line <= 8 or #line >= 199 then return DEFAULT_URL end
  return line:sub(-1) == "/" and line or line .. "/"
end

-- A check, from tapping Check until it's done.
local function run(status, name, list_changed)
  local base, tried_http = base_url(), false

  local function say(text)
    pcall(status.set, status, text) -- the screen may have closed
  end

  -- A file from the site, or nil and the status (0 when nothing answered).
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
  if not update.signed(text, signature) then return "The update didn't check out, so it wasn't used." end
  local manifest = update.parse_manifest(text)

  -- The list for this firmware, in format 2 where the site has it. It's kept as lists/<name>.txt
  -- either way.
  local entry, path, result = manifest.lists2[name] or manifest.lists[name], "lists/" .. name .. ".txt", nil
  if not entry then
    result = "There are no patches for this firmware yet."
  elseif update.matches(nk.file.read(path), entry) then
    result = "Everything is up to date."
  else
    say("Downloading patches...")
    local data = get(entry.path or path)
    if not data then return "The patch list couldn't be downloaded. Try again later." end
    if not update.matches(data, entry) then return "The download didn't match what was expected, so it wasn't used." end
    if not nk.file.write(path, data) then return "The patch list couldn't be saved. Check there's free space." end
    list_changed()
    result = "New patches are ready. Close Kobo Patcher and open it again to see them."
  end

  -- Then the mod itself: only on the Kobos the manifest names, where it has been tried, and only
  -- when it's newer than this one.
  local mod, ours = manifest.mod, false
  for target in (mod and mod.targets or ""):gmatch("[^,]+") do ours = ours or target == name end
  if not ours or not update.newer(mod) then return result end
  say(result)
  if not nk.confirm("Update Kobo Patcher?",
      "A new version of Kobo Patcher is ready. Your Kobo will restart to install it, and your choices are kept.",
      "Update", "Later") then
    return "A new Kobo Patcher is ready whenever you want it."
  end
  say("Downloading Kobo Patcher...")
  local data = get("libpatches.so")
  if not data then return "The new Kobo Patcher couldn't be downloaded. Try again later." end
  if not update.matches(data, mod) then return "The download didn't match what was expected, so it wasn't used." end
  if not nk.replace_self(data, mod.hash) then return "The new Kobo Patcher couldn't be installed. Nothing was changed." end
  return "Restarting to finish the update.", true
end

-- Starts a check, reporting progress in status, a button row's label. list_changed runs after a
-- new list is saved. Does nothing while a check is already running.
function update.check(status, model, version, list_changed)
  if busy then return end
  busy = true
  local ok, result, restart = pcall(run, status, model .. "-" .. version, list_changed)
  busy = false
  if not ok then error(result, 0) end
  pcall(status.set, status, result)
  nk.log("update check: " .. result)
  if restart then nk.reboot() end
end

return update
