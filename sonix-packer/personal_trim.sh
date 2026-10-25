#!/bin/bash
#
# Personal build only -- this file is not part of what upstream ships.
#
# The image this fork's owner carries: English and Polish in the language
# picker, and MiSans as the only UI face. <redacted>
# upstream needs every language and every face, so main keeps the complete
# asset tree; only the *staged root* of a personal build is trimmed here.
#
# The player needs no source change for any of this, which is the point:
#
#   * lang.c reads the language directory, so a .ini that is not there is a
#     language the picker never offers ("the list is the directory");
#   * fonts_choices() skips a face whose files are absent (see the table in
#     fonts.c), so a Neon.ttf that is not there is a face that is not offered.
#
# So the cost of merging upstream is this script and nothing else, and the
# result is a smaller squashfs and a shorter Settings page.
#
# Usage: personal_trim.sh <staged rootfs>
#   Called by sonix_firmware_packer.sh just before it builds the filesystem,
#   and only on a branch that carries this file.

set -euo pipefail

ROOT="${1:-}"
[ -n "$ROOT" ] || { echo "personal_trim: usage: $0 <staged rootfs>" >&2; exit 2; }
[ -d "$ROOT" ] || { echo "personal_trim: $ROOT is not a directory" >&2; exit 2; }

RES="$ROOT/usr/resource/sonix"
LANG_DIR="$RES/language"
FONT_DIR="$RES/fonts"

# What stays; everything else of its kind goes.
KEEP_LANGS="English.ini Polski.ini"
# MiSans regular and bold, and the licence that has to travel with them.
KEEP_FONTS="default.otf default.ttf bold.otf bold.ttf FONT-LICENSES.txt"

# The e-book reader's own reading face (fonts/ebook/) is not a UI face and is
# left alone: it is what the text viewer draws books with.

removed=0

keeps() { # keeps <name> <space-separated keep list>
	case " $2 " in
		*" $1 "*) return 0 ;;
	esac
	return 1
}

if [ -d "$LANG_DIR" ]; then
	for file in "$LANG_DIR"/*.ini; do
		[ -e "$file" ] || continue
		keeps "$(basename "$file")" "$KEEP_LANGS" && continue
		rm -f "$file"
		removed=$((removed + 1))
	done
fi

# Top level only: the families the UI font picker offers. A broken card font
# is the reader's own business and is not shipped here at all.
if [ -d "$FONT_DIR" ]; then
	for file in "$FONT_DIR"/*.ttf "$FONT_DIR"/*.otf; do
		[ -e "$file" ] || continue
		keeps "$(basename "$file")" "$KEEP_FONTS" && continue
		rm -f "$file"
		removed=$((removed + 1))
	done
fi

# Two things a personal image cannot do without, said out loud rather than
# patched over: a build that lost its assets quietly is worse than one that
# stops here, where the log can be read.
if [ -d "$LANG_DIR" ] && [ ! -f "$LANG_DIR/English.ini" ]; then
	echo "personal_trim: no English.ini in $LANG_DIR -- the interface would show bare tags" >&2
	exit 1
fi
if [ -d "$FONT_DIR" ] && ! compgen -G "$FONT_DIR/default.*" > /dev/null; then
	echo "personal_trim: no default (MiSans) font in $FONT_DIR -- nothing left to draw with" >&2
	exit 1
fi

say_langs="$(cd "$LANG_DIR" 2>/dev/null && ls *.ini 2>/dev/null | tr '\n' ' ' || true)"
say_faces="$(cd "$FONT_DIR" 2>/dev/null && ls *.ttf *.otf 2>/dev/null | tr '\n' ' ' || true)"
echo "    personal trim: $removed file(s) removed"
echo "    languages: ${say_langs:-none}"
echo "    UI faces:  ${say_faces:-none}"
