# Three branches: his, yours, and the one in your pocket

This fork keeps upstream's tree intact and does the trimming at build time, so
you can keep working with mr-f0xx on the full thing and still carry a lean
image of your own. Nothing in `main` is ever deleted for your convenience; the
only branch that differs is `personal`, and it differs by one script, a stamp
of the version it is released as, and the line of workflow that makes a push
to it cut a release.

| Branch      | What it is                                                       | Builds an image?                     |
| ----------- | ---------------------------------------------------------------- | ------------------------------------ |
| `main`      | Upstream's `main` merged with your finished features. The only branch a pull request to mr-f0xx comes from. | Yes, on every push (already set up). |
| `dev`       | Where features are written and tried before they are good enough for `main`. | On demand: Actions → Run workflow, `build_ref = dev`. |
| `personal`  | `dev` plus `personal_trim.sh`: Polish and English only, MiSans only, its own version and update source. What you actually flash. | Yes, on every push (this branch adds itself to the workflow's push list). |

The flow is one direction:

```
mr-f0xx/main ──fetch──> main ──merge──> dev ──merge──> personal
                          ^              │
                          └── merge ─────┘        (and a PR from main back to mr-f0xx)
```

## One-time setup

Both remotes are already in this checkout:

```bash
git remote -v
# origin    https://github.com/qir3k/sonix-player-tempotec-v1.git      (yours)
# upstream  https://github.com/mr-f0xx/sonix-player-tempotec-v1.git    (his)
```

If you ever rebuild the clone, add his again with:

```bash
git remote add upstream https://github.com/mr-f0xx/sonix-player-tempotec-v1.git
```

## Bringing his work in

```bash
git switch main
git fetch upstream
git merge upstream/main
git push origin main
```

GitHub's **Sync fork** button on your fork does the same thing, then `git pull`
here. From that point:

```bash
git switch dev
git merge main          # dev follows main
```

Do it before starting a feature rather than in the middle of one: a merge with
uncommitted work is the one that produces a mess.

## A feature, from idea to his repo

```bash
git switch dev
# ...edit, build, commit...
git commit -am "DAC page: centre the status copy"
git push origin dev

# try it on the device
#   Actions -> Build TempoTec V1 firmware -> Run workflow
#   Use workflow from: dev   |   build_ref: dev   |   acknowledge: ticked
#   publish_release_tag: empty

# when it works, it belongs in main
git switch main
git merge dev
git push origin main        # this also builds an image of main

# and from the fork's main to his:
#   github.com/mr-f0xx/sonix-player-tempotec-v1
#   Compare & pull request <- qir3k:main
```

A pull request from `main` shows only the commits upstream does not have, even
though `main` also carries your merges of his work.

If you would rather not propose the whole of `main`, cut a topic branch from it
and push that instead:

```bash
git switch -c topic/dac-status main
git push -u origin topic/dac-status
# PR: qir3k:topic/dac-status -> mr-f0xx:main
```

## The personal branch

```bash
git switch personal
git merge dev               # or main, if you only want what you have proposed
git push origin personal    # tags v1.0.x, builds v1.upt, publishes the release
```

That is the whole loop: merge, push, wait for the green run, flash. The patch
number goes up by one every push, and the image carries that same number, so
the device knows when it is already on the newest release.

### What it takes out, and why it is safe

`sonix-packer/personal_trim.sh` runs inside the packer just before the
filesystem is built, and deletes from the **staged root**, never from the
repository:

- **Languages** — everything but `English.ini` and `Polski.ini`. `lang.c`
  builds its picker from the language directory ("one in is all it takes to add
  a language: the list is the directory"), so a removed file is a language that
  is simply not offered.
- **UI fonts** — everything but `default` (MiSans) and `bold`, plus the licence
  file. `fonts_choices()` already hides a face whose files are absent, so Neon
  80s and Roboto leave the *Settings → Appearance → UI font* list by
  themselves, with no source change. The same removes the Japanese, Korean,
  Arabic and Thai fallbacks and the Roboto Mono file the other model uses.
- **Left alone** — `fonts/ebook/` (Bookerly, the text viewer's own reading
  face) and every card font, which is not shipped anyway.

What you lose: track or podcast titles in Japanese, Korean, Chinese, Arabic or
Thai will draw as boxes, because their fallback faces are gone. If that ever
matters, put the file back (see below) or drop that line from the script.

### What the image says about itself

The walkie-talkie between the image and the release page is
`sonix-packer/assets/V1/usr/resource/sonix/components/system-info.json`. The
workflow copies the version into it before the packer runs, and only on
`personal`:

| Key          | Written by the personal build                        | What it does |
| ------------ | ---------------------------------------------------- | ------------ |
| `OS_version` | the tag the push is released as, e.g. `1.0.5`        | Settings → System → About shows it, and the update check compares release tags against it. |
| `ota-repo`   | `qir3k/sonix-player-tempotec-v1`                      | Settings → System → Update firmware looks here, so it finds your releases and not upstream's. |
| `forked-by`  | `qirke (github.com/qir3k)`                           | The credit line under About. |

Nothing stamps those keys on `main` or `dev`: their images keep the asset
file's values — upstream's repository and the upstream fork's credit — so a
build you hand upstream behaves exactly as upstream's does.

The tag has to exist before the image is packed, which is why the workflow
makes it first and builds second. Without that the image could only say
`beta`, which reads as version 0.0.0 to the update check and makes every
release on the page look newer than the one already on the card.

### Getting something back

```bash
# keep the CJK fallbacks after all: comment out the font loop, or add the
# names to KEEP_FONTS in sonix-packer/personal_trim.sh
git switch personal
$EDITOR sonix-packer/personal_trim.sh
git commit -am "personal: keep the CJK fallbacks"
git push origin personal
```

Adding a language is the same: add its `.ini` name to `KEEP_LANGS`.

### Merging when a conflict lands on the trim

The trim never edits an asset file, so the usual conflicts are the ordinary
ones — `player.c`, a page, a translated string. Resolve them the way you always
would, and do not accept a merge that adds `- personal` to the workflow a second
time or drops it: that one line is why a push to `personal` builds itself.

The script refuses to finish if the image ends up without `English.ini` or
without MiSans, so a mistake there stops the build instead of producing an
image full of bare tags.

## Reminders

- The firmware workflow builds a **commit**, not a folder. Uncommitted edits
  are not in the image.
- The artifact is `sonix-tempotec-v1-<sha>.zip`, kept 14 days: it holds
  `v1.upt`, `v1_md5.txt`, `BUILD-REPORT.txt` and the validation logs. Copy the
  first two together to the card's root.
- Keep an official `v1.upt` and `v1_md5.txt` on another card. That is the way
  back.
