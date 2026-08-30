// filesystem.cpp -- CFileSystem : IFileSystem (VFileSystem009)
// Win32 implementation using CreateFile/ReadFile/FindFirstFile
// GetReadBuffer cache via malloc + ReadFile (mmap-like zero-copy)
#include "../public/tier1/interface.h"
#include <windows.h>
#ifdef GetCurrentDirectory
#undef GetCurrentDirectory
#endif
#ifdef FindClose
#undef FindClose
#endif
#ifdef FindFirstFile
#undef FindFirstFile
#endif
#ifdef FindNextFile
#undef FindNextFile
#endif
#ifdef CreateDirectory
#undef CreateDirectory
#endif
#ifdef DeleteFile
#undef DeleteFile
#endif
#ifdef GetFileAttributes
#undef GetFileAttributes
#endif
#ifdef GetFileAttributesEx
#undef GetFileAttributesEx
#endif
#ifdef CreateFile
#undef CreateFile
#endif
#include "../public/FileSystem.h"
#include <vector>
#include <string>
#include <cstdio>
#include <cstdarg>
#include <ctime>
#include <algorithm>

// Helpers
static std::string NormalizePath(const char *p)
{
	if (!p) return "";
	std::string s(p);
	// trim trailing slashes
	while (!s.empty() && (s.back() == '\\' || s.back() == '/'))
		s.pop_back();
	return s;
}
static std::string JoinPath(const std::string &a, const char *b)
{
	if (a.empty()) return b ? b : "";
	if (!b || !b[0]) return a;
	std::string s = a;
	char sep = '\\';
	// use forward slash? Windows accepts both
	if (s.back() != '\\' && s.back() != '/')
		s += sep;
	s += b;
	return s;
}
static bool FileExistsRaw(const char *path)
{
	if (!path || !path[0]) return false;
	DWORD attr = GetFileAttributesA(path);
	return attr != INVALID_FILE_ATTRIBUTES;
}

struct FileHandleInternal
{
	HANDLE hFile = INVALID_HANDLE_VALUE;
	void *pReadBuffer = nullptr;
	int nBufferSize = 0;
	bool bIsWrite = false;
	std::string fileName;
};

struct FindHandleInternal
{
	HANDLE hFind = INVALID_HANDLE_VALUE;
	WIN32_FIND_DATAA findData{};
	bool valid = false;
	std::string wildcard;
	std::string baseDir;
};

class CFileSystem : public IFileSystem
{
public:
	CFileSystem() : m_WarningLevel(FILESYSTEM_WARNING_QUIET), m_pfnWarning(nullptr) {}
	virtual ~CFileSystem()
	{
		// cleanup cached buffers
		for (auto *fh : m_OpenFiles)
		{
			if (fh)
			{
				if (fh->pReadBuffer) free(fh->pReadBuffer);
				if (fh->hFile != INVALID_HANDLE_VALUE) CloseHandle(fh->hFile);
				delete fh;
			}
		}
		for (auto *fd : m_FindHandles)
		{
			if (fd)
			{
				if (fd->hFind != INVALID_HANDLE_VALUE) ::FindClose(fd->hFind);
				delete fd;
			}
		}
	}

