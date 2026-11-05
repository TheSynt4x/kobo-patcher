# Copy the kit's Qt plugin metadata with a mod's own class name and key, so Qt loads it beside
# Kobo Patcher and other mods. Both keep the kit's lengths, since the metadata is binary JSON.
# rename_metadata.py IN OUT CLASSNAME KEY
import sys
source, target, class_name, key = sys.argv[1:]
data = bytes(int(x) for x in open(source).read().split(","))
for old, new in ((b"KoboPatcherMods", class_name.encode()), (b"knkm", key.encode())):
    assert len(old) == len(new), f"{new.decode()} must be {len(old)} characters"
    assert data.count(old) >= 1, old
    data = data.replace(old, new)
open(target, "w").write(",".join(str(x) for x in data) + "\n")
