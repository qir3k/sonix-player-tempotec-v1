// Headless checks of the notes folder: which files in it are notes, the order
// they are listed in, the names they are written under, and the decoding of
// what is in them.
//
// The folder and the reading/writing are plain C (src/system/notes and
// src/system/text), which is what makes them testable here: a note dropped on
// the card by a computer is read with whatever a computer wrote -- a Windows
// line ending, a byte order mark, an ANSI accent -- and none of that needs a
// screen to be checked. The card is a temporary folder, thrown away at the end.

#include <assert.h>
#include <dirent.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "src/system/notes/notesstore.h"
#include "src/system/text/textfile.h"

static char dir[512];

static void write_bytes(const char *name, const void *data, size_t size) {
	char path[1024];
	// The folder is made here rather than by notes_write(): these files stand
	// in for the ones a computer put on the card, and what notes_write makes
	// is checked on its own below.
	if (mkdir(dir, 0755) != 0) {
		struct stat st;
		assert(stat(dir, &st) == 0 && S_ISDIR(st.st_mode));
	}
	assert(notes_path(path, sizeof(path), dir, name));
	FILE *file = fopen(path, "wb");
	assert(file);
	assert(size == 0 || fwrite(data, 1, size, file) == size);
	assert(fclose(file) == 0);
}

static void write_text(const char *name, const char *text) {
	write_bytes(name, text, strlen(text));
}

static char *read_note(const char *name, bool *truncated) {
	char path[1024];
	assert(notes_path(path, sizeof(path), dir, name));
	return notes_read(path, truncated);
}

static void expect_note(const char *name, const char *expected) {
	bool truncated = true;
	char *text = read_note(name, &truncated);
	assert(text);
	assert(!truncated);
	if (strcmp(text, expected) != 0) {
		fprintf(stderr, "%s: expected \"%s\", read \"%s\"\n", name, expected, text);
		assert(!"wrong text");
	}
	free(text);
}

/* ---- the folder ---------------------------------------------------------- */

static void test_names(void) {
	// Notes are the text files the file manager already opens, plus Markdown.
	assert(notes_is_note_name("shopping.txt"));
	assert(notes_is_note_name("recipe.md"));
	assert(notes_is_note_name("lyrics.LRC"));
	assert(notes_is_note_name("release.nfo"));
	assert(notes_is_note_name("settings.ini"));
	// Not notes: a picture, a half-written file, a hidden one, a folder's own
	// name, and an extension the page would not know how to draw.
	assert(!notes_is_note_name("cover.jpg"));
	assert(!notes_is_note_name("half-written.tmp"));
	assert(!notes_is_note_name(".hidden.txt"));
	assert(!notes_is_note_name(".txt"));
	assert(!notes_is_note_name("no-extension"));
	assert(!notes_is_note_name(""));
	assert(!notes_is_note_name(NULL));

	// A long name is still a note: the store holds what a card can carry, and
	// a row cuts it to fit when it draws it.
	char long_name[NOTES_NAME_MAX + 16];
	memset(long_name, 'a', NOTES_NAME_MAX);
	memcpy(long_name + NOTES_NAME_MAX - 4, ".txt", 5); // ".txt" and its terminator
	assert(strlen(long_name) == NOTES_NAME_MAX);
	assert(notes_is_note_name(long_name));
	// One character more than the store can hold is not a note.
	long_name[NOTES_NAME_MAX] = 'a';
	long_name[NOTES_NAME_MAX + 1] = '\0';
	assert(!notes_is_note_name(long_name));

	// The file a name is written as.
	char file[NOTES_NAME_MAX + 8];
	assert(notes_file_name("list", file, sizeof(file)) && strcmp(file, "list.txt") == 0);
	assert(notes_file_name("list.md", file, sizeof(file)) && strcmp(file, "list.md") == 0);
	assert(notes_file_name("list.TXT", file, sizeof(file)) && strcmp(file, "list.TXT") == 0);
	// A dot that is part of the name, not an extension the page knows: the
	// name is kept and .txt added after it.
	assert(notes_file_name("2026.10.16", file, sizeof(file)) && strcmp(file, "2026.10.16.txt") == 0);
	assert(notes_file_name("holiday.photos", file, sizeof(file)) && strcmp(file, "holiday.photos.txt") == 0);
	assert(!notes_file_name(NULL, file, sizeof(file)));
	assert(!notes_file_name("list", file, 4)); // no room for the extension

	// Names that must not reach the filesystem.
	assert(notes_name_ok("shopping"));
	assert(notes_name_ok("groceries for sunday"));
	assert(!notes_name_ok(""));
	assert(!notes_name_ok("   "));
	assert(!notes_name_ok("a/b"));
	assert(!notes_name_ok("a\\b"));
	assert(!notes_name_ok(".."));
	assert(!notes_name_ok("line\nbreak"));
	assert(!notes_name_ok(NULL));

	// What a row shows: the name without the extension it is stored under.
	char shown[NOTES_NAME_MAX + 1];
	notes_display_name("list.txt", shown, sizeof(shown));
	assert(strcmp(shown, "list") == 0);
	notes_display_name("2026.10.16.txt", shown, sizeof(shown));
	assert(strcmp(shown, "2026.10.16") == 0);
	notes_display_name("recipe.md", shown, sizeof(shown));
	assert(strcmp(shown, "recipe") == 0);
	notes_display_name("no-extension", shown, sizeof(shown));
	assert(strcmp(shown, "no-extension") == 0);
	notes_display_name(NULL, shown, sizeof(shown));
	assert(strcmp(shown, "") == 0);

	// The paths the page builds.
	char path[512];
	assert(notes_dir(path, sizeof(path), "/sdcard"));
	assert(strcmp(path, "/sdcard/Notes") == 0);
	assert(!notes_dir(path, 4, "/sdcard"));
	assert(!notes_dir(path, sizeof(path), NULL));
	assert(notes_path(path, sizeof(path), "/sdcard/Notes", "list.txt"));
	assert(strcmp(path, "/sdcard/Notes/list.txt") == 0);
	assert(!notes_path(path, sizeof(path), "/sdcard/Notes", ""));
}

