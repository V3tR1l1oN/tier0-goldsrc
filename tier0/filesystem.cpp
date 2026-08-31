// filesystem.cpp -- CFileSystem : IFileSystem (VFileSystem009)
// Win32 implementation using CreateFile/ReadFile/FindFirstFile
// GetReadBuffer: true zero-copy via CreateFileMapping/MapViewOfFile (mmap).
// Fallback path uses SBArena_AllocForFileSystem (VirtualAlloc arena) instead of malloc.
#ifdef _LINUX
#define _GNU_SOURCE
#endif
#include "../public/tier1/interface.h"
#ifdef _LINUX
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/mman.h>
#include <fnmatch.h>
#include <climits>
#include <cerrno>
#ifndef INVALID_HANDLE_VALUE
#define INVALID_HANDLE_VALUE ((HANDLE)(intptr_t)-1)
#endif
#ifndef FILE_ATTRIBUTE_DIRECTORY
#define FILE_ATTRIBUTE_DIRECTORY 0x10
#endif
#ifndef INVALID_FILE_ATTRIBUTES
#define INVALID_FILE_ATTRIBUTES ((DWORD)-1)
#endif
#else
#include <windows.h>
#endif
#include "../public/tier0/memalloc.h"

// SBArena filesystem arena (exported from mem.cpp, VirtualAlloc-backed)
extern "C" PLATFORM_INTERFACE void* SBArena_AllocForFileSystem(size_t nSize);
extern "C" PLATFORM_INTERFACE void  SBArena_FreeForFileSystem(void* p, size_t nSize);
#ifndef _LINUX
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
#endif // !_LINUX
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
#ifdef _LINUX
	struct stat st;
	return ::stat(path, &st) == 0;
#else
	DWORD attr = GetFileAttributesA(path);
	return attr != INVALID_FILE_ATTRIBUTES;
#endif
}

struct FileHandleInternal
{
	HANDLE hFile = INVALID_HANDLE_VALUE;
	void *pReadBuffer = nullptr;
	int nBufferSize = 0;
	bool bIsWrite = false;
	bool bIsMapped = false;               // true: pReadBuffer is MapViewOfFile
	HANDLE hMapping = NULL;               // file mapping handle for zero-copy
	std::string fileName;
};

