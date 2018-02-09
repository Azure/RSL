#pragma once

#include "logging.h"
#include <crtdbg.h>
#include <stdio.h>
#include <stdlib.h>

#define LogAssert(exp, ...) \
do { \
   if (!(exp)) { \
        LogAssertInternal(#exp, __VA_ARGS__ ); \
    } \
} while (0)

