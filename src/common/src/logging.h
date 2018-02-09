#pragma once


#define LogAssertInternal(exp, ...)  \
do { \
LogInternal(LogID_Common, LogLevel_Assert, exp, __VA_ARGS__); \
char assertMessage[1024]; \
sprintf_s(assertMessage, "Assert -- %s, %d: %s", __FILE__, __LINE__, exp); \
printf("%s\n", assertMessage); \
RSLibImpl::Logger::FailFast(assertMessage); \
} while (0)

#include "logging_old.h"
