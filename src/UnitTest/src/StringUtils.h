#pragma once

#include <stdio.h>
#include <stdlib.h>
#include "basic_types.h"
#include "logging.h"

namespace RSLibImpl
{

class CStringUtil
{
public:
    static int MyTolower(int ch);
    static char *FindStringI(const char * str1, const char * str2);
    static char * FindStringIN(const char * str1, size_t cchBuffer, const char * str2);
    static bool StringEqual(const char * str1, const char * str2, size_t cchBuffer);
};

} // namespace RSLibImpl
