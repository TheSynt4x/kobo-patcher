# Writing a mod in Lua

A mod adds to Kobo's own screens, with Kobo's own widgets, from Lua scripts. The kit (`kit/`) does the work that has to happen inside Nickel, and gives the scripts the calls below. This page is the reference: every call, what it does, and what's been tried. `example/` is a showcase to copy from: a row on the More page and a tab, with a page for every kind of block, waiting and dialogs, the tools and the network, this Kobo, and what a mod can add to Kobo's own screens, each switched on there. `patches/` is Kobo Patcher, a mod that does a real job.

## A mod

A mod is a folder:

- `main.lua`, which runs when Nickel loads the mod, and says where the mod goes: its rows, tabs and screens.
- Any other `.lua` files, loaded with `require "name"`.
- `icons/`, PNG files for rows and tabs, built into the mod.
- `plugin`, the Qt plugin class and key the mod loads as (15 and 4 characters), so it loads beside other mods.
- `version`, for a mod that updates itself: its version, a whole number from 1, which `nk.updates` offers higher ones by.

`build.sh <folder>` builds it into one `lib<folder>.so`, scripts and icons included, which goes in `/usr/local/Kobo/imageformats/` on the Kobo. Each mod keeps its own files in `/mnt/onboard/.adds/<mod>/`.

While you work on a mod, its scripts can come from `.adds/<mod>/dev/` on the Kobo instead of the built-in ones. When a script there changes, `main.lua` runs again the next time one of the mod's rows or tabs opens, with no restart. `nk.keep` keeps state across those runs.

## Rules

- **`main.lua` runs while Qt is still loading the mod,** before Kobo has built anything. There it only says what the mod adds: `nk.settings`, `nk.tab`, `nk.on_screen` and the like. Those calls only record what they're asked for. Don't decode images, read Kobo's database, or wait for the network there: Qt's plugin loader is busy loading the mod, and anything that needs it waits for ever, so Nickel never starts. Everything else happens in callbacks, once Kobo is running.
- **Callbacks can wait.** Each runs as a Lua coroutine, so `nk.http.get`, `nk.download`, `nk.confirm` and `nk.sleep` wait for their answer without stopping Nickel. A few callbacks have to answer straight away, and say so below: they can't wait.
- **A Lua error can't take Nickel down.** It's shown in a dialog, and the mod carries on.
- **Firmware differences are handled by the kit.** Every Nickel function the kit uses is checked when the mod loads. A call that needs something this firmware lacks returns false or does nothing, and the rest of the mod still works.
- **A mod's own screens go on the More page,** one row per mod with its icon, and its settings there too, so Kobo's Settings and its other screens stay as Kobo has them. Where a mod changes one of Kobo's own screens, the switch for it is on the mod's screen, so there's one place to find everything a mod does.
- **The crash guard.** A mod that crashes Nickel as it starts, or freezes it, is set aside, and the Kobo starts again without it. See "What keeps a Kobo safe" in `README.md`.

## Places: where a mod shows up

### `nk.settings{name = , build = , after = or before = }`

A row in Kobo's Settings, opening a screen built by `build(page)`. `after` or `before` name one of Kobo's rows, such as `"deviceInformation"`, `"account"` or `"language"`; without either it goes at the top. A mod can add several. If the named row isn't there, the row goes at the top and the log says why.

### `nk.more{name = , icon = , open = , after = or before = , first = }`

A row on Kobo's More page, with `icons/<icon>.png`, running `open()`. The icon is scaled as Kobo's own are on each Kobo, 40 pixels on the Nia, so draw it larger than it shows: 80 pixels, with the drawing near its edges. `after` or `before` name one of Kobo's rows, such as `"wishlistButton"`, `"betaFeaturesButton"` or `"helpButton"`; without either it goes after Help, the last of Kobo's rows, with the other mods' rows, so what's added is together below what's Kobo's. `first = true` puts it first of them. A mod can add several.

Rows are put in each time Kobo builds the screen, so when `main.lua` runs again, renamed rows show their new name and removed ones go.

### `nk.tab{name = , icon = , after = or before = , build = }`

