#pragma once
#include "unittest.h"

using namespace std;

static const int c_SleepTime = 20;
static const int c_WaitIterations = 5*(60*1000)/c_SleepTime;

// Makes the current thread wait until condition _x becomes true.
// Asserts if _x is still false after 1 minute.
#define WaitUntil(_x) \
    for (int _i = 0; _i <= c_WaitIterations && (_x) == false; _i++) \
    { \
        if (_i == c_WaitIterations) { \
            UT_AssertFail(#_x); \
        } \
        Sleep(c_SleepTime); \
    }

#define CalcMajority(_size) (((_size) / 2) + 1)

#define CallForAll(_list,_func) \
    for (size_t _i=0; _i < (*_list).size(); _i++) \
    { \
        (*_list)[_i]->_func; \
    } 
