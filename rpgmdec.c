#include <stdio.h>
#include <stdint.h>
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define UNICODE
#include <Windows.h>
#include <shellapi.h>
#include <strsafe.h>
#include <shlwapi.h>
#include <wchar.h>
#include <threadpoolapiset.h>

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "shlwapi.lib")

static const uint8_t rpgmvpHeader[16] = {
	0x52, 0x50, 0x47, 0x4D, 0x56, 0x00, 0x00, 0x00,
	0x00, 0x03, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00,
};

static const uint8_t pngHeader[16] = {
	// PNG magic
	0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A,
	// Chunk length: 13, this is always fixed
	0x00, 0x00, 0x00, 0x0D,
	// Chunk type: "IHDR"
	0x49, 0x48, 0x44, 0x52,
};

__attribute__((noreturn))
static void panic(const char* msg) {
    puts(msg);
    ExitProcess(1);
}

static TP_POOL* threadpool;
static TP_CALLBACK_ENVIRON callbackEnviron;
static TP_CLEANUP_GROUP* workFinished;

static long jobsCreated;
static long jobsFinished;

static void decryptWorkerImpl(const wchar_t* name) {
    // printf("decryptWorkerImpl running on thread %lu\n", GetCurrentThreadId());

    HANDLE handle = INVALID_HANDLE_VALUE;
    HANDLE newHandle = INVALID_HANDLE_VALUE;
    uint8_t* file = NULL;

    handle = CreateFileW(name, GENERIC_READ, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (handle == INVALID_HANDLE_VALUE) {
        puts("failed to open file\n");
        goto done;
    }

    LARGE_INTEGER fileSize;
    if (!GetFileSizeEx(handle, &fileSize)) {
        puts("failed to get file size\n");
        goto done;
    }

    if (fileSize.QuadPart < _countof(rpgmvpHeader) + _countof(pngHeader)) {
        puts("file is too small\n");
        goto done;
    }

    file = malloc(fileSize.QuadPart);
    if (!file) {
        puts("out of memory\n");
        goto done;
    }

    DWORD bytesRead;
    if (!ReadFile(handle, file, (uint32_t)fileSize.QuadPart, &bytesRead, NULL)) {
        puts("failed to read file\n");
        goto done;
    }

    if ((uint64_t)bytesRead != fileSize.QuadPart) {
        puts("incomplete read\n");
        goto done;
    }

    if (0 != memcmp(file, rpgmvpHeader, _countof(rpgmvpHeader))) {
        puts("invalid magic\n");
        goto done;
    }

    uint8_t* newFile = &file[_countof(rpgmvpHeader)];
    memcpy(newFile, pngHeader, _countof(pngHeader));
    uint32_t newFileSize = (uint32_t)(fileSize.QuadPart - _countof(rpgmvpHeader));

    wchar_t outPath[MAX_PATH];
    if (FAILED(StringCchCopyW(outPath, MAX_PATH, name))) {
        puts("failed to copy path\n");
        goto done;
    }

    wchar_t* ext = PathFindExtensionW(outPath);
    if (ext && *ext) {
        if (FAILED(StringCchCopyW(ext, MAX_PATH - (ext - outPath), L".png"))) {
            puts("failed to set extension\n");
            goto done;
        }
    }

    newHandle = CreateFileW(outPath, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (newHandle == INVALID_HANDLE_VALUE) {
        puts("failed to create output file\n");
        goto done;
    }

    DWORD bytesWritten = 0;
    if (!WriteFile(newHandle, newFile, newFileSize, &bytesWritten, NULL)) {
        puts("failed to write output file\n");
        goto done;
    }

    if (bytesWritten != newFileSize) {
        puts("incomplete write to output file\n");
        goto done;
    }

done:
    free(file);
    CloseHandle(handle);
    CloseHandle(newHandle);
}

static void __stdcall decryptWorker(TP_CALLBACK_INSTANCE* instance, void* parameter, TP_WORK* work) {
    decryptWorkerImpl((const wchar_t*)parameter);
    free(parameter);
    _InterlockedIncrement(&jobsFinished);
}

static void decrypt(const wchar_t* name) {
    size_t nameLen = wcslen(name) + 1;
    wchar_t* namePtr = malloc(sizeof(name[0]) * nameLen);
    wmemcpy(namePtr, name, nameLen);

    TP_WORK* work = CreateThreadpoolWork(decryptWorker, namePtr, &callbackEnviron);
    if (!work) {
        panic("failed to create thread pool work\n");
    }

    _InterlockedIncrement(&jobsCreated);
    SubmitThreadpoolWork(work);
}

static int endsWith(const wchar_t* str, const wchar_t* suffix) {
    size_t strLen = wcslen(str);
    size_t suffixLen = wcslen(suffix);
    if (suffixLen > strLen) return 0;
    return wmemcmp(str + strLen - suffixLen, suffix, suffixLen) == 0;
}

static void decryptDir(const wchar_t* name) {
    WIN32_FIND_DATAW findData;
    wchar_t searchPath[MAX_PATH];
    HANDLE hFind;

    // Build search path: name\*
    swprintf_s(searchPath, MAX_PATH, L"%s\\*", name);

    hFind = FindFirstFileExW(searchPath, FindExInfoBasic, &findData, 0, NULL, FIND_FIRST_EX_LARGE_FETCH);
    if (hFind == INVALID_HANDLE_VALUE) {
        panic("FindFirstFileExW failed");
    }

    do {
        if (wcscmp(findData.cFileName, L".") == 0 || wcscmp(findData.cFileName, L"..") == 0)
            continue;

        wchar_t fullPath[MAX_PATH];
        swprintf(fullPath, MAX_PATH, L"%s\\%s", name, findData.cFileName);

        if (findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            decryptDir(fullPath);
        } else if (endsWith(findData.cFileName, L".rpgmvp") || endsWith(findData.cFileName, L".png_")) {
            decrypt(fullPath);
        }
    } while (FindNextFileW(hFind, &findData));

    FindClose(hFind);
}

static void __stdcall printProgressWorker(TP_CALLBACK_INSTANCE* instance, void* context, TP_TIMER* timer) {
    printf("Processed %ld/%ld files...\n", jobsFinished, jobsCreated);
}

int main(int _argc, char** _argv) {
    InitializeThreadpoolEnvironment(&callbackEnviron);

    threadpool = CreateThreadpool(NULL);
    if (!threadpool) {
        panic("failed to create threadpool\n");
    }

    SYSTEM_INFO sysInfo;
    GetSystemInfo(&sysInfo);
    DWORD threadCount = sysInfo.dwNumberOfProcessors;
    if (threadCount < 1) threadCount = 1;

    SetThreadpoolThreadMaximum(threadpool, threadCount);
    if (!SetThreadpoolThreadMinimum(threadpool, threadCount)) {
        panic("set threadpool minimum\n");
    }

    SetThreadpoolCallbackPool(&callbackEnviron, threadpool);

    workFinished = CreateThreadpoolCleanupGroup();
    if (!workFinished) {
        panic("failed to create cleanup group\n");
    }

    SetThreadpoolCallbackCleanupGroup(&callbackEnviron, workFinished, NULL);

    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv || argc < 2) {
        panic("directory required\n");
    }

    LARGE_INTEGER start;
    QueryPerformanceCounter(&start);

    TP_TIMER* timer = CreateThreadpoolTimer(printProgressWorker, NULL, NULL);
    if (!timer) {
        panic("failed to create timer\n");
    }

    FILETIME startAt = { -300 * 1000000, 0 };
    SetThreadpoolTimer(timer, &startAt, 300, 0);

    decryptDir(argv[1]);

    CloseThreadpoolCleanupGroupMembers(workFinished, FALSE, NULL);
    
    LARGE_INTEGER end, frequency;
    QueryPerformanceCounter(&end);
    QueryPerformanceFrequency(&frequency);

    SetThreadpoolTimer(timer, NULL, 0, 0);
    WaitForThreadpoolTimerCallbacks(timer, TRUE);
    CloseThreadpoolTimer(timer);

    printf("Processed %ld/%ld files\n", jobsFinished, jobsCreated);
	printf("Done in %gms\n", 1000.0 * ((double)(end.QuadPart - start.QuadPart) / frequency.QuadPart));

    return 0;
}