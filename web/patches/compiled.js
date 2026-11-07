// Written by tools/compile_patches.py from patches.yaml. Run `make patches` to change it.
export const FIRMWARE = [
  {
    "model": "N306",
    "name": "Kobo Nia",
    "firmware": "4.38.23852",
    "url": "https://ereaderfiles.kobo.com/firmwares/kobo7/Oct2026/kobo-update-4.38.23852.zip",
    "google_drive": true,
    "dropbox": true,
    "mod": true,
    "sizeMB": 133,
    "nickelSha256": "0e8e8ac3414a620d3386e60afcf4ccbf4ad3b8fdec8aeb5b553b01697966ec52",
    "nickelBinSha256": "cb3383aa0fcca1158558d9efcaf2248cee7a0d648ee26db5bc2ebfb26fff9992"
  },
  {
    "model": "N428",
    "name": "Kobo Libra Colour",
    "firmware": "4.46.23852",
    "url": "https://ereaderfiles.kobo.com/firmwares/kobo13/Oct2026/kobo-update-4.46.23852.zip",
    "mod": true,
    "sizeMB": 162,
    "nickelSha256": "d5c04fb1bee4ec9b4e06d974d1f73c213b16f52387d2ba8fba56a678b6b378fc",
    "nickelBinSha256": "5aaa04c2add60c8d926b100b81f85507f746e38e2a0f8ba0d04956971ce34791"
  }
];

export const PATCH_CATEGORIES = [
  "Reading",
  "Cloud storage",
  "Covers",
  "Ads and upsells",
  "Updates and USB"
];

export const ALL_SWITCH = ["Ads and upsells"];

export const COMPILED_PATCHES = [
  {
    "id": "upgrade-confirmation",
    "kind": "patch",
    "category": "Updates and USB",
    "title": "Ask before firmware updates",
    "description": "Always shows Kobo's confirmation before an update, so a silent update can't remove these patches.",
    "group": "updates",
    "settings": [],
    "offSettings": [],
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
    "category": "Updates and USB",
    "title": "Block firmware updates",
    "description": "Kobo always reports it's up to date, so an update can't remove these patches. Wi-Fi and sync aren't affected.",
    "warning": "You won't get security fixes or new features until you remove this patch.",
    "group": "updates",
    "settings": [],
    "offSettings": [],
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
    "category": "Reading",
    "title": "Finer margins",
    "description": "Makes the margin slider go up in smaller steps, from 0 to 8% of the screen width.",
    "settings": [],
    "offSettings": [],
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
    "category": "Reading",
    "title": "More small font sizes",
    "description": "Adds finer steps at small sizes. The largest sizes go away, so skip this if you read very large text.",
    "settings": [],
    "offSettings": [],
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
    "category": "Reading",
    "title": "Narrower line spacing",
    "description": "Replaces the line spacing slider with 10 values from 0.8 to 1.1, instead of 15 that go much wider.",
    "settings": [],
    "offSettings": [],
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
    "id": "no-kobo-store",
    "kind": "patch",
    "category": "Ads and upsells",
    "title": "No Kobo store",
    "description": "Takes away Home and Discover, with all their recommendations, store links and Kobo Plus offers, as Kobo does when it's set up without an account. My Books opens first, and Activity gets its own button. Your account, sync, Google Drive and Dropbox stay as they are.",
    "settings": [],
    "offSettings": [],
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
    "id": "no-end-of-book-recommendations",
    "kind": "patch",
    "category": "Ads and upsells",
    "title": "No recommendations at the end of a book",
    "description": "Hides the books Kobo suggests after the last page. Your rating and review stay.",
    "settings": [],
    "offSettings": [],
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
    "category": "Ads and upsells",
    "title": "No My Wishlist or My Articles",
    "description": "Hides both from the More page. Everything else on that page stays.",
    "settings": [],
    "offSettings": [],
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
    "category": "Updates and USB",
    "title": "USB drive even when locked",
    "description": "Lets a locked Kobo connect as a USB drive if it's plugged in while the sleep screen is showing.",
    "warning": "This makes the lock screen PIN useless for protecting your books and sign-ins.",
    "settings": [],
    "offSettings": [],
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
    "id": "google-drive",
    "kind": "patch",
    "category": "Cloud storage",
    "title": "Google Drive in the More menu",
    "description": "Turns off the device-model check that hides Google Drive, and keeps Kobo's address for linking it.",
    "settings": [
      [
        "OneStoreServices",
        "googledrive_link_account_start",
        "https://authorize.kobo.com/{region}/{language}/linkcloudstorage/provider/google_drive"
      ]
    ],
    "offSettings": [],
    "needsFirmware": true,
    "targets": {
      "N306": [
        "4.38.23852"
      ]
    }
  },
  {
    "id": "dropbox",
    "kind": "patch",
    "category": "Cloud storage",
    "title": "Dropbox in the More menu",
    "description": "Turns off the device-model check that hides Dropbox, and keeps Kobo's address for linking it.",
    "settings": [
      [
        "OneStoreServices",
        "dropbox_link_account_poll",
        "https://authorize.kobo.com/{region}/{language}/LinkDropbox"
      ]
    ],
    "offSettings": [],
    "needsFirmware": true,
    "targets": {
      "N306": [
        "4.38.23852"
      ]
    }
  },
  {
    "id": "home-covers",
    "kind": "patch",
    "category": "Covers",
    "title": "Larger home screen covers",
    "description": "Reduces the home screen's margins so covers can be bigger.",
    "settings": [],
    "offSettings": [],
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
    "category": "Covers",
    "title": "Larger library covers",
    "description": "Increases the cover thumbnails in the My Books list.",
    "settings": [],
    "offSettings": [],
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
    "category": "Reading",
    "title": "More room in the dictionary pop-up",
    "description": "Trims the pop-up's header, footer and margins to leave more space for definitions.",
    "settings": [],
    "offSettings": [],
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
    "category": "Reading",
    "title": "Dictionaries for every book language",
    "description": "Picks a dictionary by the book's own language, even one Kobo doesn't list. Without one it uses the usual dictionary.",
    "settings": [],
    "offSettings": [],
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
