#pragma once

// For stdout and fflush
#include <stdio.h>
#include <basic_types.h>
#include "ptr.h"
//#include "dynstring.h"
// For critical section

// For LogID's
#include "logids.h"

#pragma warning (push)
#pragma warning (disable: 4820) // '4' bytes padding added after data member 'XXX'

namespace RSLibImpl
{

// Logging
//
// To log something, call the Log(logid, level, title, ...) macro.  
//
// The "LogID" enumeration is a shared enumeration across all components.   Items which
// use the same LogID should be in a related activity (e.g. everything which happens
// during an URL merge, or everything at the TCP Transport layer).  You shouldn't be
// creating a new LogID for each log statement, that is just excessive.
//
// The "title" parameter can be anything you want.  You can consider "title" and
// "fmt, ..." to be related, separating them is just a way to keep the data and non-data
// parts of a log statement separate, for log parsing purposes.  The data associated
// with a particular log statement should be in the "fmt, ..." part.
//
// For the log level, there are 3 levels; informational, warning, and error.  Error
// is something that will get propagated to operations for someone to look at, as soon
// as that infrastructure is in place.
//
// In the config.ini file, you can use rules to state what goes where (to stdout, to
// certain logging files), and whether you want source file/function/line info
// to be output.
//
// The logging config can be updated at any time and the logging system will pick up
// the changes.
//
// To see/edit the logging enumeration, see logid.h and logid.cpp
//
// Sample config file:
//

/*
[Counters]
Filename=counters.prf

[LogRules]
Rule1=*,*,*,stdout
Rule2=*,*,*,dumpster
Rule3=Crawler,*,*,crawlerstuff

[stdout]
LogSourceInfo=1

[dumpster]
FileNameBase=everything
LogSourceInfo=1
MaxFiles=10
MaxFileSize=100000
BufferSize=60000

[crawlerstuff]
FileNameBase=c:\logs\crawlerstuff
LogSourceInfo=0
MaxFiles=10
MaxFileSize=100000
BufferSize=60000
*/

typedef enum
{
	// IMPORTANT:
	// IF YOU UPDATE THIS ENUM, MAKE SURE YOU DO CONSEQUENTLY WITH file ManagedRSLib.h, enum NotificationLevel
    LogLevel_Debug,
    LogLevel_Info,
    LogLevel_Status,
    LogLevel_Warning,
    LogLevel_Error,
    LogLevel_Assert, 
    LogLevel_Alert
} LogLevel;

// These are the data types supported by log tags
// The data type is actually stored in the high order bits of the log tag ID
typedef enum
{
    LogTagType_None = 0,
    LogTagType_Int16,
    LogTagType_UInt16,
    LogTagType_Int32,
    LogTagType_Int64,
    LogTagType_String,
    LogTagType_Float,
    LogTagType_Hex32,
    LogTagType_Hex64,
    LogTagType_UInt32,
    LogTagType_UInt64,
} LogTagType;

// Macros for dealing with log tags

// Given a log tag, retrieve the LogTagType
#define LogTagToType(tag) ((LogTagType) ((tag) >> 24))

// Given a log tag, retrieve just the index
#define LogTagToIndex(tag) ((tag) & 0x00FFFFFF)

// Given a log tag type and log tag index, create the tag value
#define MakeLogTag(index,type) (((type)<<24)+(index))

// Now we declare the log tag enumeration, by including logtagids.h, which happens to
// also be included in logging.cpp but with a different #define for DeclareTag()
#define DeclareTag(index, id, name, type) id=MakeLogTag(index,type)

// Declare the log tag enumeration
typedef enum
{
#include "logtagids.h"
} LogTag;

// Max size of a single log entry
// Anything larger than this will get truncated when written
const int c_logMaxEntrySize = 8192;

 class Trace
 {
#define CONVERT(_x, _y) _y = Verify(_x, _y);
    
     static const int c_BufSize = 1024;

     void Write(LogTag tag, ...);
     public:
     Trace()
     {}

