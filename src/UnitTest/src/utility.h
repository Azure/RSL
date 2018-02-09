#pragma once

#include "basic_types.h"
#include "logging.h"

// Created by HaoyongZ, Nov 2003
// Don't include this file directly, include "unittest.h" instead.

// The class CUnitTestUtil contains bunch of utility functions used to implement the unmanaged
// unittest framework. None of these functions are intended to server outside of this
// scope.
// See implement for more detailed information.

namespace RSLibImpl
{

class CUnitTestUtil
{
public:
    static char * MakeStringCopy (const char * cszInput);
    static char * MakeStringCopyWithHtmlEncoding (const char * cszInput);
    static ULONGLONG GetTimeAndTicks (SYSTEMTIME * pFill);
    static double DiffSeconds (ULONGLONG ullEnd, ULONGLONG ullBegin);

    static void InternalAssertHandler (BOOL fExpr, const char * cszComment, const char * cszError, const char * cszSourceFile, UINT32 u32LineNumber);
    static void LogTestCaseBegin (const char * cszName);
    static void AreEqual (INT64 n1, INT64 n2, const char * cszError, const char * cszSourceFile, UINT32 u32LineNumber);
    static void AreAlmostEqual (double lf1, double lf2, double lfPrecision, const char * cszError, const char * cszSourceFile, UINT32 u32LineNumber);

    static void SleepLoop(UInt32 seconds, UInt32 display);
    static void TouchFile(const char *filename);
    static BOOL FileExists(const char *filename);
    static void RewriteConfigParameter(const char *filename, const char *section,
                            const char *param, const char *value);
    static void ReadConfigParameter( const char *filename, const char *section,
                                     const char *param, char *outValue,
                                     const size_t outValueLen );

    static BOOL PurgeDirectory(const char *pszDir);
    static void MirrorDirectoryTree(const char *sourceDir, const char *destDir);
    static BOOL FindStringInLogFile(const char *filename, const char *searchString);
    static BOOL FindStringInFilesInDirectory(const char *dirname, const char* filetype, const char *searchString);
};

} // namespace RSLibImpl
