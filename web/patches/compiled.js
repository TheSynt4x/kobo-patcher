// Written by tools/compile_patches.py from patches.yaml. Run `make patches` to change it.
export const FIRMWARE = [
  {
    "model": "N306",
    "name": "Kobo Nia",
    "firmware": "4.38.23852",
    "url": "https://ereaderfiles.kobo.com/firmwares/kobo7/Oct2026/kobo-update-4.38.23852.zip",
    "google_drive": true,
    "dropbox": true,
    "sizeMB": 133,
    "nickelSha256": "0e8e8ac3414a620d3386e60afcf4ccbf4ad3b8fdec8aeb5b553b01697966ec52",
    "nickelBinSha256": "cb3383aa0fcca1158558d9efcaf2248cee7a0d648ee26db5bc2ebfb26fff9992"
  },
  {
    "model": "N428",
    "name": "Kobo Libra Colour",
    "firmware": "4.46.23852",
    "url": "https://ereaderfiles.kobo.com/firmwares/kobo13/Oct2026/kobo-update-4.46.23852.zip",
    "sizeMB": 162,
    "nickelSha256": "d5c04fb1bee4ec9b4e06d974d1f73c213b16f52387d2ba8fba56a678b6b378fc",
    "nickelBinSha256": "5aaa04c2add60c8d926b100b81f85507f746e38e2a0f8ba0d04956971ce34791"
  }
];

export const COMPILED_PATCHES = [
  {
    "id": "upgrade-confirmation",
    "kind": "patch",
    "title": "Ask before firmware updates",
    "description": "Always shows Kobo's confirmation before an update, so a silent update can't remove these patches.",
    "needsFirmware": true,
    "targets": {
      "N306": [
        "4.38.23852"
      ],
      "N428": [
        "4.46.23852"
      ]
    }
  },
  {
    "id": "block-updates",
    "kind": "patch",
    "title": "Block firmware updates",
    "description": "Kobo always reports it's up to date, so an update can't remove these patches. Wi-Fi and sync aren't affected.",
    "warning": "You won't get security fixes or new features until you remove this patch.",
    "needsFirmware": true,
    "targets": {
      "N306": [
        "4.38.23852"
      ],
      "N428": [
        "4.46.23852"
      ]
    }
  },
  {
    "id": "margins",
    "kind": "patch",
    "title": "Finer margins",
    "description": "Makes the margin slider go up in smaller steps, from 0 to 8% of the screen width.",
    "needsFirmware": true,
    "targets": {
      "N306": [
        "4.38.23852"
      ],
      "N428": [
        "4.46.23852"
      ]
    }
  },
  {
    "id": "font-sizes",
    "kind": "patch",
    "title": "More small font sizes",
    "description": "Adds finer steps at small sizes. The largest sizes go away, so skip this if you read very large text.",
    "needsFirmware": true,
    "targets": {
      "N306": [
        "4.38.23852"
      ],
      "N428": [
        "4.46.23852"
      ]
    }
  },
  {
    "id": "line-spacing",
    "kind": "patch",
    "title": "Narrower line spacing",
    "description": "Replaces the line spacing slider with 10 values from 0.8 to 1.1, instead of 15 that go much wider.",
    "needsFirmware": true,
    "targets": {
      "N306": [
        "4.38.23852"
      ],
      "N428": [
        "4.46.23852"
      ]
    }
  },
  {
    "id": "no-recommendations",
    "kind": "patch",
    "title": "No recommendations on the home screen",
    "description": "Hides the recommendations column that shows when fewer than 2 books are open.",
    "needsFirmware": true,
    "targets": {
      "N306": [
        "4.38.23852"
      ],
      "N428": [
        "4.46.23852"
      ]
    }
  },
  {
    "id": "no-kobo-plus",
    "kind": "patch",
    "title": "No Kobo Plus suggestion on the home screen",
    "description": "Stops the home screen's rotating suggestion links from offering Kobo Plus.",
    "needsFirmware": true,
    "targets": {
      "N306": [
        "4.38.23852"
      ],
      "N428": [
        "4.46.23852"
      ]
    }
  },
  {
    "id": "no-discover",
    "kind": "patch",
    "title": "No Discover button",
    "description": "Hides Discover, with Kobo Plus and the store, from the bottom bar. Your books and Google Drive stay.",
    "needsFirmware": true,
    "targets": {
      "N306": [
        "4.38.23852"
      ],
      "N428": [
        "4.46.23852"
      ]
    }
  },
  {
    "id": "no-more-extras",
    "kind": "patch",
    "title": "No My Wishlist or My Articles",
    "description": "Hides both from the More page. Everything else on that page stays.",
    "needsFirmware": true,
    "targets": {
      "N306": [
        "4.38.23852"
      ],
      "N428": [
        "4.46.23852"
      ]
    }
  },
  {
    "id": "usb-when-locked",
    "kind": "patch",
    "title": "USB drive even when locked",
    "description": "Lets a locked Kobo connect as a USB drive if it's plugged in while the sleep screen is showing.",
    "warning": "This makes the lock screen PIN useless for protecting your books and sign-ins.",
    "needsFirmware": true,
    "targets": {
      "N306": [
        "4.38.23852"
      ],
      "N428": [
        "4.46.23852"
      ]
    }
  },
  {
    "id": "home-covers",
    "kind": "patch",
    "title": "Larger home screen covers",
    "description": "Reduces the home screen's margins so covers can be bigger.",
    "group": "home-layout",
    "needsFirmware": true,
    "targets": {
      "N306": [
        "4.38.23852"
      ],
      "N428": [
        "4.46.23852"
      ]
    }
  },
  {
    "id": "no-home-footer-covers",
    "kind": "patch",
    "title": "No Kobo Plus footer, and larger home screen covers",
    "description": "Removes the row that says \"Find your next book in Kobo Plus or our store\", and makes covers bigger.",
    "group": "home-layout",
    "needsFirmware": true,
    "targets": {
      "N306": [
        "4.38.23852"
      ],
      "N428": [
        "4.46.23852"
      ]
    }
  },
  {
    "id": "library-covers",
    "kind": "patch",
    "title": "Larger library covers",
    "description": "Increases the cover thumbnails in the My Books list.",
    "needsFirmware": true,
    "targets": {
      "N306": [
        "4.38.23852"
      ],
      "N428": [
        "4.46.23852"
      ]
    }
  },
  {
    "id": "dictionary-area",
    "kind": "patch",
    "title": "More room in the dictionary pop-up",
    "description": "Trims the pop-up's header, footer and margins to leave more space for definitions.",
    "needsFirmware": true,
    "targets": {
      "N306": [
        "4.38.23852"
      ],
      "N428": [
        "4.46.23852"
      ]
    }
  },
  {
    "id": "book-language-dictionaries",
    "kind": "patch",
    "title": "Dictionaries for every book language",
    "description": "Picks a dictionary by the book's own language, even one Kobo doesn't list, like Russian. Without one it uses the usual dictionary.",
    "needsFirmware": true,
    "targets": {
      "N306": [
        "4.38.23852"
      ],
      "N428": [
        "4.46.23852"
      ]
    }
  }
];
