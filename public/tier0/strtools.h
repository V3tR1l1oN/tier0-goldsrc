// tier0 -- clean-room functional reconstruction of GoldSrc tier0.dll (MIT).
#ifndef STRTOOLS_H
#define STRTOOLS_H

#include <tchar.h>
#include <string.h>
#include <stdlib.h>
#ifndef _UNICODE
#define _tcsicmp _stricmp
#define _tcscpy strcpy
#define _tcsrchr strrchr
#endif

#define V_strncpy		strncpy
#define Q_snprintf		_snprintf
#define Q_strcpy_s		strcpy_s

#endif // STRTOOLS_H
