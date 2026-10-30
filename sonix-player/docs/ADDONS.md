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
description and a switch, grouped under the headings the module list itself
carries -- **Streaming services**, **Music features**, **Library**, **Tools**
-- so a module added later lands under the right heading without the page
knowing its id. Music features is what the player does with a track (lyrics,
AutoEQ, Last.fm, ListenBrainz); Library is the shelves (audiobooks, books);
Tools is what is not music at all.
Switching one writes `[addons] <id> = true|false` into `device_config.ini` and
shows a *Restart to apply* toast: a page that has already been built cannot give
its memory back, so the saving is real from the next start.

Below the modules, under **Interface**, sit the choices that are not modules --
the favourite mark every page draws, the destination that takes the home
screen's library tile while Audiobooks is off, and the Languages & Fonts page.
They cost one bitmap or one config line and apply at once, and the page says so
rather than putting them beside switches that need a restart.

**Themes** has a heading of its own at the bottom of the page, with a switch for
each palette this build carries. A theme is not a module either: nothing is
built for one, so its switch applies at once. It is the one list where a switch
cannot go off freely -- an interface with no palette has nothing to draw with --
so the last enabled theme refuses to be switched off, and turning off the one on
screen hands over to the first still enabled straight away. A card whose theme
has since been switched off is put right at startup by `theme_init()`, which is
what makes a hand-edited config safe.

## The modules

| id | what it brings | where it is offered |
|---|---|---|
| `tidal` | Tidal pages, playlists, offline cache | Streaming |
| `qobuz` | Qobuz pages, playlists, offline cache | Streaming |
| `ytmusic` | YouTube Music search, charts, Explore, downloads | Streaming |
| `podcasts` | Podcast search, subscriptions, saved episodes | Streaming |
| `audiobooks` | The Audiobooks section: books, series, authors, its settings | Library |
| `ebook` | The Books shelf and the EPUB reader | Library |
| `lyrics` | The lyrics pane and the online lyrics search (Lyrics Fetcher) | Music features |
| `artwork` | Cover Art Fetcher: the online sleeve search and its tag writing | Music features |
| `eq` | The graphic equaliser page and the tone controls built on it | Music features |
| `peq` | The parametric equaliser and its presets | Music features |
| `mseb` | The MSEB sliders | Music features |
| `sleep_timer` | The sleep timers of every page and the control centre's | Music features |
| `autoeq` | AutoEQ and its headphone curves | Music features |
| `lastfm` | Scrobbling to Last.fm | Music features |
| `listenbrainz` | Scrobbling to ListenBrainz, on its own switch | Music features |
| `subsonic` | A Subsonic / Navidrome server | Streaming |
| `radio` | Internet radio and the station directory | Streaming |
| `airpods` | AirPods and Beats: battery, noise control, stem presses, and the row that opens the page | Earbuds |
| `earfun` | EarFun earbuds over GAIA: battery, noise control, game mode, in-ear detection | Earbuds |
| `metronome` | The metronome | More |
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

1. A row in `src/system/addons/addons.c`:
   `{"mything", "mything", "addon_mything_note", "plugins_section_tools"}` --
   id, label tag, description tag, and the Plugins heading it belongs under.
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
  already costs nothing at runtime, and which languages the picker *offers* is a
  switch under Plugins -> Languages & Fonts rather than something a build has to
  remove: every `.ini` stays in the package and each one is one config line. The
  same page switches the faces the font picker lists and the script fallbacks
  behind them, so a barebones player and a fully loaded one are one image.
