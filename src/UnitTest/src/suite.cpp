#include "unittest.h"

namespace RSLibImpl
{

// We need to run code before/after main().
// In order to do this, we create a helper class
// and declare a global variable of this helper class
// This way, the constructor of this class will be
// called before main and the destructor of this class will be
// be called when the global variable is out-of-scope,
// which is after main().
class CUnitTestHelper
{
public:
    CUnitTestHelper ();
    ~CUnitTestHelper ();
};

CUnitTestHelper g_CUnitTestHelper;

CUnitTestHelper::CUnitTestHelper ()
{
    CUnitTestSuiteInner::StaticInitialize ();
}

CUnitTestHelper::~CUnitTestHelper ()
{
    CUnitTestSuiteInner::StaticDeinitialize ();
}

// We need to initialize the static members of CUnitTestSuiteInner that are
// used to chain all the test-suites together.
CQueue<CUnitTestSuiteInner> * CUnitTestSuiteInner::ms_pqSuites = NULL;
CQueue<CUnitTestSuiteInner> * CUnitTestSuiteInner::ms_pqSuiteResults = NULL;
UINT32 CUnitTestSuiteInner::ms_u32NumFailures = 0;
HANDLE CUnitTestSuiteInner::ms_hHeap = NULL;

BOOL CUnitTestSuiteInner::StaticInitialize ()
{
    BOOL fSuccess = false;

    // many unittest program calls CommonInit(). CommonInit() would fail
    // if there wasn't an autopilot.ini file in the parent directory. And
    // the autopilot.ini file needs to point a data directory.
    //
    // To avoid each unittest program having to create their own autopilot.ini
    // file, we will binplace one right at "unittest" directory which will
    // be the parent directory to most of the unittest programs. That autopilot.ini
    // file will points to the directory below as the data dir. In here, we will
    // make sure the directory is created.
    //
    // If a specific unittest program needs anything more complicated than
    // provided here, it should arrange its own settings.
    if (!CreateDirectoryA ("\\unittestgenericdatadir", NULL))
    {
        if (ERROR_ALREADY_EXISTS != GetLastError())
        {
            goto ErrorHandling;
        }
    }

    if (NULL == (ms_hHeap = HeapCreate (0, 2*1024*1024 /* initial 2 M */, 0 /* unlimit growth */))
        || NULL == (ms_pqSuites = CQueue<CUnitTestSuiteInner>::CreateNew (ms_hHeap))
        || NULL == (ms_pqSuiteResults = CQueue<CUnitTestSuiteInner>::CreateNew (ms_hHeap)))
    {
        goto ErrorHandling;
    }

    fSuccess = true;
ErrorHandling:
    return fSuccess;
}

static bool g_RunAllSuitesCalled = false;
void CUnitTestSuiteInner::StaticDeinitialize ()
{
    // Make sure all the test suites got run before the main() function exists.
    if (!g_RunAllSuitesCalled)
    {
        printf ("Error: CUnitTestSuite::RunAllSuites() is not called before main() exists.\n");
    }
    else
    {
        int numTests = 0;
        CUnitTestSuiteInner * pSuite;
        while (ms_pqSuites->Dequeue (&pSuite))
        {
            CUnitTestCase * pCase;
            while (pSuite->m_pqCases->Dequeue (&pCase))
            {
                numTests ++;
            }
        }
        if (numTests > 0)
        {
            printf ("Error: %d test(s) did not get a chance to run.\n", numTests);
            printf ("Look for possible mini crash dumps in either \"\\minidumps\" or \"<data dir>\\minidumps\"\n");
            printf ("To enable DebugBreak on failures, set env. var UT_DEBUGBREAK=any-value.\n");
        }
    }
}

static LONG WINAPI HandleUnhandledException(EXCEPTION_POINTERS* exceptionPointers)
{
    printf ("Unit Tests cannot continue due to unhandled exception, "
            "trying to write a mini crash dump in either \"\\minidumps\" or \"<data dir>\\minidumps\"\n"
            "To enable DebugBreak on failures, set env. var UT_DEBUGBREAK=any-value.\n");
    bool fRet;
    RSLibImpl::Logger::WriteExceptionMiniDump (exceptionPointers, 15*60*1000, &fRet);
    if (!fRet)
        printf ("Failed to write out minidump\n");
    ::ExitProcess(1);
}

// This function runs all the test-suites.
UINT32 CUnitTestSuiteInner::RunAllSuites ()
{
    bool mustDebugBreakOnFailure = CUnitTestAssertInfo::MustDebugBreakOnFailure();
    // Do not unhandled exception filter if we want to DebugBreak on failure (otherwise the exception handler will also handle the "attach-to-debugger" exception)
    if ( ! mustDebugBreakOnFailure )
        SetUnhandledExceptionFilter(HandleUnhandledException);

    g_RunAllSuitesCalled = true;

    CUnitTestSuiteInner * pSuite;
    while (ms_pqSuites->Dequeue (&pSuite))
    {
        pSuite->RunTests ();
        if (!pSuite->m_fSuccess)
        {
            ms_u32NumFailures ++;
        }
        ms_pqSuiteResults->Enqueue (pSuite);
    }
    CUnitTestSuiteInner::WriteAllToLog ();

    return ms_u32NumFailures;
}

// This function display the output XML
// log file. It first display the summary
// information, and then display the
// information of each suite, and then
// output the outter level closing tags.
void CUnitTestSuiteInner::WriteAllToLog ()
{
    /*
     * figure out the log file name
     */
    static const size_t cbFile = MAX_PATH+1;
    char szExeName[cbFile];
    FILE * fpOutputXml = NULL;

    {
        GetModuleFileNameA (NULL, szExeName, cbFile);
        szExeName[cbFile-1] = '\0';

        char szLogName[cbFile];
        _snprintf (szLogName, cbFile-1, "%s.xml", szExeName);
        szLogName[cbFile-1] = '\0';

        fpOutputXml = fopen (szLogName, "w");
        if (fpOutputXml == NULL)
        {
            printf ("can't open log file %s to write\n", szLogName);
            exit (-1);
        }
    }

    /*
     * get the current time
     */
    SYSTEMTIME systime;
    CUnitTestUtil::GetTimeAndTicks (&systime);

    /*
     * output the xml
     */
    fprintf (fpOutputXml, "<?xml version=\"1.0\" encoding=\"utf-8\" standalone=\"yes\"?>\n");

    fprintf (fpOutputXml, "<test-results name=\"%s\" total=\"1\" failures=\"%I32u\" not-run=\"0\" date=\"%I32u/%I32u/%I32u\" time=\"%I32u:%.2I32u\">\n",
        szExeName, ms_u32NumFailures,
        (UINT32)systime.wMonth, (UINT32)systime.wDay, (UINT32)systime.wYear,
        (UINT32)systime.wHour, (UINT32)systime.wMinute);

    CUnitTestSuiteInner * pSuite;
    while (ms_pqSuiteResults->Dequeue (&pSuite))
    {
        pSuite->WriteToLog (fpOutputXml);
    }

    fprintf (fpOutputXml, "</test-results>\n");

    if (fpOutputXml != NULL)
    {
        fclose (fpOutputXml);
    }
}

// Constructor
CUnitTestSuiteInner::CUnitTestSuiteInner (const char * cszSuiteName)
{
    //TODO: handling possible exceptions
    m_szSuiteName = CUnitTestUtil::MakeStringCopyWithHtmlEncoding (cszSuiteName);
    m_pqCases = CQueue<CUnitTestCase>::CreateNew (ms_hHeap);
    m_pqResults = CQueue<CUnitTestCaseResult>::CreateNew (ms_hHeap);
    m_fSuccess = true;
    m_lfDuration = 0.0;
}

// Add a test case to the suite
void CUnitTestSuiteInner::AddTestCase (const char * cszTestCaseName, void(*func)())
{
    char * szNameCopy = CUnitTestUtil::MakeStringCopyWithHtmlEncoding (cszTestCaseName);
    CUnitTestCase * pNewCase = new CUnitTestCase (szNameCopy, func);
    m_pqCases->Enqueue (pNewCase);
}

// Run all the test in a suite
void CUnitTestSuiteInner::RunTests ()
{
    ULONGLONG ullBegin = CUnitTestUtil::GetTimeAndTicks (NULL);

    /*
     * Output to console to indicate that we are running this
     * suite, bright+green+blue color for the characters.
     */
    HANDLE hStdOut = GetStdHandle (STD_OUTPUT_HANDLE);
    CONSOLE_SCREEN_BUFFER_INFO bufferOriginal;
    GetConsoleScreenBufferInfo (hStdOut, &bufferOriginal);
    SetConsoleTextAttribute (hStdOut, FOREGROUND_BLUE|FOREGROUND_GREEN|FOREGROUND_INTENSITY);

    printf ("Running suite \"%s\" ...\n", m_szSuiteName);

    SetConsoleTextAttribute (hStdOut, bufferOriginal.wAttributes);

    /*
     * Run all the test cases, and collect all the test results.
     */
    CUnitTestCase * pCase;
    while (m_pqCases->Dequeue (&pCase))
    {
        CUnitTestCaseResult * pResult = NULL;

        pResult = pCase->Execute ();

        if (pResult->m_pError != NULL)
        {
            m_fSuccess = false;
        }

        m_pqResults->Enqueue (pResult);
    }

    ULONGLONG ullEnd = CUnitTestUtil::GetTimeAndTicks (NULL);

    m_lfDuration = CUnitTestUtil::DiffSeconds (ullBegin, ullEnd);
}

// Output the result of a test-suite in XML.
void CUnitTestSuiteInner::WriteToLog (FILE * fpXmlOutput)
{
    fprintf (fpXmlOutput, "<test-suite name=\"%s\" success=\"%s\" time=\"%lg\">\n",
        m_szSuiteName, m_fSuccess?"true":"false", m_lfDuration);

   // add <results>...</results> here...
   fprintf(fpXmlOutput,"<results>\n");

    CUnitTestCaseResult * pResult;
    while (m_pqResults->Dequeue (&pResult))
    {
        pResult->WriteToLog (fpXmlOutput);
    }

   fprintf(fpXmlOutput,"</results>\n");

    fprintf (fpXmlOutput, "</test-suite>\n");
}

CUnitTestSuite::CUnitTestSuite (const char * cszSuiteName)
{
    m_pInner = new CUnitTestSuiteInner (cszSuiteName);
    CUnitTestSuiteInner::ms_pqSuites->Enqueue (m_pInner);
}

void CUnitTestSuite::AddTestCase (const char * cszTestCaseName, TestCaseFunction func)
{
    m_pInner->AddTestCase (cszTestCaseName, func);
}

UINT32 CUnitTestSuite::RunAllSuites ()
{
    return CUnitTestSuiteInner::RunAllSuites ();
}

}
