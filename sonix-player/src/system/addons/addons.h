#ifndef ADDONS_H
#define ADDONS_H

#include <stdbool.h>

// The optional modules of the player, as a list rather than a heap of #ifdefs.
//
// The player is one binary: a module's code is always linked in. What "off"
// means is that nothing of that module is *built* -- its pages are never
// created, its rows are not in the menus -- so the RAM those pages would hold
// stays free for the music. That is the memory that matters on a player with a
// fixed heap; flash is trimmed with the build instead (see docs/ADDONS.md).
//
// Each module is identified by a short id, which is also the key its switch is
// stored under in the config's [addons] section, so a switch survives updates
// and a module added later starts on.

typedef struct {
	const char *id;    // "tidal", "calculator": the config key and the gate
	const char *label; // the row's name, a tag or a literal ("YouTube Music")
	const char *note;  // one line on what it adds, a tag
} addon_t;

// The modules this build carries, in the order the Plugins page lists them.
int addons_count(void);
const addon_t *addons_at(int index);
const addon_t *addons_find(const char *id);

// Whether a module is switched on. Unknown ids answer true: a gate is a way to
// leave something out, and a misspelt one must not quietly remove a feature.
bool addons_enabled(const char *id);

// Switches one on or off and stores it. The caller says it needs a restart --
// a page that has already been built cannot give its memory back.
void addons_set(const char *id, bool on);

// Defaults, in order, for a build that wants to start from a smaller set.
void addons_apply_defaults(bool (*is_default)(const char *id));

#endif /* ADDONS_H */
