#pragma once

// Created by HaoyongZ, Nov 2003
// See private\tools\unittest\sample for sample usage
// This framework in trying to model some of functionalities of NUnit
// in unmanaged code. Documents about NUnit are checked in under
// private\tools\NUnit\doc.
// A schema for the NUnit output is checked in under
// private\tools\NUnit\src\framework\Results.xsd.
//
// This header file defines a framework to facilite and to standarize
// the implementation of UnitTest in unmanaged code.
//
// The basic steps to create unit test using this framework are:
//   1. Create instances of CUnitTestSuite class, each of them represent
//      a test-suite
//   2. Use the AddTestCase method in CUnitTestSuite class to add test
//      cases to the suite. Each test case should be a function that takes
//      no parameters and returns nothing.
//   3. When writing the test case, use macro provided by this header file
//      to assert condition.
//
// The class to use is
//
//     class CUnitTestSuite
//     {
//     public:
//         CUnitTestSuite (const char * cszSuiteName);
//         void AddTestCase (const char * cszTestCaseName, TestCaseFunction func);
//
//         static void RunAllSuites ();
//     }
//
// The available macros are:
//
//   UT_AssertIsTrue (bool fExpr, const char * cszFormat, ...)
//   UT_AssertIsFalse (bool fExpr, const char * cszFormat, ...)
//   UT_AssertIsNull (void * ptr, const char * cszFormat, ...)
//   UT_AssertIsNotNull (void * ptr, const char * cszFormat, ...)
//   UT_AssertAreSame (void * ptr1, void * ptr2, const char * cszFormat, ...)
//   UT_AssertAreEqual (INT64 v1, INT64 v2, const char * cszFormat, ...)
//   UT_AssertAreEqual (double v1, double v2, double precision, const char * cszFormat, ...)
//   UT_AssertAreEqual (const CHashValue & v1, const CHashValue & v2, const char * cszFormat, ...)
//   UT_AssertFail (const char * cszFormat, ...)
//   UT_AssertSzAreEqual (const char * csz1, const char * csz2, char * cszFormat, ...)
//   UT_AssertWzAreEqual (const WCHAR * cwz1, const WCHAR * cwz2, char * cszFormat, ...)
//
//   UT_AssertIsTrue (bool fExpr)
//   UT_AssertIsFalse (bool fExpr)
//   UT_AssertIsNull (void * ptr)
//   UT_AssertIsNotNull (void * ptr)
//   UT_AssertAreSame (void * ptr1, void * ptr2)
//   UT_AssertAreEqual (INT64 v1, INT64 v2)
//   UT_AssertAreEqual (double v1, double v2, double precision)
//   UT_AssertFail ()
//   UT_AssertSzAreEqual (const char * csz1, const char * csz2)
//   UT_AssertWzAreEqual (const WCHAR * cwz1, const WCHAR * cwz2)
//
// A typical usage will be:
//
//   void MyTest1 ()
//   {
//      int a, b;
//      // do some work with a, b
//      UT_AssertAreEqual (a, b, "some additional comments");
//      // do some more testing
//   }
//
//   int main (void)
//   {
//       CUnitTestSuite suite ("my first suite");
//       suite.AddTestCase (MyTest1);
//
//       // The function is optional. If it is not specifically
//       // called, all the test will be run after the "main"
//       // function exit. However, if you have other global
//       // variables that will be destructed, it would be
//       // safter to run all your test here to prevent any
//       // kind of conflict.
//       CUnitTestSuite::RunAllSuites ();
//
//       return 0;
//
//       // there will be some code running after main that
//       // triggers all the test. It was implemented as a
//       // destructor to a global variable.
//   }

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <Shellapi.h>

#include "queue.h"
#include "assertinfo.h"
#include "case.h"
#include "utility.h"
#include "suite.h"
#include "StringUtils.h"