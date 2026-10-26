#ifndef WIN_COMPAT_HOST_H
#define WIN_COMPAT_HOST_H

// Windows-host shims for the few POSIX calls the streaming modules use. The
// device's glibc has all of them; mingw does not. Included by force
// (-include) when a test suite links real module code on a Windows host, and
// compiled nowhere on Linux, where the real calls exist.

#ifdef _WIN32

#include <direct.h>
#include <io.h>
#include <ctype.h>
#include <time.h>

#define mkdir(path, mode) win_mkdir((path), (mode))
#define lstat(path, st) stat((path), (st))
#define fchmod(fd, mode) win_fchmod((fd), (mode))
#define gmtime_r(timep, result) win_gmtime_r((timep), (result))
#define strcasestr(haystack, needle) win_strcasestr((haystack), (needle))

int win_mkdir(const char *path, int mode);
int win_fchmod(int fd, unsigned mode);
struct tm *win_gmtime_r(const time_t *timep, struct tm *result);
char *win_strcasestr(const char *haystack, const char *needle);

#endif /* _WIN32 */

#endif /* WIN_COMPAT_HOST_H */