A tab in Kobo's bottom bar, opening a page built by `build(page)`. The icon is `icons/<icon>.png`, and `icons/<icon>-active.png` while the tab is open, if there is one. Draw them larger than they show (56 pixels for the Nia's 28) and they're scaled for each screen. `after` or `before` name one of Kobo's sections: `home`, `books`, `discover`, `notebooks`, `activity` or `more`. Without either it goes before More. A mod can have several tabs. Running it again with the same name gives that tab the new build.

### `nk.screen{title = , small_title = , build = }`

Opens a settings-style screen built by `build(page)`, over whatever is showing, with Kobo's back arrow and close button. `small_title` is the small line above the title. By default it's the title of the screen it opens over, or "Settings".

### `nk.on_screen(class, fn)`

Runs `fn(view)` each time Kobo makes one of its own screens of that class, such as `"MoreView"`, once Kobo has built it. It's for classes made with a parent widget. `view` is a widget, with the calls below. Returns false if this firmware has no such class. Several mods can watch the same screen.

- `widget:find(name)`: the widget inside it with that object name, or nil.
- `widget:text()` and `widget:set(text)`: its text, for labels and anything else with one.
- `widget:show(on)`: shows or hides it.
- `widget:class()`: Kobo's name for its class.
- `widget:top()`: the screen it's on, the nearest of Kobo's screens around it, to find something there such as the screen's title (`"titleLarge"`). Only once Kobo has put it on its screen: from `nk.after`, not straight from `on_screen`.
- `widget:add_section(title)`: Kobo's grey section heading at the end of the widget's layout. Returns it as a label, or nil.
- `widget:add_button(title, button, tapped)`: a row with a title and Kobo's button, like the Edit beside Kobo's dictionaries, at the end of the widget's layout. Returns the title as a label, or nil.

Kobo Patcher uses these to rename Kobo's "Language and dictionaries" to "Languages" and add a section to it, in `patches/languages.lua`.

## Pages: what a screen or tab holds

`build(page)` fills a page with blocks, top to bottom, and the page turns when they don't fit. Each block is one of Kobo's own widgets, styled by Kobo's stylesheets.

- `page:section(title)`: Kobo's grey section heading.
- `page:text(text)`: a line of text, wrapped.
- `page:paragraph(text)`: text in Kobo's regular style, as a book's description.
- `page:value(title, value)`: a row with a title and italic text on the right.
- `page:toggle(title, on, changed)`: a row with Kobo's tick box. `changed(on)` runs when it's ticked or unticked.
- `page:button(title, label, tapped)`: a row with a title and Kobo's button. Returns the title and the button.
- `page:heading(text, icon, tapped)`: a large title, as at the top of Kobo's More page. With an icon, on the right: one of Kobo's, such as `"settings"`, or the mod's own `icons/<icon>.png`.
- `page:search(placeholder, go, text)` and `page:field(placeholder, go, text)`: Kobo's search box, or its plain text box, typed in with Kobo's keyboard. `go(text)` runs on its Go. The handle has `:text()` and `:set(text)`.
- `page:books(list, tapped, {columns = })`: books as Kobo's Home shows them, three across, or 1 to 6, in the same width. A book is `{title, author, cover, status}`, where cover is a path and status shows on the cover. `tapped(i)` runs for the i-th. The handle's `:set(i, book)` changes the fields given.
- `page:book(fields, tapped)`: one book, as Kobo's book details show it: cover, title, author, Kobo's black button and a line of what's happening. Fields: `cover`, `title`, `author`, `action` (the button's text, or "" for none) and `status`. `tapped()` runs when the button is tapped.
- `page:cover(path, height)`: a book's cover, centred. `:set(path)` changes it.
- `page:open(build, title)`: a page over this one, with Kobo's back arrow to return. On a tab, it's the tab's own next page. On a screen, it's a screen over it, with this screen's title as its small title.
- `page:refresh()`: builds the page again from the script's state, once the current tap is done.

Blocks that return a handle: `handle:set(text)` changes a label's text, and `handle:show(on)` hides a block, so it takes no room. A handle whose widget has gone raises an error, not a crash.

