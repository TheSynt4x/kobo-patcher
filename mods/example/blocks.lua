-- Every kind of block a page can hold, top to bottom. The page turns by itself when they don't fit.
local blocks = {}

local state = nk.keep { refreshed = 0, searched = "" }

-- A few books for page:books and page:book. Their covers are empty outlines until there's an
-- image: a cover is a path on the Kobo, such as one nk.download saved.
local BOOKS = {
  { title = "Emma", author = "Jane Austen", status = "Read" },
  { title = "Middlemarch", author = "George Eliot", status = "New" },
  { title = "Moby-Dick", author = "Herman Melville" },
  { title = "The Odyssey", author = "Homer", status = "40%" },
}

function blocks.build(page)
  page:heading("page:heading", "settings", function()
    nk.toast("page:heading", "Its icon is one of Kobo's, or one of the mod's own.", 3000)
  end)

  -- Typing first: the keyboard takes the bottom of the screen, and the page turns to fit what's
  -- left, so a box further down would be on a page that isn't showing.
  page:section("TYPING")
  local typed = page:text(state.searched ~= "" and ("Searched for " .. state.searched) or "Nothing typed yet.")
  page:search("page:search, then Go", function(text)
    state.searched = text
    typed:set("Searched for " .. text)
  end, state.searched)
  local field
  field = page:field("page:field, then Go", function(text)
    typed:set(("Typed %q, %d characters"):format(text, #text))
    field:set("")
  end)

  page:section("TEXT")
  page:text("page:text is a line of text, wrapped when it's longer than the page is wide, like this one.")
  page:paragraph("page:paragraph is text in Kobo's regular style, as a book's description shows. It suits a few sentences that are read rather than tapped.")

  page:section("ROWS")
  page:value("page:value", "on the right")
  local line = page:text("This line is hidden and shown by the switch below, with handle:show.")
  page:toggle("page:toggle, showing the line above", true, function(on) line:show(on) end)
  local taps = 0
  local title
  title = page:button("page:button", "Tap", function()
    taps = taps + 1
    title:set(("Tapped %d time%s, with handle:set"):format(taps, taps == 1 and "" or "s"))
  end)

  page:section("BOOKS")
  local shelf
  shelf = page:books(BOOKS, function(i)
    -- handle:set changes only the fields given.
    shelf:set(i, { status = BOOKS[i].status == "Read" and "New" or "Read" })
    BOOKS[i].status = BOOKS[i].status == "Read" and "New" or "Read"
  end, { columns = 4 })
  page:text("page:books, four across. Tap a book to change what its cover says.")

  local book
  book = page:book({ title = "Persuasion", author = "Jane Austen", action = "Get", status = "page:book, as Kobo's book details show one." }, function()
    book:set({ action = "", status = "Its button was tapped, so it's gone, and this line changed." })
  end)

  page:section("A COVER")
  page:text("page:cover shows a cover, centred. Put an image at .adds/example/cover.jpg to see it here.")
  page:cover("cover.jpg", 300)

  page:section("BUILDING AGAIN")
  page:value("Built again", ("%d time%s"):format(state.refreshed, state.refreshed == 1 and "" or "s"))
  page:button("page:refresh, once the tap is done", "Refresh", function()
    state.refreshed = state.refreshed + 1
    page:refresh()
  end)
  page:button("page:open, a page over this one", "Open", function()
    page:open(function(over)
      over:text("This page was opened over the last one. Kobo's back arrow goes back to it.")
    end, "Over")
  end)
end

return blocks