static void test_listing(void) {
	char path[1024];
	note_entry_t entries[NOTES_MAX_ENTRIES];

	// A card with no Notes folder on it: an empty list, not a failure.
	assert(notes_list(dir, entries, NOTES_MAX_ENTRIES) == 0);

	write_text("shopping.txt", "milk\n");
	write_text("Apple.txt", "first\n");
	write_text("apple-note.txt", "second\n");
	write_text("note-10.txt", "tenth\n");
	write_text("note-2.txt", "second note\n");
	write_text("recipe.md", "# soup\n");
	write_text("cover.jpg", "not a note");
	write_text("leftover.tmp", "not a note");
	write_text(".hidden.txt", "not a note");
	// A directory wearing a note's name is not offered to the editor, which
	// would be offering to write over it.
	snprintf(path, sizeof(path), "%s/folder.txt", dir);
	assert(mkdir(path, 0755) == 0);

	assert(notes_list(dir, entries, NOTES_MAX_ENTRIES) == 6);
	// A-Z, case-insensitively, and digits read as numbers: note-2 before
	// note-10, which is how a folder of notes reads as the order they were
	// written in.
	// "apple-note.txt" first: the comparison is case-insensitive, so the
	// punctuation decides, and a hyphen sorts before a dot -- the same order
	// `sort -f` gives. Only the case differs between them.
	assert(strcmp(entries[0].name, "apple-note.txt") == 0);
	assert(strcmp(entries[1].name, "Apple.txt") == 0);
	assert(strcmp(entries[2].name, "note-2.txt") == 0);
	assert(strcmp(entries[3].name, "note-10.txt") == 0);
	assert(strcmp(entries[4].name, "recipe.md") == 0);
	assert(strcmp(entries[5].name, "shopping.txt") == 0);

	// The list is bounded, and says so by its own length. Which notes arrive
	// in a bounded list is the folder's business -- the page asks for all of
	// them, and only says how many there were -- so only the count and the
	// fact that they are notes are checked here.
	assert(notes_list(dir, entries, 2) == 2);
	assert(notes_is_note_name(entries[0].name));
	assert(notes_is_note_name(entries[1].name));
	assert(notes_list(dir, entries, 0) == 0);
	assert(notes_list(NULL, entries, NOTES_MAX_ENTRIES) == 0);
}

