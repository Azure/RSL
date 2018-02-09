

#include <windows.h>
#include <process.h>
#include <stdlib.h>
#include <strsafe.h>
#include <stddef.h>
#include <crtdbg.h>
#include <io.h>
#include <DbgHelp.h>
#include <time.h>
#include <shlobj.h>

#define DECLARE_DATA

#include "logging.h"
#include "DirUtils.h"
#include "scopeguard.h"
#include "datetime.h"
#include "message.h"

namespace RSLibImpl
{

// Max number of rules
    const int c_logMaxRules = 128;

// Can't be any higher than this, since we are using a bitmap
    const int c_logMaxLogDestinations = 64;

// Max name size for a log destination (e.g. "stdout")
    const int c_logMaxLogDestinationNameLength = MAX_PATH;

    // This is the count of valid tag indices
// There is no log tag index higher than this one
    const int LogTagCountIndex = LogTagToIndex(LogTag_End);

// Max size of file, function, line text when added to the log file
    const int c_logMaxSourceInfoSize = 256;

// This is used in the data structure below
    const int c_maxLogTagNameSize = 32;

    static const int c_TimeStringLength = 24;
// Every log message has UTCimeStamp field.
    static const int c_UTCTimeStampLength = 18;
    static const int c_TimeStringLengthWithMs = 24;

// Default values if they are not present in the config file, or the config file is not present
    const int c_defaultLoggingWriteBufferSize = 32768;
    const int c_defaultLoggingLogSourceInfo = TRUE;


// An abstract class indicating a destination for log text
// This could be stdout, debug output string, a file, etc.
    class LogDestination
    {
        public:

        virtual         ~LogDestination()
        {
        }

        // Initialize
        virtual BOOL     Init(const char *name);

        // Flush the log file
        virtual void     Flush(bool)
        {
        }

        // Append data to the log destination.  buffers are NOT guaranteed
        // to be null terminated.
        // Does NOT add a \r\n for you
        // You must have the lock to call this function
        // prefix contains the "LogLevel,date,LogID,Title"
        // srcInfo contains "SrcFile,srcFunc,SrcLine"
        // desc constains "Pid,Tid,User format string"
        virtual void AppendData(char *prefix, int prefixCount,
                                char* srcInfo, int srcInfoCount,
                                char* desc, int descCount) = NULL;

        void             LockBuffer();
        void             UnlockBuffer();

        void             MakeNullTerminatedString(char *dest, int destSize,
                                                  const char *source, int sourceCount,
                                                  char** end, int* remaining);

        public:
        // Section name of this destination in the .INI file
        char             m_name[c_logMaxLogDestinationNameLength];

        // A flag indicating whether we should log file/function/line numbers
        BOOL             m_logSourceInfo;

        protected:
        CRITSEC          m_criticalSection;
    };


// Implements a log destination that is stdout
    class LogDestinationStdout : public LogDestination
    {
        public:
        // Implements base class function
        void AppendData(char *prefix, int prefixCount,
                        char* srcInfo, int srcInfoCount,
                        char* desc, int descCount);
        virtual void Flush(bool) { fflush(stdout); }
    };

// Implements a log destination that is stderr
    class LogDestinationStderr : public LogDestination
    {
        public:
        // Implements base class function
        void AppendData(char *prefix, int prefixCount,
                        char* srcInfo, int srcInfoCount,
                        char* desc, int descCount);
        virtual void Flush(bool) { fflush(stderr); }
    };


// Implements a log destination that is OutputDebugString()
    class LogDestinationDebugString : public LogDestination
    {
        public:
        // Implements base class function
        void AppendData(char *prefix, int prefixCount,
                        char* srcInfo, int srcInfoCount,
                        char* desc, int descCount);
    };


// Implements a log destination that is DebugBreak()
    class LogDestinationDebugBreak : public LogDestination
    {
        public:
        // Implements base class function
        void AppendData(char *prefix, int prefixCount,
                        char* srcInfo, int srcInfoCount,
                        char* desc, int descCount);
    };


// Implements a log destination that asserts with a popup
    class LogDestinationPopup : public LogDestination
    {
        public:
        // Implements base class function
        void AppendData(char *prefix, int prefixCount,
                        char* srcInfo, int srcInfoCount,
                        char* desc, int descCount);
    };

// Implements a log destination that terminates the process
    class LogDestinationTerminate : public LogDestination
    {
        public:
        // Implements base class function
        void AppendData(char *prefix, int prefixCount,
                        char* srcInfo, int srcInfoCount,
                        char* desc, int descCount);
    };


// Implements a log destination that is a set of files; e.g. crawler0.log, crawler1.log, etc.
    class LogDestinationFile : public LogDestination
    {
        public:

        LogDestinationFile(const char* fileName, int maxSize, int maxFiles);

        ~LogDestinationFile()
        {
            CloseFile();
        }

        BOOL    Init(const char *name);

        // Implements base class function
        void    AppendData(char *prefix, int prefixCount,
                           char* srcInfo, int srcInfoCount,
                           char* desc, int descCount);

        // Flush files
        void    Flush(bool flushBuffers);
        void    FlushHaveLock(bool flushBuffers);

        // Open current file
        BOOL    OpenFile();

        // Close current file
        void    CloseFile();

        void    SetFileLastUpdateTime();

        private:
        void              MakeFilenamePrefix(char *filename, size_t count, char** lastPath, size_t* remaining);
        void              MakeFilename(int number, char *filename, size_t count);
        void              EraseLogFile(int number);
        void              DeleteAllOldFiles(int highest);
        int               FindHighestNumberedFile();

        private:
        // File name prefix for log files; e.g. "crawler" --> crawler0.log, crawler1.log
        char              m_filenameBase[MAX_PATH];

        // Max size of a particular log file
        // Once it passes this threshold a new log file is created
        int               m_maxFileSize;

        // Max number of log files before rolling over
        int               m_maxFiles;

        // Size of the log write buffer
        int               m_writeBufferSize;

        // Handle to currently open log file
        HANDLE            m_fileHandle;

        // Buffer into which log data is copied as it is logged
        BYTE *            m_writeBuffer;

        // Position in m_writeBuffer
        int               m_writeBufferPosition;

        // Disk size of the current log file
        int               m_currentFileSize;

        // Current file number; e.g. Crawler17345.log
        int               m_currentFileNumber;
    };


// A rule for determining to which destinations an entry goes, given (area,level,title)
    class LogRule
    {
        public:
        // Mask of levels affected (e.g. warning, informational, error)
        int     m_levelMask;

        // Mask of areas affected
        BYTE    m_areaMask[(LogID_Count/8) + 1];

        // Bitmap indicating which destinations
        UInt64  m_logDestinationMask;

        // Returns whether a particular area/level/title to be logged, matches this rule
        BOOL Matches(LogID area, LogLevel level);

        void SetAreaID( LogID logID )
        {
            // Set bit coresponding to logID
            m_areaMask[logID/8] |= (1 << (logID & 7));
        }
    };

// This class encapsulates all of logging
    class LoggerInternal
    {
        public:
        static BOOL  PreInit();
        static BOOL  DefaultInit();
        static BOOL  SetDirectory(const char *path);
        static BOOL AddRule(const char* area, const char* levels, const char* outputDestination);
        static void  ClearRules();
        static void  CreateDefaultRules();
        static BOOL  AddRuleInternal(BYTE *areaMask, int levelMask, UInt64 logDestinationMask);
        static int   FindLogDestination(const char *outputDestinationName);
        static int   CreateLogDestinationHelper(LogDestination *destination, const char *name);
        static int   CreateFileLogDestination(const char *name, const char* fileName, int maxSize, int maxFiles);
        static int   CreateStdoutLogDestination(const char *name);
        static int   CreateStderrLogDestination(const char *name);
        static int   CreateDebugStringLogDestination(const char *name);
        static int   CreateTerminateLogDestination(const char *name);
        static int   CreatePopupLogDestination(const char *name);
        static int   CreateDebugBreakLogDestination(const char *name);
        static int   AreaToLogID(const char *name);
        static void  _cdecl Stop();
        static void  CALLBACK StopThread(ULONG_PTR dwParam);
        static BOOL  SpinLogFlushThread();
        static unsigned __stdcall LogFlushThread(void *);

        static volatile bool     m_preInitDone;
        static CRITSEC* m_pPreInitCritSec; // on heap so never destructed and we control construct time

        public:

    // Forces logging initialization
        const static BOOL s_DefaultInitOk;

        // The rules
        static LogRule           m_rules[c_logMaxRules];

        // The log destinations; note, because this is all done without locks, log destinations
        // are NEVER deleted!
        static LogDestination *  m_logDestinations[c_logMaxLogDestinations];
        static HANDLE            m_hThread;
        static DWORD             m_dwThreadID;
        static volatile bool     m_bStopped;
    };

// Include the log tags
#ifdef DeclareTag
#undef DeclareTag
#endif

#define DeclareTag(index, id, name, type) name

// These are the names of the tags, which are used for formatting Name="Value" pairs for outputting
    const char *g_logTagNames[] =
    {
#include "logtagids.h"
    };

// This data structure is created on initialization, and is used for quick formatting of Name="Value" pairs
// It stores the Name=" header and its length, so we don't have to strlen() or concatenate ="
    struct
    {
        char  m_header[c_maxLogTagNameSize]; // includes Name="
        int   m_length;                      // length of the above
    } g_logTagNameHeaders[LogTagCountIndex];

// Name of the log rules section
    char *c_logRulesSectionName = "LogRules";

// Standard name of the destination which means to stdout
    char *c_logDestinationStdout = "stdout";

// Standard name of the destination which means to stderr
    char *c_logDestinationStderr = "stderr";

// Standard name of the destination which means to OutputDebugString
    char *c_logDestinationDebugString = "debugstring";

// Standard name of the destination which means to exit() if we see this
    char *c_logDestinationTerminate = "terminate";

// Standard name of the destination which means to debug break if we see this
    char *c_logDestinationDebugBreak = "debugbreak";

// Standard name of the destination which means to pop up an assert dialog if we see this
    char *c_logDestinationPopup = "popup";

// Parameter name of the file name base in a particular section
// If this parameter doesn't exist, it will use the section name as the filename base
    char *c_logFileBaseParameterName = "FileNameBase";

// Asserts are handled in a special way.
// No further action is taken If the config file
// specifies the destination to be one of the following
// "popup", "debugbreak", "terminate"
// If none of these destinations are specified then
// popup is the default action for debug builds
// and stdout is the default action for retail builds (the process will
// exit or failfast right after logging the assert)
#ifndef DEBUG
    char* c_defaultAssertDestination = c_logDestinationStdout;
    const int c_minidumpTimeout = 15*60*1000;
#else
    char* c_defaultAssertDestination = c_logDestinationPopup;
    const int c_minidumpTimeout = 0;
#endif

// Declare class members
    char              Logger::m_logsDir[MAX_PATH] = ".\\Logs";
    char              Logger::m_minidumpsDir[MAX_PATH] = ".\\Minidumps";

