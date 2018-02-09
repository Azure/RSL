#pragma once

// Created by HaoyongZ, Nov 2003
// Don't include this file directly, include "unittest.h" instead.

// These functions are supposed to be used internally for the UnitTest
// framework. It should be called or executed outside of this scope.

namespace RSLibImpl
{

typedef void (*TestCaseFunction) ();

// This class is used store the error information of a give test case.
class CUnitTestCaseError
{
public:
    char * m_szComment;
    char * m_szError;
    char * m_szSourceFile;
    UINT32 m_u32LineNumber;
};

// This class is used to store the test result of a test case.
class CUnitTestCaseResult
{
public:
    char * m_szTestName;
    CUnitTestCaseError * m_pError;
    double m_lfDuration;

    CUnitTestCaseResult (char * szTestName, double lfDuration, CUnitTestCaseError * pError);
    void WriteToLog (FILE * fpOutputXml);
    void WriteToConsole ();
};

// This lcass is used to store the necessary information to run a test case.
class CUnitTestCase
{
public:
    char * m_szTestName;
    //void (*m_func) ();
    TestCaseFunction m_func;

    CUnitTestCase (char * szTestName, TestCaseFunction func);
    CUnitTestCaseResult * Execute ();
    static char * GetCurrentTestName ();

private:
    static char * s_szCurrentTestName;
};

} // namespace RSLibImpl
