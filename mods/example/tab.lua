-- The example's tab: a search box, a heading with Kobo's settings icon, and a shelf of books that
-- search narrows, each opening a page of its own, as a mod that finds books would.
local tab = {}

local state = nk.keep { searched = "" }

local BOOKS = {
  { title = "Emma", author = "Jane Austen", about = "A matchmaker in a quiet English village gets almost every match wrong, her own included." },
  { title = "Middlemarch", author = "George Eliot", about = "A provincial town, and the marriages, ambitions and mistakes of the people in it." },
  { title = "Moby-Dick", author = "Herman Melville", about = "A whaling voyage, and a captain who will follow one whale anywhere." },
  { title = "The Odyssey", author = "Homer", about = "Ten years of a journey home from war, told in verse." },
  { title = "Persuasion", author = "Jane Austen", about = "A second chance, eight years after the first was turned down." },
  { title = "Walden", author = "Henry David Thoreau", about = "Two years in a cabin by a pond, and what living simply showed." },
}

-- The books whose title or author has what was searched for, in any case.
local function matching(searched)
  local wanted = nk.text.lower(searched)
  local list = {}
  for _, book in ipairs(BOOKS) do
    if wanted == "" or nk.text.lower(book.title .. " " .. book.author):find(wanted, 1, true) then
      list[#list + 1] = book
    end
  end
  return list
end

local function open_book(page, book)
  page:open(function(over)
    local details
    details = over:book({ title = book.title, author = book.author, action = "Mark as read", status = "" }, function()
      details:set({ action = "", status = "Marked as read, with the handle's :set." })
    end)
    over:section("ABOUT THE BOOK")
    over:paragraph(book.about)
  end, book.title)
end

function tab.build(page)
  page:search("Search the example's books", function(text)
    state.searched = text
    page:refresh()
  end, state.searched)
  local shown = matching(state.searched)
  page:heading(state.searched == "" and "On the example's shelf" or ("%d found"):format(#shown), "settings", function()
    nk.screen{ title = "Example", small_title = "Example", build = function(screen)
      screen:text("A screen opened from the tab's heading, with nk.screen.")
    end }
  end)
  if #shown == 0 then
    page:text("Nothing matches. Search for an empty line to see them all again.")
  else
    page:books(shown, function(i) open_book(page, shown[i]) end)
  end
end

return tab
