// FileSystem.h -- IFileSystem V009 (GoldSrc/HL SDK) - 54 virtuals
// Reconstructed from Valve HL SDK / Xash3D FWGS VFileSystem009.h
// Licensed GPL-3.0 to match tier0 reconstruction
#ifndef FILESYSTEM_H
#define FILESYSTEM_H
#ifdef _WIN32
#pragma once
#endif

#include <cstdarg>

#ifndef abstract_class
#define abstract_class class
#endif

#define FILESYSTEM_INTERFACE_VERSION "VFileSystem009"

typedef enum
{
	FILESYSTEM_SEEK_HEAD    = 0,
	FILESYSTEM_SEEK_CURRENT = 1,
	FILESYSTEM_SEEK_TAIL    = 2,
} FileSystemSeek_t;

typedef enum
{
	FILESYSTEM_WARNING_QUIET                 = 0,
	FILESYSTEM_WARNING_REPORTUNCLOSED       = 1,
	FILESYSTEM_WARNING_REPORTUSAGE          = 2,
	FILESYSTEM_WARNING_REPORTALLACCESSES    = 3,
} FileWarningLevel_t;

typedef void* FileHandle_t;
typedef int FileFindHandle_t;
typedef int WaitForResourcesHandle_t;

class IBaseInterface
{
public:
	virtual ~IBaseInterface() {}
};

abstract_class IFileSystem : public IBaseInterface
{
public:
	// 1
	virtual void Mount() = 0;
	// 2
	virtual void Unmount() = 0;
	// 3
	virtual void RemoveAllSearchPaths() = 0;
	// 4
	virtual void AddSearchPath(const char *pPath, const char *pathID) = 0;
	// 5
	virtual bool RemoveSearchPath(const char *pPath) = 0;
	// 6
	virtual void RemoveFile(const char *pRelativePath, const char *pathID) = 0;
	// 7
	virtual void CreateDirHierarchy(const char *path, const char *pathID) = 0;
	// 8
	virtual bool FileExists(const char *pFileName) = 0;
	// 9
	virtual bool IsDirectory(const char *pFileName) = 0;
	// 10
	virtual FileHandle_t Open(const char *pFileName, const char *pOptions, const char *pathID = 0) = 0;
	// 11
	virtual void Close(FileHandle_t file) = 0;
	// 12
	virtual void Seek(FileHandle_t file, int pos, FileSystemSeek_t seekType) = 0;
	// 13
	virtual unsigned int Tell(FileHandle_t file) = 0;
	// 14
	virtual unsigned int Size(FileHandle_t file) = 0;
	// 15
	virtual unsigned int Size(const char *pFileName) = 0;
	// 16
	virtual long int GetFileTime(const char *pFileName) = 0;
	// 17
	virtual void FileTimeToString(char *pStrip, int maxCharsIncludingTerminator, long fileTime) = 0;
	// 18
	virtual bool IsOk(FileHandle_t file) = 0;
	// 19
	virtual void Flush(FileHandle_t file) = 0;
	// 20
	virtual bool EndOfFile(FileHandle_t file) = 0;
	// 21
	virtual int Read(void *pOutput, int size, FileHandle_t file) = 0;
	// 22
	virtual int Write(const void *pInput, int size, FileHandle_t file) = 0;
	// 23
	virtual char *ReadLine(char *pOutput, int maxChars, FileHandle_t file) = 0;
	// 24
	virtual int FPrintf(FileHandle_t file, char *pFormat, ...) = 0;
	// 25
	virtual void *GetReadBuffer(FileHandle_t file, int *outBufferSize, bool failIfNotInCache) = 0;
	// 26
	virtual void ReleaseReadBuffer(FileHandle_t file, void *buffer) = 0;
	// 27
	virtual const char *FindFirst(const char *pWildCard, FileFindHandle_t *pHandle, const char *pathID = 0) = 0;
	// 28
	virtual const char *FindNext(FileFindHandle_t handle) = 0;
	// 29
	virtual bool FindIsDirectory(FileFindHandle_t handle) = 0;
	// 30
	virtual void FindClose(FileFindHandle_t handle) = 0;
	// 31
	virtual void GetLocalCopy(const char *pFileName) = 0;
	// 32
	virtual const char *GetLocalPath(const char *pFileName, char *pLocalPath, int localPathBufferSize) = 0;
	// 33
	virtual char *ParseFile(char *pFileBytes, char *pToken, bool *pWasQuoted) = 0;
	// 34
	virtual bool FullPathToRelativePath(const char *pFullpath, char *pRelative) = 0;
	// 35
	virtual bool GetCurrentDirectory(char *pDirectory, int maxlen) = 0;
	// 36
	virtual void PrintOpenedFiles() = 0;
	// 37
	virtual void SetWarningFunc(void (*pfnWarning)(const char *fmt, ...)) = 0;
	// 38
	virtual void SetWarningLevel(FileWarningLevel_t level) = 0;
	// 39
	virtual void LogLevelLoadStarted(const char *name) = 0;
	// 40
	virtual void LogLevelLoadFinished(const char *name) = 0;
	// 41
	virtual int HintResourceNeed(const char *hintlist, int forgetEverything) = 0;
	// 42
	virtual int PauseResourcePreloading() = 0;
	// 43
	virtual int ResumeResourcePreloading() = 0;
	// 44
	virtual int SetVBuf(FileHandle_t stream, char *buffer, int mode, long size) = 0;
	// 45
	virtual void GetInterfaceVersion(char *p, int maxlen) = 0;
	// 46
	virtual bool IsFileImmediatelyAvailable(const char *pFileName) = 0;
	// 47
	virtual WaitForResourcesHandle_t WaitForResources(const char *resourcelist) = 0;
	// 48
	virtual bool GetWaitForResourcesProgress(WaitForResourcesHandle_t handle, float *progress, bool *complete) = 0;
	// 49
	virtual void CancelWaitForResources(WaitForResourcesHandle_t handle) = 0;
	// 50
	virtual bool IsAppReadyForOfflinePlay(int appID) = 0;
	// 51
	virtual bool AddPackFile(const char *fullpath, const char *pathID) = 0;
	// 52
	virtual FileHandle_t OpenFromCacheForRead(const char *pFileName, const char *pOptions, const char *pathID = 0) = 0;
	// 53
	virtual void AddSearchPathNoWrite(const char *pPath, const char *pathID) = 0;
	// 54
	virtual long int GetFileModificationTime(const char *pFileName) = 0;
};

#endif // FILESYSTEM_H
