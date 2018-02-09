#include "unittest.h"

namespace RSLibImpl
{

// See utility.h for overview of the class

// GetTimeAndTicks returns the systemtime in an 64 bits integer,
// that integer is useful to calculated durations. The unit
// of the return value is 100-nano seconds.
// pFill should point to an existing SYSTEMTIME structure, this
// function will fill it up with the current system time. If
// pFill is NULL, then no fill up will happen.
ULONGLONG CUnitTestUtil::GetTimeAndTicks (SYSTEMTIME * pFill)
{
    SYSTEMTIME systime;
    SYSTEMTIME * pToUse = pFill == NULL ? &systime : pFill;

    FILETIME filetime;
    ULARGE_INTEGER ulargeRet;

    GetLocalTime (pToUse);
    SystemTimeToFileTime (pToUse, &filetime);
    ulargeRet.LowPart = filetime.dwLowDateTime;
    ulargeRet.HighPart = filetime.dwHighDateTime;

    return ulargeRet.QuadPart;
}

// This function allocated a new char array with sufficient
// size and copy over the input string
char * CUnitTestUtil::MakeStringCopy (const char * cszInput)
{
    size_t nLength = strlen (cszInput);
    char * szRet = new char[nLength+1];
    strncpy_s (szRet, nLength+1, cszInput, nLength);
    szRet[nLength] = '\0';
    return szRet;
}

// This function allocated a new char array with sufficient
// size and copy over the input string and the result is
// HtmlEncoded.
char * CUnitTestUtil::MakeStringCopyWithHtmlEncoding (const char * cszInput)
{
    int nTargetLength = 0;
    for (int i = 0; cszInput[i] != '\0'; i ++)
    {
        switch (cszInput[i])
        {
        case '&':
            nTargetLength += 5; // &amp;
            break;
        case '<':
        case '>':
            nTargetLength += 4; // &lt;, &gt;
            break;
        case '\"':
            nTargetLength += 6; // &quot;
            break;
        default:
            nTargetLength += 1;
        }
    }

    char * szRet = new char[nTargetLength+1];
    int iTarget = 0;
    for (int iSrc = 0; cszInput[iSrc] && iTarget < nTargetLength; iSrc ++)
    {
        switch (cszInput[iSrc])
        {
        case '&':
            szRet[iTarget++] = '&';
            szRet[iTarget++] = 'a';
            szRet[iTarget++] = 'm';
            szRet[iTarget++] = 'p';
            szRet[iTarget++] = ';';
            break;
        case '<':
            szRet[iTarget++] = '&';
            szRet[iTarget++] = 'l';
            szRet[iTarget++] = 't';
            szRet[iTarget++] = ';';
            break;
        case '>':
            szRet[iTarget++] = '&';
            szRet[iTarget++] = 'g';
            szRet[iTarget++] = 't';
            szRet[iTarget++] = ';';
            break;
        case '\"':
            szRet[iTarget++] = '&';
            szRet[iTarget++] = 'q';
            szRet[iTarget++] = 'u';
            szRet[iTarget++] = 'o';
            szRet[iTarget++] = 't';
            szRet[iTarget++] = ';';
            break;
        default:
            szRet[iTarget++] = cszInput[iSrc];
        }
    }
    szRet[iTarget] = '\0';

    return szRet;
}

double CUnitTestUtil::DiffSeconds (ULONGLONG ullBegin, ULONGLONG ullEnd)
{
    return (double) (ullEnd - ullBegin) / 10000000.0;
}


void CUnitTestUtil::SleepLoop(UInt32 seconds, UInt32 display)
{
    printf("%5u", display);
    while (seconds > 0)
    {
        Sleep(5000);
        if (display > 5)
        {
            display -= 5;
        }
        else
        {
            display = 0;
        }
        if (seconds > 5)
        {
            seconds -= 5;
        }
        else
        {
            seconds = 0;
        }
        printf("\b\b\b\b\b%5u", display);
    }
    printf("\b\b\b\b\b");
}

void CUnitTestUtil::TouchFile(const char *filename)
{
    HANDLE hFile = CreateFileA(filename, FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
        OPEN_EXISTING, NULL, NULL);
    UT_AssertIsTrue(hFile != INVALID_HANDLE_VALUE);

    FILETIME ftNow;
    GetSystemTimeAsFileTime(&ftNow);

    BOOL fRet = SetFileTime(hFile, NULL, NULL, &ftNow);
    UT_AssertIsTrue(fRet);

    CloseHandle(hFile);
}

BOOL CUnitTestUtil::FileExists(const char *filename)
{
    HANDLE hFile = CreateFileA(filename, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE , NULL,
        OPEN_EXISTING, NULL, NULL);

    if (hFile == INVALID_HANDLE_VALUE)
    {
        //printf("CreateFileA failed for %s with GetLastError = %u", filename, GetLastError());
        return FALSE;
    }

    CloseHandle(hFile);
    return TRUE;
}

void CUnitTestUtil::ReadConfigParameter( const char *filename,
                                         const char *section,
                                         const char *param,
                                         char *outValue,
                                         const size_t outValueLen )
{
    BOOL fRet = GetPrivateProfileStringA( section, param, NULL,
                outValue, static_cast<DWORD>(outValueLen), filename );
    UT_AssertIsTrue( fRet, "Failed to read config param [%s] %s in %s",
                     section, param, filename);
}

void CUnitTestUtil::RewriteConfigParameter(const char *filename, const char *section,
                            const char *param, const char *value)
{
    BOOL fRet = WritePrivateProfileStringA(section, param, value, filename);
    UT_AssertIsTrue(fRet, "Failed to rewrite config param [%s] %s:%s in %s",
        section, param, value, filename);
}

BOOL CUnitTestUtil::PurgeDirectory(const char *pszDir)
{
    char szSearch[MAX_PATH + 1];
    UT_AssertIsTrue(strlen(pszDir) + 3 < sizeof(szSearch));
    
    strcpy_s(szSearch, ARRAYSIZE(szSearch), pszDir);
    strcat_s(szSearch, ARRAYSIZE(szSearch), "\\*");

    WIN32_FIND_DATAA finddata;
    HANDLE hSearch = FindFirstFileA(szSearch, &finddata);
    if (hSearch == INVALID_HANDLE_VALUE)
    {
        return TRUE;
    }

    char szFileName[MAX_PATH + 1];
    while (TRUE)
    {
        if (!(finddata.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
        {
            UT_AssertIsTrue(strlen(pszDir) + 1 + strlen(finddata.cFileName) < sizeof(szSearch));
            strcpy_s(szFileName, ARRAYSIZE(szFileName), pszDir);
            strcat_s(szFileName, ARRAYSIZE(szFileName), "\\");
            strcat_s(szFileName, ARRAYSIZE(szFileName), finddata.cFileName);

            if (!DeleteFileA(szFileName))
            {
                UT_AssertFail("Failed to delete file %s in PurgeDirectory lastError= %u", szFileName, GetLastError());
            }
        }
            
        if (!FindNextFileA(hSearch, &finddata))
        {
            UT_AssertIsTrue(GetLastError() == ERROR_NO_MORE_FILES);
            break;
        }
    }

    BOOL f = FindClose(hSearch);
    UT_AssertIsTrue(f);

    return TRUE;

}

void CUnitTestUtil::MirrorDirectoryTree(const char *sourceDir, const char *destDir)
{
    if (!CreateDirectoryA(destDir, NULL) && GetLastError() != ERROR_ALREADY_EXISTS)
    {
        UT_AssertFail("Failed to create %s GetLastError = %u", destDir, GetLastError());
    }

    // iterate over all files and directories in the source, and copy them as needed to dest

    WIN32_FIND_DATAA finddata;

    char szSearch[MAX_PATH + 1];
    UT_AssertIsTrue (strlen(sourceDir) + 2 < sizeof(szSearch));

    strcpy_s(szSearch, ARRAYSIZE(szSearch), sourceDir);
    strcat_s(szSearch, ARRAYSIZE(szSearch), "\\*");

    HANDLE hSearch = FindFirstFileA(szSearch, &finddata);
    UT_AssertIsTrue(hSearch != INVALID_HANDLE_VALUE);

    while (true)
    {
        // test to see whether this file is the same on both ends based on size and last write time
        char szSourceName[MAX_PATH + 1];
        UT_AssertIsTrue(strlen(sourceDir) + 1 + strlen(finddata.cFileName) < sizeof(szSourceName));
        strcpy_s(szSourceName, ARRAYSIZE(szSourceName), sourceDir);
        strcat_s(szSourceName, ARRAYSIZE(szSourceName), "\\");
        strcat_s(szSourceName, ARRAYSIZE(szSourceName), finddata.cFileName);

        char szDestName[MAX_PATH + 1];
        UT_AssertIsTrue(strlen(destDir) + 1 + strlen(finddata.cFileName) < sizeof(szDestName));
        strcpy_s(szDestName, ARRAYSIZE(szDestName), destDir);
        strcat_s(szDestName, ARRAYSIZE(szDestName), "\\");
        strcat_s(szDestName, ARRAYSIZE(szDestName), finddata.cFileName);

        if (finddata.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
        {
            if (strcmp(finddata.cFileName, ".") && strcmp(finddata.cFileName, ".."))
            {
                // call self recursively on the subdir
                MirrorDirectoryTree(szSourceName, szDestName);
            }
        }
        else
        {
            // test for existence
            HANDLE hDest = CreateFileA(szDestName, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                NULL, OPEN_EXISTING, NULL, NULL);

            bool fCopy = true;
            if (hDest != INVALID_HANDLE_VALUE)
            {
                HANDLE hSource = CreateFileA(szSourceName, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    NULL, OPEN_EXISTING, NULL, NULL);

                UT_AssertIsTrue(hSource != INVALID_HANDLE_VALUE);

                // test for same size and last write time
                LARGE_INTEGER sourceSize = { 0, 0 };
                LARGE_INTEGER destSize = { 0, 0 };
                if (!GetFileSizeEx(hSource, &sourceSize) || !GetFileSizeEx(hDest, &destSize))
                {
                    UT_AssertFail("Failed to get file size in MirrorDirectoryTree GetLastError = %u", GetLastError());
                }
                if (sourceSize.QuadPart == destSize.QuadPart)
                {
                    // also compare last write times
                    FILETIME sourceLastWrite = { 0, 0 };;
                    FILETIME destLastWrite = { 0, 0 };;

                    if (!GetFileTime(hSource, NULL, NULL, &sourceLastWrite) || !GetFileTime(hDest, NULL, NULL, &destLastWrite))
                    {
                        UT_AssertFail("GetFileTime failed GetLastError = %u", GetLastError());
                    }
                    if (sourceLastWrite.dwHighDateTime == destLastWrite.dwHighDateTime
                        && sourceLastWrite.dwLowDateTime == destLastWrite.dwLowDateTime)
                    {
                        fCopy = false;
                    }
                }
                CloseHandle(hSource);
                CloseHandle(hDest);
            }
            if (fCopy)
            {
                printf("Copying %s to %s\r\n", szSourceName, szDestName);
                if (!CopyFileA(szSourceName, szDestName, FALSE))
                {
                    printf("CopyFileA failed for %s GetLastError = %u\r\n", szDestName, GetLastError());
                }
                else
                {
                    // clear the read-only flag if it is set
                    DWORD fileAttrib = GetFileAttributesA(szDestName);
                    if (fileAttrib & FILE_ATTRIBUTE_READONLY)
                    {
                        fileAttrib &= ~FILE_ATTRIBUTE_READONLY;
                        if (!SetFileAttributesA(szDestName, fileAttrib))
                        {
                            printf("SetFileAttributes failed for %s with GetLastError %u\r\n",
                                szDestName, GetLastError());
                        }
                    }
                }
            }
        }

        if (!FindNextFileA(hSearch, &finddata))
        {
            if (GetLastError() != ERROR_NO_MORE_FILES)
            {
                UT_AssertFail("FindNextFileA failed with GetLastError = %u", GetLastError());
            }
            break;
        }
    }

    if (!FindClose(hSearch))
    {
        UT_AssertFail("FindClose failed GetLastError = %u", GetLastError());
    }
}

BOOL CUnitTestUtil::FindStringInFilesInDirectory(const char *dirname, const char* filetype, const char *searchString)
{
    if (!CreateDirectoryA(dirname, NULL) && GetLastError() != ERROR_ALREADY_EXISTS)
    {
        UT_AssertFail("Failed to find %s GetLastError = %u", dirname, GetLastError());
    }

    // iterate over all files and directories in the dirname and search for string in files

    WIN32_FIND_DATAA finddata;

    char szSearch[MAX_PATH + 1];
    UT_AssertIsTrue (strlen(dirname) + 2 < sizeof(szSearch));

    sprintf_s(szSearch, ARRAYSIZE(szSearch), "%s\\%s", dirname, filetype);

    HANDLE hSearch = FindFirstFileA(szSearch, &finddata);
    if (hSearch == INVALID_HANDLE_VALUE)
    {
        return FALSE;
    }

    while (true)
    {
        char szSourceName[MAX_PATH + 1];
        UT_AssertIsTrue(strlen(dirname) + 1 + strlen(finddata.cFileName) < sizeof(szSourceName));
        sprintf_s(szSourceName, ARRAYSIZE(szSourceName), "%s\\%s", dirname, finddata.cFileName);

        if (finddata.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
        {
            if (strcmp(finddata.cFileName, ".") && strcmp(finddata.cFileName, ".."))
            {
                // call self recursively on the subdir
                if (FindStringInFilesInDirectory(szSourceName, filetype, searchString))
                {
                    return TRUE;
                }
            }
        }
        else
        {
            // test for existence
            HANDLE hFile = CreateFileA(szSourceName, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                NULL, OPEN_EXISTING, NULL, NULL);

            if (hFile != INVALID_HANDLE_VALUE)
            {
                if (CUnitTestUtil::FindStringInLogFile(szSourceName, searchString))
                {
                    return TRUE;
                }
                CloseHandle(hFile);
            }
        }

        if (!FindNextFileA(hSearch, &finddata))
        {
            if (GetLastError() != ERROR_NO_MORE_FILES)
            {
                UT_AssertFail("FindNextFileA failed with GetLastError = %u", GetLastError());
            }
            break;
        }
    }

    if (!FindClose(hSearch))
    {
        UT_AssertFail("FindClose failed GetLastError = %u", GetLastError());
    }
    return FALSE;
}

BOOL CUnitTestUtil::FindStringInLogFile(const char *filename, const char *searchString)
{
    HANDLE hFile = CreateFileA(filename, GENERIC_READ, FILE_SHARE_WRITE | FILE_SHARE_READ | FILE_SHARE_DELETE,
        NULL, OPEN_EXISTING, NULL, NULL);
    
    UT_AssertIsTrue(hFile != INVALID_HANDLE_VALUE, "CreateFileA failed for %s with GetLastError = %u", filename, GetLastError());

    size_t cbFile;
    LARGE_INTEGER liFileSize;

    BOOL fRet = GetFileSizeEx(hFile, &liFileSize);
    UT_AssertIsTrue(fRet, "GetFileSizeEx failed for %s with GetLastError = %u", filename, GetLastError());
    
    cbFile = (size_t)liFileSize.QuadPart;
    // Files with size 0 cannot be mapped.
    if (cbFile == 0)
    {
        CloseHandle(hFile);
        return (!*searchString);
    }

    HANDLE hMapping = CreateFileMapping(hFile, NULL, PAGE_READONLY, 0, 0, NULL);
    CloseHandle(hFile);

    UT_AssertIsTrue(hMapping != NULL, "CreateFileMapping failed for %s with GetLastError = %u", filename, GetLastError());

    const char *pfile = (const char *)MapViewOfFile(hMapping, FILE_MAP_READ, 0, 0, 0);
    CloseHandle(hMapping);

    UT_AssertIsTrue(pfile != NULL, "MapViewOfFile failed for %s with GetLastError = %u", filename, GetLastError());

    const char *ploc = CStringUtil::FindStringIN(pfile, cbFile, searchString);

    fRet = UnmapViewOfFile(pfile);
    UT_AssertIsTrue(fRet, "UnmapViewOfFile failed for %s with GetLastError = %u", filename, GetLastError());

    // cannot dereference ploc after UnmapViewOfFile, but can compare against NULL
    if (ploc == NULL)
    {
        return FALSE;
    }

    return TRUE;

}

}