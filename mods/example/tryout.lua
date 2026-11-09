-- Checks that the kit's tools work, without a network: JSON, patterns, text, hashes, signatures
-- and files, each against an answer known beforehand. Returns {name, ok, detail} for each.
local tryout = {}

local function check(results, name, ok, detail)
  results[#results + 1] = { name = name, ok = ok and true or false, detail = detail }
end

function tryout.offline()
  local results = {}

  local text = nk.json.encode({ title = "Emma", year = 1815, authors = { "Jane Austen" } })
  local back = nk.json.decode(text)
  check(results, "nk.json", back and back.title == "Emma" and back.year == 1815 and back.authors[1] == "Jane Austen"
    and nk.json.decode('{"a": null}').a == nk.json.null, text)

  local year = nk.re.match("Pride and Prejudice (1813)", "\\((\\d+)\\)")
  local all = nk.re.all("a1 b22 c333", "([a-z])(\\d+)")
  local first, last = nk.re.find("Moby-Dick", "dick", "i")
  check(results, "nk.re", year == "1813" and #all == 3 and all[3][2] == "333" and first == 6 and last == 9
    and nk.re.gsub("a  b   c", "\\s+", " ") == "a b c", year)

  local plain = nk.re.gsub(nk.text.decompose("Émile Zola"), "\\p{M}", "")
  check(results, "nk.text", nk.text.lower("ÅÄÖ Straße") == "åäö straße" and plain == "Emile Zola"
    and nk.text.language_name("sv") == "Svenska", plain)

  check(results, "nk.crypto hashes", nk.crypto.md5("abc") == "900150983cd24fb0d6963f7d28e17f72"
    and nk.crypto.sha1("abc") == "a9993e364706816aba3e25717850c26c9cd0d89d"
    and nk.crypto.sha256("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"
    and nk.crypto.sha512("abc"):sub(1, 16) == "ddaf35a193617aba")

  -- The first test of RFC 8032: an empty message, signed.
  local key = "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a"
  local signature = "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b"
  check(results, "nk.crypto.ed25519_verify", nk.crypto.ed25519_verify(signature, key, "")
    and not nk.crypto.ed25519_verify(signature, key, "changed"))

  nk.file.mkdir("tryout")
  local written = nk.file.write("tryout/kept.txt", "kept\n")
  local listed = nk.file.list("tryout") or {}
  check(results, "nk.file", written and nk.file.read("tryout/kept.txt") == "kept\n" and nk.file.exists("tryout/kept.txt")
    and listed[1] == "kept.txt")
  nk.file.remove("tryout/kept.txt")
  nk.file.remove("tryout")
  return results
end

return tryout
