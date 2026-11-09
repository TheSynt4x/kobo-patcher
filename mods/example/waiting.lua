-- Waiting, timers and dialogs. Every callback runs as its own coroutine, so nk.sleep, nk.confirm
-- and the network wait for their answer without holding Nickel up.
local waiting = {}

-- Sets a label that may be gone: once its page has closed, a handle raises an error, so a timer
-- still running is stopped through it.
local function show(label, text)
  return pcall(label.set, label, text)
end

function waiting.build(page)
  page:section("WAITING")
  local status
  status = page:button("Nothing yet", "Clear", function() status:set("Nothing yet") end)
  page:button("nk.sleep: three seconds", "Start", function()
    for left = 3, 1, -1 do
      status:set(left .. "...")
      nk.sleep(1000)
    end
    status:set("Done waiting")
  end)
  page:button("nk.go: two things at once", "Start", function()
    local done = { a = 0, b = 0 }
    local function count(name, ms)
      for i = 1, 4 do
        nk.sleep(ms)
        done[name] = i
        status:set(("Fast %d of 4, slow %d of 4"):format(done.a, done.b))
      end
    end
    nk.go(function() count("a", 400) end)
    nk.go(function() count("b", 900) end)
  end)
  page:button("nk.after: in two seconds", "Start", function()
    status:set("Two seconds...")
    nk.after(2000, function() show(status, "nk.after ran") end)
  end)

  local clock, timer
  clock = page:button("nk.every: a clock", "Start", function()
    if timer then
      timer:stop()
      timer = nil
      return clock:set("Stopped")
    end
    local started = nk.time()
    timer = nk.every(1000, function()
      if not show(clock, ("Running for %d s, tap to stop"):format(nk.time() - started)) then
        timer:stop()
        timer = nil
      end
    end)
  end)

  page:section("DIALOGS")
  page:button("nk.confirm: a question", "Ask", function()
    local yes = nk.confirm("A question", "Did this dialog come from Lua?", "Yes", "No")
    status:set(yes and "You said yes" or "You said no")
  end)
  page:button("nk.toast: a message that goes", "Show", function()
    nk.toast("A toast", "It goes by itself after three seconds.", 3000)
  end)
  page:button("nk.error: Kobo's error dialog", "Show", function()
    nk.error("Something went wrong", "Only on purpose: this is what nk.error shows.")
  end)
  page:button("A Lua error, which can't crash Nickel", "Fail", function()
    error("an error on purpose, to show the dialog")
  end)
end

return waiting
