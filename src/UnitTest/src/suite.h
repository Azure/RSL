#pragma once

// Created by HaoyongZ, Nov 2003
// Don't include this file directly, include "unittest.h" instead.

// This class implements the logic for "test-suite" in the unittest.
// CUnitTestSuiteInner contains most of the work, however, user should
// never call any of its functions.
// CUnitTestSuite is the interface we expose to the user.

// The instance members/methods of CUnitTestSuiteInner is used to manage
// all the test cases and suite level test results.

// The static members/methods of CUnitTestSuiteInner is used to chain all
// the test suites together. We need to chain them out so in order to be
// able to output the summary information at the beginning of the log file.

namespace RSLibImpl
{

class CUnitTestSuiteInner
{
public:
    CUnitTestSuiteInner (const char * cszSuiteName);
    void AddTestCase (const char * cszTestCaseName, TestCaseFunction func);

private:
    void RunTests ();
    void WriteToLog (FILE * fpXmlOutput);
    
    CQueue<CUnitTestCase> * m_pqCases;
    CQueue<CUnitTestCaseResult> * m_pqResults;
    char * m_szSuiteName;
    BOOL m_fSuccess;
    double m_lfDuration;

public:
    static BOOL StaticInitialize ();
    static void StaticDeinitialize ();

    static CQueue<CUnitTestSuiteInner> * ms_pqSuites;

    static UINT32 RunAllSuites ();

private:
    static void WriteAllToLog ();

    static CQueue<CUnitTestSuiteInner> * ms_pqSuiteResults;
    static HANDLE ms_hHeap;
    static UINT32 ms_u32NumFailures;
};


class CUnitTestSuite
{
public:
    CUnitTestSuite (const char * cszSuiteName);
    void AddTestCase (const char * cszTestCaseName, TestCaseFunction func);

    static UINT32 RunAllSuites ();

private:
    CUnitTestSuiteInner * m_pInner;
};

} // namespace RSLibImpl
