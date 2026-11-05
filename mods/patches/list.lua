-- The patch list for this Kobo's firmware, from a file written by tools/write_lists.py. The file
-- comes from the page or a download, so nothing in it is trusted: every line is checked, and
-- nk.patch checks the edits again before it writes any.
--
--   kobo-patcher-list	2
--   firmware	<model>	<version>	<name>
--   patch	<id>	<category>	<group or ->	<flags>	<title>	<warning>
--   edit	<file>	<offset>	<Kobo's bytes>	<the patch's bytes>
--   setting	<Section/name>	<value>	<value when it's turned off, or ->
--
local list = {}

local FORMAT = "2"
local MAX_SETTINGS = 16
local MAX_VALUE = 512
local MAX_BYTES = 1 << 20
local MAX_PATCHES = 256
local MAX_EDITS = 4096
local MAX_HEX = 1 << 16

local allowed = {["/usr/local/Kobo/libnickel.so.1.0.0"] = true, ["/usr/local/Kobo/nickel"] = true}

local function split(line)
  local fields = {}
  for field in (line .. "\t"):gmatch("([^\t]*)\t") do fields[#fields + 1] = field end
  return fields
end

local function is_hex(text)
  return #text > 0 and #text % 2 == 0 and #text <= MAX_HEX and not text:find("[^0-9a-f]")
end

-- Flags are separated by commas. Ones this mod doesn't know are left for a newer one.
local function has_flag(flags, flag)
  for each in flags:gmatch("[^,]+") do
    if each == flag then return true end
  end
  return false
end

-- A setting's value: printable, and short.
local function is_value(text)
  return #text > 0 and #text <= MAX_VALUE and not text:find("[%c]")
end

-- Reads a list's text. Returns {model, version, name, patches}, or nil and why. Each patch is {id,
-- title, category, group, warning, restart, cached, all, edits, settings}, and its edits are what
-- nk.patch takes. Each setting is {key, value, when_off}, when_off being nil to leave it as it is.
function list.parse(text)
  if #text > MAX_BYTES then return nil, "the patch list is too big" end
  if text == "" then return nil, "it's empty" end
  local firmware = {patches = {}}
  local number, edits, patch = 0, 0, nil
  for line in (text:gsub("\n$", "") .. "\n"):gmatch("([^\n]*)\n") do
    line = line:gsub("\r$", "")
    number = number + 1
    local fields = split(line)
    local kind = fields[1]
    if number == 1 then
      if #fields ~= 2 or kind ~= "kobo-patcher-list" or fields[2] ~= FORMAT then
        return nil, "it isn't a patch list this mod can read"
      end
    elseif number == 2 then
      if #fields ~= 4 or kind ~= "firmware" then return nil, "line 2 should name the firmware" end
      firmware.model, firmware.version, firmware.name = fields[2], fields[3], fields[4]
    elseif kind == "patch" then
      if #fields ~= 7 or #firmware.patches == MAX_PATCHES then
        return nil, ("line %d is a patch that can't be read"):format(number)
      end
      if patch and #patch.edits == 0 then return nil, ("a patch before line %d has no edits"):format(number) end
      patch = {
        id = fields[2], category = fields[3], title = fields[6],
        group = fields[4] ~= "-" and fields[4] or nil,
        warning = fields[7] ~= "" and fields[7] or nil,
        restart = has_flag(fields[5], "restart"), -- only takes effect once Nickel restarts
        cached = has_flag(fields[5], "cached"), -- changes code Nickel runs once
        all = has_flag(fields[5], "all"), -- switched too by "All <category>"
        edits = {},
        settings = {},
      }
      firmware.patches[#firmware.patches + 1] = patch
    elseif kind == "edit" then
      local at = #fields == 5 and fields[3]:match("^%d+$") and tonumber(fields[3])
      if not at or not patch or edits == MAX_EDITS or not allowed[fields[2]] or not is_hex(fields[4]) or
          not is_hex(fields[5]) or #fields[4] ~= #fields[5] or at > 0x7fffffff then
        return nil, ("line %d is an edit that can't be read"):format(number)
      end
      patch.edits[#patch.edits + 1] = {file = fields[2], at = math.tointeger(at), find = fields[4], replace = fields[5]}
      edits = edits + 1
    elseif kind == "setting" then
      if #fields ~= 4 or not patch or #patch.settings == MAX_SETTINGS or not fields[2]:match("^[%w_]+/[%w_]+$") or
          not is_value(fields[3]) or not is_value(fields[4]) then
        return nil, ("line %d is a setting that can't be read"):format(number)
      end
      patch.settings[#patch.settings + 1] = {key = fields[2], value = fields[3], when_off = fields[4] ~= "-" and fields[4] or nil}
    elseif kind ~= "" then
      return nil, ("line %d isn't something this mod knows"):format(number)
    end
  end
  if number < 2 then return nil, "it's empty" end
  if patch and #patch.edits == 0 then return nil, "the last patch has no edits" end
  return firmware
end

return list
