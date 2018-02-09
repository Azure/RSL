#pragma once

#include "logging.h"
#include "message.h"
#include "dynstring.h"

using namespace RSLibImpl;

#define RSLDebug(title, ...)   Log (LogID_RSLLIB, LogLevel_Debug, title, __VA_ARGS__)
#define RSLInfo(title, ...)    Log (LogID_RSLLIB, LogLevel_Info, title, __VA_ARGS__)
#define RSLWarning(title, ...) Log (LogID_RSLLIB, LogLevel_Warning, title, __VA_ARGS__)
#define RSLError(title, ...)   Log (LogID_RSLLIB, LogLevel_Error, title, __VA_ARGS__)
#define RSLAlert(title, ...)   Log (LogID_RSLLIB, LogLevel_Alert, title, __VA_ARGS__)