/* ---- reading and writing ------------------------------------------------- */

static void test_round_trip(void) {
	// A note written here reads back exactly.
	const char *text = "shopping\nmilk\nbread\n";
	assert(notes_write(dir, "list", text));
	expect_note("list.txt", text);

	// The folder is made by the first write: a card that has never had one.
	char fresh[600];
	snprintf(fresh, sizeof(fresh), "%s/nested-new", dir);
	assert(notes_write(fresh, "first", "hello\n"));
	char fresh_path[1024];
	assert(notes_path(fresh_path, sizeof(fresh_path), fresh, "first.txt"));
	bool truncated = true;
	char *read = notes_read(fresh_path, &truncated);
	assert(read && !truncated && strcmp(read, "hello\n") == 0);
	free(read);

	// An empty note is a note: the page makes one before it opens the editor.
	assert(notes_write(dir, "blank", ""));
	expect_note("blank.txt", "");

	// A name that already carries an extension is not given a second one.
	assert(notes_write(dir, "recipe.md", "# soup\n"));
	expect_note("recipe.md", "# soup\n");
	assert(notes_write(dir, "2026.10.16", "a date\n"));
	expect_note("2026.10.16.txt", "a date\n");

	// A note is written whole or not at all, and overwriting one replaces it
	// rather than appending to it.
	assert(notes_write(dir, "list", "short\n"));
	expect_note("list.txt", "short\n");

	// Nothing is written under a name that cannot be a file.
	assert(!notes_write(dir, "a/b", "x"));
	assert(!notes_write(dir, "", "x"));
	// A write leaves no temporary of its own behind, whatever happened to it:
	// the folder is somebody's card, not a scratch space. (The folder does
	// hold a .tmp file of its own here -- the one test_listing put there, to
	// stand in for a write that was interrupted. That is not this file.)
	DIR *handle = opendir(dir);
	assert(handle);
	struct dirent *entry;
	while ((entry = readdir(handle)) != NULL) {
		const char *name = entry->d_name;
		size_t len = strlen(name);
		assert(len < 8 || strcmp(name + len - 8, ".txt.tmp") != 0);
	}
	closedir(handle);
}

static void test_decoding(void) {
	// UTF-8 with a byte order mark, as a Windows editor writes it: the mark is
	// not drawn.
	write_bytes("bom.txt", "\xef\xbb\xbfhello\n", 9);
	expect_note("bom.txt", "hello\n");

	// CRLF line endings become LF, and a tab becomes four spaces.
	write_bytes("crlf.txt", "first\r\n\tsecond\r\n", 17);
	expect_note("crlf.txt", "first\n    second\n");

	// UTF-16 with a byte order mark, which is what a Windows text file is
	// called when it is saved as "Unicode".
	write_bytes("utf16.txt", "\xff\xfeh\x00i\x00\r\x00\n\x00", 10);
	expect_note("utf16.txt", "hi\n");

	// A file that is not valid UTF-8 is read as Windows-1252: an accent typed
	// on a Windows machine arrives as the letter it looks like, in UTF-8.
	write_bytes("ansi.txt", "caf" "\xe9" " " "\x93" "quoted" "\x94" "\n", 14);
	expect_note("ansi.txt", "caf\xc3\xa9 \xe2\x80\x9cquoted\xe2\x80\x9d\n");

	// Control characters a text file should not have are left out rather than
	// drawn as boxes, and a NUL does not cut the note short.
	// One escape per byte: C reads \x01b as a single hexadecimal escape, so
	// consecutive escapes have to be closed off.
	write_bytes("control.txt", "a" "\x01" "b" "\x00" "c" "\x1b" "d" "\n", 8);
	expect_note("control.txt", "abcd\n");

	// A file that is not there, and one that is not a text file at all.
	assert(read_note("missing.txt", NULL) == NULL);
	write_bytes("empty.txt", "", 0);
	expect_note("empty.txt", "");

	// A note longer than the editor will hold is read up to the ceiling, and
	// says that it was cut: the editor refuses it rather than silently
	// dropping the end of what somebody wrote.
	char *big = malloc(NOTES_MAX_BYTES + 32);
	assert(big);
	memset(big, 'a', NOTES_MAX_BYTES + 32);
	write_bytes("long.txt", big, NOTES_MAX_BYTES + 32);
	free(big);
	bool truncated = false;
	char *text = read_note("long.txt", &truncated);
	assert(text);
	assert(truncated);
	assert(strlen(text) == NOTES_MAX_BYTES);
	free(text);
}