struct FindHandleInternal
{
#ifdef _LINUX
	DIR *pDir = nullptr;
	bool valid = false;
	std::string wildcard;
	std::string baseDir;
	char cFileName[MAX_PATH];
	DWORD dwFileAttributes = 0;
#else
	HANDLE hFind = INVALID_HANDLE_VALUE;
	WIN32_FIND_DATAA findData{};
	bool valid = false;
	std::string wildcard;
	std::string baseDir;
#endif
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
				if (fh->pReadBuffer)
				{
					if (fh->bIsMapped)
					{
#ifdef _LINUX
						munmap(fh->pReadBuffer, fh->nBufferSize);
#else
						UnmapViewOfFile(fh->pReadBuffer);
						if (fh->hMapping) CloseHandle(fh->hMapping);
#endif
					}
					else
					{
						SBArena_FreeForFileSystem(fh->pReadBuffer, fh->nBufferSize);
					}
				}
#ifdef _LINUX
				{ int _fd = (int)(intptr_t)fh->hFile; if (_fd >= 0) ::close(_fd); }
#else
				if (fh->hFile != INVALID_HANDLE_VALUE) CloseHandle(fh->hFile);
#endif
				delete fh;
			}
		}
		for (auto *fd : m_FindHandles)
		{
			if (fd)
			{
#ifdef _LINUX
				if (fd->pDir) ::closedir(fd->pDir);
#else
				if (fd->hFind != INVALID_HANDLE_VALUE) ::FindClose(fd->hFind);
#endif
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
#ifdef _LINUX
		::unlink(pRelativePath);
		for (auto &sp : m_SearchPaths)
		{
			std::string full = JoinPath(sp, pRelativePath);
			::unlink(full.c_str());
		}
#else
		// try direct
		DeleteFileA(pRelativePath);
		for (auto &sp : m_SearchPaths)
		{
			std::string full = JoinPath(sp, pRelativePath);
			DeleteFileA(full.c_str());
		}
#endif
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
#ifdef _LINUX
					::mkdir(cur.c_str(), 0755);
#else
					CreateDirectoryA(cur.c_str(), nullptr);
#endif
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
#ifdef _LINUX
		struct stat st;
		if (::stat(pFileName, &st) == 0)
			return S_ISDIR(st.st_mode);
		for (auto &sp : m_SearchPaths)
		{
			std::string full = JoinPath(sp, pFileName);
			if (::stat(full.c_str(), &st) == 0)
				return S_ISDIR(st.st_mode);
		}
		return false;
#else
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
#endif
	}
	virtual FileHandle_t Open(const char *pFileName, const char *pOptions, const char *pathID = 0) override
	{
		(void)pathID;
		if (!pFileName || !pOptions) return nullptr;
		bool bRead = (strchr(pOptions, 'r') != nullptr) || (strchr(pOptions, 'R') != nullptr);
		bool bWrite = (strchr(pOptions, 'w') != nullptr) || (strchr(pOptions, 'W') != nullptr) || (strchr(pOptions, 'a') != nullptr) || (strchr(pOptions, 'A') != nullptr);
		bool bPlus = (strchr(pOptions, '+') != nullptr);

#ifdef _LINUX
		int oflags = 0;
		if (bWrite || bPlus)
		{
			if (strchr(pOptions,'w') || strchr(pOptions,'W'))
				oflags = O_RDWR | O_CREAT | O_TRUNC;
			else if (strchr(pOptions,'a') || strchr(pOptions,'A'))
				oflags = O_RDWR | O_CREAT | O_APPEND;
			else if (bPlus)
				oflags = O_RDWR | O_CREAT;
			else
				oflags = O_WRONLY | O_CREAT | O_TRUNC;
		}
		else
		{
			oflags = O_RDONLY;
		}
		bool isWriteCreation = (oflags & O_CREAT) != 0;

		// try to resolve file location for reading
		std::string resolved = pFileName;
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
					resolved = pFileName;
				}
			}
		}

		int fd = ::open(resolved.c_str(), oflags, 0666);
		if (fd < 0)
		{
			if (isWriteCreation)
			{
				char dir[MAX_PATH];
				strncpy(dir, resolved.c_str(), MAX_PATH-1); dir[MAX_PATH-1]=0;
				char *slash = strrchr(dir, '/');
				if (slash)
				{
					*slash = 0;
					CreateDirHierarchy(dir, nullptr);
					fd = ::open(resolved.c_str(), oflags, 0666);
				}
			}
			if (fd < 0)
				return nullptr;
		}

		FileHandleInternal *fh = new FileHandleInternal();
		fh->hFile = (HANDLE)(intptr_t)fd;
		fh->bIsWrite = bWrite;
		fh->fileName = resolved;
		m_OpenFiles.push_back(fh);
		return (FileHandle_t)fh;
