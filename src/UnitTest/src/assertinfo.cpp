#include "unittest.h"

namespace RSLibImpl
{

int CUnitTestAssertInfo::_debugBreakOnFailure = 0;

// Constructor
CUnitTestAssertInfo::CUnitTestAssertInfo (const char * cszFile, UINT32 u32LineNumber)
: m_cszFile (cszFile), m_u32LineNumber (u32LineNumber)
{
    m_pszError = NULL;
    m_pszComment = NULL;
}

CUnitTestAssertInfo::~CUnitTestAssertInfo()
{
    if (m_pszError)
    {
        free(m_pszError);
        m_pszError = NULL;
    }
    if (m_pszComment)
    {
        free(m_pszComment);
        m_pszComment = NULL;
    }
}

// LOCAL_FORMAT_STRING_HELPER does the work to format a string
// using the format string and "..." variables. It has to be a
// a macro because we cannot pass the "..." variables to a function.
#define LOCAL_FORMAT_STRING_HELPER(str, size, format) \
    if (format != NULL) {\
    va_list __ptr; \
    va_start (__ptr, format); \
    int __nMaxLength = size / sizeof (str[0]) - 1; \
    _vsnprintf_s (str, size, __nMaxLength, format, __ptr); \
    str[__nMaxLength] = '\0'; \
    va_end (__ptr);\
    }

// INTERNAL_HANDLER does the common task for all the unittest assert.
// It has to be a macro in order to process the "..." variables.
// We will only display the error message if fExpr is false, therefore
// we don't try to get the formatted string unless fExpr is false.
#define INTERNAL_HANDLER(fExpr,format) \
    if (!(fExpr)) {\
        m_pszError = (char *) malloc(4096 * sizeof(char));\
        *m_pszError = '\0';\
        LOCAL_FORMAT_STRING_HELPER (m_pszError, 4096, format);\
    }\
    InternalAssertHandler ((fExpr));

// Different type of failures will has different type of comments, comments such as
// "was 5 expected 6", "was false expected true", etc. This is a helper function
// to set the comment. Note that if fExpr is true, the comment will not be displayed,
// thus we don't need to set it.
void CUnitTestAssertInfo::SetComment (BOOL fExpr, const char * cszCommentFormat, ...)
{
    if (!fExpr)
    {
        m_pszComment = (char *) malloc(1024 * sizeof(char));
        if ( m_pszComment != NULL )
        {
            *m_pszComment = '\0';
            LOCAL_FORMAT_STRING_HELPER (m_pszComment, 1024, cszCommentFormat);
        }
    }
}

// This is the core code to handle the UT_Assert* macros.
// If the expression passed in is false, it will throw an
// exception with the necessary error information.
void CUnitTestAssertInfo::InternalAssertHandler (BOOL fExpr)
{
    if (!fExpr)
    {
        CUnitTestCaseError * pError = new CUnitTestCaseError ();
        pError->m_szComment = CUnitTestUtil::MakeStringCopy (m_pszComment);
        pError->m_szError = CUnitTestUtil::MakeStringCopy (m_pszError);
        pError->m_szSourceFile = CUnitTestUtil::MakeStringCopy (m_cszFile);
        pError->m_u32LineNumber = m_u32LineNumber;

        if ( MustDebugBreakOnFailure() ) {
            DebugBreak(); // Force break into debugger
        }

        throw pError;
    }
}

void CUnitTestAssertInfo::IsTrueHandler (BOOL fExpr, const char * cszFormat, ...)
{
    SetComment (fExpr, "was false expected true");
    INTERNAL_HANDLER (fExpr, cszFormat);
}

void CUnitTestAssertInfo::IsFalseHandler (BOOL fExpr, const char * cszFormat, ...)
{
    SetComment (!fExpr, "was true expected false");
    INTERNAL_HANDLER (!fExpr, cszFormat);
}

void CUnitTestAssertInfo::IsNotNullHandler (const void * ptr, const char * cszFormat, ...)
{
    BOOL fExpr = ptr != NULL;

    SetComment (fExpr, "was null expected not null");
    INTERNAL_HANDLER (fExpr, cszFormat);
}

void CUnitTestAssertInfo::IsNullHandler (const void * ptr, const char * cszFormat, ...)
{
    BOOL fExpr = ptr == NULL;

    SetComment (fExpr, "was not null expected null");
    INTERNAL_HANDLER (fExpr, cszFormat);
}

void CUnitTestAssertInfo::AreSameHandler (const void * ptr1, const void * ptr2, const char * cszFormat, ...)
{
    BOOL fExpr = ptr1 == ptr2;

    SetComment (fExpr, "were different pointers expected the same");
    INTERNAL_HANDLER (fExpr, cszFormat);
}

void CUnitTestAssertInfo::AreEqualHandler (INT64 v1, INT64 v2, const char * cszFormat, ...)
{
    BOOL fExpr = v1 == v2;

    SetComment (fExpr, "was %I64d expected %I64d", v2, v1);
    INTERNAL_HANDLER (fExpr, cszFormat);
}

void CUnitTestAssertInfo::AreEqualHandler (double lf1, double lf2, double lfPrecision, const char * cszFormat, ...)
{
    BOOL fExpr = lf1 - lf2 < lfPrecision && lf2 - lf1 < lfPrecision;
    SetComment (fExpr, "was %lg expected %lg", lf2, lf1);
    INTERNAL_HANDLER (fExpr, cszFormat);
}

void CUnitTestAssertInfo::FailHandler (const char * cszFormat, ...)
{
    SetComment (FALSE, "forced failure");
#pragma warning(push)
#pragma warning(disable:4127)  // conditional expression is constant.
    INTERNAL_HANDLER (FALSE, cszFormat);
#pragma warning(pop)
}

void CUnitTestAssertInfo::SzAreEqualHandler (const char * csz1, const char * csz2, const char * cszFormat, ...)
{
    BOOL fExpr = TRUE;

    UINT32 i = 0;
    for (i = 0; csz1[i] != '\0' || csz2[i] != '\0'; i ++)
    {
        if (csz1[i] != csz2[i])
        {
            fExpr = FALSE;
            break;
        }
    }

    if (!fExpr)
    {
        SetComment (fExpr, "was\n\"%s\"\nexpected\n\"%s\"\nfirst difference occured at Index %I32u",
            csz2, csz1, i);
    }

    INTERNAL_HANDLER (fExpr, cszFormat);
}

void CUnitTestAssertInfo::WzAreEqualHandler (const WCHAR * cwz1, const WCHAR * cwz2, const char * cszFormat, ...)
{
    BOOL fExpr = TRUE;

    UINT32 i = 0;
    for (i = 0; cwz1[i] != L'\0' || cwz2[i] != L'\0'; i ++)
    {
        if (cwz1[i] != cwz2[i])
        {
            fExpr = FALSE;
            break;
        }
    }

    if (!fExpr)
    {
        char sz1[MAX_PATH], sz2[MAX_PATH];
        WideCharToMultiByte (CP_UTF8, 0, cwz1, -1, sz1, sizeof (sz1), NULL, NULL);
        WideCharToMultiByte (CP_UTF8, 0, cwz2, -1, sz2, sizeof (sz2), NULL, NULL);

        SetComment (fExpr, "was\n\"%s\"\nexpected\n\"%s\"\nfirst difference occured at Index %I32u",
            sz2, sz1, i);
    }

    INTERNAL_HANDLER (fExpr, cszFormat);
}

bool CUnitTestAssertInfo::MustDebugBreakOnFailure()
{
    if ( _debugBreakOnFailure == 0 ) {
        char value[20];
        if ( GetEnvironmentVariableA( "UT_DEBUGBREAK", value, sizeof(value) ) > 0 )
            _debugBreakOnFailure = 1;
        else
            _debugBreakOnFailure = 2;
    }

    return _debugBreakOnFailure == 1;
}

}