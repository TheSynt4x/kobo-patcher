-- Checks that the kit's tools work, without a network: JSON, patterns, hashes and files.
-- Returns a line for each, saying ok or what went wrong.
local tryout = {}

local function check(results, name, ok, detail)
  results[#results + 1] = name .. (ok and ": ok" or (": wrong, " .. tostring(detail)))
end

function tryout.offline()
  local results = {}
  local text = nk.json.encode({ title = "Emma", year = 1815, authors = { "Jane Austen" } })
  local back = nk.json.decode(text)
  check(results, "json", back and back.title == "Emma" and back.year == 1815 and back.authors[1] == "Jane Austen", text)

  local year = nk.re.match("Pride and Prejudice (1813)", "\\((\\d+)\\)")
  local all = nk.re.all("a1 b22 c333", "([a-z])(\\d+)")
  check(results, "re", year == "1813" and #all == 3 and all[3][2] == "333" and
    nk.re.gsub("a  b   c", "\\s+", " ") == "a b c", year)

  check(results, "md5", nk.crypto.md5("abc") == "900150983cd24fb0d6963f7d28e17f72")

  local written = nk.file.write("tryout.txt", "kept\n")
  check(results, "file", written and nk.file.read("tryout.txt") == "kept\n" and nk.file.exists("tryout.txt"))
  nk.file.remove("tryout.txt")
  return results
end

return tryout
