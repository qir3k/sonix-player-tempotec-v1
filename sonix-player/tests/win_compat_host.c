// The shims win_compat_host.h declares. Windows host only: on Linux this file
// compiles to nothing, and the tests keep using the real calls.
#ifdef _WIN32

#include "win_compat_host.h"

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <windows.h>

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

struct tm *win_localtime_r(const time_t *timep, struct tm *result) {
	return localtime_s(result, timep) == 0 ? result : NULL;
}

int win_fsync(int fd) {
	// mingw has no fsync; _commit flushes the same descriptor to the disk.
	return _commit(fd);
}

int win_usleep(unsigned usec) {
	Sleep(usec / 1000u);
	return 0;
}

char *win_mkdtemp(char *tmpl) {
	// mingw has no mkdtemp: fill the XXXXXX in and take the first folder that
	// does not already exist, the same contract the POSIX call has.
	static unsigned seed;
	if (!tmpl) return NULL;
	size_t len = strlen(tmpl);
	if (len < 6 || strcmp(tmpl + len - 6, "XXXXXX") != 0) return NULL;
	for (unsigned attempt = 0; attempt < 1000; attempt++) {
		unsigned r = GetTickCount() ^ (seed++ * 2654435761u) ^ GetCurrentProcessId();
		for (int i = 0; i < 6; i++) {
			tmpl[len - 6 + (size_t)i] = (char)('a' + (r % 26));
			r /= 26;
		}
		if (_mkdir(tmpl) == 0) return tmpl;
	}
	return NULL;
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