## Kobo's keyboard

- `nk.keyboard.choose(fn)`: `fn(script, locale)` runs each time Kobo makes a keyboard, with the locale it was going to use, such as `"en_US"`. It returns a language code for Kobo's keyboard in that language, or nil to leave it, and has to answer straight away. Kobo has keyboards for Russian (with Latin letters too), Swedish and Finnish, Norwegian, Danish, German, French, Romanian, Turkish, Japanese and Chinese (Pinyin `zh_CN`, Zhuyin `zh_TW`, Cangjie `zh_HK`). Any other language gets the usual keyboard. Every keyboard in Nickel goes through this, including the kit's own text boxes and other mods'.
- `nk.keyboard.reload()`: makes the keyboard that's showing again, so `choose` runs again.
- `nk.keyboard.key{label = , tapped = }`: a key of the mod's own on every keyboard, left of the space bar, which gets one key narrower. `label` is its text, or a `function(language)` returning it, given the language `choose` picked. An empty label leaves the key off that keyboard. `tapped()` runs when it's tapped.

Each returns false if this firmware's keyboard isn't as expected. Calling one again replaces what it set.

## My Books

- `nk.books.filters(fn)`: entries for My Books' filter menu, after Kobo's own. `fn()` returns a list of `{name = , short_name = , languages = {...}}`. `name` shows in the menu, and `short_name` (by default the name) once it's chosen. An entry shows the books whose language is any of `languages`, as Kobo stores them, such as `"ru"` or `"ru-RU"`. `fn` is asked each time Kobo builds the menu or reads back the choice it saved, and has to answer straight away. The choice is saved by the entry's name, so one that has gone, or whose mod has, comes back as Kobo's default.
- `nk.books.languages()`: the languages of the books on the Kobo, each once, as Kobo stores them from the books themselves, such as `"ru"`, `"ru-RU"` or `"rus"`. Not from `main.lua`: from a callback, or from `nk.books.filters`' function.
- `nk.books.language(fn)`: the language Kobo takes a book to be in, which picks its dictionary. `fn(code)` gets what Kobo makes of the book's language: one of its own locales for a language it knows, such as `"de"` or `"en_US"`, and otherwise the interface language's, or, with the patch "Dictionaries for every book language", the book's own code, such as `"rus"`, `"swe"` or `"und"`. It returns a two-letter code, such as `"ru"`, or `""` for the interface language. `fn` is asked once per code and has to answer straight away.

## Dictionaries

- `nk.dictionaries.installed_first()`: a book gets a dictionary that's on the Kobo whenever there's one in its language, Kobo's own or one in `.kobo/custom-dict`, before one Kobo would have to download. Without it, Kobo can pick a dictionary from its catalogue that isn't downloaded, such as its English, over a custom one, and then looks nothing up. A dictionary chosen in a book sticks the same way.

Each returns false if this firmware isn't as expected. Calling `nk.books.language` again replaces `fn`.

## Waiting and time

- `nk.after(ms, fn)` and `nk.every(ms, fn)`: run `fn` once after `ms`, or every `ms`. Each returns a timer with `:stop()`.
- `nk.sleep(ms)`: waits, in a callback.
- `nk.go(fn)`: runs `fn` on its own, straight after this callback, so several things can wait at once.
- `nk.time()`: seconds since 1970, in UTC.

## Keeping things

- `nk.pref(key, default)` and `nk.set_pref(key, value)`: the mod's settings, as `key=value` lines in `.adds/<mod>/settings.ini`.
- `nk.keep(table)`: the same table again when `main.lua` runs again, for state that should last across reloads.
- `nk.file.read(path)`, `nk.file.write(path, data)`, `nk.file.exists(path)`, `nk.file.list(folder)`, `nk.file.remove(path)` and `nk.file.mkdir(path)`. Paths are under `/mnt/onboard/`, or relative to the mod's `.adds/<mod>/`. A write keeps the old file until the new one is whole. `list` gives names, with a `/` after folders. Failures return nil and why.

## The network

