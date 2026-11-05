-- Logic is kept apart from screens, so it can be tested on a computer (test/count.lua).
local count = {}

function count.describe(n)
  if n == 0 then return "None yet" end
  if n == 1 then return "One" end
  return tostring(n)
end

return count