#else
		bool bBin = (strchr(pOptions, 'b') != nullptr);
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
					resolved = pFileName;
				}
			}
		}

		HANDLE h = CreateFileA(resolved.c_str(), desiredAccess, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, creation, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (h == INVALID_HANDLE_VALUE)
		{
			if (isWriteCreation)
			{
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
#endif
	}
	virtual void Close(FileHandle_t file) override
	{
		if (!file) return;
		FileHandleInternal *fh = (FileHandleInternal*)file;
		if (fh->pReadBuffer)
		{
			if (fh->bIsMapped)
			{
#ifdef _LINUX
				munmap(fh->pReadBuffer, fh->nBufferSize);
#else
				UnmapViewOfFile(fh->pReadBuffer);
				if (fh->hMapping) CloseHandle(fh->hMapping);
#endif
			}
			else
			{
				SBArena_FreeForFileSystem(fh->pReadBuffer, fh->nBufferSize);
			}
			fh->pReadBuffer=nullptr;
			fh->bIsMapped=false;
#ifndef _LINUX
			fh->hMapping=NULL;
#endif
		}
#ifdef _LINUX
		{ int _fd = (int)(intptr_t)fh->hFile; if (_fd >= 0) ::close(_fd); }
#else
		if (fh->hFile != INVALID_HANDLE_VALUE) CloseHandle(fh->hFile);
#endif
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
#ifdef _LINUX
		int whence = SEEK_SET;
		if (seekType == FILESYSTEM_SEEK_CURRENT) whence = SEEK_CUR;
		else if (seekType == FILESYSTEM_SEEK_TAIL) whence = SEEK_END;
		::lseek((int)(intptr_t)fh->hFile, pos, whence);
#else
		DWORD method = FILE_BEGIN;
		if (seekType == FILESYSTEM_SEEK_CURRENT) method = FILE_CURRENT;
		else if (seekType == FILESYSTEM_SEEK_TAIL) method = FILE_END;
		LARGE_INTEGER li; li.QuadPart = pos;
		SetFilePointerEx(fh->hFile, li, nullptr, method);
#endif
	}
	virtual unsigned int Tell(FileHandle_t file) override
	{
		if (!file) return 0;
		FileHandleInternal *fh = (FileHandleInternal*)file;
#ifdef _LINUX
		off_t pos = ::lseek((int)(intptr_t)fh->hFile, 0, SEEK_CUR);
		return (pos < 0) ? 0 : (unsigned int)pos;
#else
		LARGE_INTEGER cur, zero; zero.QuadPart=0;
		if (!SetFilePointerEx(fh->hFile, zero, &cur, FILE_CURRENT)) return 0;
		return (unsigned int)cur.QuadPart;
#endif
	}
	virtual unsigned int Size(FileHandle_t file) override
	{
		if (!file) return 0;
		FileHandleInternal *fh = (FileHandleInternal*)file;
#ifdef _LINUX
		struct stat st;
		if (::fstat((int)(intptr_t)fh->hFile, &st) != 0) return 0;
		return (unsigned int)st.st_size;
#else
		LARGE_INTEGER sz;
		if (!GetFileSizeEx(fh->hFile, &sz)) return 0;
		return (unsigned int)sz.QuadPart;
#endif
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
#ifdef _LINUX
		struct stat st;
		if (::stat(resolved.c_str(), &st) != 0) return 0;
		if (S_ISDIR(st.st_mode)) return 0;
		return (unsigned int)st.st_size;
#else
		WIN32_FILE_ATTRIBUTE_DATA fad;
		if (!GetFileAttributesExA(resolved.c_str(), GetFileExInfoStandard, &fad)) return 0;
		if (fad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) return 0;
		ULARGE_INTEGER ul; ul.LowPart = fad.nFileSizeLow; ul.HighPart = fad.nFileSizeHigh;
		return (unsigned int)ul.QuadPart;
#endif
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
#ifdef _LINUX
				struct stat st;
				if (::stat(full.c_str(), &st) == 0) { resolved = full; break; }
#else
				WIN32_FILE_ATTRIBUTE_DATA fad;
				if (GetFileAttributesExA(full.c_str(), GetFileExInfoStandard, &fad)) { resolved = full; break; }
#endif
			}
		}
#ifdef _LINUX
		struct stat st;
		if (::stat(resolved.c_str(), &st) != 0) return 0;
		return (long int)st.st_mtime;
#else
		WIN32_FILE_ATTRIBUTE_DATA fad;
		if (!GetFileAttributesExA(resolved.c_str(), GetFileExInfoStandard, &fad)) return 0;
		FILETIME ft = fad.ftLastWriteTime;
		ULARGE_INTEGER ul; ul.LowPart = ft.dwLowDateTime; ul.HighPart = ft.dwHighDateTime;
		// convert to time_t (100ns intervals since 1601 -> 1970 diff 116444736000000000)
		ul.QuadPart -= 116444736000000000ULL;
		ul.QuadPart /= 10000000ULL;
		return (long int)ul.QuadPart;
#endif
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
#ifdef _LINUX
		return (int)(intptr_t)fh->hFile >= 0;
#else
		return fh->hFile != INVALID_HANDLE_VALUE;
#endif
	}
	virtual void Flush(FileHandle_t file) override
	{
		if (!file) return;
		FileHandleInternal *fh = (FileHandleInternal*)file;
#ifdef _LINUX
		::fsync((int)(intptr_t)fh->hFile);
#else
		FlushFileBuffers(fh->hFile);
#endif
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
#ifdef _LINUX
		ssize_t nRead = ::read((int)(intptr_t)fh->hFile, pOutput, size);
		return (nRead < 0) ? 0 : (int)nRead;
#else
		DWORD read=0;
		if (!ReadFile(fh->hFile, pOutput, (DWORD)size, &read, nullptr)) return 0;
		return (int)read;
#endif
	}
	virtual int Write(const void *pInput, int size, FileHandle_t file) override
	{
		if (!file || !pInput || size<=0) return 0;
		FileHandleInternal *fh = (FileHandleInternal*)file;
#ifdef _LINUX
		ssize_t nWritten = ::write((int)(intptr_t)fh->hFile, pInput, size);
		if (nWritten <= 0) return 0;
#else
		DWORD written=0;
		if (!WriteFile(fh->hFile, pInput, (DWORD)size, &written, nullptr)) return 0;
#endif
		// invalidate cached read buffer if we wrote
		if (fh->pReadBuffer)
		{
			if (fh->bIsMapped)
			{
#ifdef _LINUX
				munmap(fh->pReadBuffer, fh->nBufferSize);
#else
				UnmapViewOfFile(fh->pReadBuffer);
				if (fh->hMapping) CloseHandle(fh->hMapping);
#endif
			}
			else
			{
				SBArena_FreeForFileSystem(fh->pReadBuffer, fh->nBufferSize);
			}
			fh->pReadBuffer=nullptr; fh->nBufferSize=0; fh->bIsMapped=false;
#ifndef _LINUX
			fh->hMapping=NULL;
#endif
		}
#ifdef _LINUX
		return (int)nWritten;
#else
		return (int)written;
#endif
	}
	virtual char *ReadLine(char *pOutput, int maxChars, FileHandle_t file) override
	{
		if (!file || !pOutput || maxChars<=1) return nullptr;
		FileHandleInternal *fh = (FileHandleInternal*)file;
		int pos=0;
		char c=0;
#ifdef _LINUX
		int fd = (int)(intptr_t)fh->hFile;
		while (pos < maxChars-1)
		{
			ssize_t nRead = ::read(fd, &c, 1);
			if (nRead <= 0) break;
			if (c == '\n') { break; }
			if (c == '\r')
			{
				// peek next char
				char next=0;
				ssize_t r2 = ::read(fd, &next, 1);
				if (r2 == 1)
				{
					if (next != '\n')
					{
						// push back
						::lseek(fd, -1, SEEK_CUR);
					}
				}
				break;
			}
			pOutput[pos++]=c;
		}
#else
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
#endif
		if (pos==0 && EndOfFile(file)) return nullptr;
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

#ifdef _LINUX
		// --- zero-copy path: mmap ---
		int fd = (int)(intptr_t)fh->hFile;
		void *pView = mmap(nullptr, sz, PROT_READ, MAP_PRIVATE, fd, 0);
		if (pView != MAP_FAILED)
		{
			fh->pReadBuffer = pView;
			fh->nBufferSize = (int)sz;
			fh->bIsMapped = true;
			if (outBufferSize) *outBufferSize = (int)sz;
			return pView;
		}

		// --- fallback: SBArena + read ---
		void *buf = SBArena_AllocForFileSystem(sz);
		if (!buf) { if(outBufferSize) *outBufferSize=0; return nullptr; }
		// save current pos
		off_t curPos = ::lseek(fd, 0, SEEK_CUR);
		::lseek(fd, 0, SEEK_SET);
		ssize_t total = 0;
		char *ptr = (char*)buf;
		unsigned int remaining = sz;
		while (remaining > 0)
		{
			size_t toRead = remaining > 8192 ? 8192 : remaining;
			ssize_t nRead = ::read(fd, ptr + total, toRead);
			if (nRead <= 0) break;
			total += nRead;
			remaining -= (unsigned int)nRead;
		}
		// restore pos
		::lseek(fd, curPos, SEEK_SET);
		if ((unsigned int)total != sz)
		{
			sz = (unsigned int)total;
		}
		fh->pReadBuffer = buf;
		fh->nBufferSize = (int)sz;
		fh->bIsMapped = false;
		if (outBufferSize) *outBufferSize = (int)sz;
		return buf;
#else
		// --- zero-copy path: try CreateFileMapping + MapViewOfFile (mmap) ---
		HANDLE hMap = CreateFileMappingA(fh->hFile, NULL, PAGE_READONLY, 0, 0, NULL);
		if (hMap)
		{
			void *pView = MapViewOfFile(hMap, FILE_MAP_READ, 0, 0, 0);
			if (pView)
			{
				fh->pReadBuffer = pView;
				fh->nBufferSize = (int)sz;
				fh->hMapping = hMap;
				fh->bIsMapped = true;
				if (outBufferSize) *outBufferSize = (int)sz;
				return pView;
			}
			CloseHandle(hMap);
		}

		// --- fallback: SBArena (VirtualAlloc arena) + ReadFile ---
		void *buf = SBArena_AllocForFileSystem(sz);
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
			sz = total;
		}
		fh->pReadBuffer = buf;
		fh->nBufferSize = (int)sz;
		fh->hMapping = NULL;
		fh->bIsMapped = false;
		if (outBufferSize) *outBufferSize = (int)sz;
		return buf;
