# Addons: what is in the player, and what can be left out

The player is one binary. What an addon switch changes is not the code that is
linked, but whether that module's **pages are built at all**: a module that is
off is never initialised, so the LVGL screens, lists and rows it would have held
are never allocated. On a player whose heap is the scarce resource, that is the
memory that matters.

Everything is on by default. A module that is added to a later build starts on
too, so an update never quietly takes part of the interface away from somebody
who was using it.

## Where the switches are

**Settings → Plugins**, a row for each module of this build with a one-line
description and a switch. Switching one writes `[addons] <id> = true|false`
into `device_config.ini` and shows a *Restart to apply* toast: a page that has
already been built cannot give its memory back, so the saving is real from the
next start.

## The modules

| id | what it brings | where it is offered |
|---|---|---|
| `tidal` | Tidal pages, playlists, offline cache | Streaming |
| `qobuz` | Qobuz pages, playlists, offline cache | Streaming |
| `ytmusic` | YouTube Music search, charts, Explore, downloads | Streaming |
| `podcasts` | Podcast search, subscriptions, saved episodes | Streaming |
| `audiobooks` | The Audiobooks section: books, series, authors, its settings | Home, More |
| `subsonic` | A Subsonic / Navidrome server | Streaming |
| `radio` | Internet radio and the station directory | Streaming |
| `calculator` | The calculator | More |
| `notes` | The notes shelf and its editor | More |
| `flappy` | Flappy Bird | More |
| `gearboy` | The Game Boy emulator, its settings and player | More |
| `ebook` | The Books shelf and the EPUB reader | More |

## How the gate is used

Two rules, both in the modules' own files rather than in the manager:

1. **The page is built only when the module is on.** `gui_init()` asks
   `addons_enabled("calculator")` before `calculator_init()`, and the streaming
   page asks before each `*page_init()`. A module that is off leaves its screen
   as the empty object `gui_init()` allocates.
2. **The row that opens it is not drawn.** `morepage.c` asks
   `more_addon_of(slug)` and skips the entry; `streaming.c` multiplies the
   arranged visibility by `addons_enabled(item->addon)`, so the tile is hidden
   even if it was arranged to the top.

Because both questions are asked, a disabled module is unreachable through the
interface. Nothing calls `switch_screen()` with a page that was never built.

## Adding a module

1. A row in `src/system/addons/addons.c`: `{"mything", "mything", "addon_mything_note"}`.
2. Two strings: `addon_mything_note` (one line) and the module's own label, in
   every language file — the label is the row's name and must already exist if
   the feature had one.
3. The gate at the two places: `if (addons_enabled("mything")) mything_init(cfg);`
   in `gui_init()` (or in the section's own init), and the row in the menu that
   opens it.
4. Nothing else. The switch, the caption and the restart hint appear on the
   Plugins page on their own.

## What this does not do yet

- **Flash.** The module's code stays in the binary. Dropping it from the image is
  a build-time decision, not a switch: a `FEATURES` make variable that compiles
  the excluded `src/` directories out would be the next step, and the addon ids
  are the names that variable would use.
- **Installing code later.** Loading a `.so` from the card would need
  `-rdynamic`, a versioned ABI and a toolchain that builds shared objects for the
  device; the player's build is static today. The switch model is the honest
  half of "install only what you need": leave out what you do not run.
- **Languages.** Only the chosen language file is loaded, so switching language
  already costs nothing at runtime. Trimming the unused `.ini` files out of the
  package is a packaging decision, not a player one.