     template<class T> T Verify(LogTag /* tag */, T obj)
     {
         return obj;
     }

     template <class T> T* Verify(LogTag /* tag */, T* obj)
     {
         return obj;
     }

     void __cdecl Convert()
     {
         *Buffer = '\0';
     }

     void __cdecl Convert(const char* fmt, ...);
            
     template<class V1>
     void __cdecl Convert(LogTag tag1, V1 val1, LogTag end = LogTag_End)
     {
         CONVERT(tag1, val1);
         Write(tag1, val1, end);
     }

     template<class V1, class V2>
     void __cdecl Convert(LogTag tag1, V1 val1,
                          LogTag tag2, V2 val2, LogTag end = LogTag_End)
     {
         CONVERT(tag1, val1);
         CONVERT(tag2, val2);
         Write(tag1, val1, tag2, val2, end);
     }
    
     template<class V1, class V2, class V3>
     void __cdecl Convert(LogTag tag1, V1 val1,
                          LogTag tag2, V2 val2, LogTag tag3, V3 val3,
                          LogTag end = LogTag_End)
     {
         CONVERT(tag1, val1);
         CONVERT(tag2, val2);
         CONVERT(tag3, val3);
         Write(tag1, val1, tag2, val2, tag3, val3, end);
     }

     template<class V1, class V2, class V3, class V4>
     void __cdecl Convert(LogTag tag1, V1 val1,
                          LogTag tag2, V2 val2, LogTag tag3, V3 val3,
                          LogTag tag4, V4 val4,
                          LogTag end = LogTag_End)
     {
         CONVERT(tag1, val1);
         CONVERT(tag2, val2);
         CONVERT(tag3, val3);
         CONVERT(tag4, val4);
         Write(tag1, val1, tag2, val2, tag3, val3, tag4, val4, end);
     }

     template<class V1, class V2, class V3, class V4, class V5>
     void __cdecl Convert(LogTag tag1, V1 val1,
                          LogTag tag2, V2 val2, LogTag tag3, V3 val3,
                          LogTag tag4, V4 val4, LogTag tag5, V5 val5,
                          LogTag end = LogTag_End)
     {
         CONVERT(tag1, val1);
         CONVERT(tag2, val2);
         CONVERT(tag3, val3);
         CONVERT(tag4, val4);
         CONVERT(tag5, val5);
         Write(tag1, val1, tag2, val2, tag3, val3, tag4, val4, tag5, val5, end);
     }
    
     template<class V1, class V2, class V3, class V4, class V5, class V6>
     void __cdecl Convert(LogTag tag1, V1 val1,
                          LogTag tag2, V2 val2, LogTag tag3, V3 val3,
                          LogTag tag4, V4 val4, LogTag tag5, V5 val5,
                          LogTag tag6, V6 val6,
                          LogTag end = LogTag_End)
     {
         CONVERT(tag1, val1);
         CONVERT(tag2, val2);
         CONVERT(tag3, val3);
         CONVERT(tag4, val4);
         CONVERT(tag5, val5);
         CONVERT(tag6, val6);
         Write(tag1, val1, tag2, val2, tag3, val3, tag4, val4, tag5, val5, tag6, val6, end);
     }
    
     template<class V1, class V2, class V3, class V4, class V5, class V6, class V7>
     void __cdecl Convert(LogTag tag1, V1 val1,
                          LogTag tag2, V2 val2, LogTag tag3, V3 val3,
                          LogTag tag4, V4 val4, LogTag tag5, V5 val5,
                          LogTag tag6, V6 val6, LogTag tag7, V7 val7,
                          LogTag end = LogTag_End)
     {
         CONVERT(tag1, val1);
         CONVERT(tag2, val2);
         CONVERT(tag3, val3);
         CONVERT(tag4, val4);
         CONVERT(tag5, val5);
         CONVERT(tag6, val6);
         CONVERT(tag7, val7);
         Write(tag1, val1, tag2, val2, tag3, val3, tag4, val4, tag5, val5, tag6, val6, tag7, val7, end);
     }
    