- `nk.http.get(url, options)` and `nk.http.post(url, options)`: wait for the answer, and return its status (0 when nothing answered), its body, and what went wrong or nil. Options: `headers`, `body`, `type` (for a post, `"application/json"` unless said), `auth` (an Authorization header, only sent to the URL's own host), `limit` (bytes, 4 MB unless said) and `timeout` (ms, 30 s unless said).
- `nk.download(url, path, options)`: saves `url` at `path`, and returns true, or false and why, then the status. Options: `md5` or `sha512` (hex) and `size`, which the file must match; `limit` (bytes, 512 MB unless said); `referer`; `headers`; and `progress(bytes, total)`, run as it comes.

## Text

- `nk.json.decode(text)` and `nk.json.encode(value)`: the value or the text, or nil and why. JSON's null is `nk.json.null`.
- `nk.re.match(text, pattern, flags)`, `nk.re.find`, `nk.re.all` and `nk.re.gsub(text, pattern, replacement, flags)`: regular expressions, Qt's, which are Perl's. Flags: `i` to ignore case, `s` for a dot that matches new lines, `m` for `^` and `$` at each line. `find` gives where the match starts and ends, in bytes from 1.
- `nk.text.lower(text)`: lower case in every alphabet.
- `nk.text.decompose(text)`: letters apart from their accents, so é is e and then an accent, which `nk.re` can take out with `\p{M}`.
- `nk.text.language_name(code)`: a language's name in itself, as Kobo's language picker shows it: `"ru"` is Русский, `"sv"` Svenska. Empty for a code it doesn't know.

## Kobo

- `nk.device.model()` and `nk.device.firmware()`: this Kobo's model, from its serial, such as `"N306"`, and its firmware, such as `"4.38.23852"`.
- `nk.device.setting(key)` and `nk.device.set_setting(key, value)`: one of Kobo's own settings, in `.kobo/Kobo/Kobo eReader.conf`, with `key` as `"Section/name"`, such as `"FeatureSettings/GoogleDrive"`. `setting` gives Nickel's value as a string, or nil when it isn't set. `set_setting` sets it, or removes it when `value` is nil, and says whether it did. Both go through Kobo's own `Settings`, as Nickel does: a change is in Nickel's memory and the file straight away, the rest of the file stays as it is, and what listens for that setting, like the More page for Google Drive and Dropbox, follows without a restart. Some settings are only read as Kobo starts, and those still need `nk.reboot()`. Only once Nickel is running, though: from a tap or another callback, never from `main.lua`'s top level.
- `nk.device.on_setting(fn)`: `fn(key, value)` runs after each change to one of Kobo's settings, by Nickel or a mod, such as Kobo's sync writing what the store sends. `key` is `"Section/name"`, and `value` a string, or nil when the setting was removed. It runs after the change, so `set_setting` from `fn` wins, and that change comes back to `fn` too, so only set one that differs. From `main.lua`'s top level it only records `fn`: watching starts once Nickel is running.
- `nk.import()`: Kobo's import, as after a USB connection, so new books show up.
- `nk.main_window.rebuild()`: Kobo builds its main window again, as it does after a USB connection: the bottom bar, then Home or My Books, with every screen that was open closed, the one it's called from too. For a change to what Kobo builds them from, such as a patch, to show without a restart. It happens just after the callback it's called from returns. Returns false if this firmware's main window isn't as expected. Not from `main.lua`'s top level.
- `nk.reboot()`: restarts the Kobo the way Kobo does.
- `nk.spawn(command)`: runs a shell command outside Nickel. Nickel may well stop, as when it starts KOReader.
- `nk.log(...)`, and `print`: to Nickel's log, with the mod's name. `nk.name` is the mod's name.
- `nk.build`: what this build of the mod is. `"release"` when the site's CI built it (`KOBO_RELEASE=1 build.sh`), otherwise `"dev"`, the commit, `+` if the mod or the kit had changes not committed, and when, such as `"dev 3616efb+ 2026-11-08 14:56"`. A dev build's rows on the More page say DEV BUILD, without the mod doing anything.

## Dialogs

- `nk.confirm(title, text, accept, reject)`: Kobo's dialog with two buttons. Waits for the answer, true or false.
- `nk.error(title, text)`: Kobo's error dialog.
- `nk.toast(title, text, ms)`: Kobo's message that comes and goes by itself.

## A mod's own updates

A mod can update itself over Wi-Fi, from wherever its author puts it, the way Kobo Patcher does: on its own page, Check finds a newer version, asks, installs it and restarts the Kobo. It only checks when Check is tapped.

- `nk.updates{url = , key = , name = , files = , changed = }`: where this mod's updates come from. `url` is the folder holding them, `https://` or `http://`, and `key` the public half of the key that signs them, 64 hex digits. `name` is what the Kobo calls the mod, by default `nk.name`. `files` is other files the mod keeps up to date, such as Kobo Patcher's patch list: paths in its `.adds` folder, the same as where they are under `url`, or a function returning them, asked at each check. Check saves each one that has changed, once it matches the manifest, then runs `changed(paths)`, which returns what Check says. Call it from `main.lua`: it only records them. The mod needs a `version` file.
- `nk.updates.add_to(page)`: a row on the mod's page with its version, and ", dev build" for one built on a computer, and Check. While a check runs, how it's going shows in the row's place. Returns the row's title.
- `nk.updates.check(say)`: the same check, for a mod that shows it its own way. `say(text)` is told how it's going. From a callback.
- `nk.version`: the mod's version, from its `version` file, or nil without one.

To publish an update, raise the number in `version`, build the mod, and sign it with `tools/sign_mod.mjs`:

```sh
node tools/sign_mod.mjs --new-key ~/.config/my-mod/signing-key.pem    # once: prints the key for nk.updates
node tools/sign_mod.mjs path/to/mymod out --key ~/.config/my-mod/signing-key.pem
```

It writes `manifest.txt`, its signature `manifest.sig`, and `lib<mod>.so` into `out`, to put at `url`. With `--files DIR`, every file under `DIR` goes in `out` at the same path, for `files`. The manifest gives the build's SHA-512 and size, its version, and the Kobos it's for, `--targets`, by default the firmware the kit has been tried on. A Kobo only takes a manifest signed with the mod's key and a build that matches it, which is why plain HTTP is tried when TLS doesn't work. It's only offered a version higher than its own, or, on a dev build, the release of its own version, as the way back to what's published. Keep the key's private half secret: anyone with it can sign an update every Kobo with the mod would take.

If a new build crashes or freezes Nickel in its first 3 starts, as it starts or as one of its screens opens, the crash guard puts the build before it back instead of setting the mod aside (see "What keeps a Kobo safe" in README.md). Once Kobo's Home is up, a message says which version didn't start and which is back, the mod's Check row says it too, and that version, with any below it, isn't offered on that Kobo again: only a higher one is.

To test, put another address, such as `http://192.168.1.10:8000/`, in `.adds/<mod>/update-url` on the Kobo. What's there still has to be signed with the mod's key.

Kobo Patcher checks this way too, with its patch list as one of its `files`.

## Updating a mod and patching

These are for Kobo Patcher, and anyone building something like it. `nk.updates` uses the first two.

- `nk.crypto.ed25519_verify(signature, key, message)`: whether `message` carries `signature` from `key`, both in hex.
- `nk.self_sha512()` and `nk.replace_self(data, sha512)`: the hash of the mod as installed, and installing a new build of it for when Nickel next starts, only if it has that hash.
- `nk.patch.ready()`, `nk.patch.state(edits)`, `nk.patch.set(edits, on, options)` and `nk.patch.refresh()`: changing bytes in Nickel's memory while it runs, without touching the firmware's files. Every edit's bytes are checked before any is written, and a patch is written whole or not at all. See `patch.cpp`.

## What's been tried

All of it on a real Kobo Nia, firmware 4.38.23852, and a real Kobo Libra Colour, on 4.46.23852, and on the Nia's firmware in an emulator. There's no emulator for the Libra Colour.

`kit/nk.h` is the kit's own C layer, which C++ mods can use. It's internal: it isn't documented beyond its comments, and it may change in any release.