#endif
	}
	virtual void ReleaseReadBuffer(FileHandle_t file, void *buffer) override
	{
		if (!file || !buffer) return;
		FileHandleInternal *fh = (FileHandleInternal*)file;
		if (fh->pReadBuffer == buffer)
		{
			if (fh->bIsMapped)
			{
#ifdef _LINUX
				munmap(buffer, fh->nBufferSize);
#else
				UnmapViewOfFile(buffer);
				if (fh->hMapping) CloseHandle(fh->hMapping);
#endif
			}
			else
			{
				SBArena_FreeForFileSystem(buffer, fh->nBufferSize);
			}
			fh->pReadBuffer = nullptr;
			fh->nBufferSize = 0;
			fh->bIsMapped = false;
#ifndef _LINUX
			fh->hMapping = NULL;
#endif
		}
		else
		{
			// not our cached buffer
#ifdef _LINUX
			SBArena_FreeForFileSystem(buffer, 0);
#else
			// detect mapped vs arena
			MEMORY_BASIC_INFORMATION mbi;
			if (VirtualQuery(buffer, &mbi, sizeof mbi) == sizeof mbi && mbi.Type == MEM_MAPPED)
			{
				UnmapViewOfFile(buffer);
			}
			else
			{
				SBArena_FreeForFileSystem(buffer, 0);
			}
#endif
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
#ifdef _LINUX
		DIR *pDir = nullptr;
		std::string foundWildcard;
		std::string dirPath;
		std::string pattern;
		for (auto &cand : candidates)
		{
			// extract directory and glob pattern from wildcard path
			std::string path = cand;
			size_t sepPos = path.find_last_of("/\\");
			std::string d, pat;
			if (sepPos != std::string::npos)
			{
				d = path.substr(0, sepPos);
				pat = path.substr(sepPos + 1);
			}
			else
			{
				d = ".";
				pat = path;
			}
			pDir = ::opendir(d.c_str());
			if (pDir)
			{
				foundWildcard = cand;
				dirPath = d;
				pattern = pat;
				break;
			}
		}
		if (!pDir) { *pHandle = -1; return nullptr; }

		FindHandleInternal *fhi = new FindHandleInternal();
		fhi->pDir = pDir;
		fhi->wildcard = foundWildcard;
		fhi->baseDir = dirPath;
		fhi->valid = false;

		// read first entry matching pattern
		struct dirent *entry;
		while ((entry = ::readdir(pDir)) != nullptr)
		{
			// skip "." and ".." unless pattern starts with dot
			if (entry->d_name[0] == '.' && (pattern.empty() || pattern[0] != '.'))
				continue;
			if (fnmatch(pattern.c_str(), entry->d_name, FNM_CASEFOLD) != 0)
				continue;
			strncpy(fhi->cFileName, entry->d_name, MAX_PATH - 1);
			fhi->cFileName[MAX_PATH - 1] = '\0';
			// stat to get attributes
			std::string fullEntry = dirPath;
			if (!fullEntry.empty() && fullEntry.back() != '/') fullEntry += '/';
			fullEntry += entry->d_name;
			struct stat st;
			fhi->dwFileAttributes = 0;
			if (::stat(fullEntry.c_str(), &st) == 0 && S_ISDIR(st.st_mode))
				fhi->dwFileAttributes = FILE_ATTRIBUTE_DIRECTORY;
			fhi->valid = true;
			break;
		}

		if (!fhi->valid)
		{
			::closedir(pDir);
			delete fhi;
			*pHandle = -1;
			return nullptr;
		}

		// find slot
		int idx = -1;
		for (size_t i=0;i<m_FindHandles.size();++i) if (m_FindHandles[i]==nullptr) { idx=(int)i; break; }
		if (idx==-1) { idx=(int)m_FindHandles.size(); m_FindHandles.push_back(fhi); }
		else m_FindHandles[idx]=fhi;
		*pHandle = idx;
		return m_FindHandles[idx]->cFileName;
#else
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
#endif
	}
	virtual const char *FindNext(FileFindHandle_t handle) override
	{
		if (handle<0 || handle >= (int)m_FindHandles.size() || !m_FindHandles[handle]) return nullptr;
		FindHandleInternal *fhi = m_FindHandles[handle];
		if (!fhi->valid) return nullptr;
#ifdef _LINUX
		// extract pattern from wildcard
		std::string path = fhi->wildcard;
		size_t sepPos = path.find_last_of("/\\");
		std::string pattern = (sepPos != std::string::npos) ? path.substr(sepPos + 1) : path;
		struct dirent *entry;
		while ((entry = ::readdir(fhi->pDir)) != nullptr)
		{
			if (entry->d_name[0] == '.' && (pattern.empty() || pattern[0] != '.'))
				continue;
			if (fnmatch(pattern.c_str(), entry->d_name, FNM_CASEFOLD) != 0)
				continue;
			strncpy(fhi->cFileName, entry->d_name, MAX_PATH - 1);
			fhi->cFileName[MAX_PATH - 1] = '\0';
			// stat to get attributes
			std::string fullEntry = fhi->baseDir;
			if (!fullEntry.empty() && fullEntry.back() != '/') fullEntry += '/';
			fullEntry += entry->d_name;
			struct stat st;
			fhi->dwFileAttributes = 0;
			if (::stat(fullEntry.c_str(), &st) == 0 && S_ISDIR(st.st_mode))
				fhi->dwFileAttributes = FILE_ATTRIBUTE_DIRECTORY;
			return fhi->cFileName;
		}
		return nullptr;
#else
		if (!FindNextFileA(fhi->hFind, &fhi->findData)) return nullptr;
		return fhi->findData.cFileName;
#endif
	}
	virtual bool FindIsDirectory(FileFindHandle_t handle) override
	{
		if (handle<0 || handle >= (int)m_FindHandles.size() || !m_FindHandles[handle]) return false;
		FindHandleInternal *fhi = m_FindHandles[handle];
#ifdef _LINUX
		return (fhi->dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
#else
		return (fhi->findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
#endif
	}
	virtual void FindClose(FileFindHandle_t handle) override
	{
		if (handle<0 || handle >= (int)m_FindHandles.size() || !m_FindHandles[handle]) return;
		FindHandleInternal *fhi = m_FindHandles[handle];
#ifdef _LINUX
		if (fhi->pDir) ::closedir(fhi->pDir);
#else
		if (fhi->hFind != INVALID_HANDLE_VALUE) ::FindClose(fhi->hFind);
#endif
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
#ifdef _LINUX
		char fullPath[MAX_PATH];
		if (::realpath(resolved.c_str(), fullPath) == nullptr)
		{
			strncpy(pLocalPath, resolved.c_str(), localPathBufferSize - 1);
			pLocalPath[localPathBufferSize - 1] = '\0';
		}
		else
		{
			strncpy(pLocalPath, fullPath, localPathBufferSize - 1);
			pLocalPath[localPathBufferSize - 1] = '\0';
		}
#else
		char fullPath[MAX_PATH];
		if (GetFullPathNameA(resolved.c_str(), MAX_PATH, fullPath, nullptr)==0)
		{
			strncpy_s(pLocalPath, localPathBufferSize, resolved.c_str(), _TRUNCATE);
		}
		else
		{
			strncpy_s(pLocalPath, localPathBufferSize, fullPath, _TRUNCATE);
		}
#endif
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
#ifdef _LINUX
				strncpy(pRelative, rel, MAX_PATH - 1);
				pRelative[MAX_PATH - 1] = '\0';
#else
				strcpy_s(pRelative, MAX_PATH, rel);
#endif
				return true;
			}
		}
		// fallback try GetCurrentDirectory prefix
		char cur[MAX_PATH];
#ifdef _LINUX
		if (::getcwd(cur, sizeof(cur)) == nullptr) cur[0] = '\0';
#else
		GetCurrentDirectoryA(MAX_PATH, cur);
#endif
		size_t curLen = strlen(cur);
		if (_strnicmp(pFullpath, cur, curLen)==0)
		{
			const char *rel = pFullpath + curLen;
			while (*rel=='\\' || *rel=='/') rel++;
#ifdef _LINUX
			strncpy(pRelative, rel, MAX_PATH - 1);
			pRelative[MAX_PATH - 1] = '\0';
#else
			strcpy_s(pRelative, MAX_PATH, rel);
#endif
			return true;
		}
#ifdef _LINUX
		strncpy(pRelative, pFullpath, MAX_PATH - 1);
		pRelative[MAX_PATH - 1] = '\0';
#else
		strcpy_s(pRelative, MAX_PATH, pFullpath);
#endif
		return false;
	}
	virtual bool GetCurrentDirectory(char *pDirectory, int maxlen) override
	{
		if (!pDirectory || maxlen<=0) return false;
#ifdef _LINUX
		return ::getcwd(pDirectory, maxlen) != nullptr;
#else
		DWORD len = ::GetCurrentDirectoryA(maxlen, pDirectory);
		return len>0 && len < (DWORD)maxlen;
#endif
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
#ifdef _LINUX
		strncpy(p, FILESYSTEM_INTERFACE_VERSION, maxlen - 1);
		p[maxlen - 1] = '\0';
#else
		strncpy_s(p, maxlen, FILESYSTEM_INTERFACE_VERSION, _TRUNCATE);
#endif
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