static void test_default_name(void) {
	note_entry_t entries[NOTES_MAX_ENTRIES];
	char name[NOTES_NAME_MAX + 1];

	// The name a new note opens with is free, so that saving it does not land
	// on top of the note before it.
	notes_default_name(dir, name, sizeof(name));
	assert(name[0]);
	assert(notes_name_ok(name));
	assert(!notes_is_note_name(name)); // no extension yet: it is a name, not a file

	char path[1024];
	assert(notes_path(path, sizeof(path), dir, name));

	// Taken once, the next name is not the same one.
	assert(notes_write(dir, name, "first\n"));
	char first[NOTES_NAME_MAX + 1];
	snprintf(first, sizeof(first), "%s", name);
	notes_default_name(dir, name, sizeof(name));
	assert(strcmp(name, first) != 0);
	assert(notes_name_ok(name));

	// And the suggestion is never a name that is already a note.
	for (int i = 0; i < 3; i++) {
		assert(notes_write(dir, name, "x\n"));
		notes_default_name(dir, name, sizeof(name));
		char file[NOTES_NAME_MAX + 8];
		assert(notes_file_name(name, file, sizeof(file)));
		bool taken = notes_list(dir, entries, NOTES_MAX_ENTRIES) > 0;
		assert(taken);
		bool found = false;
		for (int e = 0; e < NOTES_MAX_ENTRIES; e++) {
			if (strcmp(entries[e].name, file) == 0) {
				found = true;
				break;
			}
		}
		assert(!found); // the suggestion is a name nobody has used
	}
}

// What the editor will hold, which is what decides whether a note is opened
// to be written or only to be read. Characters rather than bytes: the note of
// a Japanese or Russian user is three bytes a letter and has exactly as many
// characters as they typed.
static void test_fits_editor(void) {
	static char text[NOTES_MAX_CHARS * 4 + 8];

	// Empty, and one character short of the ceiling: both fit, and a character
	// is measured by what it is, not by how many bytes it takes.
	assert(notes_fits_editor(""));
	memset(text, 'a', NOTES_MAX_CHARS);
	text[NOTES_MAX_CHARS] = '\0';
	assert(notes_fits_editor(text)); // exactly the limit

	text[NOTES_MAX_CHARS] = 'a';
	text[NOTES_MAX_CHARS + 1] = '\0';
	assert(!notes_fits_editor(text)); // one over it

	// The same count in three-byte letters -- あ -- is still the limit.
	char *p = text;
	for (size_t i = 0; i < NOTES_MAX_CHARS; i++) {
		*p++ = (char)0xE3; // U+3042 in UTF-8
		*p++ = (char)0x81;
		*p++ = (char)0x82;
	}
	*p = '\0';
	assert(strlen(text) == NOTES_MAX_CHARS * 3);
	assert(notes_fits_editor(text)); // three bytes a letter, the limit in letters
	*p++ = (char)0xE3;
	*p++ = (char)0x81;
	*p++ = (char)0x82;
	*p = '\0';
	assert(!notes_fits_editor(text));

	// NULL is not a note.
	assert(!notes_fits_editor(NULL));
}

int main(void) {
	char root[600] = "/tmp/sonix-notes-XXXXXX";
	assert(mkdtemp(root));
	assert(notes_dir(dir, sizeof(dir), root));

	test_names();
	test_listing();
	test_round_trip();
	test_decoding();
	test_default_name();
	test_fits_editor();

	// The card is temporary; leave nothing behind for the next run.
	char command[1200];
	snprintf(command, sizeof(command), "rm -rf '%s'", root);
	assert(system(command) == 0);
	printf("PASS notes folder\n");
	return 0;
}
