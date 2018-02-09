#include <windows.h>
#include "LogAssert.h"
#include "RefCount.h"
#include "DynString.h"

using namespace RSLibImpl;

static bool g_RefCount_AssertOnError = false;

void RefCount::SetErrorMode(bool AssertOnError)
{
    g_RefCount_AssertOnError = AssertOnError;
}