     template<class V1, class V2, class V3, class V4, class V5, class V6, class V7, class V8>
     void __cdecl Convert(LogTag tag1, V1 val1,
                          LogTag tag2, V2 val2, LogTag tag3, V3 val3,
                          LogTag tag4, V4 val4, LogTag tag5, V5 val5,
                          LogTag tag6, V6 val6, LogTag tag7, V7 val7,
                          LogTag tag8, V8 val8,
                          LogTag end = LogTag_End)
     {
         CONVERT(tag1, val1);
         CONVERT(tag2, val2);
         CONVERT(tag3, val3);
         CONVERT(tag4, val4);
         CONVERT(tag5, val5);
         CONVERT(tag6, val6);
         CONVERT(tag7, val7);
         CONVERT(tag8, val8);
         Write(tag1, val1, tag2, val2, tag3, val3, tag4, val4, tag5, val5, tag6, val6, tag7, val7, tag8, val8, end);
     }

     template<class V1, class V2, class V3, class V4, class V5, class V6, class V7, class V8, class V9>
     void __cdecl Convert(LogTag tag1, V1 val1,
                          LogTag tag2, V2 val2, LogTag tag3, V3 val3,
                          LogTag tag4, V4 val4, LogTag tag5, V5 val5,
                          LogTag tag6, V6 val6, LogTag tag7, V7 val7,
                          LogTag tag8, V8 val8, LogTag tag9, V9 val9,
                          LogTag end = LogTag_End)
     {
         CONVERT(tag1, val1);
         CONVERT(tag2, val2);
         CONVERT(tag3, val3);
         CONVERT(tag4, val4);
         CONVERT(tag5, val5);
         CONVERT(tag6, val6);
         CONVERT(tag7, val7);
         CONVERT(tag8, val8);
         CONVERT(tag9, val9);
         Write(tag1, val1, tag2, val2, tag3, val3, tag4, val4, tag5, val5, tag6, val6, tag7, val7, tag8, val8, tag9, val9, end);
     }

     template<class T> Ptr<T> Verify(LogTag tag, Ptr<T> obj);
//     DynString Verify(LogTag tag, DynString obj);

     char Buffer[c_BufSize];
 };

#define Log(logid, level, title, ...) do { \
Trace t; \
t.Convert(__VA_ARGS__); \
LogInternal(logid, level, title, "%s", t.Buffer); \
} while (0)


// This is the macro used to log things
// Usage: Log(logarea, level, title, fmtstring, ...)
#define LogInternal LogTraceInfo(__FILE__, __FUNCTION__, __LINE__)

typedef void (*LogEntryCallbackInternal)(const char *file, const char *function, int line, int logId, int level, const char *title, const char *fmt, va_list args);

// This class encapsulates all of logging
class Logger
{
public:
    
    // Initializes logging
    static BOOL Init(const char * path);

    static void Stop();

    // Helper function called by Log() macro
    static void  LogV(const char *file, const char *function, const int line, LogID logID, LogLevel level, const char *title, const char *fmt, va_list args);

    static int   CreateFileLogDestination(const char *name, const char* fileName, int maxSize, int maxFiles);
    static BOOL AddRule(const char* area, const char* levels, const char* outputDestination);
    
    // Flush the contents to disk
    // You shouldn't really be calling this, but it's there because there is 
    // intentionally no "clean shutdown" code in the system.
    static void  Flush(bool flushBuffers = false);

    // This function handles all unhandled exceptions. It is hooked up
    // to the global exception handler in CommonInit.
    static LONG LogAndExitProcess(EXCEPTION_POINTERS*);

	// This function sets the callback to be called when an assertion is logged.
	// when the thread comes back, a MiniDump will be taken and the process will be exited.
	// It is legal to not return from this call.
	static void SetNotificationsCallback( void (*callback)(int level, int logId, const char* title, const char *message) )
	{
		Logger::m_notificationsCallback = callback;
	}

