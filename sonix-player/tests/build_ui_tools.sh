#!/usr/bin/env bash
# Compiles the headless LVGL UI test for the tools/More pages by hand, because
# this host has no make(1). Mirrors the tests/Makefile's "ui" tools target.
set -u
cd "$(dirname "$0")" || exit 1
export PATH=/c/msys64/mingw64/bin:$PATH

ROOT=..
BUILD=.build/ui
CONF="$(pwd)/ui/lv_conf.h"
CF="-std=gnu11 -O0 -g -D_FILE_OFFSET_BITS=64 -DGB_CORE=1 -I$ROOT -I$ROOT/lvgl -I. -include win_compat_host.h -DLV_CONF_PATH=\"$CONF\""

mkdir -p "$BUILD" || exit 1

# The GUI sources under test, with the same flags the Makefile uses.
SRC="ui/test_tools_pages.c $ROOT/src/gui/calculator/calculator.c $ROOT/src/gui/calculator/calcmodel.c \
     $ROOT/src/gui/notes/notes.c $ROOT/src/gui/notes/noteeditor.c $ROOT/src/gui/metronome/metronome.c \
     $ROOT/src/system/notes/notesstore.c $ROOT/src/system/text/textfile.c \
     $ROOT/src/gui/library/textview.c $ROOT/src/gui/library/pictureview.c \
     $ROOT/src/gui/shell/morepage.c $ROOT/src/gui/shell/gridpage.c $ROOT/src/gui/shell/menuorder.c \
     $ROOT/src/gui/shell/settingsrow.c $ROOT/src/gui/shell/keyboard.c $ROOT/src/gui/shell/popover.c \
     $ROOT/src/system/input/kblayout.c $ROOT/src/gui/shell/theme.c $ROOT/src/gui/shell/icons.c \
     $ROOT/src/system/core/config.c $ROOT/src/system/core/lang.c $ROOT/src/system/addons/addons.c \
     win_compat_host.c"

echo "== compiling the test's own sources (-Wall -Wextra -Werror) =="
OBJS=""
for f in $SRC; do
	o="$BUILD/$(echo "$f" | sed 's|^\.\./||; s|/|_|g; s|\.c$|.o|')"
	if gcc $CF -Wall -Wextra -Werror -c "$f" -o "$o" 2>>/tmp/ui_test_build.log; then
		OBJS="$OBJS $o"
	else
		echo "FAILED: $f"
	fi
done

echo "== compiling LVGL (warnings off, as upstream is not maintained here) =="
while IFS= read -r f; do
	o="$BUILD/$(echo "$f" | sed 's|^\.\./||; s|/|_|g; s|\.c$|.o|')"
	if gcc $CF -w -c "$f" -o "$o" 2>>/tmp/ui_test_build.log; then
		OBJS="$OBJS $o"
	else
		echo "FAILED: $f"
	fi
done < <(find "$ROOT/lvgl/src" -name '*.c')

echo "== linking =="
gcc -o "$BUILD/test_tools_pages" $OBJS -lpthread -lm -Wl,--gc-sections
echo "link exit=$?"
