local count = require "count"
assert(count.describe(0) == "None yet")
assert(count.describe(1) == "One")
assert(count.describe(12) == "12")
