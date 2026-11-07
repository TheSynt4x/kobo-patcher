-- Languages: Kobo's own keyboards in the languages you choose, switched with a key beside the space
-- bar, an entry per language of your books in My Books' filter menu, and each book's dictionary
-- by its cleaned language.
--
-- Kobo's "Language and dictionaries" page becomes "Languages", with a KEYBOARDS section under
-- Kobo's own two, whose Edit opens the list of keyboards to tick. Kobo's interface language and its
-- dictionaries stay Kobo's. With nothing ticked, the keyboard is Kobo's usual one.
local languages = {}

-- Kobo's keyboards, by the language code that gets each one. Russian has Latin letters too.
local KEYBOARDS = {
  {code = "en"}, {code = "ru"}, {code = "sv"}, {code = "nb"}, {code = "da"}, {code = "de"},
  {code = "fr"}, {code = "ro"}, {code = "tr"}, {code = "ja"},
  {code = "zh_CN", name = "中文 (拼音)"}, {code = "zh_TW", name = "中文 (注音)"}, {code = "zh_HK", name = "中文 (倉頡)"},
}

local function keyboard_name(code)
  for _, keyboard in ipairs(KEYBOARDS) do
    if keyboard.code == code and keyboard.name then return keyboard.name end
  end
  local name = nk.text.language_name(code)
  return name ~= "" and name or code
end

-- The keyboards ticked, in the order they were ticked, and the one in use.
local function chosen()
  local list = {}
  for code in nk.pref("keyboards", ""):gmatch("[^,]+") do list[#list + 1] = code end
  return list
end

local function current()
  local list = chosen()
  local now = nk.pref("keyboard", "")
  for _, code in ipairs(list) do
    if code == now then return code end
  end
  return list[1]
end

local function save(list)
  nk.set_pref("keyboards", table.concat(list, ","))
  local now = nk.pref("keyboard", "")
  local kept = false
  for _, code in ipairs(list) do kept = kept or code == now end
  if not kept then nk.set_pref("keyboard", list[1] or "") end
end

-- The short name on the key, as phones show it: EN, RU, SV.
local function key_label(code)
  return (code:match("^(%a%a)") or code):upper()
end

local function summary()
  local names = {}
  for _, code in ipairs(chosen()) do names[#names + 1] = keyboard_name(code) end
  return #names == 0 and "Kobo's usual keyboard" or table.concat(names, ", ")
end

local function edit_keyboards()
  nk.screen{title = "Keyboards", small_title = "Languages", build = function(page)
    page:text("Tick the keyboards you type in. With two or more, the key left of the space bar switches between them, in the order they were ticked.")
    local list = chosen()
    for _, keyboard in ipairs(KEYBOARDS) do
      local on = false
      for _, code in ipairs(list) do on = on or code == keyboard.code end
      page:toggle(keyboard_name(keyboard.code), on, function(ticked)
        local now = chosen()
        for i = #now, 1, -1 do
          if now[i] == keyboard.code then table.remove(now, i) end
        end
        if ticked then now[#now + 1] = keyboard.code end
        save(now)
      end)
    end
  end}
end

-- ---- the keyboard ----

local function hook_keyboard()
  nk.keyboard.choose(function()
    return current()
  end)
  nk.keyboard.key{
    -- No key with fewer than two keyboards to switch between.
    label = function(language)
      if #chosen() < 2 then return "" end
      return key_label(language ~= "" and language or current() or "")
    end,
    tapped = function()
      local list, now = chosen(), current()
      if #list < 2 then return end
      local next_one = list[1]
      for i, code in ipairs(list) do
        if code == now then next_one = list[i % #list + 1] end
      end
      nk.set_pref("keyboard", next_one)
      nk.keyboard.reload()
    end,
  }
end

-- ---- the Languages page ----

local function hook_page()
  -- The row in Settings that opens it.
  nk.on_screen("N3SettingsView", function(view)
    local row = view:find("language")
    if row then row:set("Languages") end
  end)
  nk.on_screen("N3SettingsLanguageView", function(view)
    local content = view:find("mainContent")
    if content then
      content:add_section("KEYBOARDS")
      content:add_button(summary(), "Edit", edit_keyboards)
    end
    -- Its title is the screen's, around the page, which Kobo puts it in and names a moment later.
    local tries = 0
    local function rename()
      local ok, top = pcall(view.top, view)
      local title = ok and top and top:class() ~= view:class() and top:find("titleLarge")
      if title and title:text() ~= "" then return title:set("Languages") end
      tries = tries + 1
      if tries < 20 then nk.after(50, rename) end
    end
    nk.after(0, rename)
  end)
end

-- ---- the filter in My Books ----

-- Three-letter codes whose first two letters aren't the language, as swe is sv and not Swahili.
local THREE = {
  ara = "ar", chi = "zh", zho = "zh", cze = "cs", ces = "cs", dan = "da", dut = "nl", nld = "nl",
  eng = "en", fin = "fi", fra = "fr", fre = "fr", ger = "de", deu = "de", gre = "el", ell = "el",
  heb = "he", hun = "hu", ita = "it", jpn = "ja", kor = "ko", nor = "no", nob = "nb", pol = "pl",
  por = "pt", ron = "ro", rum = "ro", rus = "ru", spa = "es", swe = "sv", tur = "tr", ukr = "uk",
}

-- Codes that say a book has no one language: undetermined, several, none, and not listed.
local NO_LANGUAGE = {und = true, mul = true, zxx = true, mis = true}

-- Lowercased, without its region, so ru, ru-RU, RU and rus are all ru. A three-letter code that
-- isn't above stays as it is, since cutting it short can make another language of it, and one
-- that names no language is "", so the book gets no entry of its own and is only under All.
function languages.clean(code)
  local c = code:lower():gsub("[-_].*$", "")
  if NO_LANGUAGE[c] then return "" end
  return THREE[c] or c
end

local function filters()
  local groups, order = {}, {}
  for _, raw in ipairs(nk.books.languages()) do
    local code = languages.clean(raw)
    if code ~= "" then
      if not groups[code] then
        groups[code] = {}
        order[#order + 1] = code
      end
      table.insert(groups[code], raw)
    end
  end
  -- With one language, its entry would just repeat All.
  if #order < 2 then return {} end
  local entries = {}
  for _, code in ipairs(order) do
    local name = nk.text.language_name(code)
    entries[#entries + 1] = {name = name ~= "" and name or code, languages = groups[code]}
  end
  table.sort(entries, function(a, b) return a.name < b.name end)
  return entries
end

-- ---- dictionaries ----

-- A book in rus, ru-RU or swe looks words up in Russian or Swedish, and one with no language in the
-- interface language, and a dictionary on the Kobo, Kobo's or custom, wins over one it would
-- have to download.
local function hook_dictionaries()
  nk.books.language(languages.clean)
  nk.dictionaries.installed_first()
end

function languages.start()
  hook_keyboard()
  hook_page()
  nk.books.filters(filters)
  hook_dictionaries()
end

return languages
