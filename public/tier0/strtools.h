// tier0 -- clean-room functional reconstruction of GoldSrc tier0.dll (GPL-3.0).
#ifndef STRTOOLS_H
#define STRTOOLS_H

#ifdef _LINUX
#include <wchar.h>
#include <string.h>
#ifndef _MSC_VER
#include <strings.h>
#endif
#include <stdlib.h>
#include <stdio.h>

#ifndef _snprintf
#define _snprintf snprintf
#endif
#ifndef _vsnprintf
#define _vsnprintf vsnprintf
#endif
#ifndef _stricmp
#define _stricmp strcasecmp
#endif
#ifndef _strnicmp
#define _strnicmp strncasecmp
#endif

#ifndef TCHAR
#define TCHAR char
#endif
#ifndef _TCHAR_DEFINED
#define _TCHAR_DEFINED
#endif
// tchar typedef only if not already provided by platform.h (TCHAR_IS_CHAR guard)
#ifndef TCHAR_IS_CHAR
typedef char tchar;
#endif

#ifndef _T
#define _T(x) x
#endif
#ifndef _TEXT
#define _TEXT(x) x
#endif

#ifndef _tcsicmp
#define _tcsicmp strcasecmp
#endif
#ifndef _tcsnicmp
#define _tcsnicmp strncasecmp
#endif
#ifndef _tcscpy
#define _tcscpy strcpy
#endif
#ifndef _tcsncpy
#define _tcsncpy strncpy
#endif
#ifndef _tcslen
#define _tcslen strlen
#endif
#ifndef _tcscat
#define _tcscat strcat
#endif
#ifndef _tcsrchr
#define _tcsrchr strrchr
#endif
#ifndef _tcschr
#define _tcschr strchr
#endif
#ifndef _tcsstr
#define _tcsstr strstr
#endif
#ifndef _tcscmp
#define _tcscmp strcmp
#endif
#ifndef _tcsncmp
#define _tcsncmp strncmp
#endif
#ifndef _tprintf
#define _tprintf printf
#endif
#ifndef _ftprintf
#define _ftprintf fprintf
#endif
#ifndef _sntprintf
#define _sntprintf snprintf
#endif
#ifndef _vsntprintf
#define _vsntprintf vsnprintf
#endif
#ifndef _stprintf
#define _stprintf sprintf
#endif

#else // !_LINUX (Windows)
#include <tchar.h>
#include <string.h>
#include <stdlib.h>
#ifndef _UNICODE
#define _tcsicmp _stricmp
#define _tcscpy strcpy
#define _tcsrchr strrchr
#endif
#endif // _LINUX

#define V_strncpy		strncpy
#define Q_snprintf		_snprintf
#define Q_strcpy_s		strcpy_s

#endif // STRTOOLS_H