    volatile int      Logger::m_numRules;
    LogRule           LoggerInternal::m_rules[c_logMaxRules];

    volatile int      Logger::m_numLogDestinations;
    LogDestination *  LoggerInternal::m_logDestinations[c_logMaxLogDestinations];
    HANDLE            LoggerInternal::m_hThread;
    DWORD             LoggerInternal::m_dwThreadID;
    volatile bool     LoggerInternal::m_bStopped;

	void (*Logger::m_notificationsCallback)(int level, int logId, const char* title, const char *message) = NULL;

    LogEntryCallbackInternal Logger::m_logEntryCallback = NULL;

    const BOOL LoggerInternal::s_DefaultInitOk = LoggerInternal::PreInit();


    volatile bool     LoggerInternal::m_preInitDone = false;
    CRITSEC* LoggerInternal::m_pPreInitCritSec = NULL; // on heap so never destructed and we control construct time

    bool              g_prefixAppName = false;
// The log level mask to be used for spewing to stderr before initialization. Defaults to everything.
    int g_preInitializeLogLevelMask = -1;

    bool              Logger::m_minidumpEnabled = true;

// Initialize the values of the log tag name headers
// Initialize each header to Name="
// Store the length of the above string
// Returns FALSE if there wasn't enough room to initialize a particular parameter
BOOL InitializeLogTagHeaders()
{
    for (int i = 0; i < LogTagCountIndex; i++)
    {
        HRESULT hr = StringCchPrintfA(
            g_logTagNameHeaders[i].m_header,
            NUMELEM(g_logTagNameHeaders[i].m_header),
            "%s=\"",
            g_logTagNames[i]);

        if (FAILED(hr))
        {
            fprintf(stderr, "Log tag name was too long: %s\n", g_logTagNames[i]);
            return FALSE;
        }

        g_logTagNameHeaders[i].m_length = (int) strlen(g_logTagNameHeaders[i].m_header);
    }

    return TRUE;
}

void CALLBACK LoggerInternal::StopThread(ULONG_PTR dwParam)
{
    UNREFERENCED_PARAMETER(dwParam);
}

// Given an area name, return its LogID
// Returns -1 if not found
int LoggerInternal::AreaToLogID(const char *name)
{
    for (int i = 0; i < LogID_Count; i++)
    {
        if (_stricmp(g_LogIDNames[i], name) == 0)
            return i;
    }

    return -1;
}


// Create default rules for before we have initialized logging
void LoggerInternal::CreateDefaultRules()
{
    ClearRules();

    // Default rule is log everything to stderr
    int destinationIndex = FindLogDestination(c_logDestinationStderr);
    if (destinationIndex >= 0)
    {
        AddRuleInternal(NULL, g_preInitializeLogLevelMask, (1ULL << destinationIndex));
    }
}


// Rules are in the format:
//
// [LoggingRules]
// Rule1=areas,severity,title,output location
// Rule2=...
//
// Use * to indicate everything:
//
// e.g. Rule1=*,W,*,stdout
//      Rule2=*,*,*,foo
//
// For areas you can provide a list of areas in the form:
// Rule1=csm|netlib|en|pn,severity,title,output location
//
// You can also use ~ to remove things, e.g. for everything except netlib:
// Rule1=*|~netlib,severity,title,output location
//
// IMPORTANT NONE: The rule database is updated without using locks, while other people
//                 may be reading from it.  Therefore, care must be taken to ensure that,
//                 when race conditions happen, they don't cause anything bad to happen.
//
//                 The main thing that needs to be avoided is having a rule output to
//                 a destination that has not been initialized yet.  So a destination
//                 needs to be initialized before a rule can point to it.
//
BOOL LoggerInternal::AddRule(
    const char* ruleArea,
    const char* levels,
    const char* outputLocation)
{
    char area[1024];
    char *areaPtr;
    bool doneParsingAreas = false;
    int  levelMask;
    int  destinationIndex;
    BYTE areaMask[ (LogID_Count/8) + 1 ];

    if (FAILED(StringCchCopyA(area, NUMELEM(area), ruleArea)))
    {
        return FALSE;
    }

    memset(areaMask, 0, sizeof(areaMask));

    // Now parse the log area
    areaPtr = area;
    do
    {
        char *currentAreaPtr; // points to beginning of current area

        currentAreaPtr = areaPtr;

        // Find next | or end of string
        char *q = strchr(currentAreaPtr, '|');

        if (q == NULL)
        {
            // No | means we're done after this area
            doneParsingAreas = true;
        }
        else
        {
            // | means there's more, so null out the | and advance areaPtr for the next iteration
            *q = '\0';
            areaPtr = q+1;
        }

        if (strcmp(currentAreaPtr, "*") == 0)
        {
            memset(areaMask, 0xFF, sizeof(areaMask));
        }
        else
        {
            bool inverted = false;

            // ~area means remove that area from the mask
            if (*currentAreaPtr == '~')
            {
                currentAreaPtr++;
                inverted = true;
            }

            int logID = AreaToLogID(currentAreaPtr);
            if (logID < 0)
            {
                fprintf(stderr, "Parsing log rules: area not found '%s'\n", currentAreaPtr);
                continue; // Invalid log ID
            }

            LogAssert(logID < LogID_Count);

            if (inverted)
            {
                areaMask[logID / 8] &= ~(1 << (logID & 7));
            }
            else
            {
                areaMask[logID / 8] |= (1 << (logID & 7));
            }
        }
    } while (doneParsingAreas == false);

    // p currently points to the comma before the levels

    // Allow multiple levels to be used
    levelMask = 0;

    const char* p = levels;
    do
    {
        if (*p == 'I')
            levelMask |= 1 << LogLevel_Info;
        else if (*p == 'D')
            levelMask |= 1 << LogLevel_Debug;
        else if (*p == 'S')
            levelMask |= 1 << LogLevel_Status;
        else if (*p == 'W')
            levelMask |= 1 << LogLevel_Warning;
        else if (*p == 'E')
            levelMask |= 1 << LogLevel_Error;
        else if (*p == 'X')
            levelMask |= 1 << LogLevel_Alert;
        else if (*p == 'A')
            levelMask |= 1 << LogLevel_Assert;
        else if (*p == '*')
            levelMask = -1; // All levels (all bits set)
        else
            return FALSE;
    } while (*++p != '\0');

    if (0 == ::strcmp(c_logDestinationPopup, outputLocation) ||
        0 == ::strcmp(c_logDestinationDebugBreak, outputLocation))
    {
#pragma prefast(push)
#pragma prefast(disable:309, "Potential NULL argument 1 to 'SetUnhandledExceptionFilter'.")
        ::SetUnhandledExceptionFilter(NULL);
#pragma prefast(pop)
    }

    destinationIndex = FindLogDestination(outputLocation);
    if (destinationIndex < 0)
    {
        return FALSE;
    }

    // We now have a fully initialized destination, so it's ok to point a rule at it
    AddRuleInternal(areaMask, levelMask, (1ULL << destinationIndex));
    return TRUE;
}

// Remove all existing rules
// NOTE: This runs simultaneously with people reading from the rules, without any locks
void LoggerInternal::ClearRules()
{
    Logger::m_numRules = 0;

    for (int i = 0; i < c_logMaxRules; i++)
    {
        m_rules[i].m_logDestinationMask = 0;
        memset(m_rules[i].m_areaMask, 0, sizeof(m_rules[i].m_areaMask));
        m_rules[i].m_levelMask = 0;
    }
}

// Given a particular log destination (e.g. "stdout" or some set of files), find the index we are
// using to refer to it, or -1 if not found.
int LoggerInternal::FindLogDestination(const char *outputDestinationName)
{
    for (int i = 0; i < Logger::m_numLogDestinations; i++)
    {
        if (m_logDestinations[i] != NULL)
        {
            if (strcmp(m_logDestinations[i]->m_name, outputDestinationName) == 0)
                return i;
        }
    }

    return -1;
}

// Helper function
// Given a LogDestination object and its name, initialize it and put it in the list
// Return the log ID
// If it fails to initialize, the log destination object is deleted and -1 is returned
int LoggerInternal::CreateLogDestinationHelper(LogDestination *destination, const char *name)
{
    LogAssert( destination != NULL);
    if (destination->Init(name) == FALSE)
    {
        delete destination;
        return -1;
    }

    m_logDestinations[Logger::m_numLogDestinations] = destination;

    // Interlocked increment because other threads may be reading from this location
    // We are the only writer, however
    InterlockedIncrement((long *) &Logger::m_numLogDestinations);

    return Logger::m_numLogDestinations-1;
}

// Create the destination which is stdout
// Return the log ID
int LoggerInternal::CreateStdoutLogDestination(const char *name)
{
    LogDestinationStdout *destination = new LogDestinationStdout();
    LogAssert(destination);
    return CreateLogDestinationHelper(destination, name);
}

// Create the destination which is stderr
// Return the log ID
int LoggerInternal::CreateStderrLogDestination(const char *name)
{
    LogDestinationStderr *destination = new LogDestinationStderr();
    LogAssert(destination);
    return CreateLogDestinationHelper(destination, name);
}

// Create the destination which is outputdebugstring
// Return the log ID
int LoggerInternal::CreateDebugStringLogDestination(const char *name)
{
    LogDestinationDebugString *destination = new LogDestinationDebugString();
    LogAssert(destination);
    return CreateLogDestinationHelper(destination, name);
}

// Create the destination which is debugbreak
// Return the log ID
int LoggerInternal::CreateDebugBreakLogDestination(const char *name)
{
    LogDestinationDebugBreak *destination = new LogDestinationDebugBreak();
    LogAssert(destination);
    return CreateLogDestinationHelper(destination, name);
}

// Create the destination which is terminate
// Return the log ID
int LoggerInternal::CreateTerminateLogDestination(const char *name)
{
    LogDestinationTerminate *destination = new LogDestinationTerminate();
    LogAssert(destination);
    return CreateLogDestinationHelper(destination, name);
}

// Create the destination which is terminate
// Return the log ID
int LoggerInternal::CreatePopupLogDestination(const char *name)
{
    LogDestinationPopup *destination = new LogDestinationPopup();
    LogAssert(destination);
    return CreateLogDestinationHelper(destination, name);
}

// Create a log destination that is actually a set of files
//
// The file destination database is not protected by locks
// However, other people will only be reading from destination IDs already allocated.
// Log destination IDs are never freed.
// Only one thread will ever be writing to this structure at the same time.
int LoggerInternal::CreateFileLogDestination(const char *name, const char* fileName, int maxSize, int maxFiles)
{
    LogDestinationFile *destination = new LogDestinationFile(fileName, maxSize, maxFiles);
    LogAssert(destination);
    return CreateLogDestinationHelper(destination, name);
}


// Add a new rule
// Returns FALSE if unsuccessful
// title = "*" means all
// If areaMask == NULL, it means "use all areas", otherwise areaMask must be a bitmap of size (LogID_Count/8) + 1
BOOL LoggerInternal::AddRuleInternal(BYTE *areaMask, int levelMask, UInt64 logDestinationMask)
{
    if (!m_preInitDone) {
        BOOL f = DefaultInit();
        if (!f) {
            return FALSE;
        }
    }

    if (Logger::m_numRules >= c_logMaxRules)
        return FALSE;

    LogRule *rule = &m_rules[Logger::m_numRules];

    rule->m_levelMask = levelMask;

    if (areaMask == NULL)
    {
        memset(rule->m_areaMask, 0xFF, (LogID_Count/8) + 1);
    }
    else
    {
        memcpy(rule->m_areaMask, areaMask, (LogID_Count/8) + 1);
    }

    rule->m_logDestinationMask = logDestinationMask;

    // This needs to be interlocked because a logging thread may be reading this value
    InterlockedIncrement((volatile long *) &Logger::m_numRules);

    return TRUE;
}

// Returns whether this rule matches the provided parameters
BOOL LogRule::Matches(LogID area, LogLevel level)
{
    if ((unsigned int) area >= (unsigned int) LogID_Count)
        return FALSE;

    if ((m_areaMask[area/8] & (1 << (area & 7))) == 0)
        return FALSE;

    if ((m_levelMask & (1 << level)) == 0)
        return FALSE;
    return TRUE;
}

// Determine the directory used for logging and minidump,
// and create the directory if it doesn't already exist
// This is done only once, at startup
BOOL LoggerInternal::SetDirectory(const char *path)
{
    char * lPath = (path == NULL || path[0] == '\0') ? ".\\" : path;

    size_t pathLength = strlen(lPath);
    char * sep = (lPath[pathLength-1] != '\\') ? "\\" : "";

    if (FAILED(StringCchPrintfA(
        Logger::m_logsDir, MAX_PATH, "%s%sLogs", lPath, sep)))
    {
        return FALSE;
    }
    if (FAILED(StringCchPrintfA(
        Logger::m_minidumpsDir, MAX_PATH, "%s%sMinidumps", lPath, sep)))
    {
        return FALSE;
    }
    return TRUE;
}

// Initialize logging independently of the configuration availability.
BOOL LoggerInternal::PreInit()
{
    if (m_pPreInitCritSec == NULL) {
        m_pPreInitCritSec = new CRITSEC();
        LogAssert(m_pPreInitCritSec != NULL);
    }

    AutoCriticalSection lock(m_pPreInitCritSec);

    if (InitializeLogTagHeaders() == FALSE)
    {
        fprintf(stderr, "InitializeLogTagHeaders() failed\n");
        return (FALSE);
    }

    return (TRUE);
}

// Initialize logging independently of the configuration availability.
BOOL LoggerInternal::DefaultInit()
{
    if (m_pPreInitCritSec == NULL) {
        m_pPreInitCritSec = new CRITSEC();
        LogAssert(m_pPreInitCritSec != NULL);
    }

    AutoCriticalSection lock(m_pPreInitCritSec);

    if (!s_DefaultInitOk)
    {
        return (FALSE);
    }

    if (!m_preInitDone) {
        m_preInitDone = true;

        // Pre-create log destinations in order terminate, popup, debugstring, stdout
        // This is because we output to the highest numbered log destinations first, and we want
        // to potentially output to stdout before terminating
        CreateTerminateLogDestination(c_logDestinationTerminate);
        CreatePopupLogDestination(c_logDestinationPopup);
        CreateDebugBreakLogDestination(c_logDestinationDebugBreak);
        CreateDebugStringLogDestination(c_logDestinationDebugString);
        CreateStdoutLogDestination(c_logDestinationStdout);
        CreateStderrLogDestination(c_logDestinationStderr);


        CreateDefaultRules();
    }
    return (TRUE);
}


// Initialize logging
BOOL Logger::Init(const char *path)
{
    if (!LoggerInternal::s_DefaultInitOk)
    {
        return (FALSE);
    }

    if (!LoggerInternal::m_preInitDone) {
        BOOL f = LoggerInternal::DefaultInit();
        if (!f) {
            return FALSE;
        }
    }

    BOOL success = FALSE;

    // Set/create the logging directory
    if (LoggerInternal::SetDirectory(path) == FALSE)
    {
        fprintf(stderr, "SetDirectory() failed\n");
        goto exit;
    }


    LoggerInternal::ClearRules();

    success = LoggerInternal::SpinLogFlushThread();

    if (success)
    {
        atexit(&LoggerInternal::Stop);
    }

exit:

    return success;
}

void Logger::Stop()
{
    LoggerInternal::Stop();
}

void LoggerInternal::Stop()
{
    if (!m_preInitDone) {
        DefaultInit();
    }

    m_bStopped = true;

    if (m_hThread != NULL)
    {
        QueueUserAPC(&LoggerInternal::StopThread, m_hThread, 0);
        Logger::WaitForThreadOrExit(m_hThread, m_dwThreadID);
        CloseHandle(m_hThread);
        m_hThread = NULL;
    }
    Logger::Flush();

    // m_numLogDestinations can not be increased at this time by another thread
    for (int i = 0; i < Logger::m_numLogDestinations; i++)
    {
        // Free the LogDestination, which in turn should free the resources
        // as file handles, etc
		LogDestination *dest = LoggerInternal::m_logDestinations[i];
        LoggerInternal::m_logDestinations[i] = NULL;
        delete dest;
    }

    Logger::m_numLogDestinations = 0;
}

void Logger::Terminate(UINT ec)
{
    ::TerminateProcess(GetCurrentProcess(), ec);
}


// Convert the current time to a string, in a suitable format for logging
// Format is: yyyy/mm/dd hh:mm:ss:mmm
// Returns the number of characters written, not including the null terminator
// Note: The size of the string (including terminating null) must be exactly
// c_TimeStringLength. If this ever changes, remember to change c_TimeStringLength;
static void CurrentTimeToString(const FILETIME* ft, char *string, size_t count)
{
    SYSTEMTIME utc, local;

    FileTimeToSystemTime(ft, &utc);
    SystemTimeToTzSpecificLocalTime(NULL, &utc, &local);

    StringCchPrintfA(
        string,
        count,
        "%02d/%02d/%04d %02d:%02d:%02d:%03d",
        local.wMonth,
        local.wDay,
        local.wYear,
        local.wHour,
        local.wMinute,
        local.wSecond,
        local.wMilliseconds
    );
}

// Given a log level, return the character used to represent it in the log file
// We prefix the log info level at the beginning of the line; e.g.
//
// !yyyy/mm/dd hh:mm:ss blah blah blah
//
// Informational log entries use a space as their character, so that ? and !
// entries (warning and errors, respectively) easily stand out where they occur.
static char LogLevelToCharacter(LogLevel level)
{
    switch (level)
    {
        default:
        case LogLevel_Debug:
        case LogLevel_Info:
            return 'i';

        case LogLevel_Warning:
            return 'w';

        case LogLevel_Status:
            return 's';

        case LogLevel_Assert:
            return 'a';

        case LogLevel_Alert :
            return 'x';

        case LogLevel_Error:
            return 'e';
    }
}

static CallBackLogLevel GetCallBackLogLevel(LogLevel level)
{
    switch (level)
    {
    case LogLevel_Info:
        return CallBackLogLevel_Info;

    case LogLevel_Warning:
        return CallBackLogLevel_Warning;

    case LogLevel_Status:
        return CallBackLogLevel_Status;

    case LogLevel_Assert:
        return CallBackLogLevel_Assert;

    case LogLevel_Alert:
        return CallBackLogLevel_Alert;

    case LogLevel_Error:
        return CallBackLogLevel_Error;

    case LogLevel_Debug:
    default:
        return CallBackLogLevel_Debug;
    }
}

void Logger::LogV(const char *file, const char *function, const int line, LogID logID, LogLevel level, const char *title, const char* fmt,  va_list args)
{
    DWORD   lastWin32Error;
    UInt64  logDestinationMask = 0;
    char    localBuffer[c_logMaxEntrySize];
    char *  buffer    = localBuffer;
    char *  bufferEnd = &localBuffer[sizeof(localBuffer)-3]; // always leave space for \r\n\0 at the end
    char    sourceInfo[c_logMaxSourceInfoSize]; // space for source file info (file,function,line). Not null terminated
    int srcLen = 0;
    HRESULT hr = S_OK;

    if (!LoggerInternal::m_preInitDone) {
        BOOL f = LoggerInternal::DefaultInit();
        if (!f) {
            return;
        }
    }

    if (level == LogLevel_Assert && m_logEntryCallback == NULL)
    {
		Logger::CallNotificationsIfDefined(level, logID, title, fmt);

		WriteMiniDump (c_minidumpTimeout);
    }

    // Check for invalid log entry ID
    if (((unsigned int) logID) >= (unsigned int) LogID_Count)
        return;

    // Invoke the log entry callback if provided
    if (m_logEntryCallback != NULL)
    {
        LogEntryCallbackInternal logEntryCallback = m_logEntryCallback;
        if (logEntryCallback != NULL)
        {
            logEntryCallback(file, function, line, logID, GetCallBackLogLevel(level), title, fmt, args);
            return;
        }
    }

    // See which output destinations we accumulate for this log entry
    //
    // Note that we are reading from the rules database without taking a lock, even though it could be
    // updated at any time.
    for (int i = 0; i < Logger::m_numRules; i++)
    {
        if (LoggerInternal::m_rules[i].Matches(logID, level))
        {
            logDestinationMask |= LoggerInternal::m_rules[i].m_logDestinationMask;
        }
    }

    // If there are no log destinations that matched any rules, then we don't log this
    if (logDestinationMask == 0)
    {
        return;
    }

    // Determine whether any log destination wants full source info
    // If true, we need to do log formatting twice
    sourceInfo[0] = '\0';

    for (int i = 0; i < Logger::m_numLogDestinations; i++)
    {
        if (((logDestinationMask >> i) & 1) && (LoggerInternal::m_logDestinations[i]->m_logSourceInfo))
        {
            // Remove the path prefix from the source filename
            // Take everything after the last slash
            const char *p = strrchr(file, '\\');
            if (p != NULL)
            {
                file = p+1;
            }

            char *end = NULL;

            hr = StringCchPrintfExA(
                sourceInfo,
                NUMELEM(sourceInfo),
                &end,
                NULL,
                0,
                "SrcFile=\"%s\" SrcFunc=\"%s\" SrcLine=\"%d\" ",
                file,
                function,
                line);

            if (hr == S_OK)
            {
                srcLen = (int) (end - sourceInfo);
            }
            else if (hr == STRSAFE_E_INSUFFICIENT_BUFFER)
            {
                srcLen = (int) (NUMELEM(sourceInfo) - 1);
            }
            else
            {
                sourceInfo[0] = '\0';
                srcLen = 0;
            }

            break;
        }
    }

    // Put in date,area,title,

    // Assume that the date, area, and title cannot overflow the end of the localBuffer
    // This is a reasonable assumption because date is only 20 characters, areas are hard-coded names,
    // and title is a user supplied constant string

    *buffer++ = LogLevelToCharacter(level);
    *buffer++ = ',';
    char *dateBeginBuffer = buffer;
    buffer += c_TimeStringLength;

    hr = StringCchCopyExA(
        buffer,
        bufferEnd - buffer,
        g_LogIDNames[logID],
        &buffer,
        NULL,
        0);

    if (FAILED(hr))
    {
        return;
    }

    *buffer++ = ',';

    //Title can't have ','s, otherwise, the collection server won't be able to
    //parse the right field to db.
    // Leave 255 characters for putting PID, TID and log data.
    const char *pch = title;
    while (*pch && buffer < bufferEnd - 255)
    {
        *buffer++ = (',' == *pch) || ('\r' == *pch) || ('\n' == *pch) ?
            '.' : *pch; ++pch;
    }

    *buffer++ = ',';

    // Save the last Win32 error in case we overwrite it
    lastWin32Error = GetLastError();

    // Record where the title ends
    // This is in case we need to log source file info, so we know where to insert it
    char *titleEnd = buffer;

    // write the processid and threadid
    hr = StringCchPrintfExA(
        buffer,
        bufferEnd - buffer,
        &buffer,
        NULL,
        0,
        "Pid=\"%d\" Tid=\"%d\" TS=\"",
        GetCurrentProcessId(),
        GetCurrentThreadId());

    if (FAILED(hr))
    {
        return;
    }

    // Record where the timestamp will begin. We need to set the timestamp field
    // before writing the log to the destination
    char *timeStampBeginBuffer = buffer;
    buffer += c_UTCTimeStampLength;
    *buffer++ = '\"';
    *buffer++ = ' ';

    char* end;
    hr = StringCchVPrintfExA(
        buffer,
        bufferEnd-buffer,
        &end,
        NULL,
        STRSAFE_IGNORE_NULLS,
        fmt,
        args);

    if (FAILED(hr))
    {
        end = buffer;
    }

    buffer = end;

    // Add \r\n\0
    *buffer++ = '\r';
    *buffer++ = '\n';
    *buffer = '\0'; // Don't increment buffer, because we don't want to write out a null terminator

    // Go through each log destination in the mask, and output the log info there

    // Start with the highest numbered log destinations first
    // The reason is, the low ordered log destinations are created as Assert, Terminate, etc., and we
    // want to write to the log files before terminating the process
    for (int curDestination = Logger::m_numLogDestinations-1; curDestination >= 0; curDestination--)
    {
        if ((logDestinationMask >> curDestination) & 1)
        {
            LogDestination* dest = LoggerInternal::m_logDestinations[curDestination];

			if (dest==NULL)
			{
				// we are concurently unloading the Logger.
				continue;
			}

            dest->LockBuffer();

            FILETIME ft;
            GetSystemTimeAsFileTime(&ft);

            CurrentTimeToString(&ft, dateBeginBuffer, c_TimeStringLength);
            dateBeginBuffer[c_TimeStringLength-1] = ',';

            StringCchPrintf(timeStampBeginBuffer, c_UTCTimeStampLength+1, "0x%08X%08X", ft.dwHighDateTime, ft.dwLowDateTime);
            timeStampBeginBuffer[c_UTCTimeStampLength] = '\"';

            dest->AppendData(localBuffer, (int) (titleEnd-localBuffer),
                             sourceInfo, (dest->m_logSourceInfo) ? srcLen : 0,
                             titleEnd, (int) (buffer - titleEnd));

            dest->UnlockBuffer();
        }
    }

    if (level >= LogLevel_Assert)
    {
        Logger::Flush();

        int curDestination = Logger::m_numLogDestinations-1;
        // if the list of destination did not include popup, debugbreak or terminate, call it
        for (; curDestination >= 0; curDestination--)
        {
            if ((logDestinationMask >> curDestination) & 1)
            {
                LogDestination* dest = LoggerInternal::m_logDestinations[curDestination];
				if (dest!=NULL)
				{
					// we are concurently unloading the Logger.

					if (0 == ::strcmp(c_logDestinationPopup, dest->m_name) ||
						0 == ::strcmp(c_logDestinationDebugBreak, dest->m_name))
					{
						break;
					}
				}
            }
        }
        if (curDestination < 0)
        {
            curDestination = LoggerInternal::FindLogDestination(c_defaultAssertDestination);
            LogDestination* dest = LoggerInternal::m_logDestinations[curDestination];

			if (dest != NULL)
			{
				// we are concurently unloading the Logger.
				dest->LockBuffer();

				FILETIME ft;
				GetSystemTimeAsFileTime(&ft);

				CurrentTimeToString(&ft, dateBeginBuffer, c_TimeStringLength);
				dateBeginBuffer[c_TimeStringLength-1] = ',';

				StringCchPrintf(timeStampBeginBuffer, c_UTCTimeStampLength+1, "0x%08X%08X\"", ft.dwHighDateTime, ft.dwLowDateTime);
				timeStampBeginBuffer[c_UTCTimeStampLength] = '\"';

				dest->AppendData(localBuffer, (int) (titleEnd-localBuffer),
								 sourceInfo, (dest->m_logSourceInfo) ? srcLen : 0,
								 titleEnd, (int) (buffer - titleEnd));

				dest->UnlockBuffer();
			}
        }
    }

    SetLastError(lastWin32Error);
}

// Base initialization method for a log destination
// Initialization methods for derived classes should call this first
BOOL LogDestination::Init(const char *destinationName)
{
    m_logSourceInfo = TRUE;
    if (FAILED(StringCchCopy(m_name, NUMELEM(m_name), destinationName)))
    {
        return FALSE;
    }
    return TRUE;
}

void LogDestination::LockBuffer()
{
    m_criticalSection.Enter();
}

void LogDestination::UnlockBuffer()
{
    m_criticalSection.Leave();
}

// Given a source buffer and count, which are not null terminated, fill in a dest buffer,
// of size destCount, and ensure it is null terminated
// destSize must be >= 1
void LogDestination::MakeNullTerminatedString(char *dest, int destSize,
                                              const char *source, int sourceCount,
                                              char** end, int* remaining)
{
    if (sourceCount >= destSize)
    {
        sourceCount = destSize-1;
    }
    memcpy(dest, source, sourceCount);
    dest[sourceCount] = '\0';

    if (end)
    {
        *end = &dest[sourceCount];
    }
    if (remaining)
    {
        *remaining = destSize - sourceCount;
    }
}

LogDestinationFile::LogDestinationFile(const char* fileName, int maxSize, int maxFiles)
{
    m_maxFileSize = maxSize;
    m_maxFiles = maxFiles;
    m_writeBufferSize = c_defaultLoggingWriteBufferSize;

    // Get base name for logging files
    // e.g. Filebase=BadLogs means logs will be BadLogs0.log, BadLogs1.log, etc.
    // If it's not present, we will just use the destination name as the base
    HRESULT hr = StringCchCopy(m_filenameBase, NUMELEM(m_filenameBase), fileName);
    LogAssert(SUCCEEDED(hr));
}

// Base initialization method for a log destination that is a file
BOOL LogDestinationFile::Init(const char *destinationName)
{
    m_fileHandle            = INVALID_HANDLE_VALUE;
    m_writeBuffer           = NULL;
    m_writeBufferPosition   = 0;
    m_currentFileSize       = 0;
    m_currentFileNumber     = 0;

    char moduleName[MAX_PATH+1];
    if (GetModuleFileNameA(NULL, moduleName, sizeof(moduleName)) == 0)
    {
        fprintf(stderr, "Unable to get module path\n");
        return FALSE;
    }

    // moduleName is e.g. foo\bar\csm.exe
    char *moduleNameFilenamePart = strrchr(moduleName, '\\');
    if (moduleNameFilenamePart == NULL)
    {
        moduleNameFilenamePart = moduleName;
    }
    else
    {
        moduleNameFilenamePart++;
    }

    // Initialize base class
    if (LogDestination::Init(destinationName) == FALSE)
        return FALSE;

    size_t len = strlen(m_filenameBase);

    HRESULT hr = StringCchPrintfA(
        m_filenameBase+len,
        NUMELEM(m_filenameBase)-len,
        "_%s",
        moduleNameFilenamePart);

    if (FAILED(hr))
    {
        fprintf(stderr, "module path too long '%s'", moduleName);
        return FALSE;
    }

    m_currentFileNumber = FindHighestNumberedFile();

    // If we didn't find a file, start at 0; otherwise try to append to highest file found
    if (m_currentFileNumber < 0)
        m_currentFileNumber = 0;

    // We may have way old files lying around from a previous run, that are outside the [highest-maxFiles]
    // range, and we need to delete those
    DeleteAllOldFiles(m_currentFileNumber);

    if (OpenFile() == FALSE)
        return FALSE;

    // On startup we may be appending to a large file already
    if (m_currentFileSize >= m_maxFileSize)
    {
        CloseFile();

        EraseLogFile(m_currentFileNumber - m_maxFiles + 1);
        m_currentFileNumber++;

        if (OpenFile() == FALSE)
            return FALSE;
    }

    m_writeBuffer = new BYTE[m_writeBufferSize];
    if (m_writeBuffer == NULL)
    {
        CloseFile();
        SetLastError(ERROR_OUTOFMEMORY);
        return FALSE;
    }

    return TRUE;
}

// Make the absolute path name for log file number <number>
// e.g. c:\data\logs\crawler_0.log
void LogDestinationFile::MakeFilename(int number, char *filename, size_t count)
{
    char* lastPath = NULL;
    size_t remaining = 0;

    MakeFilenamePrefix(filename, count, &lastPath, &remaining);
    LogAssert(SUCCEEDED(StringCchPrintfA(lastPath, remaining, "_%d.log", number)));
}

// Make the absolute path name prefix for a log file; this does not include the "_0.log" part
// e.g. c:\data\logs\crawler
void LogDestinationFile::MakeFilenamePrefix(char *filename, size_t count, char** lastPath, size_t* remaining)
{
    HRESULT hr = S_OK;

    // If the filename base has a colon in it or starts with a slash, use it as an absolute path
    // Otherwise, prefix the appropriate log output directory
    if ((m_filenameBase[0] == '\\') || (strchr(m_filenameBase, ':') != NULL))
    {
        hr = StringCchCopyExA(filename, count, m_filenameBase, lastPath, remaining, 0);
    }
    else
    {
        hr = StringCchPrintfExA(filename, count, lastPath, remaining, 0, "%s\\%s", Logger::m_logsDir, m_filenameBase);
    }

    LogAssert(SUCCEEDED(hr));

}

// Given the highest log file number provided, enumerate all files in the directory
// and delete all ones outside of the keep range
void LogDestinationFile::DeleteAllOldFiles(int highest)
{
    struct _finddata_t  file;
    intptr_t            handle;
    char                wildcardPathname[MAX_PATH];
    char*               lastPath;
    size_t              remaining;

    // Make wildcard path to find all logs of our service
    MakeFilenamePrefix(wildcardPathname, NUMELEM(wildcardPathname), &lastPath, &remaining);
    LogAssert(SUCCEEDED(StringCchCatA(lastPath, remaining, "_*.log")));

    if ((handle = _findfirst(wildcardPathname, &file)) == -1L)
        return; // no files found

    do
    {
        // Look at a filename like crawler12345.log and read the 12345 part
        // c_file.name includes only the filename and not the pathname
        char *p = file.name;

        // Find any number inside here and parse it
        while (*p != '\0' && !isdigit(((unsigned char) *p)))
            p++;

        int number = atoi(p);

        // e.g. Highest file found is 15, and MaxFiles is 10
        // Therefore we want to delete <= 5 but keep 6...15
        if (number <= (highest-m_maxFiles))
        {
            char pathName[MAX_PATH];

            MakeFilename(number, pathName, NUMELEM(pathName));
            DeleteFileA(pathName);
        }
    } while (_findnext(handle, &file) == 0);

    _findclose(handle);
}

// Scan the directory and find the highest numbered log.X file that applies to us
// If none are found or there is some other error, return -1
int LogDestinationFile::FindHighestNumberedFile()
{
    struct _finddata_t  file;
    intptr_t            handle;
    char                wildcardPathname[MAX_PATH];
    char*               lastPath = NULL;
    size_t              remaining = 0;

    int                 highest = -1; // highest file seen

    // Make wildcard path to find all logs of our service
    MakeFilenamePrefix(wildcardPathname, NUMELEM(wildcardPathname), &lastPath, &remaining);
    LogAssert(SUCCEEDED(StringCchCatA(lastPath, remaining,  "_*.log")));

    if ((handle = _findfirst(wildcardPathname, &file)) == -1L)
        return -1; // no files found

    do
    {
        // Look at a filename like crawler12345.log and read the 12345 part
        // file.name includes only the filename and not the pathname
        char *p = file.name;

        // Find any number inside here and parse it
        while (*p != '\0' && !isdigit(((unsigned char) *p)))
            p++;

        int number = atoi(p);
        if (number > highest)
            highest = number;
    } while (_findnext(handle, &file) == 0);

    _findclose(handle);
    return highest;
}

// Erase a particular numbered log file
// If number < 0 this does nothing
void LogDestinationFile::EraseLogFile(int number)
{
    if (number >= 0)
    {
        char destFilename[MAX_PATH];

        MakeFilename(number, destFilename, NUMELEM(destFilename));
        DeleteFileA(destFilename);
    }
}

// Open the current log file.  Closes any existing log file.
// You must have the lock to call this function
BOOL LogDestinationFile::OpenFile()
{
    char filename[MAX_PATH];

    // Close any existing log file we have open
    CloseFile();

    MakeFilename(m_currentFileNumber, filename, NUMELEM(filename));

    if (!DirUtils::MakeDirectoryForFile(filename))
    {
        return (false);
    }

    m_fileHandle = CreateFileA(
        filename,
        GENERIC_WRITE,
        FILE_SHARE_READ,
        NULL,
        OPEN_ALWAYS,
        FILE_FLAG_SEQUENTIAL_SCAN,
        NULL
    );

    if (m_fileHandle == INVALID_HANDLE_VALUE)
        return FALSE;

    // Seek to end of file and upate current file size
    SetFilePointer(m_fileHandle, 0, NULL, FILE_END);

    m_currentFileSize = GetFileSize(m_fileHandle, NULL);
    if (m_currentFileSize == INVALID_FILE_SIZE)
        m_currentFileSize = 0;

    return TRUE;
}

// Close the current log file
// You must have the lock to call this function
void LogDestinationFile::CloseFile()
{
    if (m_fileHandle != INVALID_HANDLE_VALUE)
    {
        SetFileLastUpdateTime();

        CloseHandle(m_fileHandle);
        m_fileHandle = INVALID_HANDLE_VALUE;
    }

    m_currentFileSize = 0;
}

// Flush the contents of all log buffers for all log destinations
void Logger::Flush(bool flushBuffers /*= false*/)
{
    if (!LoggerInternal::m_preInitDone) {
        LoggerInternal::DefaultInit();
    }

    // m_numLogDestinations can be increased by another thread, but that's ok
    for (int i = 0; i < Logger::m_numLogDestinations; i++)
    {
        LoggerInternal::m_logDestinations[i]->Flush(flushBuffers);
    }
}

// Flush this log file
// Call this only if you don't have the lock
void LogDestinationFile::Flush(bool flushBuffers)
{
    LockBuffer();
    FlushHaveLock(flushBuffers);
    UnlockBuffer();
}

// Make sure the last write time gets updated
// For some reason, without this code, the file times of logs are often not updated when new data is written
void LogDestinationFile::SetFileLastUpdateTime()
{
    if (m_fileHandle != INVALID_HANDLE_VALUE)
    {
        SYSTEMTIME systemtime;
        FILETIME filetime;

        GetSystemTime(&systemtime);

        if (SystemTimeToFileTime(&systemtime, &filetime))
        {
            SetFileTime(
                m_fileHandle,
                NULL, // create time
                NULL, // last access time
                &filetime // last write time
            );
        }
    }
}

// Flush the contents of the log buffer to the current file
// You must have the lock to call this function
void LogDestinationFile::FlushHaveLock(bool flushBuffers)
{
    DWORD bytesWritten = 0;

    if (m_fileHandle != INVALID_HANDLE_VALUE)
    {
        WriteFile(
            m_fileHandle,
            m_writeBuffer,
            m_writeBufferPosition,
            &bytesWritten,
            NULL
        );

        SetFileLastUpdateTime();

        // Not too much we can do if the write fails
        m_currentFileSize += bytesWritten;
        if (flushBuffers)
        {
            FlushFileBuffers(m_fileHandle);
        }
    }

    m_writeBufferPosition = 0;
}

// Take the text given by (buffer, count) and append that and \r\n to the log
// You must have the lock to call this function
void LogDestinationFile::AppendData(char *prefix, int prefixCount,
                                    char* srcInfo, int srcInfoCount,
                                    char* desc, int descCount)
{
    int count = prefixCount + srcInfoCount + descCount;

    if (m_writeBufferPosition + count > m_writeBufferSize)
    {
        FlushHaveLock(false);
    }

    // Need to start a new log file?
    if (m_currentFileSize >= m_maxFileSize)
    {
        CloseFile();

        // Kill the oldest file
        // e.g. we just finished log file Foo10.log, and we want to keep 10 log files.
        //      Kill Foo1.log, because we're about to start Foo11.log.
        EraseLogFile(m_currentFileNumber - m_maxFiles + 1);
        m_currentFileNumber++;

        OpenFile();
    }

    // Copy into buffer
    // We should have space by now, given the above code, but if not, ignore this entry
    if (m_writeBufferPosition + count <= m_writeBufferSize)
    {
        BYTE* buf = &m_writeBuffer[m_writeBufferPosition];
        memcpy(buf, prefix, prefixCount);
        buf += prefixCount;
        if (srcInfoCount)
        {
            memcpy(buf, srcInfo, srcInfoCount);
            buf += srcInfoCount;
        }
        memcpy(buf, desc, descCount);
        m_writeBufferPosition += count;
    }
}

void LogDestinationStdout::AppendData(char *prefix, int prefixCount,
                                      char* srcInfo, int srcInfoCount,
                                      char* desc, int descCount)
{
    fwrite(prefix, 1, prefixCount, stdout);
    fwrite(srcInfo, 1, srcInfoCount, stdout);
    fwrite(desc, 1, descCount, stdout);
}

void LogDestinationStderr::AppendData(char *prefix, int prefixCount,
                                      char* srcInfo, int srcInfoCount,
                                      char* desc, int descCount)
{
    fwrite(prefix, 1, prefixCount, stderr);
    fwrite(srcInfo, 1, srcInfoCount, stderr);
    fwrite(desc, 1, descCount, stderr);
}

// Buffer is not guaranteed to be null terminated, so need to make a null terminated string
void LogDestinationDebugString::AppendData(char *prefix, int prefixCount,
                                           char* srcInfo, int srcInfoCount,
                                           char* desc, int descCount)
{
    char localBuffer[1024];
    char* dest = localBuffer;
    int remaining = (int) sizeof(localBuffer);
    MakeNullTerminatedString(dest, remaining, prefix, prefixCount, &dest, &remaining);
    MakeNullTerminatedString(dest, remaining, srcInfo, srcInfoCount, &dest, &remaining);
    MakeNullTerminatedString(dest, remaining, desc, descCount, &dest, &remaining);

    OutputDebugStringA(localBuffer);
}

// Make a popup
// buffer is not guaranteed to be null terminated
void LogDestinationPopup::AppendData(char *prefix, int prefixCount,
                                     char* srcInfo, int srcInfoCount,
                                     char* desc, int descCount)
{
    Logger::Flush();

    char localBuffer[1024];
    char moduleName[MAX_PATH+1];

    if (GetModuleFileNameA(NULL, moduleName, sizeof(moduleName)) == 0)
    {
        moduleName[0] = '\0';
        return;
    }

    StringCchPrintf(
        localBuffer,
        NUMELEM(localBuffer),
        "Module: %s\r\n\r\n%.*s\r\n%.*s\r\n%.*s\r\n",
        moduleName,
        prefixCount, prefix,
        srcInfoCount, srcInfo,
        descCount, desc);

    HWND hWndParent = NULL;
    BOOL fNonInteractive = FALSE;
    HWINSTA hwinsta;
    USEROBJECTFLAGS uof;
    DWORD nDummy;
    UInt32 uType = MB_TASKMODAL|MB_ICONHAND|MB_ABORTRETRYIGNORE|MB_SETFOREGROUND;

    if (NULL == (hwinsta = GetProcessWindowStation()) ||
        !GetUserObjectInformation(hwinsta, UOI_FLAGS, &uof, sizeof(uof), &nDummy) ||
        (uof.dwFlags & WSF_VISIBLE) == 0)
    {
        fNonInteractive = TRUE;
    }
    if (fNonInteractive)
    {
        uType |= MB_SERVICE_NOTIFICATION;
    }
    else
    {
        hWndParent = GetActiveWindow();

        if (hWndParent != NULL)
        {
            hWndParent = GetLastActivePopup(hWndParent);
        }
    }

    int nCode = MessageBoxA(hWndParent,
                            localBuffer,
                            "Assert! -- Hit Retry to enter the debugger",
                            uType);

    if (IDABORT == nCode)
    {
        Logger::Terminate(3);
    }

    if (IDRETRY == nCode)
    {
        DebugBreak();
    }
    // else ignore and continue execution
}

// buffer is not guaranteed to be null terminated
void LogDestinationDebugBreak::AppendData(char *, int, char*, int, char*, int)
{
    // Doesn't flush the log in this case
    DebugBreak();
}

// Make the process terminate
void LogDestinationTerminate::AppendData(char *, int, char*, int, char*, int)
{
    Logger::Flush();
    if (IsDebuggerPresent())
    {
        DebugBreak();
    }
    Logger::Terminate(1);
}

int
Logger::CreateFileLogDestination(const char *name, const char* fileName, int maxSize, int maxFiles)
{
    return LoggerInternal::CreateFileLogDestination(name, fileName, maxSize, maxFiles);
}

BOOL
Logger::AddRule(const char* area, const char* levels, const char* outputDestination)
{
    return LoggerInternal::AddRule(area, levels, outputDestination);
}

// This thread just flushes the logs every second
unsigned __stdcall LoggerInternal::LogFlushThread(void *)
{
    while (TRUE)
    {
        SleepEx(1000, TRUE);
        if (m_bStopped)
        {
            return 0;
        }
        Logger::Flush();
    }
}

// Spin a thread to flush the logs every second
BOOL LoggerInternal::SpinLogFlushThread()
{
    unsigned int threadID;
    m_hThread = (HANDLE)_beginthreadex(NULL, 0, LogFlushThread, NULL, 0, &threadID);
    if (m_hThread == NULL)
    {
        Log(LogID_Logging, LogLevel_Error, "Failed to create log flush thread");
        return false;
    }
    m_dwThreadID = threadID;
    return true;
}

void Trace::Write(LogTag tag, ...)
    {
        va_list ptr;
        va_start(ptr, tag);
        char* buffer = Buffer;
        // leave 3 bytes for \r\n\0
        char* bufferEnd = (char*) buffer + c_BufSize - 3;
        char* end = NULL;

        while (tag != LogTag_End)
        {
            // Strip out type information from the LogTag and get just the log tag index
            int index = LogTagToIndex(tag);
            if (index < 0 || index >= LogTagCountIndex)
                break; // Index out of range

            // Append Name=" to the buffer
            if (buffer + g_logTagNameHeaders[index].m_length >= bufferEnd)
                break; // Not enough room, stop here

            memcpy(buffer, g_logTagNameHeaders[index].m_header, g_logTagNameHeaders[index].m_length);
            buffer += g_logTagNameHeaders[index].m_length;

            const char* pch;
            // Now append Value" to the buffer
            switch (LogTagToType(tag))
            {
                default:
                    return; // Error, unknown parameter type!

                case LogTagType_String:
                {
                    char *string = va_arg(ptr, char*);

                    if (string == NULL)
                        string = "(null)";

                    if (tag == LogTag_RSLMsg)
                    {
                        const Message *message = (const Message *) string;
                        message->LogString(buffer, bufferEnd-buffer, &end);
                        buffer = end;
                    }
                    else if (tag == LogTag_RSLBallot)
                    {
                        const BallotNumber *ballot = (const BallotNumber *) string;
                        ballot->LogString(buffer, bufferEnd-buffer, &end);
                        buffer = end;
                    }
                    else
                    {
                        int   stringLength = (int) strlen(string);
                        const char * pchEnd;

                        if (buffer + stringLength >= bufferEnd)
                        {
                            // Copy what we can
                            int availableSpace = (int) (bufferEnd - buffer);

                            if (availableSpace > 0)
                            {
                                pch = string;
                                pchEnd = string + availableSpace;
                                while (pch < pchEnd)
                                {
                                    *buffer++ = (('\r' == *pch) || ('\n' == *pch)) ?
                                        '.' : *pch;
                                    ++pch;
                                }
                                *buffer++ = '\"';
                                *buffer++ = ' ';
                            }

                            goto exitNoRoom; // Not enough room, stop here
                        }

                        pch = string;
                        pchEnd = string + stringLength;
                        while (pch < pchEnd)
                        {
                            *buffer++ = (('\r' == *pch) || ('\n' == *pch)) ?
                                '.' : *pch;
                            ++pch;
                        }
                    }
                    *buffer++ = '\"';
                    *buffer++ = ' ';
                    break;
                }

                case LogTagType_Int32:
                {
                    int int32Value = va_arg(ptr, int);
                    if (FAILED(StringCchPrintfExA(buffer, bufferEnd-buffer, &end, NULL, 0, "%d\" ", int32Value)))
                    {
                        goto exitNoRoom;
                    }
                    buffer = end;
                    break;
                }

                case LogTagType_UInt32:
                {
                    int int32Value = va_arg(ptr, int);
                    if (FAILED(StringCchPrintfExA(buffer, bufferEnd-buffer, &end, NULL, 0, "%u\" ", int32Value)))
                    {
                        goto exitNoRoom;
                    }
                    buffer = end;
                    break;
                }

                case LogTagType_UInt64:
                {
                    unsigned __int64 int64Value = va_arg(ptr, unsigned __int64);
                    if (FAILED(StringCchPrintfExA(buffer, bufferEnd-buffer, &end, NULL, 0, "%I64u\" ", int64Value)))
                    {
                        goto exitNoRoom;
                    }
                    buffer = end;
                    break;
                }

                case LogTagType_Hex32:
                {
                    int int32Value = va_arg(ptr, int);
                    if (FAILED(StringCchPrintfExA(buffer, bufferEnd-buffer, &end, NULL, 0, "0x%08X\" ", int32Value)))
                    {
                        goto exitNoRoom;
                    }
                    buffer = end;
                    break;
                }

                case LogTagType_Int64:
                {
                    __int64 int64Value = va_arg(ptr, __int64);
                    if (FAILED(StringCchPrintfExA(buffer, bufferEnd-buffer, &end, NULL, 0, "%I64d\" ", int64Value)))
                    {
                        goto exitNoRoom;
                    }
                    buffer = end;
                    break;
                }

                case LogTagType_Hex64:
                {
                    // Ensure enough room for 0x<16 digits>"<space>
                    __int64 int64Value = va_arg(ptr, __int64);
                    if (FAILED(StringCchPrintfExA(buffer, bufferEnd-buffer, &end, NULL, 0, "0x%016I64X\" ", int64Value)))
                    {
                        goto exitNoRoom;
                    }
                    buffer = end;
                    break;
                }

                // For floats, the compiler will promote to double when it passes it in on the stack
                // Therefore, you have to take a double off the stack; if you take off a float, you will crash
                case LogTagType_Float:
                {
                    double doubleValue = va_arg(ptr, double);
                    if (FAILED(StringCchPrintfExA(buffer, bufferEnd-buffer, &end, NULL, 0, "%f\" ", doubleValue)))
                    {
                        goto exitNoRoom;
                    }
                    buffer = end;
                    break;
                }
            }
            tag = va_arg(ptr, LogTag);
        }

      exitNoRoom:

        *buffer = '\0'; // Don't increment buffer, because we don't want to write out a null terminator

        va_end(ptr);
    }

void Trace::Convert(const char* fmt, ...)
{
    va_list ptr;
    va_start(ptr, fmt);
    StringCchVPrintfA(Buffer, NUMELEM(Buffer), fmt, ptr);
    va_end(ptr);
}

void Logger::FailFast(const char* message)
{
    if (Logger::m_minidumpEnabled)
    {
        exit(1);
    }
    else
    {
        EXCEPTION_RECORD exr;
        ZeroMemory(&exr, sizeof(EXCEPTION_RECORD));
        exr.ExceptionCode = ERROR_FAIL_FAST_EXCEPTION;
        exr.ExceptionFlags = EXCEPTION_NONCONTINUABLE;
        exr.NumberParameters = 1;
        exr.ExceptionInformation[0] = reinterpret_cast<ULONG_PTR>(message);
        RaiseFailFastException(&exr, 0, FAIL_FAST_GENERATE_EXCEPTION_ADDRESS);
    }
}


// Compute the full path name for the directory where minidumps should be stored.
bool Logger::GetMiniDumpDataDir (char * szMiniDumpDataDir, size_t cbMiniDumpDataDir)
{
    if (szMiniDumpDataDir == NULL)
    {
        return false;
    }

    if (FAILED(StringCchCopyA(szMiniDumpDataDir, cbMiniDumpDataDir, Logger::m_minidumpsDir)))
    {
        return false;
    }

    return true;
}

// For current process, compute the filenames for the minidump and the
// crash log file. The minidump would contain the dump and the crash
// log file would contain information when the crash happend and
// at what time the process should be done writing the dump.
static bool GetCurrentProcessDumpAndCrashLogFileNameFullPath (
    const char * cszMiniDumpDataDir,    // the directory to store the files
    SYSTEMTIME stCurrent,             // the timestamp to use as the current time
    char * szMiniDumpFileFullPath,
    size_t cbMiniDumpFileFullPath,
    char * szCrashLogFileFullPath,
    size_t cbCrashLogFileFullPath)
{
    if (cszMiniDumpDataDir == NULL || szMiniDumpFileFullPath == NULL || szCrashLogFileFullPath == NULL)
        return false;

    // Get the relative path of the current process with the component name and the process
    // name in the format of <component>-<process>. For example, "d:\app\watchdog.12345\watchdog.exe"
    // would become something like "watchdog.12345-watchdog.exe".
    const char * cszDumpAndCrashLogFileNameBase;
    char szCurrentProgramNameFullPath[MAX_PATH];
    // Get the full path of the current process
    GetModuleFileNameA (NULL, szCurrentProgramNameFullPath, sizeof(szCurrentProgramNameFullPath));

    // create the relative path in the <component>-<process> format
    char * szShortProgramNameTemp = strrchr (szCurrentProgramNameFullPath, '\\');
    *szShortProgramNameTemp = '-';
    cszDumpAndCrashLogFileNameBase = strrchr (szCurrentProgramNameFullPath, '\\');

    if (cszDumpAndCrashLogFileNameBase == NULL)
        return false;

    cszDumpAndCrashLogFileNameBase ++; // skip the '\' char.

    DWORD dwProcessId = GetCurrentProcessId();

    char szTempPrefix[MAX_PATH];

    HRESULT h;
    h = StringCchPrintfA (
        szTempPrefix,
        NUMELEM(szTempPrefix),
        "%s\\%s.%u.GMT-%04u-%02u-%02u-%02u-%02u-%02u",
        cszMiniDumpDataDir, cszDumpAndCrashLogFileNameBase,
        dwProcessId,
        stCurrent.wYear,stCurrent.wMonth,stCurrent.wDay,
        stCurrent.wHour,stCurrent.wMinute,stCurrent.wSecond
        );
    if (FAILED(h))
        return false;

    h = StringCbPrintfA (szMiniDumpFileFullPath, cbMiniDumpFileFullPath, "%s.dmp", szTempPrefix);
    if (FAILED(h))
        return false;

    h = StringCbPrintfA (szCrashLogFileFullPath, cbCrashLogFileFullPath, "%s.crash", szTempPrefix);
    if (FAILED(h))
        return false;

    return true;
}

static bool GetProcessStartTime (HANDLE hProcess, APDateTime * pdtStart)
{
    FILETIME ftCreate, ftExit, ftKernel, ftUser;
    if (!GetProcessTimes (hProcess, &ftCreate, &ftExit, &ftKernel, &ftUser))
    {
        Log(LogID_Logging, LogLevel_Error, "Cannot get process start time", "Last Error Code %u", GetLastError());
        return false;
    }
    pdtStart->Set (ftCreate);
    return true;
}

// This file writes to the crash log. It is paired up with the function
// ReadCrashLogFile. The two functions should match.
// In the crash log file, we would like to store the process ID and the
// process start time, these 2 values can uniquely identify a process.
// We would also specify a time that the process is expected to die. If
// by the specified time, the process still exists, that means the process
// needs to be killed by someone.
static int g_Verifying_FILETIME_SAME_SIZE_AS_UINT64[sizeof(FILETIME)==sizeof(UInt64)?1:0]; // a trick to assert at compile time
static bool WriteCrashLogFile (const char * cszCrashLogFileFullPath, unsigned int timeoutMilliSeconds)
{
    // get the current process start time
    APDateTime dtStart;
    if (!GetProcessStartTime(GetCurrentProcess(), &dtStart))
        return false;

    // get the current process id
    DWORD dwCurrentProcessId = GetCurrentProcessId();

    // compute the expiration time
    FILETIME ftExpire;
    GetSystemTimeAsFileTime(&ftExpire);
    UInt64 u64Expire; // should be using ULARGE_INTEGER, but the compilte time assert g_Verifying_FILETIME_SAME_SIZE_AS_UINT64 makes sure this works
    memcpy (&u64Expire, &ftExpire, sizeof(u64Expire)); // according to MSDN, we need to do this to avoid memory alignment problem on 64bit Windows
    UInt64 u64Timeout = (UInt64) timeoutMilliSeconds * (UInt64) 10000; // 1 millisecond = 10000 100-nanoseconds
    u64Expire += u64Timeout;
    memcpy (&ftExpire, &u64Expire, sizeof(ftExpire));
    APDateTime dtExpire (ftExpire);

    // open to write the crash file
    FILE * fp = fopen (cszCrashLogFileFullPath, "w");
    if (fp == NULL)
    {
        Log(LogID_Logging, LogLevel_Error, "Cannot Write Crash Log File", "File %s, Last Error Code %u", cszCrashLogFileFullPath, GetLastError());
        return false;
    }
    ScopeGuard fileGuard = MakeGuard (fclose, fp);

    // write the crash log file
    char line[1024];
    fprintf (fp, "%I64u\n", (Int64)dtExpire.ConvertToUInt64()); // accurate expiration time the code actually uses
    dtExpire.ToString (line, sizeof(line));
    fprintf (fp, "%s\n", line); // human friendly expiration time for debugging
    fprintf (fp, "%u\n", dwCurrentProcessId); // write the process id
    fprintf (fp, "%I64u\n", (Int64)dtStart.ConvertToUInt64()); // accurate process start time the code actually uses
    dtStart.ToString (line, sizeof(line));
    fprintf (fp, "%s\n", line); // human friendly expiration time for debugging

    Log(LogID_Logging, LogLevel_Info, "Wrote Crash Log File", "File %s, ProcessId %u", cszCrashLogFileFullPath, dwCurrentProcessId);
    return true;
}

// This file reads to the crash log. It is paired up with the function
// WriteCrashLogFile. The two functions should match.
// In the crash log file, we would like to store the process ID and the
// process start time, these 2 values can uniquely identify a process.
// We would also specify a time that the process is expected to die. If
// by the specified time, the process still exists, that means the process
// needs to be killed by someone.
static bool ReadCrashLogFile (
    const char * cszCrashLogFileFullPath,
    APDateTime * pdtExpire,
    DWORD * pdwProcessId,
    APDateTime * pdtProcessStart)
{
    FILE * fp = fopen (cszCrashLogFileFullPath, "r");
    if (fp == NULL)
    {
        Log(LogID_Logging, LogLevel_Error, "Cannot Read Crash Log File", "File %s, Last Error Code %u", cszCrashLogFileFullPath, GetLastError());
        return false;
    }
    ScopeGuard fileGuard = MakeGuard (fclose, fp);

    char line[1024];
    ZeroMemory(line, sizeof(line));
    // read the exact expiration time in the form of UInt64
    if (!fgets(line, sizeof(line)-1, fp))
    {
        Log(LogID_Logging, LogLevel_Error, "Cannot Read Exact Expire Time", "File %s, Last Error Code %u", cszCrashLogFileFullPath, GetLastError());
        return false;
    }
    char *endptr;
    UInt64 u64Expire = _strtoui64 (line, &endptr, 10);
    if (*endptr != 0 && !isspace((unsigned char)(*endptr)))
    {
        Log(LogID_Logging, LogLevel_Error, "Cannot Parse Exact Expire Time", "Text %s, File %s, Last Error Code %u", line, cszCrashLogFileFullPath, GetLastError());
        return false;
    }
    // read the expiration time
    if (!fgets(line, sizeof(line)-1, fp))
    {
        Log(LogID_Logging, LogLevel_Error, "Cannot Read Friendly Expire Time", "File %s, Last Error Code %u", cszCrashLogFileFullPath, GetLastError());
        return false;
    }
    *pdtExpire = APDateTime::ConvertToDateTime(u64Expire);

    // read the process id
    if (!fgets(line, sizeof(line)-1, fp))
    {
        Log(LogID_Logging, LogLevel_Error, "Cannot Read Process Id", "File %s, Last Error Code %u", cszCrashLogFileFullPath, GetLastError());
        return false;
    }
    *pdwProcessId = atoi(line);
    if (*pdwProcessId == 0)
    {
        Log(LogID_Logging, LogLevel_Error, "Cannot Parse Process Id", "Text %s, File %s, Last Error Code %u", line, cszCrashLogFileFullPath, GetLastError());
        return false;
    }

    // read the exact start time in the form of UInt64
    if (!fgets(line, sizeof(line)-1, fp))
    {
        Log(LogID_Logging, LogLevel_Error, "Cannot Read Exact Start Time", "File %s, Last Error Code %u", cszCrashLogFileFullPath, GetLastError());
        return false;
    }
    UInt64 u64Start = _strtoui64 (line, &endptr, 10);
    if (*endptr != 0 && !isspace((unsigned char)(*endptr)))
    {
        Log(LogID_Logging, LogLevel_Error, "Cannot Parse Exact Start Time", "Text %s, File %s, Last Error Code %u", line, cszCrashLogFileFullPath, GetLastError());
        return false;
    }

    // read the process start time
    if (!fgets(line, sizeof(line)-1, fp))
    {
        Log(LogID_Logging, LogLevel_Error, "Cannot Read Process Start Time", "File %s, Last Error Code %u", cszCrashLogFileFullPath, GetLastError());
        return false;
    }
    *pdtProcessStart = APDateTime::ConvertToDateTime(u64Start);

    return true;
}

// Given a crash file that are written together with a minidump, this program will read the crash file and
// use the information in the crash file to decide what to do. If the process has expired and the process is
// still running, then this function will terminate that process. If there is the process is no longer running,
// this fucntion will delete the crash file.
static bool CheckAndKillAProcessFromCrashLogFile (const char * cszCrashLogFileFullPath, const APDateTime& dtCurrent)
{
    APDateTime dtExpire, dtStartTimeInLog, dtCurrentStartTime;
    DWORD dwProcessId;

    if (!ReadCrashLogFile (cszCrashLogFileFullPath, &dtExpire, &dwProcessId, &dtStartTimeInLog))
    {
        Log(LogID_Logging, LogLevel_Error, "Failed to Read Crash Log File", "File %s, Last Error Code %u", cszCrashLogFileFullPath, GetLastError());
        return false;
    }
    Log(LogID_Logging, LogLevel_Info, "Read Crash Log File", "File %s", cszCrashLogFileFullPath);

    if (dtCurrent < dtExpire)
    {
        Log(LogID_Logging, LogLevel_Info, "Not Expired Yet", "File %s, Expire %lu, Current %lu, Last Error Code %u",
            cszCrashLogFileFullPath, dtExpire.ConvertToUInt64(), dtCurrent.ConvertToUInt64(), GetLastError());
        return true; // if the process has not expired yet, we don't want to kill it
    }
    Log(LogID_Logging, LogLevel_Info, "Expired", "File %s, Expire %lu, Current %lu",
        cszCrashLogFileFullPath, dtExpire.ConvertToUInt64(), dtCurrent.ConvertToUInt64());

    // the process has expired, we need to check if the process still exists.
    // note: to uniquely identify a process through time, we need both process id
    // and process start time since the system recycle process id
    HANDLE hProcess = OpenProcess (PROCESS_QUERY_INFORMATION|PROCESS_TERMINATE|SYNCHRONIZE, false, dwProcessId);
    if (hProcess != NULL)
    {
        ScopeGuard processGuard = MakeGuard (CloseHandle, hProcess);

        // Eventhough we can get a handle to a process, the process might have already
        // exited. The OS might just keep it around a bit. We should check to see if
        // it has exited.
        DWORD dwWait = WaitForSingleObject(hProcess, 0);
        if (dwWait != WAIT_OBJECT_0)
        {
            // the process has not exited
            // we will check the start time to make sure this is the process we are looking for
            if (!GetProcessStartTime(hProcess, &dtCurrentStartTime))
            {
                Log(LogID_Logging, LogLevel_Error, "Failed to Get Process Start Time", "File %s, Process %u, Last Error Code %u", cszCrashLogFileFullPath, dwProcessId, GetLastError());
                return false;
            }

            if (dtStartTimeInLog == dtCurrentStartTime)
            {
                // The process has the same start time, it is the same process still not died
                if (!TerminateProcess(hProcess, 1))
                {
                    DWORD dwError = GetLastError();
                    Log(LogID_Logging, LogLevel_Error, "Failed to Kill Process", "File %s, Process %u, Last Error Code %u", cszCrashLogFileFullPath, dwProcessId, dwError);
                    return false;
                }
            }
            else
            {
                Log(LogID_Logging, LogLevel_Warning,
                    "Same PID With Different Start Time", "File %s, Process %u, Start Time in Log %I64u, Start Time in System %I64u",
                    cszCrashLogFileFullPath, dwProcessId, dtStartTimeInLog.ConvertToUInt64(), dtCurrentStartTime.ConvertToUInt64());
            }

        }
    }

    Log(LogID_Logging, LogLevel_Info, "Deleting File", "File %s", cszCrashLogFileFullPath);
    // When the code reach here, either the process has been long dead or it has been killed successfully.
    // In that case, we no longer need the crash log file.
    if (!DeleteFileA (cszCrashLogFileFullPath))
    {
        Log(LogID_Logging, LogLevel_Error, "Failed To Delete File", "File %s, Last Error Code %u", cszCrashLogFileFullPath, GetLastError());
        return false;
    }
    Log(LogID_Logging, LogLevel_Info, "Successfully Deleted File", "File %s", cszCrashLogFileFullPath);
    return true;
}

// For every minidump written, we will write out a crash log file. It contains information about
// the process that was writing the minidump and when the process expected to be considered as hung.
// This function will go through the minidump data dir and make sure those hunging process
// are killed.
// Note: the expire time is normally the current system time. We expose the parameter to make it easier
// for testing. I don't want to use any date time data structure we implemented (such as APDateTime) because
// I want to avoid circluar includes.
bool Logger::KillPossibleHungMiniDumpWritingProcess (const SYSTEMTIME& stExpire)
{
    APDateTime dtExpire;
    dtExpire.Set (stExpire);

    char szMiniDumpDataDir[MAX_PATH];
    if (!Logger::GetMiniDumpDataDir(szMiniDumpDataDir, sizeof(szMiniDumpDataDir)))
        return false;

    // We will go through all the *.crash files under the minidump dir
    char szFilesToFind[MAX_PATH];
    HRESULT hr = StringCchPrintfA (szFilesToFind, NUMELEM(szFilesToFind), "%s\\*.crash", szMiniDumpDataDir);
    if (FAILED(hr))
        return false;

    WIN32_FIND_DATAA findFileData;
    HANDLE hFind = FindFirstFileA (szFilesToFind, &findFileData);
    if (hFind == INVALID_HANDLE_VALUE)
    {
        DWORD dwError = GetLastError();
        if (dwError != ERROR_PATH_NOT_FOUND && dwError != ERROR_FILE_NOT_FOUND)
        {
            Log(LogID_Logging, LogLevel_Error, "Failed to Search MiniDumpDataDir", "Pattern %s, Last Error Code %u", szFilesToFind, dwError);
            return false;
        }
        else
            return true; // If there are no crash files found, there is nothing to do, but the function is considered succeeded
    }
    else
    {
        ScopeGuard findGuard = MakeGuard (FindClose, hFind);
        do
        {
            // generate the full path of the crash file found
            char szFileFullPath[MAX_PATH];
            hr = StringCchPrintfA (szFileFullPath, NUMELEM(szFileFullPath), "%s\\%s", szMiniDumpDataDir, findFileData.cFileName);
            if (FAILED(hr))
                return false;

            // check to see if the process associated with the crash file has exited, if not, terminate the process
            // if the process is successfully terminated, the crash file will be deleted, otherwise, the file would be
            // left alone.
            Log(LogID_Logging, LogLevel_Info, "Trying to Check and Removing Hanging Process", "File %s", szFileFullPath);
            if (!CheckAndKillAProcessFromCrashLogFile (szFileFullPath, dtExpire))
            {
                Log(LogID_Logging, LogLevel_Error, "Failed to Clean", "File %s, Last Error Code %u", szFileFullPath, GetLastError());
            }
        } while (FindNextFileA(hFind, &findFileData) != 0);

        DWORD dwError = GetLastError();
        if (dwError != ERROR_NO_MORE_FILES)
        {
            Log(LogID_Logging, LogLevel_Info, "Find Next File Failed", "Last Error Code %u", dwError);
            return false;
        }
        return true;
    }
}

// This function is called by Logger::LogAndExitProcess and
// Logger::WriteMiniDump.
//
// It writes out a process mini dump that can be used later to debug the
// unhandled exceptions. It will return EXCEPTION_EXECUTE_HANDLER so
// that the code will continue if it is used as an exception handler.
static volatile LONG g_startedMiniDump = 0;
LONG Logger::WriteExceptionMiniDump (EXCEPTION_POINTERS* exceptionPointers, unsigned int timeoutMilliSeconds, bool* pRet)
{
    *pRet = false;

    //Since writing a dump is a one time occurrence dont write it if its already
    //started.
    if (0 != ::InterlockedExchange(&g_startedMiniDump, 1))
    {//The dump is already started
        return EXCEPTION_EXECUTE_HANDLER;
    }

    // If this function actually causes exceptions itself, we will be
    // doomed with deadlock. So, we catch all the possible exceptions
    // generated by this function and do nothing.
    try
    {
        // flush out the current logs before preparing writing minidumps.
        Logger::Flush ();

        // Construct absolute path for the folder where the files will be written.
        // If SysInfo is not initialized, then the path will be "\minidumps".
        char szMiniDumpDataDir[MAX_PATH];
        if (!Logger::GetMiniDumpDataDir (szMiniDumpDataDir, sizeof(szMiniDumpDataDir)))
        {
            Log(LogID_Logging, LogLevel_Error, "Cannot Get Minidump Dir", "Last Error Code %u", GetLastError());
            return EXCEPTION_EXECUTE_HANDLER;
        }

        // Make sure the minidump directory is already created
        if (!DirUtils::MakeDirectory(szMiniDumpDataDir))
        {
            Log(LogID_Logging, LogLevel_Error, "Failed to create minidump dir", "Dir %s, Last Error Code %u", szMiniDumpDataDir, GetLastError());
            return EXCEPTION_EXECUTE_HANDLER;
        }

        // figure out the file name for the mini dump
        // and the log file about the crash
        char szMiniDumpFileFullPath[MAX_PATH];
        char szCrashLogFileFullPath[MAX_PATH];
        {
            SYSTEMTIME stNow;
            GetSystemTime(&stNow);

            bool f = GetCurrentProcessDumpAndCrashLogFileNameFullPath (
                szMiniDumpDataDir, stNow,
                szMiniDumpFileFullPath, sizeof(szMiniDumpFileFullPath),
                szCrashLogFileFullPath, sizeof(szCrashLogFileFullPath));
            if (!f)
                return EXCEPTION_EXECUTE_HANDLER;
        }

        // write the crash log so if the process hangs, it can be picked
        // up by the cleaning service (killhungprocess) later
        if (!WriteCrashLogFile (szCrashLogFileFullPath, timeoutMilliSeconds))
            return EXCEPTION_EXECUTE_HANDLER;

        // flush out the current logs before start writing minidumps.
        Logger::Flush ();

        // write the actual dump
        HANDLE hMiniDumpFile = CreateFileA (
            szMiniDumpFileFullPath,
            GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_WRITE | FILE_SHARE_READ,
            0,
            CREATE_ALWAYS,
            0,
            0
            );

        if (hMiniDumpFile != INVALID_HANDLE_VALUE)
        {
            ScopeGuard fileGuard = MakeGuard (CloseHandle, hMiniDumpFile);

            MINIDUMP_EXCEPTION_INFORMATION dumpExceptionInfo;
            dumpExceptionInfo.ThreadId = GetCurrentThreadId();
            dumpExceptionInfo.ExceptionPointers = exceptionPointers;
            dumpExceptionInfo.ClientPointers = TRUE;

            *pRet = TRUE == MiniDumpWriteDump (
                GetCurrentProcess(),
                GetCurrentProcessId(),
                hMiniDumpFile,
                MiniDumpWithDataSegs,
                &dumpExceptionInfo,
                NULL,
                NULL
                );
            if (!*pRet)
            {
                Log(
                    LogID_Logging,
                    LogLevel_Error,
                    "Failed to write minidump",
                    "Last Error Code %u",
                    GetLastError()
                    );
            }
        }
    }
    catch (...)
    {
        // do nothing
    }

    return EXCEPTION_EXECUTE_HANDLER;
}

void Logger::WaitForThreadOrExit(HANDLE handle, DWORD threadID)
{
    if (threadID != GetCurrentThreadId())
    {
        // wait for a maximum of 5 seconds. If the thread does not
        // exit within 5 seconds, Exit the process.
        DWORD ret = WaitForSingleObject(handle, 5*1000);
        if (ret != WAIT_OBJECT_0)
        {
            Logger::Terminate(1);
        }
    }
}

LONG Logger::LogAndExitProcess(EXCEPTION_POINTERS* exceptionPointers)
{
    Log(
        LogID_Logging,
        LogLevel_Error,
        "An unhandled exception was thrown, exiting process..."
        );
    Logger::Flush();

    bool fRet;
    WriteExceptionMiniDump (exceptionPointers, 15*60*1000, &fRet);
    Logger::Terminate(1);
}

// This function will write a process mini dump (if enabled).
// We need to turn off compiler optimization to guareente
// this function to work. Otherwise, sometimes under AMD64 retail build,
// the exception in this function might not happen.
#pragma optimize ("", off)
bool Logger::WriteMiniDump (unsigned int timeoutMilliSeconds)
{
    // According to MSDN (and with experience), the system
    // call that writes out the mini dump doesn't work well without the exception pointers.
    // Thus, in order to write out a mini dump, we need to get the thread state before
    // starting to write the dump.
    //
    // Therefore, we need a hack here. We generate a NULL-referencing exception to get the
    // thread state. We are not using "throw" because it creates more garbage in the stack
    // trace. We could also call the mini dump function from a new worker thread and filter
    // this worker thread from the dump, but that seems to be too much work as well.
    // We have also tried to use RaiseException function, but that couldn't do the job.
    bool fRet = false;
    if (Logger::m_minidumpEnabled)
    {
        __try
        {
            int * ptr = NULL;
#pragma prefast(suppress:11, "We intentionly generated NULL referencing here, reviewed by minara")
            *ptr = 0;
        }
        __except (WriteExceptionMiniDump (GetExceptionInformation(), timeoutMilliSeconds, &fRet))
        {
#pragma prefast(suppress:322, "The work has to be done in exception filter above, nothing to do here, reviewed by minara")
            // do nothing
        }
    }
    return fRet;
}
#pragma optimize ("", on)
// Important: Since the optimize pragma doesn't seem to support push/pop, I put this function at
// the end of the file, so it won't affect any other functions. If you add your function after
// this line, you will be affected by the possiblly changed optimization settings.

} // namespace RSLibImpl
