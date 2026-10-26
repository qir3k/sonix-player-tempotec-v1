// The shims win_compat_host.h declares. Windows host only: on Linux this file
// compiles to nothing, and the tests keep using the real calls.
#ifdef _WIN32

#include "win_compat_host.h"

#include <stdio.h>
#include <string.h>

int win_mkdir(const char *path, int mode) {
	(void)mode;
	return _mkdir(path);
}

int win_fchmod(int fd, unsigned mode) {
	// The 0600 permission of the session sidecar: there is nothing on a
	// Windows host that reads it, so the call only has to succeed.
	(void)fd;
	(void)mode;
	return 0;
}

struct tm *win_gmtime_r(const time_t *timep, struct tm *result) {
	return gmtime_s(result, timep) == 0 ? result : NULL;
}

char *win_strcasestr(const char *haystack, const char *needle) {
	size_t needle_len = strlen(needle);
	if (!needle_len) return (char *)haystack;
	for (; *haystack; haystack++) {
		size_t i = 0;
		while (i < needle_len && tolower((unsigned char)haystack[i]) == tolower((unsigned char)needle[i])) {
			i++;
		}
		if (i == needle_len) return (char *)haystack;
	}
	return NULL;
}

#endif /* _WIN32 */