    // This function sets the flag controlling whether minidumps will be taken or not.
    // If false, a failfast exception will be raised following a LogAssert call, in order to
    // invoke the system-defined post-mortem debugger instead.
    static void SetMinidumpEnabled(bool enabled)
    {
        Logger::m_minidumpEnabled = enabled;
    }

    // This function invokes the failfast behavior depending on the m_minidumpEnabled flag.
    static void FailFast(const char* message);

	// if there is a callback defined, call it.
	static void CallNotificationsIfDefined(int level, int logId, const char *title, const char *message = NULL)
	{
		// to do so, copy it locally first, to avoid a race with the removal
		void (*cb)(int level, int logId, const char *title, const char *message) = m_notificationsCallback;

		if (cb != NULL)
		{
			cb(level, logId, title, message);
		}
	}

    // This function sets the callback to be called when a log entry is logged.
    static void SetLogEntryCallback(LogEntryCallbackInternal logEntryCallback)
    {
        Logger::m_logEntryCallback = logEntryCallback;
    }

    // calls waitforsingleobject on the handle. If the handle does not
    // get signalled within some timeout, exits the process
    static void WaitForThreadOrExit(HANDLE handle, DWORD threadID);

    // Terminates the process using ::TerminateProcess API
    __declspec(noreturn) static void Terminate(UINT ec);
    
    // Write out a mini dump of the running thread.
    // Timeout == 0 means never timeout.
    static bool WriteMiniDump (unsigned int timeoutMilliSeconds);

    // Write out a mini dump when exception pointers are know. This function should be called from
    // an exception handling code block.
    static LONG WriteExceptionMiniDump (EXCEPTION_POINTERS* exceptionPointers, unsigned int timeoutMilliSeconds, bool* pRet);

    // Return the full path of the directory where minidumps and crash logs will be written to.
    // This function is exposed to allow other programs to find out where the minidumps will be stored.
    static bool GetMiniDumpDataDir (char * szMiniDumpDataDir, size_t cbMiniDumpDataDir);

    // For every minidump written, we will write out a crash log file. It contains information about
    // the process that was writing the minidump and when the process expected to be considered as hung.
    // This function will go through the minidump data dir and make sure those hunging process
    // are killed.
    // Note: the expire time is normally the current system time. We expose the parameter to make it easier
    // for testing. I don't want to use any date time data structure we implemented (such as APDateTime) because
    // I want to avoid circluar includes.
    static bool KillPossibleHungMiniDumpWritingProcess (const SYSTEMTIME& stExpire);

    static volatile int m_numRules;

    // Directory in which to create the log files
    static char              m_logsDir[MAX_PATH];
    static char              m_minidumpsDir[MAX_PATH];

    static volatile int      m_numLogDestinations;

private:
	static void (*m_notificationsCallback)(int level, int logId, const char* title, const char* message);

    static LogEntryCallbackInternal m_logEntryCallback;
    static bool m_minidumpEnabled;
};

// Fake class enabling us to log file, function, line, without explicitly specifying parameters
class LogTraceInfo
{
public:
    LogTraceInfo(const char *file, const char *function, int line)
        : m_Filename(file), m_Function(function), m_Line(line)
    {}

    void __cdecl operator()(LogID logID, LogLevel level, const char *title, const char *pszFmt, ...) const
    {
        va_list ptr; va_start(ptr, pszFmt);
        Logger::LogV(m_Filename, m_Function, m_Line, logID, level, title, pszFmt, ptr);
        va_end(ptr);
    }
    
    void __cdecl operator()(LogID logID, LogLevel level, const char *title) const
    {
        Logger::LogV(m_Filename, m_Function, m_Line, logID, level, title, NULL, NULL);
    }

private:
    const char *m_Filename;
    const char *m_Function;
    int m_Line;
};

} // namespace RSLibImpl

#pragma warning (pop)


#include "logassert.h"
