#include "unittest.h"

namespace RSLibImpl
{

// Constructor
CUnitTestCaseResult::CUnitTestCaseResult (char * szTestName, double lfDuration, CUnitTestCaseError * pError)
{
    this->m_szTestName = szTestName;
    this->m_lfDuration = lfDuration;
    this->m_pError = pError;
}

// Output a test case result in XML
void CUnitTestCaseResult::WriteToLog (FILE * fpOutputXml)
{
    bool fSuccess = m_pError == NULL;

    fprintf (fpOutputXml,
        "<test-case name=\"%s\" executed=\"true\" success=\"%s\" time=\"%lg\"%s\n",
        m_szTestName, fSuccess?"true":"false", m_lfDuration, fSuccess?"/>":">");
    
    if (!fSuccess)
    {
        fprintf (fpOutputXml, 
            "<![CDATA[\nAt %s:%I32u, %s, %s\n]]>\n</test-case>\n",
            m_pError->m_szSourceFile, m_pError->m_u32LineNumber, m_pError->m_szComment, m_pError->m_szError);
    }
}

// Display a test case result to a console, output any error in red.
void CUnitTestCaseResult::WriteToConsole ()
{
    bool fSuccess = m_pError == NULL;

    printf ("Test %s has %s in %lf seconds\n", m_szTestName, fSuccess?"Succeeded":"Failed", m_lfDuration);

    if (!fSuccess)
    {
        HANDLE hStdOut = GetStdHandle (STD_OUTPUT_HANDLE);
        CONSOLE_SCREEN_BUFFER_INFO bufferOriginal;
        GetConsoleScreenBufferInfo (hStdOut, &bufferOriginal);
        SetConsoleTextAttribute (hStdOut, FOREGROUND_RED|FOREGROUND_INTENSITY);

        printf ("At %s:%I32u: %s, %s\n", 
            m_pError->m_szSourceFile, m_pError->m_u32LineNumber, m_pError->m_szComment, m_pError->m_szError);

        SetConsoleTextAttribute (hStdOut, bufferOriginal.wAttributes);
    }
}

// constructor
CUnitTestCase::CUnitTestCase (char * szTestName, void(*func)())
{
    this->m_szTestName = szTestName;
    this->m_func = func;
}

char * CUnitTestCase::s_szCurrentTestName = NULL;

char * CUnitTestCase::GetCurrentTestName ()
{
    return s_szCurrentTestName;
}

// Execute a test case, if any of the UT_Assert* macros failed, an
// exception with "CUnitTestCaseError *" will be thrown. We will return
// the test result.
CUnitTestCaseResult * CUnitTestCase::Execute ()
{
    ULONGLONG ullBegin = CUnitTestUtil::GetTimeAndTicks (NULL);
    
    // Set the current test name
    s_szCurrentTestName = m_szTestName;

    CUnitTestCaseError * pError = NULL;
    try
    {
        this->m_func ();
    }
    catch (CUnitTestCaseError * pExceptionError)
    {
        pError = pExceptionError;
    }
    //TODO: should we handle other exception cases, and if we do, how
    // do we display it?

    ULONGLONG ullEnd = CUnitTestUtil::GetTimeAndTicks (NULL);
    double lfDuration = CUnitTestUtil::DiffSeconds (ullBegin, ullEnd);

    CUnitTestCaseResult * pResult = new CUnitTestCaseResult (m_szTestName, lfDuration, pError);

    pResult->WriteToConsole ();
    return pResult;
}
}