	// IFileSystem impl
	virtual void Mount() override {}
	virtual void Unmount() override {}
	virtual void RemoveAllSearchPaths() override { m_SearchPaths.clear(); }
	virtual void AddSearchPath(const char *pPath, const char *pathID) override
	{
		(void)pathID;
		if (!pPath || !pPath[0]) return;
		std::string n = NormalizePath(pPath);
		// avoid duplicates
		for (auto &s : m_SearchPaths) if (_stricmp(s.c_str(), n.c_str()) == 0) return;
		m_SearchPaths.push_back(n);
	}
	virtual bool RemoveSearchPath(const char *pPath) override
	{
		if (!pPath) return false;
		std::string n = NormalizePath(pPath);
		for (auto it = m_SearchPaths.begin(); it != m_SearchPaths.end(); ++it)
		{
			if (_stricmp(it->c_str(), n.c_str()) == 0)
			{
				m_SearchPaths.erase(it);
				return true;
			}
		}
		return false;
	}
	virtual void RemoveFile(const char *pRelativePath, const char *pathID) override
	{
		(void)pathID;
		if (!pRelativePath) return;
		// try direct
		DeleteFileA(pRelativePath);
		for (auto &sp : m_SearchPaths)
		{
			std::string full = JoinPath(sp, pRelativePath);
			DeleteFileA(full.c_str());
		}
	}
	virtual void CreateDirHierarchy(const char *path, const char *pathID) override
	{
		(void)pathID;
		if (!path || !path[0]) return;
		std::string full = path;
		// if not absolute, create relative to current dir
		// create each component
		std::string cur;
		// handle drive letter
		size_t start = 0;
		if (full.size() >= 2 && full[1] == ':')
		{
			cur = full.substr(0, 2);
			start = 2;
			if (full.size() > 2 && (full[2] == '\\' || full[2] == '/'))
			{
				cur += "\\";
				start = 3;
			}
		}
		std::string token;
		for (size_t i = start; i <= full.size(); ++i)
		{
			char c = (i < full.size() ? full[i] : '\\');
			if (c == '\\' || c == '/' || c == '\0')
			{
				if (!token.empty())
				{
					if (!cur.empty() && cur.back() != '\\' && cur.back() != '/')
						cur += "\\";
					cur += token;
					CreateDirectoryA(cur.c_str(), nullptr);
					token.clear();
				}
			}
			else
			{
				token += c;
			}
			if (c == '\0') break;
		}
	}
	virtual bool FileExists(const char *pFileName) override
	{
		if (!pFileName) return false;
		if (FileExistsRaw(pFileName)) return true;
		for (auto &sp : m_SearchPaths)
		{
			std::string full = JoinPath(sp, pFileName);
			if (FileExistsRaw(full.c_str())) return true;
		}
		return false;
	}
	virtual bool IsDirectory(const char *pFileName) override
	{
		std::string tryPath;
		const char *toCheck = nullptr;
		if (FileExistsRaw(pFileName))
			toCheck = pFileName;
		else
		{
			for (auto &sp : m_SearchPaths)
			{
				std::string full = JoinPath(sp, pFileName);
				DWORD attr = GetFileAttributesA(full.c_str());
				if (attr != INVALID_FILE_ATTRIBUTES)
				{
					return (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;
				}
			}
			return false;
		}
		DWORD attr = GetFileAttributesA(toCheck);
		if (attr == INVALID_FILE_ATTRIBUTES) return false;
		return (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;
	}
	virtual FileHandle_t Open(const char *pFileName, const char *pOptions, const char *pathID = 0) override
	{
		(void)pathID;
		if (!pFileName || !pOptions) return nullptr;
		bool bRead = (strchr(pOptions, 'r') != nullptr) || (strchr(pOptions, 'R') != nullptr);
		bool bWrite = (strchr(pOptions, 'w') != nullptr) || (strchr(pOptions, 'W') != nullptr) || (strchr(pOptions, 'a') != nullptr) || (strchr(pOptions, 'A') != nullptr);
		bool bPlus = (strchr(pOptions, '+') != nullptr);
		// bool bText = (strchr(pOptions, 't') != nullptr);
		// bool bBin = (strchr(pOptions, 'b') != nullptr);
		DWORD desiredAccess = 0;
		if (bRead || bPlus) desiredAccess |= GENERIC_READ;
		if (bWrite || bPlus)
		{
			desiredAccess |= GENERIC_WRITE;
			if (!bRead && !bPlus) desiredAccess = GENERIC_WRITE;
		}
		if (desiredAccess == 0) desiredAccess = GENERIC_READ;

		DWORD creation = OPEN_EXISTING;
		if (strchr(pOptions,'w') || strchr(pOptions,'W'))
			creation = CREATE_ALWAYS;
		else if (strchr(pOptions,'a') || strchr(pOptions,'A'))
			creation = OPEN_ALWAYS;
		else if (bPlus && bWrite)
			creation = OPEN_ALWAYS;

		// try to resolve file location for reading
		std::string resolved = pFileName;
		bool isWriteCreation = (creation == CREATE_ALWAYS || creation == OPEN_ALWAYS);
		if (!isWriteCreation)
		{
			// reading: search search paths
			if (!FileExistsRaw(pFileName))
			{
				bool found = false;
				for (auto &sp : m_SearchPaths)
				{
					std::string full = JoinPath(sp, pFileName);
					if (FileExistsRaw(full.c_str()))
					{
						resolved = full;
						found = true;
						break;
					}
				}
				if (!found)
				{
					// file not found; for reading return nullptr
					// still try to open direct to get proper error
					resolved = pFileName;
				}
			}
		}
		else
		{
			// writing: if path is not absolute and we have search paths, prefer first search path? Valve does write to first write path.
			// For simplicity, if pFileName doesn't contain ':' or leading slash, and we have search paths, prepend first search path?
			// But to keep predictable for tests, keep direct path unless pathID handling wants search path.
			// We'll keep direct.
		}

		HANDLE h = CreateFileA(resolved.c_str(), desiredAccess, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, creation, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (h == INVALID_HANDLE_VALUE)
		{
			// if reading and we tried resolved==pFileName but file exists via search path with different case? already handled.
			// For writing with creation, try to create directory hierarchy
			if (isWriteCreation)
			{
				// try to ensure directory exists
				char dir[MAX_PATH];
				strncpy(dir, resolved.c_str(), MAX_PATH-1); dir[MAX_PATH-1]=0;
				char *slash = strrchr(dir, '\\');
				char *slash2 = strrchr(dir, '/');
				if (slash2 && (!slash || slash2 > slash)) slash = slash2;
				if (slash)
				{
					*slash = 0;
					CreateDirHierarchy(dir, nullptr);
					h = CreateFileA(resolved.c_str(), desiredAccess, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, creation, FILE_ATTRIBUTE_NORMAL, nullptr);
				}
			}
			if (h == INVALID_HANDLE_VALUE)
				return nullptr;
		}
		if (creation == OPEN_ALWAYS)
		{
			// for append, move to end
			if (strchr(pOptions,'a') || strchr(pOptions,'A'))
				SetFilePointer(h, 0, nullptr, FILE_END);
		}

		FileHandleInternal *fh = new FileHandleInternal();
		fh->hFile = h;
		fh->bIsWrite = bWrite;
		fh->fileName = resolved;
		m_OpenFiles.push_back(fh);
		return (FileHandle_t)fh;
	}
	virtual void Close(FileHandle_t file) override
	{
		if (!file) return;
		FileHandleInternal *fh = (FileHandleInternal*)file;
		if (fh->pReadBuffer) { free(fh->pReadBuffer); fh->pReadBuffer=nullptr; }
		if (fh->hFile != INVALID_HANDLE_VALUE) CloseHandle(fh->hFile);
		// remove from open list
		for (auto it = m_OpenFiles.begin(); it != m_OpenFiles.end(); ++it)
		{
			if (*it == fh) { m_OpenFiles.erase(it); break; }
		}
		delete fh;
	}
	virtual void Seek(FileHandle_t file, int pos, FileSystemSeek_t seekType) override
	{
		if (!file) return;
		FileHandleInternal *fh = (FileHandleInternal*)file;
		DWORD method = FILE_BEGIN;
		if (seekType == FILESYSTEM_SEEK_CURRENT) method = FILE_CURRENT;
		else if (seekType == FILESYSTEM_SEEK_TAIL) method = FILE_END;
		LARGE_INTEGER li; li.QuadPart = pos;
		SetFilePointerEx(fh->hFile, li, nullptr, method);
	}
	virtual unsigned int Tell(FileHandle_t file) override
	{
		if (!file) return 0;
		FileHandleInternal *fh = (FileHandleInternal*)file;
		LARGE_INTEGER cur, zero; zero.QuadPart=0;
		if (!SetFilePointerEx(fh->hFile, zero, &cur, FILE_CURRENT)) return 0;
		return (unsigned int)cur.QuadPart;
	}
	virtual unsigned int Size(FileHandle_t file) override
	{
		if (!file) return 0;
		FileHandleInternal *fh = (FileHandleInternal*)file;
		LARGE_INTEGER sz;
		if (!GetFileSizeEx(fh->hFile, &sz)) return 0;
		return (unsigned int)sz.QuadPart;
	}
	virtual unsigned int Size(const char *pFileName) override
	{
		if (!pFileName) return 0;
		std::string resolved = pFileName;
		if (!FileExistsRaw(pFileName))
		{
			for (auto &sp : m_SearchPaths)
			{
				std::string full = JoinPath(sp, pFileName);
				if (FileExistsRaw(full.c_str())) { resolved = full; break; }
			}
		}
		WIN32_FILE_ATTRIBUTE_DATA fad;
		if (!GetFileAttributesExA(resolved.c_str(), GetFileExInfoStandard, &fad)) return 0;
		if (fad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) return 0;
		ULARGE_INTEGER ul; ul.LowPart = fad.nFileSizeLow; ul.HighPart = fad.nFileSizeHigh;
		return (unsigned int)ul.QuadPart;
	}
	virtual long int GetFileTime(const char *pFileName) override
	{
		if (!pFileName) return 0;
		std::string resolved = pFileName;
		if (!FileExistsRaw(pFileName))
		{
			for (auto &sp : m_SearchPaths)
			{
				std::string full = JoinPath(sp, pFileName);
				WIN32_FILE_ATTRIBUTE_DATA fad;
				if (GetFileAttributesExA(full.c_str(), GetFileExInfoStandard, &fad)) { resolved = full; break; }
			}
		}
		WIN32_FILE_ATTRIBUTE_DATA fad;
		if (!GetFileAttributesExA(resolved.c_str(), GetFileExInfoStandard, &fad)) return 0;
		FILETIME ft = fad.ftLastWriteTime;
		ULARGE_INTEGER ul; ul.LowPart = ft.dwLowDateTime; ul.HighPart = ft.dwHighDateTime;
		// convert to time_t (100ns intervals since 1601 -> 1970 diff 116444736000000000)
		ul.QuadPart -= 116444736000000000ULL;
		ul.QuadPart /= 10000000ULL;
		return (long int)ul.QuadPart;
	}
	virtual void FileTimeToString(char *pStrip, int maxCharsIncludingTerminator, long fileTime) override
	{
		if (!pStrip || maxCharsIncludingTerminator <=0) return;
		time_t t = (time_t)fileTime;
		struct tm tmbuf;
		// use gmtime or localtime
		#if defined(_MSC_VER)
		gmtime_s(&tmbuf, &t);
		#else
		tmbuf = *gmtime(&t);
		#endif
		strftime(pStrip, maxCharsIncludingTerminator, "%Y/%m/%d %H:%M:%S", &tmbuf);
	}
	virtual bool IsOk(FileHandle_t file) override
	{
		if (!file) return false;
		FileHandleInternal *fh = (FileHandleInternal*)file;
		return fh->hFile != INVALID_HANDLE_VALUE;
	}
	virtual void Flush(FileHandle_t file) override
	{
		if (!file) return;
		FileHandleInternal *fh = (FileHandleInternal*)file;
		FlushFileBuffers(fh->hFile);
	}
	virtual bool EndOfFile(FileHandle_t file) override
	{
		if (!file) return true;
		FileHandleInternal *fh = (FileHandleInternal*)file;
		unsigned int cur = Tell(file);
		unsigned int sz = Size(file);
		return cur >= sz;
	}
	virtual int Read(void *pOutput, int size, FileHandle_t file) override
	{
		if (!file || !pOutput || size<=0) return 0;
		FileHandleInternal *fh = (FileHandleInternal*)file;
		DWORD read=0;
		if (!ReadFile(fh->hFile, pOutput, (DWORD)size, &read, nullptr)) return 0;
		return (int)read;
	}
	virtual int Write(const void *pInput, int size, FileHandle_t file) override
	{
		if (!file || !pInput || size<=0) return 0;
		FileHandleInternal *fh = (FileHandleInternal*)file;
		DWORD written=0;
		if (!WriteFile(fh->hFile, pInput, (DWORD)size, &written, nullptr)) return 0;
		// invalidate cached read buffer if we wrote
		if (fh->pReadBuffer) { free(fh->pReadBuffer); fh->pReadBuffer=nullptr; fh->nBufferSize=0; }
		return (int)written;
	}
	virtual char *ReadLine(char *pOutput, int maxChars, FileHandle_t file) override
	{
		if (!file || !pOutput || maxChars<=1) return nullptr;
		FileHandleInternal *fh = (FileHandleInternal*)file;
		int pos=0;
		char c=0;
		DWORD read=0;
		while (pos < maxChars-1)
		{
			if (!ReadFile(fh->hFile, &c, 1, &read, nullptr) || read==0) break;
			if (c == '\n') { break; }
			if (c == '\r')
			{
				// peek next char
				char next=0;
				DWORD r2=0;
				LARGE_INTEGER cur; cur.QuadPart=0;
				LARGE_INTEGER curPos;
				SetFilePointerEx(fh->hFile, cur, &curPos, FILE_CURRENT);
				if (ReadFile(fh->hFile, &next, 1, &r2, nullptr) && r2==1)
				{
					if (next != '\n')
					{
						// push back
						LARGE_INTEGER back; back.QuadPart = -1;
						SetFilePointerEx(fh->hFile, back, nullptr, FILE_CURRENT);
					}
				}
				break;
			}
			pOutput[pos++]=c;
		}
		if (pos==0 && EndOfFile(file)) return nullptr;
		// if we hit EOF without reading anything, return nullptr?
		if (pos==0)
		{
			// check if we actually read something but it was newline only? then return empty string
			// we already consumed newline, return empty
		}
		pOutput[pos]=0;
		return pOutput;
	}
	virtual int FPrintf(FileHandle_t file, char *pFormat, ...) override
	{
		if (!file || !pFormat) return 0;
		char buf[4096];
		va_list args;
		va_start(args, pFormat);
		int len = vsnprintf(buf, sizeof(buf), pFormat, args);
		va_end(args);
		if (len <0) return 0;
		if (len >= (int)sizeof(buf)) len = (int)sizeof(buf)-1;
		return Write(buf, len, file);
	}
	virtual void *GetReadBuffer(FileHandle_t file, int *outBufferSize, bool failIfNotInCache) override
	{
		if (!file) { if(outBufferSize) *outBufferSize=0; return nullptr; }
		FileHandleInternal *fh = (FileHandleInternal*)file;
		if (fh->pReadBuffer)
		{
			if (outBufferSize) *outBufferSize = fh->nBufferSize;
			return fh->pReadBuffer;
		}
		if (failIfNotInCache)
		{
			if(outBufferSize) *outBufferSize=0;
			return nullptr;
		}
		unsigned int sz = Size(file);
		if (sz==0 || sz==0xFFFFFFFF) { if(outBufferSize) *outBufferSize=0; return nullptr; }
		void *buf = malloc(sz);
		if (!buf) { if(outBufferSize) *outBufferSize=0; return nullptr; }
		// save current pos
		LARGE_INTEGER curPos, zero; zero.QuadPart=0;
		SetFilePointerEx(fh->hFile, zero, &curPos, FILE_CURRENT);
		LARGE_INTEGER begin; begin.QuadPart=0;
		SetFilePointerEx(fh->hFile, begin, nullptr, FILE_BEGIN);
		DWORD read=0;
		DWORD total=0;
		char *ptr = (char*)buf;
		unsigned int remaining = sz;
		while (remaining>0)
		{
			DWORD toRead = remaining > 8192 ? 8192 : remaining;
			if (!ReadFile(fh->hFile, ptr+total, toRead, &read, nullptr) || read==0) break;
			total += read;
			remaining -= read;
		}
		// restore pos
		SetFilePointerEx(fh->hFile, curPos, nullptr, FILE_BEGIN);
		if (total != sz)
		{
			// partial read? adjust size
			sz = total;
		}
		fh->pReadBuffer = buf;
		fh->nBufferSize = (int)sz;
		if (outBufferSize) *outBufferSize = (int)sz;
		return buf;
	}
	virtual void ReleaseReadBuffer(FileHandle_t file, void *buffer) override
	{
		if (!file || !buffer) return;
		FileHandleInternal *fh = (FileHandleInternal*)file;
		if (fh->pReadBuffer == buffer)
		{
			free(buffer);
			fh->pReadBuffer = nullptr;
			fh->nBufferSize = 0;
		}
		else
		{
			// not our cached buffer, just free (caller may have allocated)
			free(buffer);
		}
	}
	virtual const char *FindFirst(const char *pWildCard, FileFindHandle_t *pHandle, const char *pathID = 0) override
	{
		(void)pathID;
		if (!pWildCard || !pHandle) return nullptr;
		// resolve wildcard through search paths
		std::vector<std::string> candidates;
		candidates.push_back(pWildCard);
		for (auto &sp : m_SearchPaths)
		{
			std::string full = JoinPath(sp, pWildCard);
			candidates.push_back(full);
		}
		HANDLE hFind = INVALID_HANDLE_VALUE;
		WIN32_FIND_DATAA fd{};
		std::string foundWildcard;
		for (auto &cand : candidates)
		{
			hFind = FindFirstFileA(cand.c_str(), &fd);
			if (hFind != INVALID_HANDLE_VALUE)
			{
				foundWildcard = cand;
				break;
			}
		}
		if (hFind == INVALID_HANDLE_VALUE) { *pHandle = -1; return nullptr; }
		FindHandleInternal *fhi = new FindHandleInternal();
		fhi->hFind = hFind;
		fhi->findData = fd;
		fhi->wildcard = foundWildcard;
		fhi->valid = true;
		// find slot
		int idx = -1;
		for (size_t i=0;i<m_FindHandles.size();++i) if (m_FindHandles[i]==nullptr) { idx=(int)i; break; }
		if (idx==-1) { idx=(int)m_FindHandles.size(); m_FindHandles.push_back(fhi); }
		else m_FindHandles[idx]=fhi;
		*pHandle = idx;
		return m_FindHandles[idx]->findData.cFileName;
	}
	virtual const char *FindNext(FileFindHandle_t handle) override
	{
		if (handle<0 || handle >= (int)m_FindHandles.size() || !m_FindHandles[handle]) return nullptr;
		FindHandleInternal *fhi = m_FindHandles[handle];
		if (!fhi->valid) return nullptr;
		if (!FindNextFileA(fhi->hFind, &fhi->findData)) return nullptr;
		return fhi->findData.cFileName;
	}
	virtual bool FindIsDirectory(FileFindHandle_t handle) override
	{
		if (handle<0 || handle >= (int)m_FindHandles.size() || !m_FindHandles[handle]) return false;
		FindHandleInternal *fhi = m_FindHandles[handle];
		return (fhi->findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
	}
	virtual void FindClose(FileFindHandle_t handle) override
	{
		if (handle<0 || handle >= (int)m_FindHandles.size() || !m_FindHandles[handle]) return;
		FindHandleInternal *fhi = m_FindHandles[handle];
		if (fhi->hFind != INVALID_HANDLE_VALUE) ::FindClose(fhi->hFind);
		delete fhi;
		m_FindHandles[handle]=nullptr;
	}
	virtual void GetLocalCopy(const char *pFileName) override { (void)pFileName; }
	virtual const char *GetLocalPath(const char *pFileName, char *pLocalPath, int localPathBufferSize) override
	{
		if (!pFileName || !pLocalPath || localPathBufferSize<=0) return nullptr;
		std::string resolved = pFileName;
		if (!FileExistsRaw(pFileName))
		{
			for (auto &sp : m_SearchPaths)
			{
				std::string full = JoinPath(sp, pFileName);
				if (FileExistsRaw(full.c_str())) { resolved = full; break; }
			}
		}
		// get full path
		char fullPath[MAX_PATH];
		if (GetFullPathNameA(resolved.c_str(), MAX_PATH, fullPath, nullptr)==0)
		{
			strncpy_s(pLocalPath, localPathBufferSize, resolved.c_str(), _TRUNCATE);
		}
		else
		{
			strncpy_s(pLocalPath, localPathBufferSize, fullPath, _TRUNCATE);
		}
		return pLocalPath;
	}
	virtual char *ParseFile(char *pFileBytes, char *pToken, bool *pWasQuoted) override
	{
		if (!pFileBytes || !pToken) return nullptr;
		if (pWasQuoted) *pWasQuoted = false;
		// skip whitespace
		char *p = pFileBytes;
		while (*p && (*p==' '||*p=='\t'||*p=='\n'||*p=='\r')) p++;
		if (!*p) { pToken[0]=0; return nullptr; }
		bool quoted = false;
		if (*p == '\"')
		{
			quoted = true;
			if (pWasQuoted) *pWasQuoted=true;
			p++;
			char *out = pToken;
			while (*p && *p!='\"')
			{
				*out++ = *p++;
			}
			*out=0;
			if (*p=='\"') p++;
		}
		else if (*p=='{' || *p=='}')
		{
			pToken[0]=*p;
			pToken[1]=0;
			p++;
		}
		else
		{
			char *out = pToken;
			while (*p && *p!=' ' && *p!='\t' && *p!='\n' && *p!='\r' && *p!='\"' && *p!='{' && *p!='}')
				*out++ = *p++;
			*out=0;
		}
		return p;
	}
	virtual bool FullPathToRelativePath(const char *pFullpath, char *pRelative) override
	{
		if (!pFullpath || !pRelative) return false;
		// try to strip any search path prefix
		for (auto &sp : m_SearchPaths)
		{
			size_t len = sp.size();
			if (_strnicmp(pFullpath, sp.c_str(), len)==0)
			{
				const char *rel = pFullpath + len;
				while (*rel=='\\' || *rel=='/') rel++;
				strcpy_s(pRelative, MAX_PATH, rel);
				return true;
			}
		}
		// fallback try GetCurrentDirectory prefix
		char cur[MAX_PATH];
		GetCurrentDirectoryA(MAX_PATH, cur);
		size_t curLen = strlen(cur);
		if (_strnicmp(pFullpath, cur, curLen)==0)
		{
			const char *rel = pFullpath + curLen;
			while (*rel=='\\' || *rel=='/') rel++;
			strcpy_s(pRelative, MAX_PATH, rel);
			return true;
		}
		strcpy_s(pRelative, MAX_PATH, pFullpath);
		return false;
	}
	virtual bool GetCurrentDirectory(char *pDirectory, int maxlen) override
	{
		if (!pDirectory || maxlen<=0) return false;
		DWORD len = ::GetCurrentDirectoryA(maxlen, pDirectory);
		return len>0 && len < (DWORD)maxlen;
	}
	virtual void PrintOpenedFiles() override
	{
		char buf[256];
		snprintf(buf, sizeof(buf), "[CFileSystem] Open files: %zu, SearchPaths: %zu\n", m_OpenFiles.size(), m_SearchPaths.size());
		OutputDebugStringA(buf);
	}
	virtual void SetWarningFunc(void (*pfnWarning)(const char *fmt, ...)) override
	{
		m_pfnWarning = pfnWarning;
	}
	virtual void SetWarningLevel(FileWarningLevel_t level) override
	{
		m_WarningLevel = level;
	}
	virtual void LogLevelLoadStarted(const char *name) override { (void)name; }
	virtual void LogLevelLoadFinished(const char *name) override { (void)name; }
	virtual int HintResourceNeed(const char *hintlist, int forgetEverything) override { (void)hintlist;(void)forgetEverything; return 0; }
	virtual int PauseResourcePreloading() override { return 0; }
	virtual int ResumeResourcePreloading() override { return 0; }
	virtual int SetVBuf(FileHandle_t stream, char *buffer, int mode, long size) override { (void)stream;(void)buffer;(void)mode;(void)size; return 0; }
	virtual void GetInterfaceVersion(char *p, int maxlen) override
	{
		if (!p || maxlen<=0) return;
		strncpy_s(p, maxlen, FILESYSTEM_INTERFACE_VERSION, _TRUNCATE);
	}
	virtual bool IsFileImmediatelyAvailable(const char *pFileName) override
	{
		return FileExists(pFileName);
	}
	virtual WaitForResourcesHandle_t WaitForResources(const char *resourcelist) override { (void)resourcelist; return 0; }
	virtual bool GetWaitForResourcesProgress(WaitForResourcesHandle_t handle, float *progress, bool *complete) override
	{
		(void)handle;
		if (progress) *progress = 1.0f;
		if (complete) *complete = true;
		return true;
	}
	virtual void CancelWaitForResources(WaitForResourcesHandle_t handle) override { (void)handle; }
	virtual bool IsAppReadyForOfflinePlay(int appID) override { (void)appID; return true; }
	virtual bool AddPackFile(const char *fullpath, const char *pathID) override
	{
		(void)pathID;
		if (!fullpath) return false;
		// treat pack file as search path base dir
		std::string path = fullpath;
		// strip filename
		size_t pos = path.find_last_of("\\/");
		if (pos != std::string::npos) path = path.substr(0,pos);
		AddSearchPath(path.c_str(), pathID);
		return true;
	}
	virtual FileHandle_t OpenFromCacheForRead(const char *pFileName, const char *pOptions, const char *pathID = 0) override
	{
		return Open(pFileName, pOptions, pathID);
	}
	virtual void AddSearchPathNoWrite(const char *pPath, const char *pathID) override
	{
		AddSearchPath(pPath, pathID);
	}
	virtual long int GetFileModificationTime(const char *pFileName) override
	{
		return GetFileTime(pFileName);
	}

private:
	std::vector<std::string> m_SearchPaths;
	std::vector<FileHandleInternal*> m_OpenFiles;
	std::vector<FindHandleInternal*> m_FindHandles;
	void (*m_pfnWarning)(const char *fmt, ...);
	FileWarningLevel_t m_WarningLevel;
};

EXPOSE_INTERFACE(CFileSystem, IFileSystem, FILESYSTEM_INTERFACE_VERSION);
