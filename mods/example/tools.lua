-- The kit's tools: offline, checked against known answers by tryout.lua, and the network, at
-- addresses from .adds/example/settings.ini.
local tools = {}

function tools.build(page)
  page:section("WITHOUT A NETWORK")
  page:text("Each is checked against an answer known beforehand: tryout.lua.")
  for _, result in ipairs(require("tryout").offline()) do
    page:value(result.name, result.ok and "works" or ("wrong: " .. tostring(result.detail)))
  end

  page:section("THE NETWORK")
  page:text("These use addresses from .adds/example/settings.ini: fetch=, post=, and download= with md5=.")
  local status = page:text("Nothing fetched yet.")
  page:button("nk.http.get the fetch address", "Fetch", function()
    local url = nk.pref("fetch")
    if not url then return status:set("No fetch= in settings.ini") end
    status:set("Fetching...")
    local code, body, why = nk.http.get(url, { timeout = 15000 })
    local answer = body and nk.json.decode(body)
    status:set(why or ("%d, %d bytes%s"):format(code, #body, answer and ", JSON" or ""))
  end)
  page:button("nk.http.post JSON to the post address", "Post", function()
    local url = nk.pref("post")
    if not url then return status:set("No post= in settings.ini") end
    status:set("Posting...")
    local code, body, why = nk.http.post(url, { body = nk.json.encode({ from = nk.name, at = nk.time() }) })
    status:set(why or ("%d, %d bytes back"):format(code, #body))
  end)
  page:button("nk.download the download address", "Download", function()
    local url = nk.pref("download")
    if not url then return status:set("No download= in settings.ini") end
    local ok, why = nk.download(url, "download", {
      md5 = nk.pref("md5"),
      progress = function(got, total) status:set(("Downloaded %d of %d bytes"):format(got, total)) end,
    })
    status:set(ok and "Downloaded and checked, in .adds/example/download" or ("Download failed: " .. why))
  end)

  page:section("SETTINGS")
  page:toggle("nk.pref and nk.set_pref: remember a choice", nk.pref("choice", "off") == "on", function(on)
    nk.set_pref("choice", on and "on" or "off")
  end)
  page:text("Kept in .adds/example/settings.ini, as choice=on or choice=off.")
end

return tools
