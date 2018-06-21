#include "legislator.h"
#include "rsldebug.h"
#include "hirestime.h"
#include "apdiskio.h"
#include "limits.h"
#include "Interlocked.h"
#include "scopeGuard.h"
#include <strsafe.h>
#include <algorithm>
#include <utility>
#include <process.h>
#include <time.h>
#include <windows.h>
#include <strsafe.h>

#pragma warning(push )
#pragma warning(disable : 4201) // nonstandard extension used : nameless struct/union
#include <winioctl.h>
#pragma warning(pop )

#define MAX_SINGLE_IO_SIZE      (32*1024*1024)      // max size we can pass to unbuffered ReadFile/WriteFile (MDL size limit of XP kernel :-)
#define SAFE_IO_ALIGNMENT       (1024*1024)         // safe alignment for buffer & size (shall be multiple of page size and sector size)

using namespace std;
using namespace RSLibImpl;

// Change the minimum in RSLConfig if this value is modified.
static const UInt32 c_ExecutionReadSize = 1024*1024; // 1 megabyte

static Int64 Randomize(Int64 value, UInt32 percent, Int64 maxDeviation)
{
    // generate a random number within the given percentage of the original interval,
    // with the specified maximum deviation.

    // this returns a random double between -1 and 1.
    double r = ((double) rand()*2 / RAND_MAX) - 1;
    Int64 deviation = min((Int64) (value*percent/100), maxDeviation);

    return (value + (Int64) (deviation*r));
}

struct CheckpointCopyInfo
{
    UInt32 m_ip;
    UInt16 m_port;
    UInt64 m_checkpointDecree;
    UInt64 m_checkpointSize;
};

SocketStreamReader::SocketStreamReader(StreamSocket *sock) : m_sock(sock), m_read(0)
{}

DWORD32
SocketStreamReader::Read(void *buffer, UInt32 numBytes, UInt32 *read)
{
    DWORD32 ec = m_sock->Read(buffer, numBytes, read);
    m_read += *read;
    return ec;
}

DiskStreamReader::DiskStreamReader(APSEQREAD *seqRead) : m_seqRead(seqRead), m_read(0)
{}

DWORD32
DiskStreamReader::Read(void *buffer, UInt32 numBytes, UInt32 *read)
{
    char *buf = (char *) buffer;
    char *bufEnd = buf + numBytes;
    *read = 0;
    while(buf < bufEnd)
    {
        char *pb;
        DWORD bytes;
        DWORD32 ec;
        ec = m_seqRead->GetDataPointer((void**)&pb, (DWORD) (bufEnd - buf), &bytes);
        m_read += bytes;
        if (ec != NO_ERROR)
        {
            if (ec == ERROR_HANDLE_EOF && *read > 0)
            {
                return NO_ERROR;
            }
            return ec;
        }
        memcpy(buf, pb, bytes);
        buf += bytes;
        *read += bytes;
    }
    return NO_ERROR;
}

ExecuteQueue::ExecuteQueue() :
    m_lastPassedDecree(0), m_lastInsertedDecree(0), m_legislator(NULL), m_fIOWait(false), m_paused(Thread_Run)
{
    m_executeWaitHandle = CreateEvent(NULL, FALSE, TRUE, NULL);
    m_readWaitHandle = CreateEvent(NULL, FALSE, TRUE, NULL);
    m_overlapEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
}

ExecuteQueue::~ExecuteQueue()
{
    if (m_executeWaitHandle != nullptr)
    {
        CloseHandle(m_executeWaitHandle);
        m_executeWaitHandle = nullptr;
    }
    if (m_readWaitHandle != nullptr)
    {
        CloseHandle(m_readWaitHandle);
        m_readWaitHandle = nullptr;
    }
    if (m_overlapEvent != nullptr)
    {
        CloseHandle(m_overlapEvent);
        m_overlapEvent = nullptr;
    }
}

void
ExecuteQueue::Stop()
{
    m_paused = Thread_Terminate;
    SetEvent(m_readWaitHandle);
    while (m_paused!=Thread_Terminated)
    {
        Sleep(20);
    }
}

void
ExecuteQueue::Init(Legislator* legislator)
{
    m_legislator = legislator;
    m_lastPassedDecree = legislator->m_lastExecutedDecree;
    m_lastInsertedDecree = legislator->m_lastExecutedDecree;
}

void
ExecuteQueue::Run()
{
    ReadExecuteFromDiskLoop(this);
}

bool
ExecuteQueue::IsInMemory()
{
    AutoCriticalSection lock(&m_legislator->m_lock);
    return (m_lastInsertedDecree == m_lastPassedDecree);
}

void
ExecuteQueue::Enqueue(Vote *vote, bool must)
{
    AutoCriticalSection lock(&m_legislator->m_lock);

    // If this vote has already passed, ignore it.
    // A vote can pass twice because of primary change.
    if (vote->m_decree == m_lastPassedDecree)
    {
        return;
    }

    UInt32 maxSize = m_legislator->m_cfg.InMemoryExecutionQueueSize();

    // if this vote must go into the execution queue, the  queue must be in
    // memory
    LogAssert(!must || m_lastPassedDecree == m_lastInsertedDecree);
    m_lastPassedDecree = vote->m_decree;

    if (must || (m_toExecuteQ.SizeInBytes() < maxSize &&
                 m_lastPassedDecree == m_lastInsertedDecree+1))
    {
        RSLDebug("Adding to execution queue (num elements, size MB)", LogTag_RSLMsg, vote,
                 LogTag_UInt1, m_toExecuteQ.NumElements(),
                 LogTag_UInt1, m_toExecuteQ.SizeInBytes()/1024);

        m_toExecuteQ.Enqueue(vote);
        m_lastInsertedDecree = m_toExecuteQ.Tail()->m_decree;
        SetEvent(m_executeWaitHandle);
    }
    else
    {
        RSLDebug("Not inserting vote in execution queue",
                 LogTag_RSLMsg, vote,
                 LogTag_RSLDecree, m_lastInsertedDecree,
                 LogTag_UInt1, m_toExecuteQ.SizeInBytes());

        if (CheckRead())
        {
            SetEvent(m_readWaitHandle);
        }
    }
}

Vote*
ExecuteQueue::Dequeue(Ptr<Vote> *votePtr)
{
    AutoCriticalSection lock(&m_legislator->m_lock);
    Vote* vote = m_toExecuteQ.Dequeue(votePtr);
    if (vote != NULL)
    {
        // check if were waiting for a dequeue before we can start reading
        // from disk
        if (CheckRead())
        {
            SetEvent(m_readWaitHandle);
        }
    }
    return vote;
}

Vote*
ExecuteQueue::Head(Ptr<Vote> *votePtr)
{
    AutoCriticalSection lock(&m_legislator->m_lock);
    *votePtr = m_toExecuteQ.Head();
    return *votePtr;
}

void
ExecuteQueue::WaitExecuteThread(DWORD timeout)
{
    DWORD ret = WaitForSingleObject(m_executeWaitHandle, timeout);
    LogAssert(ret != WAIT_FAILED);
}

LogFile*
ExecuteQueue::CheckRead()
{
    // Check if we should read decrees from disk and add to the queue
    UInt32 maxSize = m_legislator->m_cfg.InMemoryExecutionQueueSize();
    if (m_fIOWait ||
        m_lastInsertedDecree >= m_lastPassedDecree ||
        m_toExecuteQ.SizeInBytes() + c_ExecutionReadSize >= maxSize)
    {
        return NULL;
    }
    // check which log file has this vote
    LogFile *logFile = NULL;
    vector<LogFile *>::iterator iter;

    for (iter = m_legislator->m_logFiles.begin(); iter != m_legislator->m_logFiles.end(); iter++)
    {
        if ((*iter)->HasDecree(m_lastInsertedDecree+1))
        {
            logFile = *iter;
            break;
        }
    }
    LogAssert(logFile != NULL);

    UInt64 beginOffset = logFile->GetOffset(m_lastInsertedDecree+1);
    UInt64 d = min(logFile->MaxDecree(), m_lastPassedDecree);
    UInt32 todo = (DWORD) (logFile->GetOffset(d) - beginOffset) + logFile->GetLengthOfDecree(d);

    // Read the data if:
    // 1. There is at least c_ExecutionReadSize worth of logs to read from disk
    // 2. If we are reading till the end of the log file and the log file is not the
    // last log file
    // 3. We have 50% or more free space availabe in the queue.
    if (todo >= c_ExecutionReadSize ||
        logFile != m_legislator->m_logFiles.back() ||
        m_toExecuteQ.SizeInBytes() <= maxSize/2)
    {
        return logFile;
    }
    return NULL;
}

void
ExecuteQueue::ReadLoop()
{
    size_t bufSize = 0;
    char* buf = NULL;

    m_legislator->m_lock.Enter();
    for (;;)
    {
        while (m_paused != Thread_Run)
        {
            if (m_paused == Thread_Terminate)
            {
                m_paused = Thread_Terminated;
                m_legislator->m_lock.Leave();
                return;
            }
            Sleep(10);
        }

        // dequeue votes from the m_toExecuteQ and execute them
        vector<UInt32> offsets;
        LogFile *logFile = NULL;
        DWORD todo = 0;
        UInt64 decree = 0;
        VoteQueue localQueue;

        if ((logFile = CheckRead()) == NULL)
        {
            m_legislator->m_lock.Leave();
            WaitForSingleObject(m_readWaitHandle, INFINITE);
            m_legislator->m_lock.Enter();
            continue;
        }

        decree = m_lastInsertedDecree+1;
        UInt64 beginOffset = logFile->GetOffset(decree);
        UInt64 maxDecree = min(logFile->MaxDecree(), m_lastPassedDecree);
        for (UInt64 d = decree; d <= maxDecree && todo < c_ExecutionReadSize; d++)
        {
            UInt32 offset = (DWORD) (logFile->GetOffset(d) - beginOffset);
            todo = offset + logFile->GetLengthOfDecree(d);
            offsets.push_back(offset);
        }
        // add a sentinel.
        offsets.push_back(todo);
        m_fIOWait = true;
        m_legislator->m_lock.Leave();

        if (bufSize < todo)
        {
            if (buf)
            {
                VirtualFree(buf, 0, MEM_RELEASE);
            }
            bufSize = max(s_AvgMessageLen, todo);
            buf = (char *) VirtualAlloc(NULL, bufSize, MEM_COMMIT, PAGE_READWRITE);
            LogAssert(buf);
        }

        Int64 readStarted = GetHiResTime();
        if (!logFile->Read(buf, todo, beginOffset, m_overlapEvent))
        {
            LogAssert(false, "Read Failed");
        }
        Int64 readFinished = GetHiResTime();

        {
            // Update stats under the lock.
            AutoCriticalSection(&m_legislator->m_statsLock);

            UInt32 readTime = (UInt32)(readFinished - readStarted);
            m_legislator->m_stats.m_cLogReads++;
            m_legislator->m_stats.m_cLogReadBytes += todo;
            m_legislator->m_stats.m_cLogReadMicroseconds += readTime;
            if (readTime > m_legislator->m_stats.m_cLogReadMaxMicroseconds)
                m_legislator->m_stats.m_cLogReadMaxMicroseconds = readTime;
        }

        for (size_t i = 0; i < offsets.size()-1; i++)
        {
            UInt32 offset = offsets[i];
            UInt32 size = offsets[i+1] - offset;

            Ptr<Vote> vote = new Vote();
            if (!vote->UnMarshalBuf(buf+offset, size))
            {
                LogAssert(false, "Failed to unmarshal message for execution",
                          LogTag_RSLDecree, decree,
                          LogTag_Offset, beginOffset+offset,
                          LogTag_UInt1, size);
            }
            if (!vote->VerifyChecksum(buf+offset, vote->m_unMarshalLen))
            {
                LogAssert(false, "Checksum mis-match while reading execution queue",
                          LogTag_RSLMsg, (Vote *) vote,
                          LogTag_Offset, beginOffset+offset,
                          LogTag_UInt1, size);
            }
            RSLDebug("Read vote for execution",
                     LogTag_RSLMsg, (Vote *) vote,
                     LogTag_Offset, beginOffset+offset);

            LogAssert(vote->m_decree == decree+i);
            localQueue.Enqueue(vote);
        }

        // reset the buffer
        if (bufSize > s_AvgMessageLen)
        {
            VirtualFree(buf, 0, MEM_RELEASE);
            buf = NULL;
            bufSize = 0;
        }

        m_legislator->m_lock.Enter();
        m_fIOWait = false;
        if (localQueue.Head() != NULL)
        {
            m_toExecuteQ.Append(localQueue);
            m_lastInsertedDecree = m_toExecuteQ.Tail()->m_decree;
            SetEvent(m_executeWaitHandle);
        }
    }
}

void
ExecuteQueue::ReadExecuteFromDiskLoop(void *arg)
{
    ExecuteQueue *me = (ExecuteQueue *) arg;
    me->ReadLoop();
    //LogAssert(!"Failed to execute queue from disk");
    //return 0;
}


StatusMap::StatusMap() : m_numResponses(0)
{}

StatusMap::~StatusMap()
{
    Clear();
}

void
StatusMap::Insert(StatusResponse *resp)
{
    for (StatusResponse *s = m_queue.head; s != NULL; s = (StatusResponse *) s->link.next)
    {
        if (s->m_memberId == resp->m_memberId)
        {
            m_queue.remove(s);
            delete s;
            m_numResponses--;
            break;
        }
    }
    StatusResponse *copy = new StatusResponse(*resp);
    LogAssert(copy);
    m_queue.enqueue(copy);
    m_numResponses++;
}

UInt32
StatusMap::Size()
{
    return m_numResponses;
}

void
StatusMap::Clear()
{
    StatusResponse *s;
    while ((s = m_queue.dequeue()) != NULL)
    {
        delete s;
    }
    m_numResponses = 0;
}

void
StatusMap::FindHigherLogged(UInt64 minDecreeInLog, UInt64 decree,
                            BallotNumber &ballot, StatusList *list)
{
    list->clear();
    for (StatusResponse *s = m_queue.head; s; s = (StatusResponse *) s->link.next)
    {
        if (s->m_minDecreeInLog <= minDecreeInLog && s->IsHigherDecree(decree, ballot))
        {
            list->push_back(s);
        }
    }
}

void
StatusMap::FindCheckpoint(UInt64 decree, StatusList *list)
{
    list->clear();
    for (StatusResponse *s = m_queue.head; s; s = (StatusResponse *) s->link.next)
    {
        if (s->m_checkpointedDecree >= decree)
        {
            list->push_back(s);
        }
    }
}

Int64
StatusMap::FindMinReceivedAgo()
{
    Int64 min = _I64_MAX;
    for (StatusResponse *s = m_queue.head; s; s = (StatusResponse *) s->link.next)
    {
        if (s->m_lastReceivedAgo < min)
        {
            min = s->m_lastReceivedAgo;
        }
    }
    if (min < 0)
    {
        min = 0;
    }
    return min;
}

LogFile::LogFile() : m_hFile(INVALID_HANDLE_VALUE), m_overlapEvent(nullptr),
                     m_minDecree(0), m_dataLen(0)
{
}

LogFile::~LogFile()
{
    if (m_hFile != INVALID_HANDLE_VALUE)
    {
        CloseHandle(m_hFile);
    }
    if (m_overlapEvent != nullptr)
    {
        CloseHandle(m_overlapEvent);
    }
}

DWORD32
LogFile::Open(const char *dir, UInt64 decree)
{
    HRESULT ret;
    ret = StringCchPrintfA(m_fileName, sizeof(m_fileName), "%s%I64u.log", dir, decree);
    LogAssert(SUCCEEDED(ret));
    m_hFile = CreateFileA(m_fileName,
                          GENERIC_WRITE|FILE_READ_DATA,
                          FILE_SHARE_READ,
                          NULL,
                          OPEN_ALWAYS,
                          FILE_FLAG_NO_BUFFERING|FILE_FLAG_WRITE_THROUGH|FILE_FLAG_OVERLAPPED,
                          NULL);

    if (m_hFile == INVALID_HANDLE_VALUE)
    {
        int ec = GetLastError();
        RSLError("Failed to open log", LogTag_Filename, m_fileName, LogTag_ErrorCode, ec);
        return ec;
    }

    m_overlapEvent =  CreateEvent(NULL, TRUE, FALSE, NULL);
    if (m_overlapEvent == nullptr)
    {
        int ec = GetLastError();
        RSLError("Failed to create event", LogTag_ErrorCode, ec);
        return ec;
    }

    RSLInfo("Opened log File", LogTag_Filename, m_fileName);
    return NO_ERROR;
}

bool
LogFile::Write(SIZED_BUFFER *bufs, UInt32 count)
{
    DWORD bytesToWrite = 0;
    int numTotalPages = 0;
    UInt64 offset = m_dataLen;

    DynamicBuffer<FILE_SEGMENT_ELEMENT, PAGES_PER_WRITE+1> segments;

    for (UInt32 i = 0; i < count; i++)
    {
        LogAssert((bufs[i].m_len % s_PageSize) == 0);
        bytesToWrite += bufs[i].m_len;

        UInt32 numPages = bufs[i].m_len/s_SystemPageSize;
        UInt32 remainingBytes = bufs[i].m_len % s_SystemPageSize;
        if (remainingBytes)
        {
            numPages++;
        }
        LogAssert(i == count-1 || remainingBytes == 0);

        for (UInt32 j = 0; j < numPages; j++)
        {
            BYTE *buffer = (BYTE *) bufs[i].m_buf + j*s_SystemPageSize;
            segments[numTotalPages++].Buffer = (PVOID64) buffer;
            if (numTotalPages == PAGES_PER_WRITE)
            {
                segments[numTotalPages].Buffer = (PVOID64) NULL;
                DWORD toWrite = min(PAGES_PER_WRITE*s_SystemPageSize, bytesToWrite);
                if (!IssueWriteFileGather(segments.Begin(), offset, toWrite))
                {
                    return false;
                }
                numTotalPages = 0;
                bytesToWrite -= toWrite;
                offset += toWrite;
            }
        }
    }
    if (numTotalPages > 0)
    {
        segments[numTotalPages].Buffer = (PVOID64) NULL;
        if (!IssueWriteFileGather(segments.Begin(), offset, bytesToWrite))
        {
            return false;
        }
    }
    return true;
}

bool
LogFile::IssueWriteFileGather(FILE_SEGMENT_ELEMENT *segments, UInt64 offset, DWORD bytesToWrite)
{
    LARGE_INTEGER i;
    i.QuadPart = offset;

    OVERLAPPED overlap;
    memset(&overlap, 0, sizeof(OVERLAPPED));
    overlap.Offset = i.LowPart;
    overlap.OffsetHigh = i.HighPart;
    overlap.hEvent = m_overlapEvent;

    BOOL ret = ::WriteFileGather(
        m_hFile,
        segments,
        bytesToWrite,
        0,
        &overlap);

    if (FALSE == ret)
    {
        DWORD cbRead;
        if (GetLastError() == ERROR_IO_PENDING)
        {
            ret = GetOverlappedResult(m_hFile, &overlap, &cbRead, TRUE);

            if (ret && bytesToWrite != cbRead)
            {
                ret = FALSE;
            }
        }
    }
    if (FALSE == ret)
    {
        DWORD ec = ::GetLastError();
        RSLError("Disk I/O error writing to log file",
                 LogTag_Filename, m_fileName,
                 LogTag_ErrorCode, ec,
                 LogTag_Offset, offset,
                 LogTag_UInt1, bytesToWrite);
    }
    return (ret != FALSE);
}

bool
LogFile::Read(void* buf, UInt32 numBytes, UInt64 offset, HANDLE event)
{
    while (numBytes != 0)
    {
        OVERLAPPED Overlapped;
        DWORD dwBytesRead;
        DWORD dwBytesToRead;
        BOOL  fResult;

        dwBytesToRead = numBytes;
        if (dwBytesToRead > MAX_SINGLE_IO_SIZE)
        {
            dwBytesToRead = MAX_SINGLE_IO_SIZE;
            if (numBytes < 2*MAX_SINGLE_IO_SIZE)
            {
                //
                // if we need to read more than MAX_SINGLE_IO_SIZE but less than 2*MAX_SINGLE_IO_SIZE,
                // we'd better read two large blocks of approximately numBytes/2 bytes instead of
                // one large (MAX_SINGLE_IO_SIZE) block and then one small block (numBytes - MAX_SINGLE_IO_SIZE).
                //
                dwBytesToRead = ((numBytes >> 1) + SAFE_IO_ALIGNMENT - 1) & ~(SAFE_IO_ALIGNMENT - 1);
            }
        }

        //
        // Use async overlapped IO and wait for completion immediately;
        // it is equivalent to synchronous IO but it doesn't cause modification of
        // current file pointer
        //
        Overlapped.hEvent     = event;
        Overlapped.Offset     = (ULONG) offset;
        Overlapped.OffsetHigh = (ULONG) (offset >> 32);

        dwBytesRead = 0;

        fResult = ReadFile (m_hFile, buf, dwBytesToRead, &dwBytesRead, &Overlapped);
        if (!fResult)
        {
            if (GetLastError () == ERROR_IO_PENDING)
            {
                fResult = GetOverlappedResult (m_hFile, &Overlapped, &dwBytesRead, TRUE);
            }
        }

        if (!fResult || dwBytesToRead != dwBytesRead)
        {
            Log (
                LogID_RSLLIB,
                LogLevel_Error,
                "Read failed",
                "fResult=%d m_hFile=%p buf=%p BytesToRead=0x%x BytesRead=0x%x GetLastError()=%d numBytes=0x%x",
                fResult,
                m_hFile,
                buf,
                dwBytesToRead,
                dwBytesRead,
                GetLastError (),
                numBytes
            );

            return false;
        }

        offset   += dwBytesToRead;
        numBytes -= dwBytesToRead;
        buf       = (void *) (((char *) buf) + dwBytesToRead);
    }
    return true;
}

void
LogFile::AddMessage(Message *msg)
{
    UInt32 messageLen =  RoundUpToPage(msg->GetMarshalLen());
    RSLDebug("Adding Message to Log", LogTag_RSLMsg, msg, LogTag_RSLMsgLen, messageLen,
             LogTag_Offset, m_dataLen);

    if (msg->m_msgId == Message_Vote)
    {
        if (m_decreeOffsets.size() == 0)
        {
            m_minDecree = msg->m_decree;
        }
        else
        {
            LogAssert(msg->m_decree == MaxDecree() || msg->m_decree == MaxDecree()+1);
            if (MaxDecree() == msg->m_decree)
            {
                m_decreeOffsets.pop_back();
            }
        }
        m_decreeOffsets.push_back(m_dataLen);
    }
    m_dataLen += messageLen;
}

UInt64
LogFile::GetOffset(UInt64 decree)
{
    LogAssert(decree >= m_minDecree && decree <= MaxDecree());
    UInt32 offset = (UInt32) (decree - m_minDecree);
    return (m_decreeOffsets.size() ? m_decreeOffsets[offset] : 0);
}

bool
LogFile::HasDecree(UInt64 decree)
{
    return (m_minDecree <= decree && MaxDecree() >= decree);
}

UInt32
LogFile::GetLengthOfDecree(UInt64 decree)
{
    if (decree < MaxDecree())
    {
        return (UInt32) (GetOffset(decree+1) - GetOffset(decree));
    }
    return (UInt32) (m_dataLen - GetOffset(decree));
}

UInt64
LogFile::MaxDecree()
{
    UInt32 size = (UInt32) m_decreeOffsets.size();
    return ((size) ? m_minDecree+size-1 : 0);
}

DWORD32
LogFile::SetWritePointer()
{
    LARGE_INTEGER offset;
    offset.QuadPart = m_dataLen;
    if (!SetFilePointerEx(m_hFile, offset, NULL, FILE_BEGIN))
    {
        int ec = GetLastError();
        RSLError("SetFilePointer Failed",
                 LogTag_Filename, m_fileName, LogTag_ErrorCode, ec);
        return ec;
    }
    return NO_ERROR;

}

void
ConfigurationInfo::Marshal(MarshalData *marshal, RSLProtocolVersion version)
{
    marshal->WriteUInt32(m_configurationNumber);
    marshal->WriteUInt64(m_initialDecree);
    m_memberSet->Marshal(marshal, version);
}

bool
ConfigurationInfo::UnMarshal(MarshalData *marshal, RSLProtocolVersion version)
{
    if (!marshal->ReadUInt32(&m_configurationNumber))
    {
        return false;
    }

    if (!marshal->ReadUInt64(&m_initialDecree))
    {
        return false;
    }

    m_memberSet = new MemberSet();
    if (!m_memberSet->UnMarshal(marshal, version))
    {
        return false;
    }

    return true;
}

UInt32
ConfigurationInfo::GetMarshalLen(RSLProtocolVersion version)
{
    return 4 + 8 + m_memberSet->GetMarshalLen(version);
}

UInt32
CheckpointHeader::GetMarshalLen()
{
    UInt32 len = RoundUpToPage(m_nextVote->GetMarshalLen());

    if (m_version >= RSLProtocolVersion_3)
    {
        len +=
            2 + // version
            4 + // length
            8 + // old checksum
            MemberId::GetBaseSize(m_version) + // memberId
            8 + // last execute decree
            BallotNumber::GetBaseSize(m_version) + // max ballot
            m_stateConfiguration->GetMarshalLen(m_version); // replica set
    }
    if (m_version >= RSLProtocolVersion_4)
    {
        len +=
            1 + // stateSaved
            8 + // checkpoint size
            4;  // checksum block size
    }
    return RoundUpToPage(len);
}

void
CheckpointHeader::Marshal(const char *fileName)
{
    MarshalData marshal;
    Marshal(&marshal);

    HANDLE hFile = CreateFileA(fileName,
        FILE_WRITE_DATA | FILE_WRITE_ATTRIBUTES,
        FILE_SHARE_READ|FILE_SHARE_WRITE,
        NULL,
        OPEN_ALWAYS,
        NULL,
        NULL);

    if (hFile == INVALID_HANDLE_VALUE)
    {
        DWORD ec = GetLastError();
        LogAssert(ec == NO_ERROR, "error code - %u", ec);
    }

    UInt32 marshallLen = marshal.GetMarshaledLength();
    char* buf = (char *) marshal.GetMarshaled();
    while (marshallLen > 0)
    {
        UInt32 toWrite = marshallLen;
        if (toWrite > MAX_SINGLE_IO_SIZE)
        {
            toWrite = MAX_SINGLE_IO_SIZE;
        }

        DWORD written;
        if (!WriteFile(hFile, buf, toWrite, &written, NULL))
        {
            DWORD ec = GetLastError();
            LogAssert(ec == NO_ERROR, "error code - %u", ec);
        }
        marshallLen -= written;
        buf += written;
    }

    if (!CloseHandle(hFile))
    {
        DWORD ec = GetLastError();
        LogAssert(ec == NO_ERROR, "error code - %u", ec);
    }
}

void
CheckpointHeader::Marshal(MarshalData *marshal)
{
    UInt32 marshalLen = GetMarshalLen();
    marshal->EnsureBuffer(marshalLen);
    if (m_version >= RSLProtocolVersion_3)
    {
        marshal->WriteUInt16((UInt16) m_version);
        marshal->WriteUInt32(marshalLen);
        // compute the checksum
        marshal->WriteUInt64(m_checksum);
        m_memberId.Marshal(marshal, m_version);
        marshal->WriteUInt64(m_lastExecutedDecree);
        m_maxBallot.Marshal(marshal, m_version);
        m_stateConfiguration->Marshal(marshal, m_version);
    }
    if (m_version >= RSLProtocolVersion_4)
    {
        marshal->WriteBool(m_stateSaved);
        marshal->WriteUInt64(m_size);
        marshal->WriteUInt32(m_checksumBlockSize);
    }
    DynamicBuffer<SIZED_BUFFER, 1024> buffers(m_nextVote->GetNumBuffers());
    m_nextVote->GetBuffers(buffers, buffers.Size());
    for (size_t i = 0; i < (size_t) m_nextVote->GetNumBuffers(); i++)
    {
        marshal->WriteData(buffers[i].m_len, buffers[i].m_buf);
    }
    marshal->SetMarshaledLength(marshalLen);
}

bool
CheckpointHeader::UnMarshal(const char *fileName)
{
    DWORD ec;
    auto_ptr<APSEQREAD> seqRead(new APSEQREAD());
    ec = seqRead->DoInit(fileName, 2, s_AvgMessageLen, false);
    if (ec != NO_ERROR)
    {
        RSLError("Open file failed", LogTag_Filename, fileName, LogTag_ErrorCode, ec);
        return false;
    }

    DiskStreamReader reader(seqRead.get());
    if (!UnMarshal(&reader))
    {
        return false;
    }
    if (m_version < RSLProtocolVersion_4)
    {
        m_size = seqRead->FileSize();
    }
    return true;
}

bool
CheckpointHeader::UnMarshal(MarshalData *marshal)
{
    UInt16 version;
    if (!marshal->ReadUInt16(&version))
    {
        return false;
    }

    if (!Message::IsVersionValid(version))
    {
        return false;
    }

    m_version = (RSLProtocolVersion) version;
    // if the version is greater than 1, then is the new format, otherwise only the vote has been
    // marshaled

    if (m_version >= RSLProtocolVersion_3)
    {
        m_stateConfiguration = new ConfigurationInfo();
        // Jay Lorch doesn't understand how to make checksumming work.  We
        // should verify the checksum here.
        if (!marshal->ReadUInt32(&m_unMarshalLen) ||
            !marshal->ReadUInt64(&m_checksum) ||
            !m_memberId.UnMarshal(marshal, m_version) ||
            !marshal->ReadUInt64(&m_lastExecutedDecree) ||
            !m_maxBallot.UnMarshal(marshal, m_version) ||
            !m_stateConfiguration->UnMarshal(marshal, m_version))
        {
            return false;
        }
        if (m_version >= RSLProtocolVersion_4)
        {
            if (!marshal->ReadBool(&m_stateSaved) ||
                !marshal->ReadUInt64(&m_size) ||
                !marshal->ReadUInt32(&m_checksumBlockSize))
            {
                return false;
            }
        }
    }
    else
    {
        // set the read pointer back to the beginning of the message
        marshal->RewindReadPointer(2);
        m_stateConfiguration = NULL;
        m_stateSaved = true;
    }

    // TODO: assert that vote->decree == checkpointdecree+1
    UInt32 startOffset = marshal->GetReadPointer();
    m_nextVote = new Vote();
    if (!m_nextVote->UnMarshal(marshal))
    {
        return false;
    }
    LogAssert(startOffset + m_nextVote->m_unMarshalLen <= marshal->GetMarshaledLength());
    bool verified = m_nextVote->VerifyChecksum(
        (char *) marshal->GetMarshaled() + startOffset,
        m_nextVote->m_unMarshalLen);

    if (!verified)
    {
        return false;
    }

    if (m_version >= RSLProtocolVersion_3)
    {
        if (m_maxBallot < m_nextVote->m_ballot)
        {
            return false;
        }
    }
    else
    {
        m_memberId = m_nextVote->m_memberId;

        m_maxBallot = m_nextVote->m_ballot;
        m_lastExecutedDecree = m_nextVote->m_decree-1;
    }
    return true;
}

bool
CheckpointHeader::UnMarshal(StreamReader *reader)
{
    StandardMarshalMemoryManager memory(s_PageSize);
    UInt32 bytesRead;
    DWORD ec = reader->Read((char *) memory.GetBuffer(), s_PageSize, &bytesRead);
    if (ec != NO_ERROR || bytesRead != s_PageSize)
    {
        RSLInfo("Failed to read checkpoint header", LogTag_ErrorCode, ec);
        return false;
    }

    MarshalData hdr(memory.GetBuffer(), s_PageSize, false /* don't copy data */);

    UInt32 marshalLen;
    UInt16 version;
    hdr.ReadUInt16(&version);
    hdr.ReadUInt32(&marshalLen);

    if (!Message::IsVersionValid(version))
    {
        return false;
    }

    UInt32 writeSize = RoundUpToPage(marshalLen);

    memory.ResizeBuffer(writeSize);
    char *msgBuf = (char *) memory.GetBuffer();

    // read all of the remaining bytes

    ec = reader->Read(msgBuf+s_PageSize, writeSize - s_PageSize, &bytesRead);
    if (ec != NO_ERROR || bytesRead != writeSize - s_PageSize)
    {
        RSLInfo("Failed to read checkpoint header", LogTag_ErrorCode, ec);
        return false;
    }

    MarshalData marshal(&memory);
    marshal.SetMarshaledLength(marshalLen);
    return UnMarshal(&marshal);
}

void
CheckpointHeader::SetBytesIssued(RSLCheckpointStreamWriter * writer)
{
    m_size = writer->BytesIssued();
}

void
CheckpointHeader::GetCheckpointFileName(DynString &file, UInt64 decree)
{
    if (file.Length() > 0 && file[file.Length()-1] != '\\')
    {
        file.Append('\\');
    }
    file.AppendF("%I64u.codex", decree);
}

bool Legislator::s_ListenOnAllIPs = false;

void Legislator::EnableListenOnAllIPs()
{
    s_ListenOnAllIPs = true;
}

Legislator::Legislator( bool IndependentHeartbeat ) :
    m_spawnThread(NULL),
    m_checkpointSavedNotification(NULL),
    m_isInitializeCompleted(false),
    m_memberId(), m_stateMachine(NULL), m_self(NULL), m_netlibServer(NULL),
    m_netlibClient(NULL), m_lastPrepareMsg(NULL), m_state(PaxosInactive),
    m_electionDelay(0), m_nextElectionTime(0), m_prepareTimeout(0),
    m_actionTimeout(0), m_cpTimeout(0), m_lastTimeJoinMessagesSent(0),
    m_nextCheckpointTime(0), m_maxLogLen(0), m_isPrimary(false), m_IsTransitioningToPrimary(false),
    m_isShutting(false), m_fastReadCallbackQSize(0), m_minFastReadReq(0), m_numThreads(0),
    m_lastExecutedDecree(0),  m_copyCheckpointAtDecree(0), m_saveCheckpointAtDecree(0),
    m_checkpointedDecree(0), m_checkpointSize(0), m_checkpointAllowed(true),
    m_mustCheckpoint(false), m_isSavingCheckpoint(false), m_copyCheckpointThread(nullptr),
    m_ipPaused(MainThread_Run), m_exePaused(MainThread_Run), m_acceptPaused(MainThread_Run),
    m_frPaused(MainThread_Run), m_frPausedCount(0),
    m_paused(MainThread_Run), m_version(RSLProtocolVersion_1), m_hbPaused(MainThread_Run),
    m_serializeFastReadsWithReplicate(true), m_highestDefunctConfigurationNumber(0), m_IndependentHeartbeat( IndependentHeartbeat ),
    m_relinquishPrimary(false), m_forceReelection(false), m_pFetchSocket(StreamSocket::CreateStreamSocket()), m_votePayload(0),
    m_acceptMessages(true)
{
    m_primaryCookie = new PrimaryCookie();
    m_waitHandle = CreateEvent(NULL, false, true, NULL);
    m_hbWaitHandle = CreateEvent(NULL, false, true, NULL);
    m_fastReadWaitHandle = CreateEvent(NULL, false, true, NULL);
    m_resolveWaitHandle = CreateEvent(NULL, FALSE, FALSE, NULL);
}

RSLResponseCode
Legislator::ReplicateRequest(void* request, size_t len, void* cookie)
{
    LogAssert(request && len > 0);
    AutoCriticalSection lock(&m_lock);
    if (!m_isPrimary)
    {
        return RSLNotPrimary;
    }

    if (m_isShutting)
    {
        return RSLShuttingDown;
    }

    Vote *lastVote = m_votesToSend.Tail();
    if (lastVote == NULL ||
        lastVote->m_isReconfiguration ||
        lastVote->m_isExclusiveVote ||
        (lastVote->GetMarshalLen() + len + sizeof(UInt32)) > s_AvgMessageLen)
    {
        lastVote = new Vote(m_version, m_memberId, 0, 0, BallotNumber(), m_primaryCookie);
        LogAssert(lastVote);
        m_votesToSend.Enqueue(lastVote);
    }
    lastVote->AddRequest((char *) request, (UInt32) len, cookie);

    return RSLSuccess;
}

RSLResponseCode
Legislator::ReplicateRequestExclusiveVote(void* request, size_t len, void* cookie)
{
    return ReplicateRequestExclusiveVote(request, len, cookie, false);
}

RSLResponseCode
Legislator::ReplicateRequestExclusiveVote(void* request, size_t len, void* cookie, bool setShutdownAfter)
{
    LogAssert(request && len > 0);
    AutoCriticalSection lock(&m_lock);
    if (!m_isPrimary)
    {
        return RSLNotPrimary;
    }

    if (m_isShutting)
    {
        return RSLShuttingDown;
    }

    Vote *newVote = new Vote(m_version, m_memberId, 0, 0, BallotNumber(), m_primaryCookie);
    LogAssert(newVote);
    newVote->m_isExclusiveVote = true;
    m_votesToSend.Enqueue(newVote);
    newVote->AddRequest((char *)request, (UInt32)len, cookie);

    if (setShutdownAfter)
    {
        // Note we don't need to relinquish primariness here explicitly.
        // RelinquishPrimary();
        DeclareShutdown();
    }

    return RSLSuccess;
}

RSLResponseCode
Legislator::FastReadRequest(UINT64 maxSeenSeqNo, void* request, size_t len,
                            void* cookie, UInt32 timeout)
{
    LogAssert(request && len > 0);
    AutoCriticalSection lock(&m_lock);

    // if we are in the middle of making a checkpointing,
    // and we haven't executed this decree, fail the request

    if (m_isShutting)
    {
        return RSLShuttingDown;
    }

    Int64 diff = maxSeenSeqNo - GetCurrentSequenceNumber();
    if (diff > 0 && (diff > m_cfg.FastReadSeqThreshold() || timeout == 0 || m_isSavingCheckpoint == true))
    {
        return RSLFastReadStale;
    }

    void *reqCopy = malloc(len);
    LogAssert(reqCopy);
    memcpy(reqCopy, request, len);
    RequestCtx *ctx = new RequestCtx(maxSeenSeqNo, reqCopy, len, cookie);
    LogAssert(ctx);
    ctx->m_timeout = GetHiResTime() + HRTIME_MSECONDS(timeout);
    m_fastReadQ.enqueue(ctx);

    RequestCtx *c = m_fastReadTimeoutQ.tail;

    // insert the timeouts in the sorted order. The assumption is
    // that most users will specify the same timeout for every req.
    // So, for majority of the cases, ctx will be inserted at the
    // end of the m_Timeoutq. If it turns out that we have to do a lot
    // of work to keep this list sorted, then maybe we should split up
    // the list by timeouts.
    for (;c; c = c->m_timeoutLink.prev)
    {
        if (c->m_timeout <= ctx->m_timeout)
        {
            m_fastReadTimeoutQ.insert(ctx, ctx->m_timeoutLink, c);
            break;
        }
    }

    if (!c)
    {
        m_fastReadTimeoutQ.push(ctx, ctx->m_timeoutLink);
    }

    // otherwise, put this in a queue
    if (maxSeenSeqNo < m_minFastReadReq)
    {
        m_minFastReadReq = maxSeenSeqNo;
    }

    if (GetCurrentSequenceNumber() >= maxSeenSeqNo)
    {
        SetEvent(m_fastReadWaitHandle);
    }
    // otherwise the execute thread will signal us when
    // the minimum decree needed gets executed.
    return RSLSuccess;
}

RSLResponseCode
Legislator::ChangeConfiguration(MemberSet *configuration, void* cookie)
{
    if (configuration->Verify(m_version) == false)
    {
        return RSLInvalidParameter;
    }
    AutoCriticalSection lock(&m_lock);
    if (!m_isPrimary)
    {
        return RSLNotPrimary;
    }
    if (m_isShutting)
    {
        return RSLShuttingDown;
    }
    Ptr<MemberSet> memberSet(new MemberSet(*configuration));

    RSLInfo("Change configuration requested", LogTag_UInt1, memberSet->GetNumMembers());

    Vote *newVote = new Vote(m_version, memberSet, cookie, m_primaryCookie);
    LogAssert(newVote);
    m_votesToSend.Enqueue(newVote);
    return RSLSuccess;
}

void
Legislator::GetConfiguration(MemberSet *configuration, UInt32 *number)
{
    AutoCriticalSection lock(&m_lock);
    configuration->Copy(m_stateConfiguration->GetMemberSet());

    if (number != NULL)
    {
        *number = m_stateConfiguration->GetConfigurationNumber();
    }
}

RSLResponseCode
Legislator::CopyStateFromReplica(void *cookie)
{
    AutoCriticalSection lock(&m_lock);

    if (m_isShutting)
    {
        return RSLShuttingDown;
    }

    RSLInfo("CopyStateFromReplica called");

    RunThread(&Legislator::CopyStateThread, "CopyStateThread", cookie);
    return RSLSuccess;
}

void
Legislator::CopyStateThread(void *cookie)
{
    DWORD ec = NO_ERROR;
    ResponseMap replicasTried;
    UInt32 ip = 0;

    do
    {
        ip = 0;
        UInt16 port = 0;
        MemberId memberId;

        {
            CAutoPoolLockShared lock(&m_replicasLock);

            vector<Replica *> replicas;
            Replica *replica = NULL;

            // push all the replicas that we haven't tried yet into replicas vector

            for (ReplicaIter i = m_replicas.begin(); i != m_replicas.end(); i++)
            {
                replica = *i;
                memberId = MemberId(replica->m_node.m_memberIdString);
                if (replica->m_node.m_ip != 0 &&
                    replicasTried.find(memberId) == replicasTried.end())
                {
                    replicas.push_back(replica);
                }
            }

            // pick a random replica from replicas vector

            if (replicas.size() > 0)
            {
                int id = rand() % (int) replicas.size();
                replica = replicas[id];
                memberId = MemberId(replica->m_node.m_memberIdString);
                replicasTried[memberId] = true;
                ip = replica->m_node.m_ip;
                port = replica->m_node.m_rslLearnPort;
            }
        }

        // we don't have any more replicas to try.

        if (ip == 0)
        {
            CAutoPoolLockShared lock(&m_callbackLock);
            // notify the state machine
            m_stateMachine->StateCopied(0, NULL, cookie);
            return;
        }

        MarshalData marshal;

        std::unique_ptr<StreamSocket> pSock(StreamSocket::CreateStreamSocket());

        Message req(
            m_version,
            Message_StatusQuery,
            m_memberId,
            0, // dummy decree
            1, // dummy Configuration number
            BallotNumber());

        req.Marshal(&marshal);

        RSLInfo("Sending status request message", LogTag_RSLMemberId, memberId);

        ec = pSock->Connect(ip, port, m_cfg.ReceiveTimeout(), m_cfg.SendTimeout());
        if (ec != NO_ERROR)
        {
            RSLInfo("Failed to connect", LogTag_ErrorCode, ec);
            continue;
        }
        ec = pSock->Write(marshal.GetMarshaled(), marshal.GetMarshaledLength());
        if (ec != NO_ERROR)
        {
            RSLInfo("Failed to send request", LogTag_ErrorCode, ec);
            continue;
        }

        StatusResponse response;
        if (!response.ReadFromSocket(pSock.get(), m_cfg.MaxMessageSize()))
        {
            continue;
        }

        m_lock.Enter();
        UInt64 checkpointDecree = m_checkpointedDecree;
        m_lock.Leave();

        if (response.m_checkpointedDecree < checkpointDecree ||
            !CopyCheckpoint(ip, port, response.m_checkpointedDecree, response.m_checkpointSize, cookie))
        {
            continue;
        }

        RSLInfo("Copied checkpoint",
                LogTag_RSLDecree, response.m_checkpointedDecree,
                LogTag_RSLMemberId, memberId);

        ip = 0;

    } while (ip != 0);
}

void
Legislator::GetStatisticsSnapshot(RSLStats * pStats)
{
    // In future versions we can use m_cbSizeOfThisStruct as a versioning mechanism (as we add new statistics we can support older clients which don't know
    // about these fields, we can never remove fields however). But this is the first version of this API, so we expect the size to be exact. If and when we
    // move to a new version this assert will change to check that we're never handed anything smaller that the size of a V1 RSLStats structure.
    LogAssert(pStats->m_cbSizeOfThisStruct == sizeof(RSLStats));

    // Take the lock to ensure a consistent snapshot.
    AutoCriticalSection lock(&m_statsLock);

    // While the caller's and our RSLStats structures match we can copy most of the stats straight over.
    size_t cbCallersStats = pStats->m_cbSizeOfThisStruct;
    memcpy(pStats, &m_stats, cbCallersStats);
    pStats->m_cbSizeOfThisStruct = cbCallersStats;

    // Our m_cCaptureTimeInMicroseconds field actually holds the time stamp for the start of the capture period, so subtract the current time.
    LogAssert(HRTIME_USECOND == 1);
    pStats->m_cCaptureTimeInMicroseconds = GetHiResTime() - m_stats.m_cCaptureTimeInMicroseconds;

    // Likewise our m_cDecreesExecuted actually holds the current decree at the start of the capture period.
    pStats->m_cDecreesExecuted = GetCurrentSequenceNumber() - m_stats.m_cDecreesExecuted;

    // Reset statistics ready to start a new capture period.
    ResetStatistics();
}

void
Legislator::ResetStatistics()
{
    memset(&m_stats, 0, sizeof(m_stats));
    m_stats.m_cCaptureTimeInMicroseconds = GetHiResTime();
    m_stats.m_cDecreesExecuted = GetCurrentSequenceNumber();
}

void
Legislator::GetPaxosConfiguration(Ptr<MemberSet> &configuration, UInt32 *number)
{
    AutoCriticalSection lock(&m_lock);
    configuration = m_paxosConfiguration->GetMemberSet();
    if (number != NULL)
    {
        *number = m_paxosConfiguration->GetConfigurationNumber();
    }
}

UInt32
Legislator::GetHighestDefunctConfigurationNumber()
{
    AutoCriticalSection lock(&m_lock);
    return m_highestDefunctConfigurationNumber;
}

void
Legislator::SetNextElectionTime(UInt32 secondsFromNow)
{
    Int64 now = GetHiResTime();
    m_nextElectionTime = now + HRTIME_MSECONDS(secondsFromNow);
}

Message*
Legislator::UnMarshalMessage(IMarshalMemoryManager *memory)
{
    MarshalData marshal(memory);
    Message msgHdr;
    Message *msg = NULL;

    if (!msgHdr.Peek(&marshal))
    {
        RSLError("Failed to unmarshal message");
        return NULL;
    }
    switch (msgHdr.m_msgId)
    {
        case Message_Vote:
            msg = new Vote();
            break;

        case Message_PrepareAccepted:
            msg = new PrepareAccepted();
            break;

        case Message_StatusResponse:
            msg = new StatusResponse();
            break;

        case Message_Prepare:
            msg = new PrepareMsg();
            break;

        case Message_Join:
            msg = new JoinMessage();
            break;

        case Message_Bootstrap:
            msg = new BootstrapMsg();
            break;

        case Message_VoteAccepted:
        case Message_NotAccepted:
        case Message_StatusQuery:
        case Message_DefunctConfiguration:
        case Message_JoinRequest:
        case Message_ReconfigurationDecision:
            msg = new Message();
            break;

        default:
            RSLError("Unknown message id", LogTag_RSLMsg, &msgHdr);
            return NULL;
    }
    LogAssert(msg);
    if (!msg->UnMarshal(&marshal))
    {
        RSLError("Failed to unmarshal message",  LogTag_RSLMsg, &msgHdr);
        delete msg;
        return NULL;
    }
    return msg;
}

void
Legislator::ProcessReceive(Packet *pkt, bool asClient)
{
    // unmarshal the packet
    UInt32 ip = (asClient) ? pkt->GetServerIp() : pkt->GetClientIp();
    UInt16 port = (asClient) ? pkt->GetServerPort() : pkt->GetClientPort();

    Message *msg = UnMarshalMessage(&pkt->m_MemoryManager);
    delete pkt;
    if (msg == NULL)
    {
        RSLError("Failed to unmarshal Message", LogTag_NumericIP, ip, LogTag_Port, port);
        return;
    }

    if (msg->m_msgId != Message_Vote && msg->m_msgId != Message_VoteAccepted)
    {
        RSLInfo("Unmarshaled received message",
                LogTag_RSLMsg, msg, LogTag_NumericIP, ip, LogTag_Port, port);
    }
    else
    {
        RSLDebug("Unmarshaled received message",
                 LogTag_RSLMsg, msg, LogTag_NumericIP, ip, LogTag_Port, port);
    }

    msg->m_remoteIP = ip;
    msg->m_remotePort = port;
    msg->m_asClient = asClient;

    if (asClient)
    {
        // client messages can only be one of these
        // Some message (Join, JoinRequest can be sent as either client
        // or server message
        if (msg->m_msgId != Message_VoteAccepted &&
            msg->m_msgId != Message_PrepareAccepted &&
            msg->m_msgId != Message_StatusResponse &&
            msg->m_msgId != Message_NotAccepted &&
            msg->m_msgId != Message_DefunctConfiguration &&
            msg->m_msgId != Message_Join &&
            msg->m_msgId != Message_JoinRequest)
        {
            RSLError("Invalid client message", LogTag_RSLMsg, msg);
            delete msg;
            return;
        }
    }

    if (msg->m_msgId == Message_StatusQuery)
    {
        HandleStatusQueryMsg(msg, NULL);
        delete msg;
        return;
    }
    if (msg->m_msgId == Message_Vote)
    {
        Vote *vote = (Vote *) msg;

        vote->m_receivedAt.Set();

        AutoCriticalSection lock(&m_recvQLock);

        Vote *lastVote = m_voteCache.Tail();
        if (lastVote)
        {
            if (vote->IsLowerDecree(lastVote->m_decree, lastVote->m_ballot) ||
                vote->IsSameDecree(lastVote->m_decree, lastVote->m_ballot))
            {
                RSLInfo("Discarding Vote",
                        LogTag_RSLMsg, vote, LogTag_RSLMsg, lastVote);
                delete vote;
                return;
            }
        }
        // if the size > MaxCacheLength, clear the cache
        if (m_voteCache.SizeInBytes() > m_cfg.MaxCacheLength())
        {
            RSLInfo("Max cache size reached; clearing", LogTag_RSLMsg, vote);
            m_voteCache.Clear();
        }
        // this vote follows the last cached vote
        m_voteCache.Enqueue(vote);
    }
    else
    {
        AutoCriticalSection lock(&m_recvQLock);
        m_recvMsgQ.enqueue(msg);
    }
    SetEvent(m_waitHandle);

}

void
Legislator::ProcessConnect(UInt32 ip, UInt16 port, ConnectHandler::ConnectState state,
                           bool asClient)
{
    if (!asClient)
    {
        return;
    }
    CAutoPoolLockShared lock(&m_replicasLock);

    for (ReplicaIter i = m_replicas.begin(); i != m_replicas.end(); i++)
    {
        Replica *replica = *i;
        if (replica->m_node.m_ip == ip && replica->m_node.m_rslPort == port)
        {
            replica->m_connected = (state == ConnectHandler::Connected);
            if (replica->m_connected == false)
            {
                replica->m_needsResolve = true;
                SetEvent(m_resolveWaitHandle);
            }
            // don't break here. Multiple replicas might be sharing the same ip+port
        }
    }
}

bool
Legislator::ForDebuggingPurposesUpdateCheckpointFile(
    RSLProtocolVersion version,
    const char* fileName,
    const MemberSet *rslmemberset)
{

    if (version < RSLProtocolVersion_3)
    {
        RSLError("Can not change replica set for checkpoint version less than RSLProtocolVersion_3");
        return false;
    }

    if (!rslmemberset->Verify(version))
    {
        return false;
    }

    DynString tempFileName((char *) fileName);
    tempFileName.Append(".tmp");

    Ptr<MemberSet> memberset(new MemberSet(*rslmemberset));
    CheckpointHeader header;
    CheckpointHeader oldHeader;
    if (!header.UnMarshal(fileName) || !oldHeader.UnMarshal(fileName))
    {
        return false;
    }

    header.m_version = version;
    if (oldHeader.m_version <= RSLProtocolVersion_2)
    {
        header.m_stateConfiguration = new ConfigurationInfo(1, 0, memberset);
    }
    else
    {
        header.m_stateConfiguration->UpdateMemberSet(memberset);
    }


    {
        int ec;
        RSLCheckpointStreamReader reader;
        if ((ec = reader.Init((char*) fileName, &oldHeader)) != NO_ERROR)
        {
            RSLError("Failed to read checkpoint",
                     LogTag_Filename, (char*) fileName, LogTag_ErrorCode, ec);
            return false;
        }

        RSLCheckpointStreamWriter writer;
        ec = writer.Init(tempFileName, &header);
        if (ec != NO_ERROR)
        {
            return false;
        }

        unsigned long bytesRead;

        while (1)
        {
            BYTE * pb;
            ec = reader.GetDataPointer((void**) &pb, s_ChecksumBlockSize, &bytesRead);
            if (ec == ERROR_HANDLE_EOF)
            {
                break;
            }
            if (ec != NO_ERROR)
            {
                return false;
            }

            ec = writer.Write(pb, bytesRead);
            if (ec != NO_ERROR)
            {
                return false;
            }
        }

        // Write checkpoint header
        header.m_size = writer.BytesIssued();
        if (writer.Close() != NO_ERROR)
        {
            return false;
        }
        header.Marshal(tempFileName);
    }

    if (!MoveFileExA(tempFileName, fileName, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    {
        return false;
    }

    return true;
}

bool
Legislator::ForDebuggingPurposesUpdateLogFile(DynString& dir, UInt64 logDecree,
                                              std::map<UInt64, RSLNodeCollection*> &configs)
{
    DynString sourceFile, destinFile;
    {
        LogFile logSource, logDestin;
        if (logSource.Open(dir, logDecree) != NO_ERROR)
        {
            return false;
        }
        sourceFile = logSource.m_fileName;
        if (logDestin.Open(dir, logDecree + 10000000L) != NO_ERROR)
        {
            return false;
        }
        destinFile = logDestin.m_fileName;

        APSEQREAD *seqRead = new APSEQREAD();
        if (seqRead == NULL)
        {
            return false;
        }
        DWORD ec = seqRead->DoInit(logSource.m_fileName, 2, s_AvgMessageLen, true);
        if (ec != NO_ERROR)
        {
            RSLError("Failed to read log file", LogTag_Filename, logSource.m_fileName, LogTag_ErrorCode, ec);
            return false;
        }

        DiskStreamReader reader(seqRead);
        StandardMarshalMemoryManager memory(s_AvgMessageLen);
        while (1)
        {
            Message * msg = NULL;
            if (ReadNextMessage(&reader, &memory, &msg, true) == false)
            {
                return false;
            }
            if (msg == NULL)
            {
                break;
            }

            if (msg->m_msgId == Message_Vote)
            {
                Vote * vote = (Vote *) msg;
                if (vote->m_isReconfiguration)
                {
                    if (vote->m_membersInNewConfiguration == NULL)
                    {
                        return false;
                    }
                    RSLNodeCollection * rslNodes = configs[msg->m_decree];
                    if (rslNodes != NULL)
                    {
                        UInt32 cookieLen;
                        void *configCookie = vote->m_membersInNewConfiguration->GetConfigurationCookie(&cookieLen);
                        Ptr<MemberSet> memberset = new MemberSet(*rslNodes, configCookie, cookieLen);
                        Vote *newVote = new Vote((RSLProtocolVersion) vote->m_version, memberset, NULL, &vote->m_primaryCookie);
                        newVote->m_decree = vote->m_decree;
                        newVote->m_ballot = vote->m_ballot;
                        newVote->m_configurationNumber = vote->m_configurationNumber;
                        newVote->m_memberId = vote->m_memberId;
                        newVote->CalculateChecksum();
                        delete vote;
                        vote = newVote;
                        msg = newVote;
                    }
                }
                DynamicBuffer<SIZED_BUFFER, 1024> buffers(vote->GetNumBuffers());
                vote->GetBuffers(buffers, buffers.Size());
                if (logDestin.Write(buffers, vote->GetNumBuffers()) == false)
                {
                    return false;
                }
            }
            else
            {
                DWORD toWrite = RoundUpToPage(msg->GetMarshalLen());
                char *buf = (char *) VirtualAlloc(NULL, toWrite, MEM_COMMIT, PAGE_READWRITE);
                msg->MarshalBuf(buf, toWrite);
                msg->CalculateChecksum(buf, msg->GetMarshalLen());

                SIZED_BUFFER buffers[1];
                buffers[0].m_buf = buf;
                buffers[0].m_len = toWrite;

                if (logDestin.Write(buffers, 1) == false)
                {
                    return false;
                }
                VirtualFree(buf, 0, MEM_RELEASE);
            }
            logDestin.AddMessage(msg);
            delete msg;
        }
        delete seqRead;
    }
    if (!MoveFileExA(destinFile, sourceFile, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    {
        return false;
    }
    return true;
}

Replica *Legislator::GetReplica(MemberId &memberId, unsigned int remoteIp)
{
    CAutoPoolLockShared lock(&m_replicasLock);

    for (ReplicaIter i = m_replicas.begin(); i != m_replicas.end(); i++)
    {
        Replica* replica = *i;

        // we found the replica for the message
        if (memberId.Compare(replica->m_node.m_memberIdString) == 0 &&
            replica->m_node.m_ip == remoteIp)
        {
            return replica;
        }
    }

    return NULL;
}

bool
Legislator::VerifyMessage(Message *msg)
{
    UInt32 ip = msg->m_remoteIP;
    UInt16 port = msg->m_remotePort;

    if (msg->m_configurationNumber <= m_highestDefunctConfigurationNumber)
    {
        RSLDebug("Received message from defunct configuration",
                 LogTag_RSLMsg, msg,
                 LogTag_UInt1, m_highestDefunctConfigurationNumber);
        SendDefunctConfigurationMessage(ip, port, msg->m_asClient);
        return false;
    }

    if (msg->m_configurationNumber < PaxosConfiguration())
    {
        RSLDebug("Dropping RSL message from earlier configuration",
                 LogTag_RSLMsg, msg);
        return false;
    }
    if (msg->m_configurationNumber > PaxosConfiguration())
    {
        RSLInfo("Received message for later configuration",
                LogTag_RSLMsg, msg, LogTag_UInt1, PaxosConfiguration());
        SendJoinRequestMessage(msg->m_configurationNumber, ip, port, msg->m_asClient);
        return false;
    }
    if (m_state == PaxosInactive)
    {
        RSLInfo("Ignoring message while in Paxos-inactive state", LogTag_RSLMsg, msg);
        return false;
    }

    {
        msg->m_replica = GetReplica(msg->m_memberId, msg->m_remoteIP);

        // we got a message from this server. Mark this server
        // as healthy
        if (msg->m_replica)
        {
            if (msg->m_replica->m_status == 0)
            {
                RSLInfo("Marking replica healthy",
                        LogTag_RSLMemberId, msg->m_replica->m_node.m_memberIdString);
                msg->m_replica->m_status = 1;
            }
        }
    }

    if (msg->m_memberId == m_memberId)
    {
        RSLInfo("Ignoring message from same member id",
                LogTag_RSLMsg, msg, LogTag_RSLMemberId, msg->m_memberId.GetValue(),
                LogTag_NumericIP, ip, LogTag_Port, port);
        return false;
    }

    if (!msg->m_replica)
    {
        RSLInfo("Ignoring message from unknown member",
                LogTag_RSLMsg, msg, LogTag_RSLMemberId, msg->m_memberId.GetValue(),
                LogTag_NumericIP, ip, LogTag_Port, port);

        m_netlibServer->CloseConnection(ip, port);
        return false;
    }

    if (msg->m_ballot > m_maxSeenBallot)
    {
        m_maxSeenBallot = msg->m_ballot;
    }
    return true;
}

void
Legislator::HandleMessage(Message *msg)
{
    if (msg->m_msgId == Message_Bootstrap)
    {
        RSLInfo("Received Bootstrap message", LogTag_RSLMsg, msg);
        HandleBootstrapMessage((BootstrapMsg *) msg);
        return;
    }

    if (msg->m_msgId == Message_DefunctConfiguration)
    {
        RSLDebug("Received DefunctConfiguration message", LogTag_RSLMsg, msg);
        UpdateDefunctInfo(msg->m_configurationNumber);
        return;
    }

    if (msg->m_configurationNumber <= m_highestDefunctConfigurationNumber)
    {
        RSLDebug("Received message from defunct configuration",
                 LogTag_RSLMsg, msg,
                 LogTag_UInt1, m_highestDefunctConfigurationNumber);
        SendDefunctConfigurationMessage(msg->m_remoteIP, msg->m_remotePort, msg->m_asClient);
        return;
    }

    if (msg->m_msgId == Message_Join)
    {
        HandleJoinMsg((JoinMessage *) msg);
    }
    else if (msg->m_msgId == Message_JoinRequest)
    {
        HandleJoinRequestMsg(msg);
    }
    else
    {
        if (!VerifyMessage(msg))
        {
            return;
        }

        if (msg->m_msgId == Message_VoteAccepted)
        {
            HandleVoteAcceptedMsg(msg);
        }
        else if (msg->m_msgId == Message_Prepare)
        {
            HandlePrepareMsg((PrepareMsg *) msg);
        }
        else if (msg->m_msgId == Message_PrepareAccepted)
        {
            HandlePrepareAcceptedMsg((PrepareAccepted *) msg);
        }
        else if (msg->m_msgId == Message_NotAccepted)
        {
            HandleNotAcceptedMsg(msg);
        }
        else if (msg->m_msgId == Message_StatusResponse)
        {
            HandleStatusResponseMsg((StatusResponse *) msg);
        }
        else if (msg->m_msgId == Message_ReconfigurationDecision)
        {
            HandleReconfigurationDecisionMsg(msg);
        }
        else
        {
            RSLError("Invalid message (Unknown message id)", LogTag_RSLMsg, msg);
        }
    }
}

void
Legislator::HeartBeatLoop()
{
    for (;;)
    {
        while (m_hbPaused != MainThread_Run)
        {
            if (m_hbPaused == MainThread_Pause)
            {
                m_hbPaused = MainThread_Paused;
            }
            if (m_hbPaused == MainThread_Terminate)
            {
                m_hbPaused = MainThread_Terminated;
                return;
            }
            Sleep(10);
        }

        // while primary, we will watch for votes being sent within the HeartBeatInterval
        // if no vote was sent in that time, we will create an empty vote and we will send it
        // we only start sending HB after the MainLoop can start seonding Votes.
        if (AmPrimary() && m_isPrimary == true && m_isShutting == false)
        {
            if (m_outstandingVote == NULL)
            {
                Int64 diff = GetHiResTime();

                m_lock.Enter();
                diff -= m_maxAcceptedVote->m_receivedAt.HiResTime();
                m_lock.Leave();

                if (diff > m_cfg.HeartBeatInterval())
                {
                    // We have to be careful to not send the HB in race condition with MainLoop.
                    // to that end, we use m_hbLock to get mutual exclusion
                    // Also, we don't want to acquire the lock unless we think we will send the HB
                    // hence the "if - lock - if" sequence
                    AutoCriticalSection lock(&m_hbLock);

                    if (m_outstandingVote == NULL)
                    {
                        diff = GetHiResTime();
                        bool isMyBallot = false;

                        m_lock.Enter();
                        diff -= m_maxAcceptedVote->m_receivedAt.HiResTime();
                        isMyBallot = (m_maxBallot.m_memberId == m_memberId);
                        m_lock.Leave();

                        if (diff > m_cfg.HeartBeatInterval() && AmPrimary() && m_isPrimary == true && isMyBallot == true)
                        {
                            Ptr<Vote> vote;

                            vote = new Vote(m_version, m_memberId, 0, 0, BallotNumber(), m_primaryCookie);
                            LogAssert(vote);

                            //
                            //  The heartbeat can work in two ways.  The first is completely independent of the main
                            //  thread.  In this mode, this thread will continue to send heartbeat votes, even if
                            //  the Legislator is hung.  The second mode involves the main thread of the Legislator
                            //  in the heartbeat process.  As such, a hang of the Legislator will prevent the heartbeat
                            //  from being sent.  If we are in independent heartbeat mode, we send the vote directly.
                            //  If we are including the main thread as part of the heartbeat, we queue the vote
                            //  to be sent by the main thread.
                            //

                            if (m_IndependentHeartbeat)
                            {
                                SendNextVote(vote);
                            }
                            else
                            {
                                m_lock.Enter();
                                m_votesToSend.Enqueue( vote );
                                m_lock.Leave();
                            }
                        }
                    }
                }
            }
        }

        // the wait time will be one third of the HeartBeatInterval time.
        // HeartBeatInterval() returns HRTIME, and we need to transform it to milliseconds.
        int msecs = (int)(m_cfg.HeartBeatInterval()/HRTIME_MSECONDS(1)/3);

        // now wait on the event.
        DWORD ret = WaitForSingleObject(m_hbWaitHandle, msecs);
        LogAssert(ret != WAIT_FAILED);
    }
}

void
Legislator::MainLoop()
{
    for (;;)
    {
        while (m_paused != MainThread_Run)
        {
            if (m_paused == MainThread_Pause)
            {
                m_paused = MainThread_Paused;
            }
            if (m_paused == MainThread_Terminate)
            {
                m_paused = MainThread_Terminated;
                return;
            }
            Sleep(10);
        }

        if (m_state == PaxosInactive)
        {
            CheckBootstrap();
        }

        // first play all the votes
        if (m_state != Initializing)
        {
            VoteQueue localQueue;
            {
                AutoCriticalSection lock(&m_recvQLock);
                localQueue.Append(m_voteCache);
            }
            HandleNewVotes(localQueue);
        }

        // now go through all the messages in the m_recvMsgQ
        Queue<Message> msgs;
        {
            AutoCriticalSection lock(&m_recvQLock);
            msgs.append(m_recvMsgQ);
            m_recvMsgQ.clear();
        }

        // First, note any defunct-configuration notifications we've received.
        Message *msg = NULL;
        while ((msg = msgs.dequeue()) != NULL)
        {
            HandleMessage(msg);
            delete msg;
        }

        if (AmPrimary())
        {
            // we are the primary.
            // check if we have to either send a vote from the queue
            // Be conservative about calling NotifyStatus when we become
            // the primary. Call it only when we've proposed m_oldFreshestVote
            // and it has been accepted by the majority.
            if (m_outstandingVote == NULL)
            {
                // We have to be careful to not send the vote in race condition with HeartBeatLoop.
                // to that end, we use m_hbLock to get mutual exclusion
                AutoCriticalSection lock(&m_hbLock);

                if (m_outstandingVote == NULL)
                {
                    CheckNotifyStatus(true);

                    // try dequeue a request from the vote queue
                    Ptr<Vote> vote;

                    m_lock.Enter();
                    m_votesToSend.Dequeue(&vote);
                    m_lock.Leave();

                    if (vote != NULL)
                    {
                        SendNextVote(vote);
                    }
                }
            }
        }
        else
        {
            CheckNotifyStatus(false);
        }

        Timer();

        SendJoinMessagesIfNecessary();

        // now wait on the event
        DWORD ret = WaitForSingleObject(m_waitHandle, 10);
        LogAssert(ret != WAIT_FAILED);
    }
}

void
Legislator::Timer()
{
    Int64 now = GetHiResTime();

    if (now >= m_nextElectionTime || m_forceReelection)
    {
        if (m_state == StableSecondary)
        {
            // before starting to learn, check if there are
            // new votes to be processed. The mainLoop can sometimes
            // take a long time to complete (more than m_nextElectionTime).
            bool newVotes;
            {
                AutoCriticalSection lock(&m_recvQLock);
                newVotes = (m_voteCache.Head()) ? true : false;
            }
            if (newVotes == false)
            {
                RSLInfo("StartInitializing");
                StartInitializing();
            }
        }
    }
    if (now > m_cpTimeout)
    {
        SendStatusRequestMessage();
    }

    if (now > m_actionTimeout)
    {
        if (AmPrimary())
        {
            ReSendCurrentVote();
        }
        else if (m_state == Preparing)
        {
            if (now > m_prepareTimeout)
            {
                StartInitializing();
            }
            else
            {
                ReSendPrepareMsg();
            }
        }
        else if (m_state == Initializing)
        {
            RSLInfo("StartInitializing");
            StartInitializing();
        }
    }
}

void
Legislator::CheckNotifyStatus(bool curStatus)
{
    if (curStatus != m_isPrimary)
    {
        const char* newStatus = ((curStatus) ? "primary" : "secondary");
        RSLInfo("New status", LogTag_String1, newStatus);

        m_lock.Enter();
        m_isPrimary = curStatus;
        m_lock.Leave();

        if (m_isPrimary == false)
        {
            AbortReplicateRequests(RSLNotPrimary);
        }
        else
        {
            CAutoPoolLockExclusive lock(&m_callbackLock);
            // notify the state machihe
            m_stateMachine->NotifyStatus(m_isPrimary);
            m_callbackAfterVote = m_maxAcceptedVote;
        }

        RSLInfo("Notified machine of new status");
    }

    //
    //  If we were transitioning to primary, it is now done.
    //

    if (m_IsTransitioningToPrimary)
    {
        m_IsTransitioningToPrimary = false;
    }
}

void
Legislator::FastReadLoop()
{
    m_shutdownLock.LockShared();

    m_lock.Enter();
    m_frPausedCount++;
    m_lock.Leave();

    for (;;)
    {
        while (m_frPaused != MainThread_Run)
        {
            if (m_frPaused == MainThread_Terminate)
            {
                m_lock.Enter();
                m_frPausedCount--;
                if (m_frPausedCount == 0)
                {
                    m_frPaused = MainThread_Terminated;
                }
                m_lock.Leave();
                return;
            }
            Sleep(10);
        }

        m_lock.Enter();

        HiResTime now = GetHiResTime();
        UInt64 curSeq = GetCurrentSequenceNumber();
        // if I'm not the primary and I'm checkpointing right now,
        // fail the requests that have not been executed
        bool saving = !AmPrimary() && m_isSavingCheckpoint;

        RequestCtx *ctx;

        // we want to avoid going through the whole queue.
        // If there is a decree in the queue that has been
        // executed or if we have to abort decrees because
        // we ar checkpointing, then we have to go through
        // the queue
        if (curSeq >= m_minFastReadReq || saving)
        {
            UInt64 minSeen = _I64_MAX;
            for (ctx = m_fastReadQ.head; ctx != NULL;)
            {
                RequestCtx *next = ctx->link.next;
                if (curSeq >= ctx->m_decree || (curSeq < ctx->m_decree && saving) ||
                    now >= ctx->m_timeout)
                {
                    m_fastReadQ.remove(ctx);
                    m_fastReadTimeoutQ.remove(ctx, ctx->m_timeoutLink);
                    m_fastReadCallbackQLock.Enter();
                    m_fastReadCallbackQ.enqueue(ctx);
                    m_fastReadCallbackQSize++;
                    m_fastReadCallbackQLock.Leave();
                }
                else
                {
                    minSeen = min(ctx->m_decree, minSeen);
                }
                ctx = next;
            }
            m_minFastReadReq = minSeen;
        }
        else
        {
            while ((ctx = m_fastReadTimeoutQ.head) != NULL && now >= ctx->m_timeout)
            {
                m_fastReadQ.remove(ctx);
                m_fastReadTimeoutQ.remove(ctx, ctx->m_timeoutLink);
                m_fastReadCallbackQLock.Enter();
                m_fastReadCallbackQ.enqueue(ctx);
                m_fastReadCallbackQSize++;
                m_fastReadCallbackQLock.Leave();
            }
        }

        m_lock.Leave();

        m_fastReadCallbackQLock.Enter();
        UInt32 size = m_fastReadCallbackQSize;
        m_fastReadCallbackQLock.Leave();

        if (size == 0)
        {
            m_shutdownLock.UnlockShared();
            DWORD ret = WaitForSingleObject(m_fastReadWaitHandle, 100);
            LogAssert(ret != WAIT_FAILED);
            m_shutdownLock.LockShared();
            continue;
        }

        // if we have more than a couple of items in the queue, wake up
        // any other thread which might be sleeping
        if (size > 2)
        {
            SetEvent(m_fastReadWaitHandle);
        }

        for (;;)
        {
            m_fastReadCallbackQLock.Enter();
            ctx = m_fastReadCallbackQ.dequeue();
            if (ctx == NULL)
            {
                m_fastReadCallbackQLock.Leave();
                break;
            }
            m_fastReadCallbackQSize--;
            m_fastReadCallbackQLock.Leave();

            if (m_serializeFastReadsWithReplicate)
            {
                m_callbackLock.LockShared();
            }
            UInt64 curSeq2 = GetCurrentSequenceNumber(); // check against current seqNo
            if (curSeq2 >= ctx->m_decree)
            {
                m_stateMachine->ExecuteFastReadRequest(ctx->m_requestBuf,
                                                       ctx->m_bufLen,
                                                       ctx->m_cookie);
            }
            else
            {
                m_stateMachine->AbortRequest(RSLFastReadStale, ctx->m_cookie);
            }
            if (m_serializeFastReadsWithReplicate)
            {
                m_callbackLock.UnlockShared();
            }

            free(ctx->m_requestBuf);
            delete ctx;
        }
    }
}

void
Legislator::ExecuteLoop()
{
    // dequeue votes from the m_executeQ and execute them
    m_shutdownLock.LockShared();
    Int64 lastLogged = 0;
    for (;;)
    {
        while (m_exePaused != MainThread_Run)
        {
            if (m_exePaused == MainThread_Terminate)
            {
                m_exePaused = MainThread_Terminated;
                return;
            }
            Sleep(10);
        }

        m_callbackLock.LockExclusive();

        // check if we are supposed to callback the state machine to
        // notify that it has fully recovered.
        if (m_callbackAfterVote && m_lastExecutedDecree >= m_callbackAfterVote->m_decree)
        {
            // if the state machine is not the primary any more, don't call it.
            if (m_isPrimary)
            {
                m_stateMachine->NotifyPrimaryRecovered();
            }
            m_callbackAfterVote = NULL;
        }

        // check if we need to process a notification about the state being copied.
        // if so, notify the application, and reset the notification object
        if (m_checkpointSavedNotification != NULL)
        {
            AutoCriticalSection lock(&m_checkpointSavedNotificationLock);

            CheckpointSavedNotification *notif = m_checkpointSavedNotification;

            if (notif != NULL)
            {
                m_checkpointSavedNotification = NULL;

                // notify the state machine
                m_stateMachine->StateCopied(notif->m_checkpointedDecree, notif->m_destFileName, notif->m_cookie);
                delete notif;
            }
        }

        UInt64 saveCheckpointAt = GetSaveCheckpointAt();

        // this is set to true if the checkpoint is waiting for the next vote
        bool waitingForVote = false;

        if (saveCheckpointAt != 0 && m_checkpointAllowed)
        {

            if (m_lastExecutedDecree >= saveCheckpointAt)
            {
                Ptr<Vote> nextVote;
                m_lock.Enter();
                // write the next vote after last executed.
                // wait until the next vote gets added to execution q
                m_executeQ.Head(&nextVote);
                if (nextVote == NULL)
                {
                    nextVote = m_maxAcceptedVote;
                }
                if (nextVote->m_decree == m_lastExecutedDecree+1)
                {
                    if (GetSaveCheckpointAt() != 0)
                    {
                        m_isSavingCheckpoint = 1;
                    }
                }
                else
                {
                    waitingForVote = true;
                }
                m_lock.Leave();
                if (m_isSavingCheckpoint)
                {
                    m_callbackLock.Downgrade();

                    bool success = SaveCheckpoint(nextVote);

                    // release the shared lock and take the exclusive lock again
                    m_callbackLock.UnlockShared();
                    m_callbackLock.LockExclusive();
                    {
                        AutoCriticalSection lock(&m_lock);
                        m_isSavingCheckpoint = 0;
                        if (success)
                        {
                            SetSaveCheckpointAt(0);
                        }
                        else
                        {
                            m_checkpointAllowed = false;
                        }
                    }
                }
                else if (GetHiResTime() - lastLogged >= HRTIME_SECONDS(1))
                {
                    RSLInfo("Checkpoint waiting for log",
                            LogTag_RSLDecree, nextVote->m_decree,
                            LogTag_RSLDecree, m_lastExecutedDecree);
                    lastLogged = GetHiResTime();
                }
            }
            else if (GetHiResTime() - lastLogged >= HRTIME_SECONDS(1))
            {
                RSLInfo("Checkpoint waiting for execution",
                        LogTag_RSLDecree, m_lastExecutedDecree,
                        LogTag_RSLDecree, saveCheckpointAt);

                lastLogged = GetHiResTime();
            }
        }

        //
        //  If this node is currently transitioning to be primary, we do not
        //  want to execute any decrees yet.  The reason for this is that the
        //  transition requires the callback lock in order to notify the state
        //  machine.  The time required to execute a decree is unbound.  As
        //  such, executing a decree could delay the transition indefintely
        //  and cause problems with the system.  So before pulling a vote to
        //  execute, we check for the transitioning state and skip execution
        //  until the transition completes.
        //

        Ptr<Vote> vote;
        if (!waitingForVote && !m_IsTransitioningToPrimary)
        {
            m_executeQ.Head(&vote);
        }

        if (vote == NULL)
        {
            m_callbackLock.UnlockExclusive();
            m_shutdownLock.UnlockShared();
            m_executeQ.WaitExecuteThread(100);
            m_shutdownLock.LockShared();
            continue;
        }

        bool shouldSave = false;
        ExecuteVote(vote, &shouldSave);

        {
            AutoCriticalSection lock(&m_lock);
            m_executeQ.Dequeue(&vote);

            if (shouldSave)
            {
                // this must be done under a lock since the main thread might
                // check and set this value
                SetSaveCheckpointAt(m_lastExecutedDecree);
                m_mustCheckpoint = true;
            }
        }

        m_callbackLock.UnlockExclusive();

    }
}

void
Legislator::IPLookup()
{
    for (;;)
    {
        while (m_ipPaused != MainThread_Run)
        {
            if (m_ipPaused  == MainThread_Terminate)
            {
                m_ipPaused  = MainThread_Terminated;
                return;
            }
            Sleep(10);
        }

        vector<Replica *> toResolve;
        m_replicasLock.LockShared();

        // lookup every 30 seconds
        for (ReplicaIter i = m_replicas.begin(); i != m_replicas.end(); i++)
        {
            Replica *replica = *i;
            if (replica->m_node.m_ip == 0 || replica->m_needsResolve)
            {
                toResolve.push_back(new Replica(replica->m_node));
                replica->m_needsResolve = true;
            }

        }
        m_replicasLock.UnlockShared();

        if (toResolve.size() > 0)
        {
            for (ReplicaIter i = toResolve.begin(); i != toResolve.end(); i++)
            {
                Replica *replica = *i;
                if (m_stateMachine->ResolveNode(&replica->m_node))
                {
                    replica->m_needsResolve = false;
                    replica->m_status = 1;
                }
            }

            CAutoPoolLockExclusive lock(&m_replicasLock);

            for (ReplicaIter i = m_replicas.begin(); i != m_replicas.end(); i++)
            {
                Replica *replica = *i;
                if (replica->m_needsResolve == true)
                {
                    for (ReplicaIter j = toResolve.begin(); j != toResolve.end(); j++)
                    {
                        Replica *newReplica = *j;
                        if (newReplica->m_needsResolve == false &&
                            MemberId::Compare(newReplica->m_node.m_memberIdString, replica->m_node.m_memberIdString) == 0)
                        {
                            if (newReplica->m_node.m_ip != replica->m_node.m_ip ||
                                newReplica->m_node.m_rslPort != replica->m_node.m_rslPort ||
                                newReplica->m_node.m_rslLearnPort != replica->m_node.m_rslLearnPort)
                            {
                                replica->DownCount();
                                newReplica->UpCount();
                                *i = newReplica;
                            }
                        }
                    }
                }
            }
        }

        WaitForSingleObject(this->m_resolveWaitHandle, 30*1000);
    }
}


// m_callbackLock/m_shutdownLock *must not* already be held by the caller
// should only be called from the main thread
void
Legislator::AbortReplicateRequests(RSLResponseCode status)
{
    RequestCtx *ctx;
    Ptr<Vote> vote;

    // wait until all the requests in the m_executeq get executed.
    for (;;)
    {
        m_executeQ.Head(&vote);
        if (vote == NULL)
        {
            break;
        }

        Sleep(10);
    }

    CAutoPoolLockExclusive lock(&m_callbackLock);
    if (m_outstandingVote != NULL)
    {
        // if there is a vote outstanding, abort that request
        for (ctx = m_outstandingVote->m_requests.head; ctx; ctx = ctx->link.next)
        {
            LogAssert(ctx->m_cookie);
            m_stateMachine->AbortRequest(status, ctx->m_cookie);
        }
        if (m_outstandingVote->m_isReconfiguration)
        {
            m_stateMachine->AbortChangeConfiguration(
                status,
                m_outstandingVote->m_reconfigurationCookie);
        }

        m_outstandingVote = NULL;
    }

    // abort all oustanding requests
    while (m_votesToSend.Dequeue(&vote) != NULL)
    {
        for (ctx = vote->m_requests.head; ctx; ctx = ctx->link.next)
        {
            m_stateMachine->AbortRequest(status, ctx->m_cookie);
        }
        if (vote->m_isReconfiguration)
        {
            m_stateMachine->AbortChangeConfiguration(
                status,
                vote->m_reconfigurationCookie);
        }
    }
    if (status == RSLNotPrimary)
    {
        m_stateMachine->NotifyStatus(m_isPrimary);
    }

}

void
Legislator::ExecuteVote(Vote *vote, bool *shouldSave)
{
    LogAssert(vote->m_decree == m_lastExecutedDecree ||
              vote->m_decree == m_lastExecutedDecree+1);

    if (vote->m_decree == m_lastExecutedDecree+1)
    {
        RSLDebug("Executing vote", LogTag_RSLMsg, vote, LogTag_UInt1, vote->m_numRequests);
        Interlocked::Increment64((volatile Int64 *) &m_lastExecutedDecree);

        HiResTime startTime = GetHiResTime();
        // execute all the decrees in the vote
        if (vote->m_requests.head != NULL)
        {
            RequestCtx *ctx;


            m_stateMachine->BeforeExecuteReplicatedRequests();

            for (ctx = vote->m_requests.head; ctx; ctx = ctx->link.next)
            {
                bool saveState = false;
                m_stateMachine->ExecuteReplicatedRequest(ctx->m_requestBuf,
                                                         ctx->m_bufLen,
                                                         ctx->m_cookie,
                                                         &saveState);
                *shouldSave |= saveState;
            }

            m_stateMachine->AfterExecuteReplicatedRequests();
        }
        // if this is a reconfiguration decree, change the configuration info in the state
        if (vote->m_isReconfiguration)
        {
            {
                AutoCriticalSection lock(&m_lock);
                m_stateConfiguration = new ConfigurationInfo(m_stateConfiguration->GetConfigurationNumber() + 1,
                                                             vote->m_decree + 1,
                                                             vote->m_membersInNewConfiguration);
            }
            m_stateMachine->NotifyConfigurationChanged(vote->m_reconfigurationCookie);
        }


        HiResTime delay = (GetHiResTime() - startTime)/HRTIME_MSECONDS(1);
        RSLInfo("vote executed (milliseconds)", LogTag_RSLMsg, vote, LogTag_Int64_1, delay);

        if (m_lastExecutedDecree >= m_minFastReadReq)
        {
            SetEvent(m_fastReadWaitHandle);
        }
    }
}

void
Legislator::HandleNewVotes(VoteQueue &localQueue)
{
    if (!localQueue.Head())
    {
        return;
    }

    if(m_relinquishPrimary)
    {
        RSLDebug("Skip handling new votes since I am reqlinquishing the primary.",
                LogTag_RSLMsg, localQueue.Head(), LogTag_RSLState, m_state,
                LogTag_RSLMsg, (Vote *)m_maxAcceptedVote, LogTag_RSLBallot, &m_maxBallot);
        return;
    }
    // got the votes
    RSLDebug("Handling new votes",
             LogTag_RSLMsg, localQueue.Head(), LogTag_RSLState, m_state,
             LogTag_RSLMsg, (Vote *)m_maxAcceptedVote, LogTag_RSLBallot, &m_maxBallot);

    Ptr<Vote> vote;
    bool acceptedVote = false;
    while ((vote = localQueue.Dequeue(&vote)) != NULL)
    {
        // Verify configuration info (handle defunct and new configuration)
        if (!VerifyMessage(vote))
        {
            continue;
        }

        // Verify if vote is older than the maxAcceptedVote. I can be when:
        // A) Either because the decree has already been accepted
        //   (vote->m_decree < m_maxAcceptedVote->m_decree)
        // B) Or new primary has successfully passed a decree
        //   (vote->m_ballot < m_maxAcceptedVote->m_ballot)
        if (vote->IsLowerDecree(m_maxAcceptedVote->m_decree, m_maxAcceptedVote->m_ballot))
        {
            RSLInfo("Discarding Vote",
                    LogTag_RSLMsg, (Vote *) vote, LogTag_RSLState, m_state,
                    LogTag_RSLMsg, (Vote *)m_maxAcceptedVote,
                    LogTag_RSLBallot, &m_maxBallot);

            // Verify if a notification should be sent. It can be when:
            // A) Another replica has already sent a prepare (vote->m_ballot < m_maxBallot)
            // B) Or another primary has successfully passed a greater decree
            //   (vote->m_ballot < m_maxAcceptedVote->m_ballot)
            // That breaks the loop
            if (vote->m_ballot < m_maxBallot || vote->m_decree+1 < m_maxAcceptedVote->m_decree)
            {
                Message msg(m_version,
                            Message_NotAccepted,
                            m_memberId,
                            m_maxAcceptedVote->m_decree,
                            PaxosConfiguration(),
                            m_maxBallot);

                SendResponse(vote->m_remoteIP, vote->m_remotePort, &msg);
            }
        }
        else if (vote->IsNextDecree(m_maxAcceptedVote->m_decree, m_maxAcceptedVote->m_ballot))
        {
			if (AmPrimary() && vote->m_ballot > m_maxBallot)
			{
				RSLInfo("Message/Vote inversion, switching to StableSecondary",
					LogTag_RSLMsg, (Vote *)vote,
					LogTag_RSLState, m_state,
					LogTag_RSLMsg, (Vote *)m_maxAcceptedVote,
					LogTag_RSLBallot, &m_maxBallot);

				StartBeingStableSecondary();
				CheckNotifyStatus(false);
			}

            RSLDebug("Accepting Vote", LogTag_RSLMsg, (Vote*) vote);
            LogAndAcceptVote(vote);
            acceptedVote = true;
            Ptr<Vote> nextVote = localQueue.Head();

            if (!nextVote ||
                nextVote->m_remoteIP != vote->m_remoteIP ||
                nextVote->m_remotePort != vote->m_remotePort)
            {
                UInt16 msgId = Message_VoteAccepted;
                bool sendResponse = true;

                // we can't accept a vote if the ballotId of the vote is less than maxBallot,
                // even though the vote has a higher decree than m_maxAcceptedVote->m_decree
                if (vote->m_ballot < m_maxBallot)
                {
                    msgId = Message_NotAccepted;
                }
                else
                {
                    // GlobalAcceptMessagesFlag feature is opt-in feature to avoid querying state machine
                    // for every vote as calling state machine here can cause MainLoop to get blocked if 
                    // state machine is managed implementation and performing garbage collection
                    if (m_cfg.UseGlobalAcceptMessagesFlag())
                    {
                        sendResponse = this->m_acceptMessages;
                    }
                    else
                    {
                        sendResponse = m_stateMachine->AcceptMessageFromReplica(
                            &m_maxAcceptedVote->m_replica->m_node,
                            m_maxAcceptedVote->m_primaryCookie.m_data,
                            m_maxAcceptedVote->m_primaryCookie.m_len);
                    } 
                }

                if (sendResponse)
                {
                    Message msg(m_version,
                                msgId,
                                m_memberId,
                                m_maxAcceptedVote->m_decree,
                                PaxosConfiguration(),
                                m_maxBallot,
                                m_votePayload);

                    SendResponse(vote->m_remoteIP, vote->m_remotePort, &msg);
                    if(vote->GetRelinquishPrimary())
                    {
                        RSLInfo("The primary is giving up the leadership. The re-election should happen immediately", LogTag_RSLMsg, (Vote *) vote,
                                LogTag_RSLState, m_state, LogTag_RSLMsg, (Vote *)m_maxAcceptedVote);
                        // We need clean up the local queue since it will be destructed and asserted if not empty.
                        while ((vote = localQueue.Dequeue(&vote)) != NULL)
                        {
                            RSLError("Race detected: new primary must be elected at the same time receiving relinquishPrimary message.",
                                LogTag_RSLMsg, (Vote *) vote,
                                LogTag_RSLState, m_state,
                                LogTag_RSLMsg, (Vote *)m_maxAcceptedVote);
                        }
                        m_forceReelection = true;
                        return;
                    }
                }
                else
                {
                    RSLInfo("State machine decided not to vote, not sending the response",
                        LogTag_RSLMsg, (Vote *) vote, LogTag_RSLState, m_state,
                        LogTag_RSLMsg, (Vote *)m_maxAcceptedVote);
                }
            }
        }
        else
        {
            // we have fallen behind
            RSLInfo("Missed votes; learning", LogTag_RSLMsg, (Vote *) vote,
                    LogTag_RSLState, m_state, LogTag_RSLMsg, (Vote *)m_maxAcceptedVote);
            localQueue.Clear();
            StartInitializing();
            return;
        }
    }

    if (acceptedVote)
    {
        StartBeingStableSecondary();
    }
}

void
Legislator::AttemptPromotion()
{
    RSLInfo("The application indicated to attempt leadership acquisition. The re-election attempt will happen immediately",
            LogTag_RSLState, m_state);

    m_forceReelection = true;
}

void
Legislator::HandleBootstrapMessage(BootstrapMsg * msg)
{
    StartBootstrap(msg->m_memberSet);
}

void
Legislator::HandleReconfigurationDecisionMsg(Message *decision)
{
    LogReconfigurationDecision(decision);
}

void
Legislator::HandleJoinMsg(JoinMessage *msg)
{
    if (msg->m_configurationNumber <= PaxosConfiguration())
    {
        if (msg->m_configurationNumber <= m_highestDefunctConfigurationNumber+1)
        {
            RSLDebug("Received join message for established configuration",
                     LogTag_RSLMsg, msg,
                     LogTag_UInt1, m_highestDefunctConfigurationNumber);
            SendDefunctConfigurationMessage(msg->m_remoteIP, msg->m_remotePort, msg->m_asClient);
        }
        return;
    }

    // When PaxosConfiguration is 0, it means replica is not bootstrapped yet.
    // In such situtation, replica cannot learn votes. This is because only
    // bootstrapping code switches configuration number from 0 to 1, so the
    // configuration number check will fail during learning.
    // Copying the checkpoint will make the replica boostrapped and will make
    // configuration number the current one being in use by the cluster.
    if (m_maxAcceptedVote->m_decree >= msg->m_minDecreeInLog && PaxosConfiguration() > 0)
    {
        RSLInfo("Learning votes in response to join message",
                 LogTag_RSLMsg, msg,
                 LogTag_RSLDecree, m_maxAcceptedVote->m_decree,
                 LogTag_RSLDecree, msg->m_minDecreeInLog);
        LearnVotes(msg->m_remoteIP, msg->m_learnPort);
    }
    else if (msg->m_checkpointedDecree >= m_maxAcceptedVote->m_decree)
    {
        RSLInfo("Copying checkpoint in response to join message",
                LogTag_RSLMsg, msg,
                LogTag_RSLDecree, m_maxAcceptedVote->m_decree,
                LogTag_RSLDecree, msg->m_minDecreeInLog,
                LogTag_RSLDecree, msg->m_checkpointedDecree);

        if (CopyCheckpoint(msg->m_remoteIP, msg->m_learnPort,
                           msg->m_checkpointedDecree, msg->m_checkpointSize, NULL))
        {
            ShutDown();
        }
    }
    else
    {
        RSLInfo("Could not learn votes or copy checkpoint - ignoring join message",
                LogTag_RSLMsg, msg,
                LogTag_RSLDecree, m_maxAcceptedVote->m_decree,
                LogTag_RSLDecree, msg->m_minDecreeInLog,
                LogTag_RSLDecree, msg->m_checkpointedDecree);
    }
}

void
Legislator::HandleJoinRequestMsg(Message *msg)
{
    if (msg->m_configurationNumber == PaxosConfiguration())
    {
        RSLInfo("Received JoinRequest", LogTag_RSLMsg, msg);
        SendJoinMessage(msg->m_remoteIP, msg->m_remotePort, msg->m_asClient);
    }
}

void
Legislator::HandleVoteAcceptedMsg(Message *msg)
{
    RSLDebug("Handling vote accepted",
             LogTag_RSLMsg, msg, LogTag_RSLState, m_state,
             LogTag_RSLMsg, (Vote *)m_maxAcceptedVote, LogTag_RSLBallot, &m_maxBallot);

    //
    // Always record time replica voted at, even if the vote has finished.
    // This is crucial for evaluating the health of the replicas.
    //
    if (AmPrimary())
    {
        RecordVoteAcceptedFromReplica(msg);
    }

    // only if I'm the primary and I have an outstanding vote,
    // and outstanding vote matches the maxAcceptedVote (outstanding vote has been logged)
    // and this is a vote for the current decree.
    if (AmPrimary() && m_outstandingVote != NULL &&
        msg->IsSameDecree(m_maxAcceptedVote->m_decree, m_maxBallot) &&
        m_outstandingVote->IsSameDecree(m_maxAcceptedVote->m_decree, m_maxBallot))
    {
        LogAssert(m_maxBallot == m_maxAcceptedVote->m_ballot);

        m_responseMap[msg->m_memberId] = true;

        if (m_responseMap.size() >= QuorumSize()-1)
        {
            RSLDebug("Vote accepted by quorum", LogTag_RSLMsg, (Vote *)m_maxAcceptedVote);

            m_outstandingVote = NULL;
            m_actionTimeout = _I64_MAX;

            // Update stats under the lock.
            {
                AutoCriticalSection lock(&m_statsLock);

                UInt32 voteTime = (UInt32)(GetHiResTime() - m_maxAcceptedVote->m_receivedAt.HiResTime());
                m_stats.m_cVotingTimeMicroseconds += voteTime;
                if (voteTime > m_stats.m_cVotingTimeMaxMicroseconds)
                    m_stats.m_cVotingTimeMaxMicroseconds = voteTime;
            }

            if (m_maxAcceptedVote->m_isReconfiguration)
            {
                SendAndLogReconfigurationDecision(m_maxAcceptedVote->m_decree,
                    m_maxAcceptedVote->m_ballot);
            }
            else if(m_maxAcceptedVote->GetRelinquishPrimary())
            {
                RSLDebug("Relinquish Primary Vote is accepted by quorum. Will shutdown",
                                LogTag_RSLMsg, (Vote *)m_maxAcceptedVote);
                ShutDown();
            }
            else
            {
                AddToExecutionQueue(m_maxAcceptedVote);
            }
        }
    }
}

void
Legislator::HandlePrepareMsg(PrepareMsg *msg)
{
    // Synchronize with the HeartbeatLoop thread if independent heartbeating
    // is enabled (which means that a vote might be sent directly from the
    // HeartbeatLoop thread rather than queueing it to the main thread).
    // Do not log a Prepare at a lower decree than a previously logged vote.
    // The lock must be held while writing to the log.
    ManualCriticalSection hbLock(&m_hbLock);
    if (m_IndependentHeartbeat)
    {
        hbLock.Enter();
    }

    if(m_relinquishPrimary)
    {
        RSLInfo("Skip handling prepare message since I am relinquishing the primary.",
                LogTag_RSLMsg, msg, LogTag_RSLState, m_state,
                LogTag_RSLMsg, (Vote *)m_maxAcceptedVote, LogTag_RSLBallot, &m_maxBallot);
        return;
    }
    RSLInfo("Handling prepare",
            LogTag_RSLMsg, msg, LogTag_RSLState, m_state,
            LogTag_RSLMsg, (Vote *)m_maxAcceptedVote, LogTag_RSLBallot, &m_maxBallot);

    if (msg->IsLowerDecree(m_maxAcceptedVote->m_decree, m_maxBallot))
    {
        if (msg->m_decree+1 < m_maxAcceptedVote->m_decree || msg->m_ballot < m_maxBallot)
        {
            Message resp(m_version,
                         Message_NotAccepted,
                         m_memberId,
                         m_maxAcceptedVote->m_decree,
                         PaxosConfiguration(),
                         m_maxBallot);
            SendResponse(msg->m_remoteIP, msg->m_remotePort, &resp);
        }
        return;
    }

    if (m_state != Initializing)
    {
        LogPrepare(msg);

        // don't hold a lock while calling upper layer (in m_stateMachine)
        hbLock.Leave();

        bool acceptPrepare;

        // GlobalAcceptMessagesFlag feature is opt-in feature to avoid querying state machine
        // for every vote as calling state machine here can cause MainLoop to get blocked if 
        // state machine is managed implementation and performing garbage collection
        if (m_cfg.UseGlobalAcceptMessagesFlag())
        {
            acceptPrepare = this->m_acceptMessages;
        }
        else
        {
            acceptPrepare = m_stateMachine->AcceptMessageFromReplica(
                &msg->m_replica->m_node,
                msg->m_primaryCookie.m_data,
                msg->m_primaryCookie.m_len);
        } 

        if (acceptPrepare)
        {
            PrepareAccepted resp(m_version,
                                 m_memberId,
                                 msg->m_decree,
                                 PaxosConfiguration(),
                                 msg->m_ballot,
                                 m_maxAcceptedVote);

            SendResponse(msg->m_remoteIP, msg->m_remotePort, &resp);
        }
        else
        {
            RSLWarning("Not replying prepare - AcceptMessageFromReplica returned false");
        }

        if (msg->m_decree > m_maxAcceptedVote->m_decree)
        {
            RSLInfo("StartInitializing");
            StartInitializing();
        }
        else
        {
            StartBeingStableSecondary();
        }
    }
}

void
Legislator::HandlePrepareAcceptedMsg(PrepareAccepted *msg)
{
    RSLInfo("Handling prepare accepted",
            LogTag_RSLMsg, msg, LogTag_RSLState, m_state,
            LogTag_RSLMsg, (Vote *)m_maxAcceptedVote, LogTag_RSLBallot, &m_maxBallot);

    if (m_state == Preparing &&
        msg->IsSameDecree(m_maxAcceptedVote->m_decree, m_maxBallot))
    {
        LogAssert(m_maxBallot.m_memberId == m_memberId);
        LogAssert(m_oldFreshestVote->m_decree == m_maxAcceptedVote->m_decree);

        Vote *vote = msg->m_vote;

        if (vote->m_decree > msg->m_decree || vote->m_ballot > msg->m_ballot)
        {
            RSLError("Invalid prepare accepted response",
                     LogTag_RSLMsg, msg, LogTag_RSLMsg, vote);
            return;
        }

        if (vote->m_decree == m_oldFreshestVote->m_decree &&
            vote->m_ballot > m_oldFreshestVote->m_ballot)
        {
            m_oldFreshestVote = vote;
        }
        m_responseMap[msg->m_memberId] = true;

        if (m_responseMap.size() >= QuorumSize() - 1)
        {
            /* a majority has accepted this prepare request so we are the
               leader and can start making progress */
            StartBeingStablePrimary();
        }
    }
}

void
Legislator::HandleNotAcceptedMsg(Message *msg)
{
    // Synchronize with the HeartbeatLoop thread if independent heartbeating
    // is enabled (which means that a vote might be sent directly from the
    // HeartbeatLoop thread rather than queueing it to the main thread).
    // Do not log a Prepare at a lower decree than a previously logged vote.
    // The lock must be held while writing to the log.
    ManualCriticalSection hbLock(&m_hbLock);
    if (m_IndependentHeartbeat)
    {
        hbLock.Enter();
    }

    RSLInfo("Handling not accepted",
            LogTag_RSLMsg, msg, LogTag_RSLState, m_state,
            LogTag_RSLMsg, (Vote *)m_maxAcceptedVote, LogTag_RSLBallot, &m_maxBallot);

    if (msg->IsLowerDecree(m_maxAcceptedVote->m_decree, m_maxAcceptedVote->m_ballot))
    {
        return;
    }

    if (m_state != Initializing)
    {
        if (msg->m_decree > m_maxAcceptedVote->m_decree)
        {
            // don't hold a lock while calling upper layer (in m_stateMachine)
            hbLock.Leave();

            // we missed some message and fallen behind.
            StartInitializing();
        }
        else if (msg->m_ballot > m_maxBallot)
        {
            PrimaryCookie primaryCookie;
            PrepareMsg prep((RSLProtocolVersion) msg->m_version,
                            msg->m_memberId,
                            msg->m_decree,
                            PaxosConfiguration(),
                            msg->m_ballot,
                            &primaryCookie);

            LogPrepare(&prep);

            // don't hold a lock while calling upper layer (in m_stateMachine)
            hbLock.Leave();

            // maybe we'll get this message soon too.
            StartBeingStableSecondary();
        }
    }
}

void
Legislator::HandleStatusQueryMsg(Message *msg, StreamSocket *sock)
{
    if(m_relinquishPrimary)
    {
        RSLInfo("Skip handling status query since I am reqlinquish the primary.", LogTag_RSLMsg, msg);
        return;
    }
    RSLInfo("Handling status query", LogTag_RSLMsg, msg);
    m_lock.Enter();
    StatusResponse resp(
        m_version,
        m_memberId,
        m_maxAcceptedVote->m_decree,
        PaxosConfiguration(),
        m_maxAcceptedVote->m_ballot);

    resp.m_queryDecree = msg->m_decree;
    resp.m_queryBallot = msg->m_ballot;

    Int64 rcvdAgo = GetHiResTime() - m_maxAcceptedVote->m_receivedAt.HiResTime();

    resp.m_lastReceivedAgo = (rcvdAgo < 0) ? 0 : rcvdAgo/HRTIME_MSECOND;
    resp.m_minDecreeInLog = m_logFiles.front()->m_minDecree;
    resp.m_checkpointedDecree = m_checkpointedDecree;
    resp.m_checkpointSize = m_checkpointSize;

    // These 2 fields are used only by the test cases.
    resp.m_maxBallot = m_maxBallot;
    resp.m_state = m_state;
    m_lock.Leave();

    if (sock == NULL)
    {
        SendResponse(msg->m_remoteIP, msg->m_remotePort, &resp);
    }
    else
    {
        StandardMarshalMemoryManager memory(resp.GetMarshalLen());
        MarshalData marshal(&memory);
        resp.Marshal(&marshal);

        DWORD ec = sock->Write(marshal.GetMarshaled(), marshal.GetMarshaledLength());
        if (ec != NO_ERROR)
        {
            RSLInfo("Write to socket failed", LogTag_ErrorCode, ec);
        }
    }
}

void
Legislator::HandleStatusResponseMsg(StatusResponse *msg)
{
    RSLInfo("Handling status response",
            LogTag_RSLMsg, msg, LogTag_RSLDecree, msg->m_minDecreeInLog,
            LogTag_RSLState, m_state, LogTag_RSLMsg, (Vote *)m_maxAcceptedVote,
            LogTag_RSLBallot, &m_maxBallot);

    // start the CopyCheckpointFromSecondary thread even though
    // the legislator might not be the primary now. It might have
    // started the copy operation when it was a primary and then
    // immediately lost primary status.
    UInt64 decree = GetCopyCheckpointAt();
    if (decree != 0 && msg->m_checkpointedDecree >= decree)
    {
        if (m_copyCheckpointThread != nullptr)
        {
            // check if the thread is still running
            DWORD ret = WaitForSingleObject(m_copyCheckpointThread, 0);
            if (ret == WAIT_OBJECT_0)
            {
                CloseHandle(m_copyCheckpointThread);
                m_copyCheckpointThread = nullptr;
            }
        }

        if (m_copyCheckpointThread == nullptr)
        {
            RSLInfo("Copying checkpoint",
                    LogTag_RSLDecree, msg->m_checkpointedDecree,
                    LogTag_RSLDecree, decree);

            CheckpointCopyInfo *info = new CheckpointCopyInfo();
            LogAssert(info);
            info->m_ip = msg->m_replica->m_node.m_ip;
            info->m_port = msg->m_replica->m_node.m_rslLearnPort;
            info->m_checkpointDecree = msg->m_checkpointedDecree;
            info->m_checkpointSize = msg->m_checkpointSize;

            m_cpTimeout = _I64_MAX;
            RunThread(&Legislator::CopyCheckpointFromSecondary, "CopyCheckpointFromSecondary", info, Priority_OutOfGC, &m_copyCheckpointThread);
        }
    }

    if (AmPrimary())
    {
        return;
    }

    // If i'm not trying to learn, or if this response is for a previous
    // message I sent bofore, don't do anything.
    if (msg->m_queryDecree != m_maxAcceptedVote->m_decree ||
        msg->m_queryBallot != m_maxAcceptedVote->m_ballot)
    {
        return;
    }

    bool learn = false;
    if (m_state != Initializing)
    {
        if (msg->m_decree == m_maxAcceptedVote->m_decree+1 &&
            msg->m_ballot >= m_maxAcceptedVote->m_ballot)
        {
            // This is to avoid the scenario where a minority of the replicas
            // have a higher decree and majority has the same decree number as ours
            // While learning, the majority with same decree  would respond and we
            // would move to preparing. In preparing, somebody from the minority
            // reject out prepare since they have a higher decree. We will then
            // move back to learning, where the majority would say that no decree
            // has been passed. To break this cycle, we have to learn even if
            // we are not in the learning state.
            AutoCriticalSection lock(&m_recvQLock);
            // If voteCache is not null, then somebody is still proposing decrees.
            // We are going to get into the above cycle.
            if (m_voteCache.Head() == NULL)
            {
                learn = true;
            }
        }
        if (learn)
        {
            LearnVotesAndTransition(msg);
        }

        return;
    }

    // make a copy of the status response
    m_statusMap.Insert(msg);

    if (m_statusMap.Size() >= QuorumSize()-1)
    {
        // if we need to copy the checkpoint, copy the checkpoint and exit.
        // choose which replica to contact
        // also remember the last time the latest decree was chosen.

        UInt64 minDecree = _I64_MAX;
        {
            AutoCriticalSection lock(&m_recvQLock);
            Vote *vote = m_voteCache.Head();
            if (vote && vote->m_decree > m_maxAcceptedVote->m_decree)
            {
                minDecree = vote->m_decree;
            }
        }
        // randomly choose a replica that has at least all the votes I
        // need to fill the holes
        StatusList responses;

        // step 1: Find members who have decree >= m_maxAcceptedVote->m_decree in their log
        // and have accepted minDecree
        m_statusMap.FindHigherLogged(m_maxAcceptedVote->m_decree,
                                     minDecree-1,
                                     m_maxAcceptedVote->m_ballot,
                                     &responses);

        if (responses.size() > 0)
        {
            // pick a member at random
            size_t id = rand() % responses.size();
            LearnVotesAndTransition(responses[id]);
            return;
        }

        // step 2: Find members who have decree >= m_maxacceptedVote in the log.
        m_statusMap.FindHigherLogged(m_maxAcceptedVote->m_decree,
                                     m_maxAcceptedVote->m_decree,
                                     m_maxAcceptedVote->m_ballot,
                                     &responses);

        if (responses.size() > 0)
        {
            // pick a member at random
            size_t id = rand() % responses.size();
            LearnVotesAndTransition(responses[id]);
            return;

        }
        // step 3: No body has a decree higher than ours in the log file. Check
        // if they have a higher decree at all
        m_statusMap.FindHigherLogged(_I64_MAX,
                                     m_maxAcceptedVote->m_decree,
                                     m_maxAcceptedVote->m_ballot,
                                     &responses);

        if (responses.size() > 0)
        {
            // find a replica with a checkpoint greater than ours
            m_statusMap.FindCheckpoint(m_maxAcceptedVote->m_decree, &responses);
            if (responses.size() > 0)
            {
                // pick a member at random
                size_t id = rand() % responses.size();

                if (CopyCheckpoint(responses[id]->m_replica->m_node.m_ip,
                                   responses[id]->m_replica->m_node.m_rslLearnPort,
                                   responses[id]->m_checkpointedDecree,
                                   responses[id]->m_checkpointSize,
                                   NULL))
                {
                    RSLInfo("We will shutdown now, since a new checkpoint was required");
                    ShutDown();
                }
                else
                {
                    StartInitializing();
                }
            }
            // I/they are probably in the middle of checkpointing or we failed to
            // copy the checkpoint. Retry or wait until more replicas reply.
            return;
        }
        else
        {
            Int64 now = GetHiResTime();
            // we only come here in three scenarios:
            // 1. when we receive a decree > m_maxAcceptedVote->m_decree
            // 2. The previous attempt to learn failed.
            // 3. We haven't received a decree in m_nextElectionTime

            // In the first 2 cases, there should already be a
            // replica which has a vote higher than ours. In the 3rd
            // case, we won't be able to find a replica only if none
            // of them have received a vote since s_BaseElectionDelay.
            // In that case, start preparing.
            Int64 minReceivedAgo = m_statusMap.FindMinReceivedAgo();

            // maxReceivedAt == -1 only in the test cases where we don't
            // want to wait till the nextElectionTime.
            if (m_nextElectionTime == _I64_MAX || minReceivedAgo == _I64_MAX)
            {
                if (minReceivedAgo > (now/HRTIME_MSECOND))
                {
                    minReceivedAgo = now/HRTIME_MSECOND;
                }
                m_nextElectionTime = now - HRTIME_MSECONDS(minReceivedAgo) + m_electionDelay;
            }

            MoveToSecondaryOrPrepare();
            return;
        }
    }
}

void
Legislator::ChangeElectionDelay(long long delayInSecs)
{
    m_cfg.Lock();
    m_cfg.ChangeElectionDelay(delayInSecs);
    m_electionDelay = Randomize(m_cfg.ElectionDelay(),
                                m_cfg.ElectionRandomize(),
                                m_cfg.MaxElectionRandomize());

    m_cfg.UnLock();
}

void
Legislator::MoveToSecondaryOrPrepare()
{
    Int64 now = GetHiResTime();

    // If its time to start a new election, or people already think
    // I'm the primary, try startting an election
    if (now >= m_nextElectionTime || m_maxBallot.m_memberId == m_memberId || m_forceReelection)
    {
        RSLPrimaryCookie cookie;
        bool canPrepare = false;
        // If we're currently not in the middle of saving (m_sSavingCheckpoint == 0)
        // or if we need to save at a future decree which we don't even have
        // then try to become the primary. Otherwise, wait until we finish
        // checkpointing
        m_lock.Enter();
        if (!m_isSavingCheckpoint)
        {
            canPrepare = true;
            if (!m_mustCheckpoint)
            {
                SetSaveCheckpointAt(0);
            }
        }
        m_lock.Leave();

        if (canPrepare && IsExecuteQueueReady() && m_stateMachine->CanBecomePrimary(&cookie))
        {
            delete m_primaryCookie;
            m_primaryCookie = cookie.m_data;
            cookie.m_data = NULL;
            if (!m_primaryCookie)
            {
                m_primaryCookie = new PrimaryCookie();
            }

            StartPreparing();
            return;
        }

        // Either we've saving a checkpoint or the state machine
        // does not want to become the primary. We should retry
        // this, if we don't get any votes/prepares within
        // heartbeatinterval.
        m_nextElectionTime = now + m_cfg.HeartBeatInterval();
        RSLInfo("Skipped trying to become primary");
    }
    StartBeingStableSecondary();
}

bool
Legislator::IsExecuteQueueReady()
{
    if (!m_executeQ.IsInMemory())
    {
       return false;
    }

    if (!m_cfg.AllowPrimaryPromotionWhileCatchingUp() && IsAnyRequestPendingExecution())
    {
       RSLInfo("IsExecuteQueueReady is false because IsAnyRequestPending is true and is required to be false");
       return false;
    }

    return true;
}

void
Legislator::HandleFetchVotesMsg(Message *msg, StreamSocket *sock)
{
    // ignore the ballot number
    // send all proposals >= msg->Decree()
    // if we don't have the starting decree, close the connection

    RSLInfo("Handling fetch votes", LogTag_RSLMsg, msg);

    UInt64 offset;
    vector<DynString> logFiles;
    {
        AutoCriticalSection lock(&m_lock);
        // check if we have this decree in the log
        vector<LogFile *>::iterator iter;
        for (iter = m_logFiles.begin(); iter != m_logFiles.end(); iter++)
        {
            if ((*iter)->HasDecree(msg->m_decree))
            {
                break;
            }
        }

        if (iter == m_logFiles.end())
        {
            RSLInfo("Requested message not found",
                    LogTag_RSLMsg, msg, LogTag_RSLMsg, (Vote *)m_maxAcceptedVote);
            return;
        }
        // we have the votes
        offset = (*iter)->GetOffset(msg->m_decree);
        for (; iter != m_logFiles.end(); iter++)
        {
            LogFile *log = *iter;
            logFiles.push_back(DynString(log->m_fileName));
        }
    }

    int ec = NO_ERROR;
    for (int i = 0; i < (int) logFiles.size() && ec == NO_ERROR; i++)
    {
        RSLInfo("Sending votes", LogTag_RSLMsg, msg,
                LogTag_Filename, logFiles[i].Str(), LogTag_Offset, offset);
        ec = SendFile(logFiles[i], offset, -1, sock);
        offset = 0;
    }
}

void
Legislator::HandleFetchCheckpointMsg(Message *msg, StreamSocket *sock)
{
    RSLInfo("Handling fetch checkpoint", LogTag_RSLMsg, msg);

    // ignore the ballot number
    UInt64 decree = msg->m_decree;
    {
        AutoCriticalSection lock(&m_lock);
        if (m_checkpointedDecree != decree)
        {
            RSLInfo("Requested checkpoint not found", LogTag_RSLMsg, msg,
                    LogTag_RSLDecree, m_checkpointedDecree);
            return;
        }
    }
    DynString checkpointFile;
    GetCheckpointFileName(checkpointFile, decree);
    RSLInfo("Sending checkpoint", LogTag_RSLMsg, msg,
            LogTag_Filename, checkpointFile.Str());
    SendFile(checkpointFile, 0, -1, sock);
}

void
Legislator::LearnVotesAndTransition(StatusResponse *resp)
{
    if (LearnVotes(resp->m_replica->m_node.m_ip, resp->m_replica->m_node.m_rslLearnPort))
    {
        StartBeingStableSecondary();
    }
    else
    {
        StartInitializing();
    }
}

bool
Legislator::LearnVotes(UInt32 ip, UInt16 port)
{
    RSLInfo("Learning votes", LogTag_RSLState, m_state, LogTag_RSLMsg, (Vote *)m_maxAcceptedVote,
            LogTag_NumericIP, ip, LogTag_Port, port);

    Ptr<Vote> minNeeded = 0;
    {
        AutoCriticalSection lock(&m_recvQLock);
        minNeeded = m_voteCache.Head();
    }

    Message req(
        m_version,
        Message_FetchVotes,
        m_memberId,
        m_maxAcceptedVote->m_decree,
        PaxosConfiguration(),
        m_maxAcceptedVote->m_ballot);

    MarshalData marshal;
    req.Marshal(&marshal);

    std::unique_ptr<StreamSocket> pSock(StreamSocket::CreateStreamSocket());

    int ec = pSock->Connect(ip, port, m_cfg.ReceiveTimeout(), m_cfg.SendTimeout());
    if (ec != NO_ERROR)
    {
        RSLInfo("Failed to connect", LogTag_ErrorCode, ec);
        return false;
    }

    ec = pSock->Write(marshal.GetMarshaled(), marshal.GetMarshaledLength());
    if (ec != NO_ERROR)
    {
        RSLInfo("Failed to send request", LogTag_ErrorCode, ec);
        // failed to connect to server. Go back to initialization
        return false;
    }

    SocketStreamReader reader(pSock.get());
    StandardMarshalMemoryManager memory(s_AvgMessageLen);
    Message *msg;
    Ptr<Vote> lastVote = NULL;
    while (ReadNextMessage(&reader, &memory, &msg, false) && msg != NULL)
    {
        if (msg->m_configurationNumber != PaxosConfiguration())
        {
            RSLError("Message from different configuration invalid",
                     LogTag_RSLMsg, msg, LogTag_RSLMsg, (Vote *)m_maxAcceptedVote);
            delete msg;
            return false;
        }

        if (msg->m_msgId == Message_Prepare)
        {
            if (msg->m_ballot > m_maxBallot)
            {
                LogPrepare((PrepareMsg *) msg);
            }
            delete msg;
            continue;
        }

        if (msg->m_msgId == Message_ReconfigurationDecision)
        {
            LogReconfigurationDecision(msg);
            lastVote = m_maxAcceptedVote;
            delete msg;
            continue;
        }

        Ptr<Vote> vote = (Vote *) msg;
        if (lastVote != NULL)
        {
            if (!vote->IsNextDecree(lastVote->m_decree, lastVote->m_ballot) ||
                vote->m_decree < m_maxAcceptedVote->m_decree ||
                vote->m_decree > m_maxAcceptedVote->m_decree+1)
            {
                // This is bad. The decrees on the remote replica are
                // not sequential.
                RSLError("Invalid vote",
                         LogTag_RSLMsg, (Vote *)vote, LogTag_RSLMsg, (Vote *) lastVote,
                         LogTag_RSLMsg, (Vote *)m_maxAcceptedVote);
                return false;
            }
        }
        else
        {
            if (vote->m_decree != m_maxAcceptedVote->m_decree)
            {
                RSLError("Invalid vote",
                         LogTag_RSLMsg, (Vote *)vote, LogTag_RSLMsg, (Vote *)m_maxAcceptedVote);
                return false;
            }
        }

        lastVote = vote;

        if (vote->m_ballot < m_maxAcceptedVote->m_ballot)
        {
            continue;
        }

        if (!vote->IsNextDecree(m_maxAcceptedVote->m_decree, m_maxAcceptedVote->m_ballot))
        {
            LogAssert(vote->m_decree == m_maxAcceptedVote->m_decree+1 &&
                      vote->m_ballot > m_maxAcceptedVote->m_ballot);

            // log a new decree with (m_maxAcceptedVote->m_decree+1, m_maxAcceptedVote->m_ballot)
            Vote *missingVote = new Vote((RSLProtocolVersion) vote->m_version,
                                         vote,
                                         &vote->m_primaryCookie);

            missingVote->m_ballot = m_maxAcceptedVote->m_ballot;
            missingVote->CalculateChecksum();
            LogAndAcceptVote(missingVote);
        }

        LogAndAcceptVote(vote);
        if (!minNeeded || minNeeded->m_decree <= vote->m_decree)
        {
            AutoCriticalSection lock(&m_recvQLock);
            minNeeded = m_voteCache.Head();
            if (minNeeded && minNeeded->m_decree <= vote->m_decree)
            {
                return true;
            }
        }
    }
    RSLInfo("Reached end of LearnVotes");
    return false;
}

bool
Legislator::ReadNextMessage(StreamReader *stream, IMarshalMemoryManager *memory,
                            Message **msg, bool restore)
{
    memory->ResizeBuffer(s_AvgMessageLen);
    memory->SetReadPointer(0);
    char *buf = (char *) memory->GetBuffer();

    *msg = NULL;
    Message msgHdr;
    UInt32 bytesRead;

    // first read the message hdr
    UInt64 curOffset = stream->BytesRead();

    int ec = stream->Read(buf, s_PageSize, &bytesRead);

    if(ec == ERROR_HANDLE_EOF)
    {
        RSLInfo("Reached end of file", LogTag_Offset, curOffset);
        return true;
    }
    if (ec != NO_ERROR || bytesRead != s_PageSize)
    {
        RSLError("Read failed", LogTag_Offset, curOffset,
                 LogTag_ErrorCode, ec, LogTag_UInt1, bytesRead);
        return false;
    }

    if (!msgHdr.UnMarshalBuf(buf, s_PageSize))
    {
        // check if all the data starting at this point is all 0.
        char zero[s_PageSize];
        memset(zero, 0, sizeof(zero));
        if (restore && memcmp(buf, zero, s_PageSize)==0 && VerifyZeroStream(stream))
        {
            RSLWarning("Zero data", LogTag_Offset, curOffset);
            return true;
        }
        else
        {
            RSLError("Failed to unmarshal message, possibly corrupt stream",
                     LogTag_Offset, curOffset);
            return false;
        }
    }

    if (msgHdr.m_msgId != Message_Vote &&
        msgHdr.m_msgId != Message_Prepare &&
        msgHdr.m_msgId != Message_ReconfigurationDecision)
    {
        RSLError("Unknown message id",
                 LogTag_Offset, curOffset, LogTag_RSLMsg, &msgHdr);
        return false;
    }

    RSLDebug("Read Message Hdr",
             LogTag_Offset, curOffset, LogTag_RSLMsg, &msgHdr);

    UInt32 bodyLen = RoundUpToPage(msgHdr.m_unMarshalLen) - s_PageSize;
    if (bodyLen > 0)
    {
        memory->ResizeBuffer(RoundUpToPage(msgHdr.m_unMarshalLen));
        buf = (char *) memory->GetBuffer();
        bytesRead = 0;
        ec = stream->Read(buf+s_PageSize, bodyLen, &bytesRead);
        if (ec != NO_ERROR || bytesRead != bodyLen)
        {
            Log (
                LogID_RSLLIB,
                LogLevel_Error,
                "Read failed",
                "GetLastError()=%d buf=%p offset=0x%I64x BytesToRead=0x%x BytesRead=0x%x",
                GetLastError (),
                buf,
                curOffset,
                bodyLen,
                bytesRead
            );

            if (restore && (ec == NO_ERROR || ec == ERROR_HANDLE_EOF))
            {
                RSLWarning("Ignoring last incomplete message in log",
                           LogTag_RSLMsg, &msgHdr,
                           LogTag_Offset, curOffset,
                           LogTag_UInt1, bodyLen,
                           LogTag_UInt1, bytesRead);
                return true;
            }
            else
            {
                RSLError("Read failed for message",
                         LogTag_RSLMsg, &msgHdr,
                         LogTag_Offset, curOffset,
                         LogTag_UInt1, bodyLen,
                         LogTag_ErrorCode, ec,
                         LogTag_UInt1, bytesRead);
                return false;
            }
        }
    }

    if (msgHdr.VerifyChecksum(buf, msgHdr.m_unMarshalLen) == false)
    {
        RSLInfo("Checksum mis-match, checking if last message",
                LogTag_Offset, curOffset, LogTag_RSLMsg, &msgHdr);

        // check if everything after this point is zero;
        if (restore && VerifyZeroStream(stream))
        {
            RSLWarning("Discarding last incomplete message",
                       LogTag_Offset, curOffset, LogTag_RSLMsg, &msgHdr);
            return true;
        }
        else
        {
            RSLError("Checksum mis-match, corrupt stream",
                     LogTag_Offset, curOffset, LogTag_RSLMsg, &msgHdr);
            return false;
        }
    }
    // we have the whole body
    memory->SetValidLength(msgHdr.m_unMarshalLen);
    *msg = UnMarshalMessage(memory);
    if (!*msg)
    {
        RSLError("Failed to unmarshal message",
                 LogTag_Offset, curOffset, LogTag_RSLMsg, &msgHdr);
        return false;
    }
    RSLDebug("Message read", LogTag_Offset, curOffset, LogTag_RSLMsg, *msg);
    return true;
}

bool
Legislator::VerifyZeroStream(StreamReader *stream)
{
    UInt32 bufLen = s_AvgMessageLen;
    char *buf = (char *) malloc(bufLen);
    char *zero = (char *) malloc(bufLen);
    LogAssert(buf);
    LogAssert(zero);

    memset(zero, 0, bufLen);
    UInt32 bytesRead;
    int ec;
    while ((ec = stream->Read(buf, bufLen, &bytesRead)) == NO_ERROR)
    {
        if (memcmp(buf, zero, bytesRead) != 0)
        {
            RSLError("Non-zero data in stream",
                     LogTag_Offset, stream->BytesRead()-bytesRead);
            free(buf);
            free(zero);
            return false;
        }
    }
    free(buf);
    free(zero);

    if (ec != ERROR_HANDLE_EOF)
    {
        RSLError("Read Failed", LogTag_Offset, stream->BytesRead(),
                 LogTag_ErrorCode, ec, LogTag_UInt1, bytesRead);
        // we're good.
        return false;
    }
    return true;
}

void
Legislator::LogAndAcceptVote(Vote *vote)
{
    UInt64 decree = m_maxAcceptedVote->m_decree;
    BallotNumber ballot = m_maxAcceptedVote->m_ballot;
    LogAssert(vote->IsNextDecree(decree, ballot));
    LogAssert(vote->m_configurationNumber == PaxosConfiguration());
    if (vote->m_decree == decree+1)
    {
        LogAssert(vote->m_ballot != BallotNumber());
        AddToExecutionQueue(m_maxAcceptedVote);
        Int64 receivedAt = vote->m_receivedAt.HiResTime();
        if (!receivedAt)
        {
            receivedAt = GetHiResTime();
        }
        // a new decree has been passed. fix m_nextElectionTime
        if (m_maxBallot.m_memberId == m_memberId)
        {
            m_nextElectionTime = receivedAt + m_cfg.HeartBeatInterval();
        }
        else
        {
            m_nextElectionTime = receivedAt + m_electionDelay;
        }
    }
    LogVote(vote);
}

void
Legislator::AddToExecutionQueue(Vote *vote)
{
    // for now, just print that the vote has been executed
    bool hasUserCookie = (vote->m_numRequests > 0 && vote->m_requests.head->m_cookie != NULL);
    bool must = (hasUserCookie) || AmPrimary();
    m_executeQ.Enqueue(vote, must);

    // if we ever execute a message with a version higher than ours,
    // we should use this version to communicate with others
    if (vote->m_version > m_version)
    {
        AutoCriticalSection lock(&m_lock);
        m_version = (RSLProtocolVersion) vote->m_version;
    }
}

void
Legislator::SetMaxBallot(BallotNumber &ballot)
{
    if (ballot > m_maxBallot)
    {
        RSLInfo("Setting max ballot", LogTag_RSLState, m_state,
                LogTag_RSLBallot, &ballot, LogTag_RSLBallot, &m_maxBallot);

        m_maxBallot = ballot;
        Int64 nextTime = GetHiResTime() + m_cfg.NewLeaderGracePeriod();
        if (m_maxBallot.m_memberId == m_memberId)
        {
            nextTime = GetHiResTime() + m_cfg.HeartBeatInterval();
        }
        if (nextTime > m_nextElectionTime)
        {
            m_nextElectionTime = nextTime;
        }
    }
}

void
Legislator::StartInitializing()
{
    RSLInfo("Setting state to Initializing", LogTag_RSLState, m_state,
            LogTag_RSLMsg, (Vote *)m_maxAcceptedVote, LogTag_RSLBallot, &m_maxBallot);
    m_state = Initializing;
    m_responseMap.clear();
    m_statusMap.Clear();
    m_actionTimeout = GetHiResTime() + m_cfg.InitializeRetryInterval();

    if (QuorumSize() == 1)
    {
        m_nextElectionTime = 0;
        MoveToSecondaryOrPrepare();
    }
    else
    {
        Message msg(m_version,
                    Message_StatusQuery,
                    m_memberId,
                    m_maxAcceptedVote->m_decree,
                    PaxosConfiguration(),
                    m_maxAcceptedVote->m_ballot);
        SendRequestToAll(&msg);
    }
}

void
Legislator::RelinquishPrimary()
{
    if(m_version < RSLProtocolVersion_5)
    {
        RSLError("RelinquishPrimary is only supported with version 5 or upper.", LogTag_RSLState, m_state,
               LogTag_RSLMsg, (Vote *)m_maxAcceptedVote, LogTag_RSLBallot, &m_maxBallot);
        return;
    }

    RSLInfo("Relinquish Primary", LogTag_RSLState, m_state,
            LogTag_RSLMsg, (Vote *)m_maxAcceptedVote, LogTag_RSLBallot, &m_maxBallot);

    if (!AmPrimary())
    {
        RSLError("I Cannot relinquish Primary since I am not primary.");
        return;
    }
    Ptr<Vote> vote;

    vote = new Vote(m_version, m_memberId, 0, 0, BallotNumber(), m_primaryCookie, true);
    LogAssert(vote);

    m_lock.Enter();
    m_votesToSend.Enqueue( vote );
    m_lock.Leave();
}

bool
Legislator::IsInStableState()
{
    return m_state == StablePrimary || m_state == StableSecondary;
}

void
Legislator::StartBeingStablePrimary()
{
    LogAssert(m_state == Preparing && m_maxBallot.m_memberId == m_memberId);

    RSLInfo("Sending vote as primary", LogTag_RSLMsg, (Vote *)m_maxAcceptedVote,
            LogTag_RSLBallot, &m_maxBallot, LogTag_RSLMsg, (Vote *)m_oldFreshestVote);

    //
    //  Set the flag indicating we are transitioning to be the primary.  This will
    //  keep the execution thread from blocking callbacks until the transition
    //  is complete.
    //

    m_IsTransitioningToPrimary = true;
    m_forceReelection = false;
    m_state = StablePrimary;
    m_outstandingVote = NULL;
    m_responseMap.clear();

    // Make a copy of m_oldFreshestVote and pass it to SendNextVote(),
    // which should set the ballotId to m_maxBallot and log the vote.
    // Never pass m_oldFreshestVote to SendNextVote - the vote will
    // never be logged
    Vote *vote = new Vote(m_version, m_oldFreshestVote, m_primaryCookie);
    SendNextVote(vote);
    m_oldFreshestVote = NULL;
}

void
Legislator::StartBeingStableSecondary()
{
    if (m_state != StableSecondary)
    {
        RSLInfo("Setting state to StableSecondary", LogTag_RSLState, m_state,
                LogTag_RSLMsg, (Vote *)m_maxAcceptedVote, LogTag_RSLBallot, &m_maxBallot);
    }
    m_forceReelection = false;

    m_state = StableSecondary;
    m_responseMap.clear();
    m_actionTimeout = _I64_MAX;
}

void
Legislator::StartPreparing()
{
    // Synchronize with the HeartbeatLoop thread if independent heartbeating
    // is enabled (which means that a vote might be sent directly from the
    // HeartbeatLoop thread rather than queueing it to the main thread).
    // Do not log a Prepare at a lower decree than a previously logged vote.
    // The lock must be held while writing to the log.
    ManualCriticalSection hbLock(&m_hbLock);
    if (m_IndependentHeartbeat)
    {
        hbLock.Enter();
    }

    RSLInfo("Setting state to Prepare", LogTag_RSLState, m_state,
            LogTag_RSLMsg, (Vote *)m_maxAcceptedVote, LogTag_RSLBallot, &m_maxBallot);

    BallotNumber newBallot;
    if (m_state == Initializing)
    {
        // we got here at the end of initialization
        m_state = Preparing;
        newBallot.m_memberId = m_memberId;
        newBallot.m_ballotId = m_maxBallot.m_ballotId+1;
    }
    else
    {
        LogAssert(m_state == Preparing);
        LogAssert(m_maxBallot.m_memberId == m_memberId);
        newBallot = m_maxBallot;
    }
    {
        AutoCriticalSection lock(&m_recvQLock);
        if (newBallot < m_maxSeenBallot)
        {
            newBallot.m_ballotId = m_maxSeenBallot.m_ballotId+1;
        }
    }

    Int64 now = GetHiResTime();
    m_actionTimeout = now + HRTIME_SECONDS(1);
    m_prepareTimeout = now + m_electionDelay * 30;

    m_responseMap.clear();
    m_oldFreshestVote = m_maxAcceptedVote;
    delete m_lastPrepareMsg;
    m_lastPrepareMsg = new PrepareMsg(m_version,
                                      m_memberId,
                                      m_maxAcceptedVote->m_decree,
                                      PaxosConfiguration(),
                                      newBallot,
                                      m_primaryCookie);

    if (QuorumSize() == 1)
    {
        LogPrepare(m_lastPrepareMsg);

        // don't hold a lock while calling upper layer (in m_stateMachine)
        hbLock.Leave();

        StartBeingStablePrimary();
    }
    else
    {
        SendRequestToAll(m_lastPrepareMsg);
        LogPrepare(m_lastPrepareMsg);
    }
}

void Legislator::SendNextVote(Vote *vote)
{
    LogAssert(m_maxBallot.m_memberId == m_memberId);
    LogAssert(m_outstandingVote == NULL);
    LogAssert((Vote *) m_maxAcceptedVote != vote);

    m_responseMap.clear();

    if (m_maxBallot == m_maxAcceptedVote->m_ballot)
    {
        vote->m_decree = m_maxAcceptedVote->m_decree+1;
    }
    else
    {
        LogAssert(m_maxBallot > m_maxAcceptedVote->m_ballot);
        vote->m_decree = m_maxAcceptedVote->m_decree;
    }

    Int64 now = GetHiResTime();
    vote->m_ballot = m_maxBallot;
    vote->m_memberId = m_memberId;
    vote->m_receivedAt.Set();
    vote->m_configurationNumber = PaxosConfiguration();
    vote->CalculateChecksum();

    if(m_relinquishPrimary)
    {
        RSLDebug("Skip sending the vote because I am relinquishing the Primary", LogTag_RSLMsg, vote);
        return;
    }

    RSLDebug("Sending vote", LogTag_RSLMsg, vote);
    if(vote->GetRelinquishPrimary())
    {
        m_relinquishPrimary = true;
    }
    if (QuorumSize() > 1)
    {
        if (m_isPrimary == false)
        {
            RSLWarning("SendNextVote arrived at SendRequestToAll but isPrimary is false");
        }

        m_outstandingVote = vote;
        m_actionTimeout = now + HRTIME_SECONDS(1);
        LogVote(vote);
        SendRequestToAll(vote);
    }
    else if (vote->m_isReconfiguration)
    {
        LogVote(vote);
        SendAndLogReconfigurationDecision(vote->m_decree, vote->m_ballot);
    }
    else
    {
        LogVote(vote);
        AddToExecutionQueue(vote);
        m_actionTimeout = _I64_MAX;
    }
}

void
Legislator::ReSendCurrentVote()
{
    // the previous attempt to send vote failed, resend the vote
    // Don't clear the response map. We don't need to get
    // responses from replicas that have already replied.
    LogAssert(m_outstandingVote != NULL);
    Ptr<Vote> vote(m_maxAcceptedVote);

    //
    // Perform deadlock check before another re-send is attempted.
    // Deadlock can occur if the following conditions are met:
    // 1. This replica is the primary replica
    // 2. It logged a vote and then failed over before sending the vote.
    // 3. It came back as primary.
    // 4. It sent a new vote.
    // 5. It requested a checkpoint copy.
    // 6. Secondary replicas are unable to deliver the checkpoint.
    // 7. It started sending status requests to monitor checkpoint.
    //
    // Secondary replicas will not send a response for the outstanding vote because they are all in Initializing state
    // until they finish learning about the missing vote. Eventually secondary replicas are going to learn about the missing vote
    // but they will not send a response for the outstanding vote until the vote is resent.
    // We are in deadlock until that happens.
    //
    // In order to get unstuck, we are going to try to resend the outstanding vote.
    // If resend fails, we are going to failover after specified amount of time.
    //
    Int64 now = GetHiResTime();
    if (now - vote->m_receivedAt.HiResTime() >= HRTIME_SECONDS(m_cfg.VoteMaxOutstandingInterval()))
    {
        RSLError("Terminating the process to recover from deadlock. Vote has been outstanding for more than max time allowed.",
            LogTag_RSLMsg, (Vote *) vote,
            LogTag_RSLBallot, &m_maxBallot,
            LogTag_UInt641, m_cfg.VoteMaxOutstandingInterval(),
            LogTag_UInt642, now - vote->m_receivedAt.HiResTime());

        // Terminate the process
        LogAssert(false);
    }

    ReSendRequest(vote, m_cfg.VoteRetryInterval());
    m_actionTimeout = now + HRTIME_SECONDS(1);
}

void
Legislator::ReSendPrepareMsg()
{
    LogAssert(m_state == Preparing);
    ReSendRequest(m_lastPrepareMsg, m_cfg.PrepareRetryInterval());
    m_actionTimeout = GetHiResTime() + HRTIME_SECONDS(1);
}

void
Legislator::SendAndLogReconfigurationDecision(UInt64 decree, BallotNumber& ballot)
{
    Message decision(
        m_version,
        Message_ReconfigurationDecision,
        m_memberId,
        decree,
        PaxosConfiguration(),
        ballot);

    RSLInfo("sending and logging reconfiguration decision",
            LogTag_RSLMsg, &decision,
            LogTag_RSLMsg, (Vote *) m_maxAcceptedVote,
            LogTag_RSLBallot, &m_maxBallot);

    StandardMarshalMemoryManager memory(decision.GetMarshalLen());
    MarshalData marshal(&memory);
    decision.Marshal(&marshal);
    decision.CalculateChecksum((char *) memory.GetBuffer(), decision.GetMarshalLen());

    SendRequestToAll(&decision);

    LogReconfigurationDecision(&decision);
}

void
Legislator::SendDefunctConfigurationMessage(UInt32 ip, UInt16 port, bool asClient)
{
    RSLInfo("SendDefunctConfigurationMessage",
            LogTag_NumericIP, ip,
            LogTag_Port, (UInt32) port,
            LogTag_UInt1, m_highestDefunctConfigurationNumber);

    Message msg(
        m_version,
        Message_DefunctConfiguration,
        m_memberId,
        0, // no decree
        m_highestDefunctConfigurationNumber,
        BallotNumber());
    SendMessageToIpPort(ip, port, &msg, asClient);
}

void
Legislator::SendJoinRequestMessage(
    UInt32 configurationNumber,
    UInt32 ip,
    UInt16 port,
    bool asClient)
{
    RSLInfo("SendJoinRequestMessage",
            LogTag_NumericIP, ip,
            LogTag_Port, (UInt32) port,
            LogTag_UInt1, configurationNumber);

    Message msg(
        m_version,
        Message_JoinRequest,
        m_memberId,
        0, // no decree
        configurationNumber,
        BallotNumber());
    SendMessageToIpPort(ip, port, &msg, asClient);
}

void
Legislator::SendJoinMessage(UInt32 ip, UInt16 port, bool asClient)
{
    m_lock.Enter();

    JoinMessage msg(
        m_version,
        m_memberId,
        m_paxosConfiguration->GetInitialDecree() - 1,
        PaxosConfiguration());

    msg.m_learnPort = m_self->m_node.m_rslLearnPort;
    msg.m_minDecreeInLog = m_logFiles.front()->m_minDecree;
    msg.m_checkpointedDecree = m_checkpointedDecree;
    msg.m_checkpointSize = m_checkpointSize;

    m_lock.Leave();

    RSLInfo("sending join message",
             LogTag_NumericIP, ip,
             LogTag_Port, (UInt32) port,
             LogTag_RSLMsg, &msg,
             LogTag_RSLDecree, msg.m_minDecreeInLog,
             LogTag_RSLDecree, msg.m_checkpointedDecree);

    if (!asClient)
    {
        SendMessageToIpPort(ip, port, &msg, false);
    }
    else
    {
        if (ip == 0)
        {
            SendRequestToAll(&msg);
        }
        else
        {
            SendMessageToIpPort(ip, port, &msg, true);
        }
    }
}

DWORD32
Legislator::SendFile(char *file, UInt64 offset, Int64 length, StreamSocket *sock)
{
    void *buf;
    DWORD bytesRead;
    int ec;

    // We want to capture the amount of time we spend reading data from the disk, but we're using APSEQREAD which involves parallel async I/O. So we'll measure
    // the total time taken to read all of the data minus the time we spend sending it.
    Int64 diskReadTimeSoFar = 0;
    Int64 timer = GetHiResTime();

    auto_ptr<APSEQREAD> reader(new APSEQREAD());
    ec = reader->DoInit(file, APSEQREAD::c_maxReadsDefault, APSEQREAD::c_readBufSize, true);
    if (ec != NO_ERROR)
    {
        RSLError("Open file failed", LogTag_Filename, file, LogTag_ErrorCode, ec);
        return ec;
    }
    if (offset > 0)
    {
        ec = reader->Reset(offset);
        if (ec != NO_ERROR)
        {
            RSLError("Reset file offset failed",
                     LogTag_Filename, file, LogTag_Offset, offset, LogTag_ErrorCode, ec);
            return ec;
        }
    }

    if (length < 0)
    {
        length = reader->FileSize() - offset;
    }

    for (Int64 toRead = length; toRead > 0; toRead -= bytesRead)
    {
        ec = reader->GetDataPointer(&buf, APSEQREAD::c_readBufSize, &bytesRead);
        if (ec != NO_ERROR)
        {
            RSLError("Read from file failed",
                     LogTag_Filename, file, LogTag_Int64_1, length,
                     LogTag_Int64_2, toRead, LogTag_ErrorCode, ec);
            return ec;
        }

        diskReadTimeSoFar += GetHiResTime() - timer;

        ec = sock->Write(buf, bytesRead);
        if (ec != NO_ERROR)
        {
            RSLInfo("Write to socket failed", LogTag_ErrorCode, ec);
            return ec;
        }

        timer = GetHiResTime();
    }

    // Update statistics under the lock.
    {
        AutoCriticalSection lock(&m_statsLock);

        m_stats.m_cLogReads++;
        m_stats.m_cLogReadBytes += length;
        m_stats.m_cLogReadMicroseconds += (UInt32)diskReadTimeSoFar;
        if (diskReadTimeSoFar > m_stats.m_cLogReadMaxMicroseconds)
            m_stats.m_cLogReadMaxMicroseconds  = (UInt32)diskReadTimeSoFar;
    }

    return NO_ERROR;
}

void
Legislator::SendStatusRequestMessage()
{
    if (!AmPrimary() || !GetCopyCheckpointAt())
    {
        SetCopyCheckpointAt(0);
        m_cpTimeout = _I64_MAX;
    }
    else
    {

        // i'm the primary and there are other replicas in the set
        Message msg(m_version,
                    Message_StatusQuery,
                    m_memberId,
                    m_maxAcceptedVote->m_decree,
                    PaxosConfiguration(),
                    m_maxAcceptedVote->m_ballot);
        SendRequestToAll(&msg);
        m_cpTimeout = GetHiResTime() + m_cfg.CPQueryRetryInterval();
    }
}

void
Legislator::SendMessageToIpPort(UInt32 ip, UInt16 port, Message *msg, bool asClient)
{
    if (!asClient)
    {
        return SendMessage(ip, port, NULL, msg, asClient);
    }

    {
    // find the replica corresponding to this ip+Port
        CAutoPoolLockShared lock(&m_replicasLock);

        for (ReplicaIter i = m_replicas.begin(); i != m_replicas.end(); i++)
        {
            Replica *replica = *i;
            if (replica->m_node.m_ip == ip && replica->m_node.m_rslPort == port)
            {
                return SendMessageToReplica(replica, msg);
            }
        }
    }
    return SendMessage(ip, port, NULL, msg, asClient);
}

void
Legislator::SendMessageToReplica(Replica *replica, Message *msg)
{
    return SendMessage(replica->m_node.m_ip, replica->m_node.m_rslPort, replica, msg, true);
}

void
Legislator::SendMessage(UInt32 ip, UInt16 port, Replica *replica, Message *msg, bool asClient)
{
    UInt32 len = msg->GetMarshalLen();

    // allocate a packet of that length;

    LegislatorPacket *pkt = new LegislatorPacket();
    pkt->m_replica = replica;
    if (replica != NULL)
    {
       InterlockedIncrement(&replica->m_numOutstanding);
    }
    pkt->m_MemoryManager.ResizeBuffer(len);

    MarshalData marshal(&pkt->m_MemoryManager);
    msg->Marshal(&marshal);
    LogAssert(marshal.GetMarshaledLength() == len);

    if (msg->m_msgId != Message_Vote && msg->m_msgId != Message_VoteAccepted)
    {
        RSLInfo("Sending message", LogTag_RSLMsg, msg,
                LogTag_RSLState, m_state, LogTag_Ptr1, pkt,
                LogTag_NumericIP, ip, LogTag_Port, port);

    }
    else
    {
        RSLDebug("Sending message", LogTag_RSLMsg, msg,
                 LogTag_RSLState, m_state, LogTag_Ptr1, pkt,
                 LogTag_NumericIP, ip, LogTag_Port, port);
    }

    TxRxStatus status;
    if (asClient)
    {
        pkt->SetServerAddr(ip, port);
        status = m_netlibClient->Send(pkt, m_cfg.SendTimeout());
        LogAssert(status == TxSuccess);

    }
    else
    {
        pkt->SetClientAddr(ip, port);
        status = m_netlibServer->Send(pkt, m_cfg.SendTimeout());
        LogAssert(status == TxSuccess || status == TxNoConnection);
    }
    if (status != TxSuccess)
    {
        RSLInfo("Failed to send message",
                LogTag_Ptr1, pkt, LogTag_NumericIP, ip,
                LogTag_Port, port, LogTag_StatusCode, status);
        delete pkt;
    }

    return;
}

void
Legislator::SendResponse(UInt32 ip, UInt16 port, Message *msg)
{
    SendMessageToIpPort(ip, port, msg, false);
}

void
Legislator::ReSendRequest(Message *msg, Int64 failedReplicaTimeout)
{
    Int64 now = GetHiResTime();

    CAutoPoolLockShared lock(&m_replicasLock);
    for (ReplicaIter i = m_replicas.begin(); i != m_replicas.end(); i++)
    {
        Replica *replica = *i;
        Int64 timeout = (replica->m_connected) ?
            HRTIME_MSECONDS(m_cfg.ReceiveTimeout()) : failedReplicaTimeout;

        MemberId memberId(replica->m_node.m_memberIdString);
        //
        // Resend the request to the replica if the following conditions are met:
        // 1. Replica did NOT respond to the request AND
        // 2. There are no outstanding messages for the target replica (messages waiting to be sent to the wire) AND
        // 3. a) Elapsed time since the last successful message send as client (any message) is larger than the timeout value OR
        //    b) The outstanding request is a Vote AND
        //       Elapsed time since the last successful vote send is larger than the timeout value
        //
        // The condition 3b) is the deadlock fix. Secondary replicas may never send reponse to a request under
        // certain conditions (take a look at the comment under ReSendCurrentVote). We have to ensure that the vote
        // is resent, note that without 3b) that would never occur if other messages are too frequent.
        //
        if ((m_responseMap.find(memberId) == m_responseMap.end()) && (replica->m_numOutstanding == 0))
        {
            bool resendMessage = false;
            if (now - replica->m_lastRequestSentAt.HiResTime() >= timeout)
            {
                resendMessage = true;

                RSLInfo("Resending message to replica",
                    LogTag_RSLMsg, msg,
                    LogTag_NumericIP, replica->m_node.m_ip,
                    LogTag_Port, replica->m_node.m_rslPort);
            }
            else if (msg->m_msgId == Message_Vote && m_outstandingVote != NULL)
            {
                // Resend the vote only if the vote is equal to the outstanding vote
                Vote* vote = (Vote*)msg;
                if (vote->IsSameDecree(m_outstandingVote->m_decree, m_outstandingVote->m_ballot))
                {
                    Int64 voteOutstandingTime = now - vote->m_receivedAt.HiResTime();

                    if (voteOutstandingTime >= timeout)
                    {
                        resendMessage = true;

                        RSLInfo("Resending vote to replica since it has been outstanding for more than the timeout interval",
                            LogTag_RSLMsg, msg,
                            LogTag_NumericIP, replica->m_node.m_ip,
                            LogTag_Port, replica->m_node.m_rslPort,
                            LogTag_UInt641, voteOutstandingTime);
                    }
                }
            }

            if (resendMessage)
            {
                SendRequestToReplica(replica, msg);
            }
        }
    }
}


void
Legislator::SendRequestToAll(Message *msg)
{
    long maxOutstanding = (long) m_cfg.MaxOutstandingPerReplica();
    CAutoPoolLockShared lock(&m_replicasLock);
    for (ReplicaIter i = m_replicas.begin(); i != m_replicas.end(); i++)
    {
        Replica *replica = *i;
        if (replica->m_numOutstanding < maxOutstanding)
        {
            SendRequestToReplica(replica, msg);
        }
    }
}

void
Legislator::SendRequestToReplica(Replica *replica, Message *msg)
{
    bool shouldSend = (replica->m_status == 1);
    // periodically, send requests to failed machines. If we are
    // able to send the request successfully, we will mark the
    // machine healthy again.
    if (replica->m_status == 0)
    {
        Int64 timeout = min(HRTIME_SECONDS(30), 3*m_cfg.SendTimeout());

        if ((GetHiResTime() - replica->m_failedAt.HiResTime()) > timeout)
        {
            shouldSend = 1;
        }
    }

    if (shouldSend && replica->m_node.m_ip != 0)
    {
        SendMessageToReplica(replica, msg);
    }
}

bool
Legislator::GetReplicaInformation(ReplicaHealth *health)
{
    if (health==NULL || !AmPrimary())
    {
        return false;
    }

    MemberId member((const char*)health->m_memberId);

    if (member.Compare(m_self->m_node.m_memberIdString) == 0)
    {
        FillCounters(m_self, health, true);
        return true;
    }
    else
    {
        CAutoPoolLockShared lock(&m_replicasLock);

        for (ReplicaIter i = m_replicas.begin(); i != m_replicas.end(); i++)
        {
            Replica *replica = *i;

            if (member.Compare(replica->m_node.m_memberIdString) != 0)
            {
                continue;
            }

            FillCounters(replica, health, false);
            return true;
        }
    }

    return false;
}

void
Legislator::FillCounters(Replica *replica, ReplicaHealth *health, bool isPrimary)
{
    if (isPrimary)
    {
        health->m_connected = true;
        health->m_consecutiveFailures = 0;
        health->m_needsResolve = 0;

        m_lock.Enter();
        health->m_lastRequestSentAt = m_maxAcceptedVote->m_receivedAt.SystemTime();
        health->m_lastRequestVotedAt = m_maxAcceptedVote->m_receivedAt.SystemTime();
        health->m_lastRequestVotedDecree = m_maxAcceptedVote->m_decree;
        health->m_lastVotePayload = m_votePayload;
        m_lock.Leave();
    }
    else
    {
        health->m_connected = replica->m_connected;
        health->m_consecutiveFailures = replica->m_consecutiveFailures;
        health->m_needsResolve = replica->m_needsResolve;
        health->m_lastRequestSentAt = replica->m_lastRequestSentAt.SystemTime();
        health->m_lastRequestVotedAt = replica->m_lastRequestVotedAt.SystemTime();
        health->m_lastRequestVotedDecree = replica->m_lastRequestVotedDecree;
        health->m_lastVotePayload = replica->m_lastVotePayload;
    }

    health->m_isPrimary = isPrimary;
    health->m_numOutstanding = replica->m_numOutstanding;
    health->m_failedAt = replica->m_failedAt.SystemTime();
}

UInt16
Legislator::GetReplicasInformation(UInt16 numEntries, ReplicaHealth *replicaSetHealth)
{
    int nrepl=0;

    if (AmPrimary())
    {
        CAutoPoolLockShared lock(&m_replicasLock);

        MemberId memberPrimary((const char*)m_self->m_node.m_memberIdString);

        for (ReplicaIter i = m_replicas.begin(); i != m_replicas.end(); i++)
        {
            Replica *replica = *i;

            if (nrepl>=numEntries)
            {
                return (UInt16)m_replicas.size();
            }

            ReplicaHealth * health = &(replicaSetHealth[nrepl]);

            memcpy(health->m_memberId, replica->m_node.m_memberIdString, sizeof(replica->m_node.m_memberIdString));
            FillCounters(replica, health, false);

            nrepl++;
        }
        //now the primary (us)
        if (nrepl>=numEntries)
        {
            return (UInt16)m_replicas.size();
        }

        ReplicaHealth * health = &(replicaSetHealth[nrepl]);

        memcpy(health->m_memberId, m_self->m_node.m_memberIdString, sizeof(m_self->m_node.m_memberIdString));
        FillCounters(m_self, health, true);
        nrepl++;
    }

    for (;nrepl<numEntries;nrepl++)
    {
        replicaSetHealth[nrepl].m_memberId[0]='\0';
    }

    return 0;
}

void
Legislator::RecordVoteAcceptedFromReplica(Message *msg)
{
    Replica *replica = msg->m_replica;

    replica->m_lastRequestVotedAt.Set();

    if (msg->m_decree > replica->m_lastRequestVotedDecree)
    {
        replica->m_lastRequestVotedDecree = msg->m_decree;
    }

    replica->m_lastVotePayload = msg->m_payload;
}

void
Legislator::ProcessSend(Packet *packet, TxRxStatus status, bool asClient)
{
    LegislatorPacket *legislatorPacket = (LegislatorPacket *) packet;
    Replica *remoteReplica = legislatorPacket->m_replica;
    UInt32 remoteIp = (asClient) ? packet->GetServerIp() : packet->GetClientIp();
    UInt16 remotePort = (asClient) ? packet->GetServerPort() : packet->GetClientPort();

    if (status != TxSuccess)
    {
        RSLInfo("Failed to send message",
                LogTag_Ptr1, packet, LogTag_NumericIP, remoteIp,
                LogTag_Port, remotePort, LogTag_StatusCode, status);
    }

    if (asClient && remoteReplica != NULL)
    {
        CAutoPoolLockShared lock(&m_replicasLock);

        for (ReplicaIter i = m_replicas.begin(); i != m_replicas.end(); i++)
        {
            Replica *replica = *i;
            if (replica == remoteReplica)
            {
                replica->m_lastRequestSentAt.Set();
                InterlockedDecrement(&replica->m_numOutstanding);
                if (status == TxSuccess)
                {
                    if (replica->m_status == 0)
                    {
                        RSLInfo("Marking replica healthy",
                                LogTag_RSLMemberId, replica->m_node.m_memberIdString);
                        replica->m_status = 1;
                    }
                    replica->m_consecutiveFailures = 0;
                    replica->m_connected=true;
                }
                else if (status != TxAbort)
                {
                    replica->m_consecutiveFailures++;
                    replica->m_connected=false;
                    m_netlibClient->CloseConnection(remoteIp, remotePort);

                    if (replica->m_consecutiveFailures >= 3)
                    {
                        if (replica->m_status == 1)
                        {
                            RSLInfo("Marking replica as failed",
                                    LogTag_RSLMemberId, replica->m_node.m_memberIdString);
                            replica->m_status = 0;
                            replica->m_failedAt.Set();
                        }
                    }
                }

                break;
            }
        }
    }
    delete packet;
}

UInt32
Legislator::QuorumSize()
{
    CAutoPoolLockShared lock(&m_replicasLock);
    return (UInt32) ((m_replicas.size()+1)/2 + 1);
}

UInt64
Legislator::GetCurrentSequenceNumber()
{
    return Interlocked::Read64((volatile Int64 *) &m_lastExecutedDecree);
}

UInt64
Legislator::GetHighestPassedSequenceNumber()
{
    return m_executeQ.GetLastPassedDecree();
}

void
Legislator::GetCurrentPrimary(RSLNode *node)
{
    // return the primary from the last accepted vote
    AutoCriticalSection lock(&m_lock);
    MemberId id = m_maxAcceptedVote->m_ballot.m_memberId;

    *node = RSLNode();

    if (id > MemberId())
    {
        if (id == m_memberId)
        {
            *node = m_self->m_node;
        }
        else
        {
            CAutoPoolLockShared replicasLock(&m_replicasLock);
            for (int i = 0; i < (int) m_replicas.size(); i++)
            {
                if (id.Compare(m_replicas[i]->m_node.m_memberIdString) == 0)
                {
                    *node = m_replicas[i]->m_node;
                    break;
                }
            }
        }
    }
}

void
Legislator::AllowSaveState(bool yes)
{
    AutoCriticalSection lock(&m_lock);
    m_checkpointAllowed = yes;
}

void
Legislator::SetVotePayload(UInt64 payload)
{
    m_votePayload = payload;
}

bool
Legislator::IsAnyRequestPendingExecution()
{
    AutoCriticalSection lock(&m_lock);

    Ptr<Vote> nextVote;

    return ( m_executeQ.Head(&nextVote) != NULL );
}

// logs the vote into the log file (if m_maxAcceptedVote != vote).
// Always sets m_maxAcceptedVote to vote.
void
Legislator::LogVote(Vote *vote)
{
    if (m_maxAcceptedVote->IsSameDecree(vote->m_decree, vote->m_ballot))
    {
        // this is the same as the previous vote, don't log this one.
        // We must change m_maxAcceptedVote.
        AutoCriticalSection lock(&m_lock);
        m_maxAcceptedVote = vote;
        return;
    }

    // make sure we don't enter the rest of the routine concurrently with other thread, since it is not reentrant.
    AutoCriticalSection logVoteLock(&m_logfileAccessLock);

    if (m_maxAcceptedVote !=NULL && !vote->IsNextDecree(m_maxAcceptedVote->m_decree, m_maxAcceptedVote->m_ballot))
    {
        AutoCriticalSection lock(&m_lock);

        if (m_maxAcceptedVote !=NULL && !vote->IsNextDecree(m_maxAcceptedVote->m_decree, m_maxAcceptedVote->m_ballot))
        {

            char buffer[220];
            char *end=NULL;

            HRESULT hr = StringCchPrintfExA(
                buffer,
                NUMELEM(buffer),
                &end,
                NULL,
                0,
                "Log entry about to be recorded with ballot in decreasing order (maxAV.ballot=%d vote.ballot=%d decree %d). Failover in progress.",
                m_maxAcceptedVote->m_ballot.m_ballotId,
                vote->m_ballot.m_ballotId,
                vote->m_decree);

            if (SUCCEEDED(hr) && end!=NULL)
            {
                LogAssert(false, buffer);
            }
            else
            {
                LogAssert(false,
                    "Log entry about to be recorded with ballot in decreasing order. Failover in progress.",
                    LogTag_UInt641, m_maxAcceptedVote->m_ballot.m_ballotId,
                    LogTag_UInt641, vote->m_ballot.m_ballotId,
                    LogTag_UInt641, vote->m_decree);
            }
        }
    }

    // if the length of the current file is too big, create a checkpoint
    // create a new log file only if we are not in the middle of checkpointing.
    LogAssert(m_logFiles.size() > 0);
    LogFile *log = m_logFiles.back();
    Int64 now = GetHiResTime();

    // By checking m_checkpointedDecree+1 >= log->m_minDecree,
    // it avoids creation of multiple log files in case checkpointing
    // takes a long time or keeps failing
    bool timeBasedCheckpoint = (m_checkpointedDecree+1 >= log->m_minDecree && m_nextCheckpointTime < now);

    if ((log->m_dataLen > m_maxLogLen || timeBasedCheckpoint) &&
        vote->m_decree > m_maxAcceptedVote->m_decree)
    {
        if (timeBasedCheckpoint == false)
        {
            RSLInfo("Switching logs due log size", LogTag_UInt641, log->m_dataLen);
        }
        else
        {
            RSLInfo("Switching logs due checkpoint interval",
                LogTag_Int64_1, m_nextCheckpointTime,
                LogTag_Int64_2, now);
        }

        m_cfg.Lock();
        UInt64 maxLen = m_cfg.MaxLogLen();
        UInt32 randomize = m_cfg.LogLenRandomize();
        m_cfg.UnLock();

        m_maxLogLen = (UInt64) Randomize(maxLen, randomize, maxLen);
        RSLInfo("Maximum log length", LogTag_UInt641, m_maxLogLen);
        log = new LogFile();
        LogAssert(log->Open(m_dataDir, vote->m_decree) == NO_ERROR);
    }

    DynamicBuffer<SIZED_BUFFER, 1024> buffers(vote->GetNumBuffers());
    vote->GetBuffers(buffers, buffers.Size());
    Int64 writeStarted = GetHiResTime();
    LogAssert(log->Write(buffers, vote->GetNumBuffers()));
    Int64 writeFinished = GetHiResTime();

    RSLDebug("Vote logged", LogTag_RSLMsg, vote, LogTag_Offset, log->m_dataLen,
             LogTag_UInt1, vote->m_numRequests);

    // Calculate total number of bytes in the votes just logged.
    Int32 cbWritten = 0;
    for (UInt32 i = 0; i < vote->GetNumBuffers(); i++)
    {
        cbWritten += buffers[(Int32)i].m_len;
    }

    {
        // Update stats under the lock.
        AutoCriticalSection statsLock(&m_statsLock);

        UInt32 writeTime = (UInt32)(writeFinished - writeStarted);
        m_stats.m_cLogWrites++;
        m_stats.m_cLogWriteBytes += cbWritten;
        m_stats.m_cLogWriteMicroseconds += writeTime;
        if (writeTime > m_stats.m_cLogWriteMaxMicroseconds)
            m_stats.m_cLogWriteMaxMicroseconds = writeTime;
    }

    // Log warning for log writes that look slow.
    if ((writeFinished - writeStarted) > HRTIME_SECONDS(1))
    {
        RSLWarning("Slow vote log", LogTag_RSLMsg, vote, LogTag_UInt641, writeFinished - writeStarted, LogTag_UInt1, cbWritten);
    }

    // Logging a vote indicates that the previous configuration is defunct.
    UpdateDefunctInfo(vote->m_configurationNumber - 1);

    AutoCriticalSection lock(&m_lock);
    if (log != m_logFiles.back())
    {
        m_logFiles.push_back(log);
    }
    m_maxAcceptedVote = vote;
    SetMaxBallot(vote->m_ballot);
    AddMessageToLog(vote);

    // checking that we are not checkpointing and setting SetSaveCheckpointAt() must be done
    // under m_lock, since the execute thread can also set SaveCheckpointAt(). We have
    // to take the lock to prevent this thread from overwriting that value
    if (log->m_minDecree > m_checkpointedDecree+1 && !IsCheckpointing() && m_state != Preparing)
    {
        RSLInfo("Starting checkpointing",
                LogTag_RSLState, m_state,
                LogTag_RSLMsg, (Vote *) m_maxAcceptedVote,
                LogTag_RSLBallot, &m_maxBallot);

        if (AmPrimary() && QuorumSize() > 1)
        {
            SetCopyCheckpointAt(log->m_minDecree-1);
            m_cpTimeout = 0;
        }
        else
        {
            SetSaveCheckpointAt(log->m_minDecree-1);
            m_mustCheckpoint = false;
        }
    }
}

void
Legislator::LogPrepare(PrepareMsg *msg)
{
    LogAssert(msg->m_ballot >= m_maxBallot);
    if (msg->m_ballot == m_maxBallot)
    {
        return;
    }
    DWORD toWrite = RoundUpToPage(msg->GetMarshalLen());
    char *buf = (char *) VirtualAlloc(NULL, toWrite, MEM_COMMIT, PAGE_READWRITE);
    LogAssert(buf);
    msg->MarshalBuf(buf, toWrite);
    msg->CalculateChecksum(buf, msg->GetMarshalLen());

    LogAssert(m_logFiles.size() > 0);
    LogFile *log = m_logFiles.back();
    SIZED_BUFFER buffers[1];
    buffers[0].m_buf = buf;
    buffers[0].m_len = toWrite;

    LogAssert(log->Write(buffers, 1));

    VirtualFree(buf, 0, MEM_RELEASE);
    RSLInfo("Prepare logged", LogTag_RSLMsg, msg, LogTag_Offset, log->m_dataLen);

    AutoCriticalSection lock(&m_lock);
    SetMaxBallot(msg->m_ballot);
    AddMessageToLog(msg);
}

void
Legislator::LogReconfigurationDecision(Message *decision)
{

    // if decistion->m_ballot is less than m_maxAcceptedVote->m_ballot
    // it means that the decision was decided after we got the vote
    // But, if it is less then maxacceptedvote was proposed after the decision
    // was taken. Paxos algorithm gaurantees that
    // if a decree d with value V has been passed then any decree d
    // proposed later with higher ballot must also have value V

    if (decision->m_decree != m_maxAcceptedVote->m_decree ||
        //decision->m_ballot > m_maxAcceptedVote->m_ballot)
        decision->m_ballot != m_maxAcceptedVote->m_ballot)
    {
        return;
    }

    LogAssert(decision->m_configurationNumber == PaxosConfiguration());

    RSLInfo("logging reconfiguration decision",
            LogTag_RSLMsg, decision, LogTag_RSLMsg, (Vote *) m_maxAcceptedVote,
            LogTag_RSLBallot, &m_maxBallot);

    LogAssert(m_maxAcceptedVote->m_isReconfiguration);

    Ptr<ConfigurationInfo> newConfiguration = new ConfigurationInfo(
        PaxosConfiguration() + 1,
        m_maxAcceptedVote->m_decree + 1,
        m_maxAcceptedVote->m_membersInNewConfiguration);

    LogAssert(m_logFiles.size() > 0);
    LogFile *log = m_logFiles.back();
    DWORD toWrite = RoundUpToPage(decision->GetMarshalLen());
    char *buf = (char *) VirtualAlloc(NULL, toWrite, MEM_COMMIT, PAGE_READWRITE);
    LogAssert(buf);
    decision->MarshalBuf(buf, toWrite);
    decision->CalculateChecksum(buf, decision->GetMarshalLen());

    SIZED_BUFFER buffers[1];
    buffers[0].m_buf = buf;
    buffers[0].m_len = toWrite;

    bool writeSuccess = log->Write(buffers, 1);
    LogAssert(writeSuccess);
    VirtualFree(buf, 0, MEM_RELEASE);
    RSLInfo("Reconfiguration decision logged",
            LogTag_RSLMsg, decision, LogTag_Offset, log->m_dataLen);

    AddToExecutionQueue(m_maxAcceptedVote);

    {
        AutoCriticalSection lock(&m_lock);
        AddMessageToLog(decision);
    }
    ChangePaxosConfiguration(newConfiguration);
}

// m_lock must already be taken
void
Legislator::AddMessageToLog(Message *msg)
{
    m_logFiles.back()->AddMessage(msg);
}

void
Legislator::StartExecuteQ()
{
    m_executeQ.Run();
}

void
Legislator::FetchServerLoop()
{
    for (;;)
    {
        while (m_acceptPaused != MainThread_Run)
        {
            if (m_acceptPaused == MainThread_Terminate)
            {
                m_acceptPaused = MainThread_Terminated;
                return;
            }
            Sleep(10);
        }

        std::unique_ptr<StreamSocket> pSock(StreamSocket::CreateStreamSocket());

        LogAssert(pSock.get());
        DWORD32 lerror = m_pFetchSocket->Accept(pSock.get(), m_cfg.ReceiveTimeout(), m_cfg.SendTimeout());
        if (lerror != NO_ERROR)
        {
            RSLError("Accept failed", LogTag_ErrorCode, lerror);
            continue;
        }

        // spawn a thread to handle the request;
        RunThread(&Legislator::HandleFetchRequest, "HandleFetchRequest", pSock.release(), Priority_OutOfGC, NULL);
    }
}

void
Legislator::HandleFetchRequest(void *ctx)
{
    std::unique_ptr<StreamSocket> pSocket((StreamSocket *) ctx);

    // decode the message from the client
    // Read the msghdr
    RSLInfo("Handling fetch request",
            LogTag_NumericIP, pSocket->GetRemoteIp(),
            LogTag_Port, pSocket->GetRemotePort());

    Message msg;
    if (!msg.ReadFromSocket(pSocket.get(), m_cfg.MaxMessageSize()))
    {
        RSLError("Failed to unmarshal message");
        return;
    }
    if (msg.m_msgId == Message_FetchVotes)
    {
        HandleFetchVotesMsg(&msg, pSocket.get());
    }
    else if (msg.m_msgId == Message_FetchCheckpoint)
    {
        HandleFetchCheckpointMsg(&msg, pSocket.get());
    }
    else if (msg.m_msgId == Message_StatusQuery)
    {
        HandleStatusQueryMsg(&msg, pSocket.get());
    }
    else
    {
        RSLError("Invalid message", LogTag_RSLMsg, &msg);
    }
    return;
}

bool
Legislator::IsCheckpointing()
{
    return (GetSaveCheckpointAt() != 0 || GetCopyCheckpointAt() != 0);
}

UInt64
Legislator::GetSaveCheckpointAt()
{
    return (Interlocked::Read64((volatile Int64 *) &m_saveCheckpointAtDecree));
}

void
Legislator::SetSaveCheckpointAt(UInt64 val)
{
    Interlocked::Exchange64((volatile Int64 *) &m_saveCheckpointAtDecree, *(Int64 *)&val);
}

UInt64
Legislator::GetCopyCheckpointAt()
{
    return (Interlocked::Read64((volatile Int64 *) &m_copyCheckpointAtDecree));
}

void
Legislator::SetCopyCheckpointAt(UInt64 val)
{
    Interlocked::Exchange64((volatile Int64 *) &m_copyCheckpointAtDecree, *(Int64 *)&val);
}

void
Legislator::CopyCheckpointFromSecondary(void *ctx)
{
    CheckpointCopyInfo *info = (CheckpointCopyInfo *) ctx;
    if (GetCopyCheckpointAt() != 0)
    {
        CopyCheckpoint(info->m_ip, info->m_port, info->m_checkpointDecree, info->m_checkpointSize, NULL, false);
        SetCopyCheckpointAt(0);
    }
    delete info;
}

bool
Legislator::SaveCheckpoint(Vote *vote, bool saveState)
{
    LogAssert(vote->m_decree > 0);
    LogAssert(vote->m_checksum != 0);
    UInt64 checkpointDecree = vote->m_decree-1;
    LogAssert(checkpointDecree == m_lastExecutedDecree);
    char file[MAX_PATH+1];
    DynString destFileName;
    RSLCheckpointStreamWriter writer;

    LogAssert(GetTempFileNameA(m_tempDir, "Codex", 0, file),
              "Failed to create temp file for checkpoint at %I64u (ErrorCode: %d)",
              checkpointDecree, GetLastError());

    RSLInfo("Writing vote to checkpoint",
            LogTag_RSLMsg, vote, LogTag_Filename, file);

    // If we are producing version 2 or higher, write the header.

    m_lock.Enter();
    CheckpointHeader header;
    header.m_version = m_version;
    header.m_memberId = m_memberId;
    header.m_lastExecutedDecree = checkpointDecree;
    header.m_stateConfiguration = m_stateConfiguration;
    header.m_nextVote = vote;
    header.m_maxBallot = m_maxBallot;
    header.m_stateSaved = saveState;
    m_lock.Leave();
    if (header.m_version >= RSLProtocolVersion_4)
    {
        header.m_checksumBlockSize = s_ChecksumBlockSize;
    }

    // Inits checkpoint file stream with header offset
    LogAssert(writer.Init(file, &header) == NO_ERROR);

    bool success = true;
    if (saveState)
    {
        RSLInfo("Calling state machine to save state");
        success = m_stateMachine->TrySaveState(&writer);
    }
    if (!success)
    {
        LogAssert(writer.Close() == NO_ERROR);
        return false;
    }

    // Write checkpoint header
    header.SetBytesIssued(&writer);
    LogAssert(writer.Close() == NO_ERROR);
    header.Marshal(file);

    RSLInfo("Checkpoint saved to file",
            LogTag_Filename, file, LogTag_UInt641, header.m_size);

    // Verify checkpoint
    if (!VerifyCheckpoint(file))
    {
        RSLError("Failed to verify the checkpoint, terminating the process to prevent codex corruption.", LogTag_Filename, file);
        LogAssert(false);
    }

    GetCheckpointFileName(destFileName, checkpointDecree);
    CheckpointDone(checkpointDecree, file, destFileName, header.m_size);

    // notify the state machine - don't need to take m_callbacklock since
    // its already taken
    m_stateMachine->StateSaved(checkpointDecree, destFileName);

    return true;
}

bool
Legislator::CopyCheckpoint(UInt32 ip, UInt16 port, UInt64 checkpointedDecree, UInt64 size, void *cookie, bool notifyStateMachine)
{
    MarshalData marshal;
    StandardMarshalMemoryManager memory(APSEQWRITE::c_writeBufSizeDefault);
    std::unique_ptr<StreamSocket> pSock(StreamSocket::CreateStreamSocket());

    SocketStreamReader reader(pSock.get());
    auto_ptr<APSEQWRITE> seqWrite(new APSEQWRITE());
    CheckpointHeader header;

    char file[MAX_PATH+1];
    DynString destFileName;

    LogAssert(GetTempFileNameA(m_tempDir, "Codex", 0, file),
              "Failed to create temp file for checkpoint at %I64u (ErrorCode: %d)",
              checkpointedDecree, GetLastError());
    LogAssert(seqWrite->DoInit(file) == NO_ERROR);

    RSLInfo("Copying checkpoint",
            LogTag_RSLDecree, checkpointedDecree, LogTag_Filename, file,
            LogTag_NumericIP, ip, LogTag_Port, port);

    Message req(
        m_version,
        Message_FetchCheckpoint,
        m_memberId,
        checkpointedDecree,
        1, // dummy Configuration number
        BallotNumber());

    req.Marshal(&marshal);

    int ec = pSock->Connect(ip, port, m_cfg.ReceiveTimeout(), m_cfg.SendTimeout());
    if (ec != NO_ERROR)
    {
        RSLInfo("Failed to connect", LogTag_ErrorCode, ec);
        goto lError;
    }

    ec = pSock->Write(marshal.GetMarshaled(), marshal.GetMarshaledLength());
    if (ec != NO_ERROR)
    {
        RSLInfo("Failed to send request", LogTag_ErrorCode, ec);
        goto lError;
    }

    if (!header.UnMarshal(&reader))
    {
        RSLInfo("Failed to read checkpoint header", LogTag_ErrorCode, ec);
        goto lError;
    }

    m_lock.Enter();
    // reset the maxballot in the header
    if (header.m_maxBallot < m_maxBallot)
    {
        header.m_maxBallot = m_maxBallot;
    }
    m_lock.Leave();

    // Marshal header
    marshal.Clear(false);
    header.Marshal(&marshal);
    LogAssert(seqWrite->Write(marshal.GetMarshaled(), marshal.GetMarshaledLength()) == NO_ERROR);

    // Marshall body
    while (reader.BytesRead() < size)
    {
        UInt32 bytesRead;
        ec = reader.Read(memory.GetBuffer(), memory.GetBufferLength(), &bytesRead);
        if (ec == NO_ERROR)
        {
            LogAssert(seqWrite->Write(memory.GetBuffer(), bytesRead) == NO_ERROR);
        }
        else
        {
            RSLInfo("Failed to read complete checkpoint",
                    LogTag_ErrorCode, ec,
                    LogTag_UInt641, seqWrite->BytesIssued(),
                    LogTag_UInt642, size);
            goto lError;
        }
    }
    LogAssert(seqWrite->Flush() == NO_ERROR);
    seqWrite->DoDispose();

    // Verify checkpoint
    if (!VerifyCheckpoint(file))
    {
        RSLError("Failed to verify the checkpoint, terminating the process to prevent codex corruption.", LogTag_Filename, file);
        LogAssert(false);
    }

    GetCheckpointFileName(destFileName, checkpointedDecree);
    CheckpointDone(checkpointedDecree, file, destFileName, size);

    if (notifyStateMachine == true)
    {
        CAutoPoolLockShared lock(&m_callbackLock);

        // notify the state machine right here
        m_stateMachine->StateCopied(checkpointedDecree, destFileName, cookie);
    }
    else
    {
        // we just copied the checkpoint from another replica,
        // we will record just the last notification, so if there is more than pending already,
        // we will discard the previous one and will record this one as the current one.
        AutoCriticalSection lock(&m_checkpointSavedNotificationLock);

        if (m_checkpointSavedNotification!=NULL)
        {
            delete m_checkpointSavedNotification;
            m_checkpointSavedNotification = NULL;
        }

        // leave the notification data, to call the state machine later
        LogAssert(m_checkpointSavedNotification == NULL);

        m_checkpointSavedNotification = new CheckpointSavedNotification(checkpointedDecree, destFileName, cookie);
    }

    return true;

  lError:
    seqWrite->DoDispose();
    DeleteFileA(file);
    return false;
}

void
Legislator::CheckpointDone(UInt64 decree, const char *tempFile, const char* file, UInt64 size)
{
    {
        AutoCriticalSection lock(&m_lock);

        if (m_cfg.MaxCheckpointSize() > 0 && size > m_cfg.MaxCheckpointSize())
        {
            RSLAlert("Checkpoint file size is too large (filesize)", LogTag_Int64_1, size);
        }
        if (m_cfg.MinCheckpointSpaceFileCount() > 0)
        {
            ULARGE_INTEGER bytesFree;
            if (GetDiskFreeSpaceExA(m_dataDir, &bytesFree, NULL, NULL))
            {
                if (bytesFree.QuadPart < m_cfg.MinCheckpointSpaceFileCount() * size)
                {
                    RSLAlert("Free space left for writing check point is too small"
                        "(freespace, checkpointsize, mincheckpointcount)",
                        LogTag_Int64_1, bytesFree.QuadPart, LogTag_Int64_1, size, LogTag_Int1, m_cfg.MinCheckpointSpaceFileCount());
                }
            }
            else {
                RSLError("failed to get checkpoint directory free space (directory, errorcode)",
                    LogTag_String1, m_dataDir.Str(), LogTag_ErrorCode, GetLastError());
            }
        }

        // rename the file if it does not exist yet.
        if (INVALID_FILE_ATTRIBUTES == GetFileAttributes(file) &&
            MoveFileExA(tempFile, file, MOVEFILE_WRITE_THROUGH) == FALSE)
        {
            int ec = GetLastError();
            RSLError("failed to move checkpoint", LogTag_Filename, tempFile,
                     LogTag_Filename, file, LogTag_ErrorCode, ec);
            LogAssert(ec != NO_ERROR);
        }

        if (decree > m_checkpointedDecree)
        {
            m_checkpointSize = size;
            m_checkpointedDecree = decree;

            if (m_nextCheckpointTime != _I64_MAX)
            {
                m_cfg.Lock();
                Int64 maxCpInterval = m_cfg.MaxCheckpointInterval();
                m_cfg.UnLock();

                Int64 rand = (Int64) Randomize(maxCpInterval, 20, maxCpInterval);
                m_nextCheckpointTime = GetHiResTime() + rand;
                RSLInfo("Next check point is scheduled for", LogTag_Int64_1, m_nextCheckpointTime);
            }

        }
    }
    CleanupLogsAndCheckpoint();
}

void
Legislator::CleanupLogsAndCheckpoint()
{
    char *cpPrefix = "*.codex";
    char *logPrefix = "*.log";
    vector<UInt64> checkpoints;
    vector<UInt64> logs;

    LogAssert(GetFileNumbers(m_dataDir, cpPrefix, checkpoints) == NO_ERROR);
    LogAssert(GetFileNumbers(m_dataDir, logPrefix, logs) == NO_ERROR);

    m_cfg.Lock();
    UInt32 maxCheckpoints = m_cfg.MaxCheckpoints();
    UInt32 maxLogs = m_cfg.MaxLogs();
    m_cfg.UnLock();

    {
        AutoCriticalSection lock(&m_lock);
        UInt64 minDecree = min(m_checkpointedDecree, GetCurrentSequenceNumber());
        // delete all the logs before the checkpoint
        while (m_logFiles.size() > 1 && m_logFiles[1]->m_minDecree <= minDecree+1)
        {
            delete m_logFiles[0];
            m_logFiles.erase(m_logFiles.begin());
        }
    }
    LogAssert(maxCheckpoints > 0 && maxLogs > 0);
    // delete old checkpoints
    for (int i = 0; maxCheckpoints+i < checkpoints.size(); i++)
    {
        DynString cpFile;
        GetCheckpointFileName(cpFile, checkpoints[i]);
        if (!DeleteFileA(cpFile))
        {
            RSLError("Failed to delete checkpoint file",
                     LogTag_Filename, cpFile.Str(), LogTag_ErrorCode, GetLastError());
        }
    }
    //delete old logs
    for (int i = 0; logs.size()-i > maxLogs && logs[i+1] <= checkpoints[0]+1; i++)
    {
        DynString logFile(m_dataDir);
        logFile.AppendF("%I64u.log", logs[i]);
        if (!DeleteFileA(logFile))
        {
            RSLError("Failed to delete log file",
                     LogTag_Filename, logFile.Str(), LogTag_ErrorCode, GetLastError());
        }
    }
}

bool
Legislator::VerifyCheckpoint(const char* filename)
{
    CheckpointHeader header;
    if (!header.UnMarshal(filename))
    {
        RSLError("Could not read checkpoint header", LogTag_Filename, filename);
        return false;
    }

    int ec = NO_ERROR;
    RSLCheckpointStreamReader reader;
    if ((ec = reader.Init(filename, &header)) != NO_ERROR)
    {
        RSLError("Failed to read checkpoint", LogTag_Filename, filename, LogTag_ErrorCode, ec);
        return false;
    }

    return true;
}

void
Legislator::GetCheckpointFileName(DynString &file, UInt64 decree)
{
    file.Set(m_dataDir);
    CheckpointHeader::GetCheckpointFileName(file, decree);
}

DWORD32
Legislator::GetFileNumbers(DynString &dir, char *prefix, vector<UInt64> &numbers)
{
    DynString szBuf(dir);
    szBuf.Append(prefix);

    DWORD32 ec = NO_ERROR;
    HANDLE  handleFind = INVALID_HANDLE_VALUE;
    WIN32_FIND_DATAA find;

    handleFind = FindFirstFileA(szBuf, &find);

    if (handleFind == INVALID_HANDLE_VALUE)
    {
        ec = GetLastError();
        if (ec == ERROR_NO_MORE_FILES || ec == ERROR_FILE_NOT_FOUND)
        {
            // these ecs indicate that there are no files
            ec = NO_ERROR;
        }
        return ec;
    }

    do
    {
        // skip special names "." and ".." by ignoring files that start with .
        //
        if (find.cFileName[0] == '.')
        {
            continue;
        }

        UInt64 val;
        if (sscanf_s(find.cFileName, "%I64u", &val) <= 0)
        {
            FindClose(handleFind);
            return ERROR_INVALID_PARAMETER;
        }
        numbers.push_back(val);
    } while (FindNextFileA(handleFind, &find));

    ec = GetLastError();
    if (ec == ERROR_NO_MORE_FILES)
    {
        ec = NO_ERROR;
    }
    sort(numbers.begin(), numbers.end());
    FindClose(handleFind);
    return ec;
}

UInt64
Legislator::GetLastWriteFileTime(const char * fileName)
{
    WIN32_FIND_DATAA find;
    HANDLE handleFind = FindFirstFileA(fileName, &find);
    if (handleFind != INVALID_HANDLE_VALUE)
    {
        ULARGE_INTEGER uliFileTime;
        uliFileTime.HighPart = find.ftLastWriteTime.dwHighDateTime;
        uliFileTime.LowPart = find.ftLastWriteTime.dwLowDateTime;
        FindClose(handleFind);

        // FILETIME is in units of 100ns, so divide by 10 to get micro seconds
        Int64 fileTimeInUs = HRTIME_USECONDS(uliFileTime.QuadPart / 10);
        return fileTimeInUs;
    }
    return 0;
}

bool
Legislator::RestoreState(UInt64 maxDecree, bool readOnly, UInt32 initialConfigNumber)
{
    char *cpPrefix = "*.codex";
    char *logPrefix = "*.log";
    vector<UInt64> checkpoints;
    vector<UInt64> logs;

    int ec;

    if ((ec = GetFileNumbers(m_dataDir, cpPrefix, checkpoints)) != NO_ERROR ||
        (ec = GetFileNumbers(m_dataDir, logPrefix, logs)) != NO_ERROR)
    {
        RSLError("Failed to enumerate working directory", LogTag_ErrorCode, ec);
        return false;
    }

    RSLNodeCollection emptySet;
    MemberSet *initialMemberSet = new MemberSet(emptySet, NULL, 0);
    m_stateConfiguration = new ConfigurationInfo(initialConfigNumber,
                                                 0, // initial decree number is 0
                                                 initialMemberSet);
    m_paxosConfiguration = m_stateConfiguration;

    m_maxAcceptedVote = new Vote(m_version, m_memberId,
        m_stateConfiguration->GetInitialDecree(),
        m_stateConfiguration->GetConfigurationNumber(),
        BallotNumber(), m_primaryCookie);
    m_maxAcceptedVote->CalculateChecksum();

    Ptr<Vote> cpVote;
    int i = -1;

    UInt64 cpFileTime = 0;
    for (i = (int)checkpoints.size()-1; i >= 0; i--)
    {
        if (checkpoints[i] > maxDecree)
        {
            continue;
        }

        m_checkpointedDecree = checkpoints[i];
        m_lastExecutedDecree = m_checkpointedDecree;

        if (logs.size() > 0 && logs[logs.size()-1] > m_checkpointedDecree+1)
        {
            int logNo;
            // make sure we have the logs after this point
            for (logNo = (int) logs.size()-1;
                 logNo >= 0 && logs[logNo] > m_checkpointedDecree+1;
                 logNo--)
                ;
            if (logNo < 0)
            {
                RSLError("Missing logs after checkpoint at decree",
                         LogTag_RSLDecree, m_checkpointedDecree);
                return false;
            }
        }

        DynString cpFile;
        GetCheckpointFileName(cpFile, m_checkpointedDecree);

        CheckpointHeader header;
        if (!header.UnMarshal(cpFile))
        {
            RSLError("Could not read checkpoint header", LogTag_Filename, cpFile.Str());
            continue;
        }

        m_checkpointSize = header.m_size;

        RSLCheckpointStreamReader reader;
        if ((ec = reader.Init(cpFile, &header)) != NO_ERROR)
        {
            RSLError("Failed to read checkpoint",
                     LogTag_Filename, cpFile.Str(), LogTag_ErrorCode, ec);
            continue;
        }

        bool loadStateResult = false;
        if (header.m_stateSaved)
        {
            loadStateResult = m_stateMachine->LoadState(&reader);
        }
        else
        {
            RSLInfo("No machine state on checkpoint");
            loadStateResult = m_stateMachine->LoadState(NULL);
        }
        if (loadStateResult)
        {
            if (header.m_stateConfiguration != NULL)
            {
                m_stateConfiguration = header.m_stateConfiguration;
                m_paxosConfiguration = m_stateConfiguration;

                RSLDebug("Restoring checkpoint configuration",
                         LogTag_UInt1, m_stateConfiguration->GetConfigurationNumber(),
                         LogTag_UInt2, m_stateConfiguration->GetNumMembers());

            }

            cpVote = header.m_nextVote;
            m_maxBallot = header.m_maxBallot;
            cpFileTime = GetLastWriteFileTime(cpFile.Str());
            LogAssert(m_maxBallot >= cpVote->m_ballot);
            break;
        }
        RSLError("StateMachine LoadState() returned failure",
                 LogTag_Filename, cpFile.Str());
    }
    if (i < 0)
    {
        m_checkpointSize = 0;
        m_checkpointedDecree = 0;
        m_lastExecutedDecree = 0;

        if (logs.size() > 0 && logs[0] != 0)
        {
            RSLError("Missing logs until decree",
                     LogTag_RSLDecree, logs[0]);
            return false;
        }
        m_stateMachine->LoadState(NULL);
        if (logs.size() == 0)
        {
            m_logFiles.push_back(new LogFile());
            if (!readOnly && m_logFiles.back()->Open(m_dataDir, 0) != NO_ERROR)
            {
                return false;
            }
            return true;
        }
    }

    for (i = (int)logs.size()-1; i >= 0 && logs[i] > m_checkpointedDecree+1; i--)
        ;

    UInt64 lastLogFileTime = 0;
    BallotNumber emptyBallot;
    for (; i >= 0 && i < (int) logs.size() && m_lastExecutedDecree < maxDecree; i++)
    {
        LogFile *log = new LogFile();
        m_logFiles.push_back(log);
        if (log->Open(m_dataDir, logs[i]) != NO_ERROR)
        {
            return false;
        }
        UInt64 fileTime = GetLastWriteFileTime(log->m_fileName);
        if (cpFileTime == 0)
        {
            cpFileTime = fileTime;
        }
        if (lastLogFileTime < fileTime)
        {
            lastLogFileTime = fileTime;
        }

        APSEQREAD *seqRead = new APSEQREAD();
        LogAssert(seqRead);
        if ((ec = seqRead->DoInit(log->m_fileName, 2, s_AvgMessageLen, true)) != NO_ERROR)
        {
            RSLError("Failed to read log file",
                     LogTag_Filename, log->m_fileName, LogTag_ErrorCode, ec);
            return false;
        }

        DiskStreamReader reader(seqRead);
        StandardMarshalMemoryManager memory(s_AvgMessageLen);
        while (m_lastExecutedDecree < maxDecree)
        {
            Message *msg;
            if (ReadNextMessage(&reader, &memory, &msg, true) == false)
            {
                return false;
            }
            if (msg == NULL)
            {
                break;
            }

            LogAssert(msg->m_ballot != emptyBallot);
            AddMessageToLog(msg);
            LogAssert(log->m_dataLen == reader.BytesRead());
            if (msg->m_decree < m_lastExecutedDecree)
            {
                LogAssert(m_maxBallot == BallotNumber() || m_maxBallot >= msg->m_ballot);
                delete msg;
                continue;
            }
            else if (msg->m_decree == m_lastExecutedDecree)
            {
                if (msg->m_msgId != Message_Prepare)
                {
                    LogAssert(m_maxBallot == BallotNumber() || m_maxBallot >= msg->m_ballot);
                }
                else
                {
                    LogAssert(m_maxBallot == BallotNumber() || m_maxBallot >= msg->m_ballot || (m_maxBallot.m_ballotId + 1 == msg->m_ballot.m_ballotId));
                }
                delete msg;
                continue;
            }

            if (msg->m_msgId == Message_Prepare)
            {
                if (msg->m_ballot > m_maxBallot)
                {
                    m_maxBallot = msg->m_ballot;
                }
                delete msg;
                continue;
            }

            if (msg->m_msgId == Message_ReconfigurationDecision)
            {
                LogAssert(m_maxAcceptedVote->m_decree == msg->m_decree &&
                          m_maxAcceptedVote->m_ballot >= msg->m_ballot);
                LogAssert(m_maxAcceptedVote->m_isReconfiguration == 1);

                LogAssert(msg->m_decree > m_lastExecutedDecree);
                bool shouldSave = false;
                ExecuteVote(m_maxAcceptedVote, &shouldSave);
                LogAssert(shouldSave == false);

                m_paxosConfiguration = m_stateConfiguration;
                m_maxAcceptedVote = new Vote(m_version,
                                             m_memberId,
                                             msg->m_decree + 1,
                                             0,
                                             BallotNumber(),
                                             m_primaryCookie);

                m_maxBallot = BallotNumber(m_maxBallot.m_ballotId + 1, MemberId());
                delete msg;
                continue;
            }

            LogAssert(msg->m_msgId == Message_Vote);
            Ptr<Vote> vote = (Vote *) msg;

            if (m_maxAcceptedVote->m_ballot != emptyBallot ||
                m_maxAcceptedVote->m_decree != 0)
            {
                LogAssert(vote->IsNextDecree(m_maxAcceptedVote->m_decree,
                                             m_maxAcceptedVote->m_ballot));

                if (vote->m_decree == m_maxAcceptedVote->m_decree+1)
                {
                    bool shouldSave = false;
                    ExecuteVote(m_maxAcceptedVote, &shouldSave);
                    if (shouldSave && !readOnly)
                    {
                        if (!SaveCheckpoint(vote))
                        {
                            m_checkpointAllowed = false;
                        }
                        cpVote = vote;
                    }
                }
            }

            m_maxAcceptedVote = vote;
            if (vote->m_ballot > m_maxBallot)
            {
                m_maxBallot = vote->m_ballot;
            }
        }
        delete seqRead;
    }

    // Using the time of the checkpoint and last saved log, it is possible
    // to determine how long the replica worked for before going down.
    // This information is used to schedule the checkpoint to an earlier time
    if (m_nextCheckpointTime != _I64_MAX && lastLogFileTime > cpFileTime)
    {
        Int64 timeUpInUs = (Int64)(lastLogFileTime - cpFileTime);
        if (m_nextCheckpointTime - timeUpInUs > GetHiResTime())
        {
            m_nextCheckpointTime -= timeUpInUs;
            RSLInfo("Next check point is scheduled for", LogTag_Int64_1, m_nextCheckpointTime);
        }
    }

    if (cpVote != NULL && cpVote->m_decree > m_maxAcceptedVote->m_decree)
    {
        // Whenever we copy a checkpoint, we make sure that the copied
        // checkpoint has all the votes executed up until the
        // maxacceptedVote. The cpVote in the checkpoint will be
        // m_maxAccepted+1.  we don't have this decree in any log. We
        // probably copied this file from a replica.

        for (int pos = 0; pos < (int) m_logFiles.size(); pos++)
        {
            delete m_logFiles[pos];
        }
        m_logFiles.clear();
        LogFile *log = new LogFile();
        m_logFiles.push_back(log);
        if (!readOnly)
        {
            if (log->Open(m_dataDir, cpVote->m_decree) != NO_ERROR)
            {
                return false;
            }

            // if cpVote->m_ballot is empty, this means that the checkpoint
            // was taken right after a reconfiguration. We don't have to log
            // this fake decree to disk
            if (cpVote->m_ballot != emptyBallot)
            {
                DynamicBuffer<SIZED_BUFFER, 1024> buffers(cpVote->GetNumBuffers());
                cpVote->GetBuffers(buffers, buffers.Size());
                if (log->Write(buffers, cpVote->GetNumBuffers()) == false)
                {
                    return false;
                }
                log->AddMessage(cpVote);
            }
        }
        m_maxAcceptedVote = cpVote;
    }

    LogAssert(m_lastExecutedDecree == 0 ||
              m_lastExecutedDecree == m_maxAcceptedVote->m_decree-1);
    if (!readOnly && m_logFiles.back()->SetWritePointer() != NO_ERROR)
    {
        return false;
    }

    return true;
}

bool
Legislator::Start(
    RSLConfigParam *cfg,
    const RSLNodeCollection &membersOfInitialConfiguration,
    const RSLNode &self,
    RSLProtocolVersion version,
    bool serializeFastReadsWithReplicate,
    RSLStateMachine *sm)
{
    Ptr<MemberSet> memberset(new MemberSet(membersOfInitialConfiguration, NULL, 0));

    if (memberset->Verify(version) == false)
    {
        RSLError("version is not compatible with this library");
        Logger::CallNotificationsIfDefined(LogLevel_Error, LogID_RSLLIB, "version is not compatible with this library");
        return false;
    }

    m_isInitializeCompleted = StartImpl(cfg, membersOfInitialConfiguration, self, version,
        serializeFastReadsWithReplicate, sm);
    return m_isInitializeCompleted;
}

bool
Legislator::Start(
    RSLConfigParam *cfg,
    const RSLNode &self,
    RSLProtocolVersion version,
    bool serializeFastReadsWithReplicate,
    RSLStateMachine *sm)
{
    if (version <= RSLProtocolVersion_3)
    {
        RSLError("This method is not supported for versions prior RSLProtocolVersion_3");
        Logger::CallNotificationsIfDefined(LogLevel_Error, LogID_RSLLIB, "This method is not supported for versions prior RSLProtocolVersion_3");
        return false;
    }
    RSLNodeCollection emptySet;

    m_isInitializeCompleted = StartImpl(cfg, emptySet, self, version, serializeFastReadsWithReplicate, sm);
    return m_isInitializeCompleted;
}


bool
Legislator::StartImpl(
    RSLConfigParam *cfg,
    const RSLNodeCollection &membersOfInitialConfiguration,
    const RSLNode &self,
    RSLProtocolVersion version,
    bool serializeFastReadsWithReplicate,
    RSLStateMachine *sm)
{
    if (cfg == NULL)
    {
        RSLError("RSLConfigParam cannot be NULL");
        return false;
    }

    AutoCriticalSection lock(&m_lock);

    m_memberId = MemberId(self.m_memberIdString);
    m_self = new Replica(self);

    if (!Netlib::Initialize())
    {
        Logger::CallNotificationsIfDefined(LogLevel_Error, LogID_RSLLIB, "Could not initialize Netlib");
        return false;
    }

    LogAssert(Message::IsVersionValid((UInt16) version));
    m_version = version;
    m_serializeFastReadsWithReplicate = serializeFastReadsWithReplicate;
    srand((unsigned int) time(NULL));
    m_stateMachine = sm;

    if (m_cfg.Reload(cfg) == false)
    {
        Logger::CallNotificationsIfDefined(LogLevel_Error, LogID_RSLLIB, "Could not load config");
        return false;
    }

    m_cfg.Lock();
    m_electionDelay = Randomize(m_cfg.ElectionDelay(),
                                m_cfg.ElectionRandomize(),
                                m_cfg.MaxElectionRandomize());

    Int64 maxCpInterval = m_cfg.MaxCheckpointInterval();

    m_maxLogLen = (UInt64) Randomize(m_cfg.MaxLogLen(),
                                     m_cfg.LogLenRandomize(),
                                     m_cfg.MaxLogLen());
    m_cfg.UnLock();

    RSLInfo("Maximum log length", LogTag_UInt641, m_maxLogLen);

    m_nextCheckpointTime = _I64_MAX;
    if (maxCpInterval > 0 && maxCpInterval != _I64_MAX)
    {
        m_nextCheckpointTime = GetHiResTime() + (Int64) Randomize(maxCpInterval, 50, maxCpInterval);
        RSLInfo("Next check point is scheduled for", LogTag_Int64_1, m_nextCheckpointTime);
    }

    m_actionTimeout = _I64_MAX;
    m_nextElectionTime = _I64_MAX;
    m_cpTimeout = _I64_MAX;

    APSEQREAD::SetCancelDiskIo(m_cfg.CancelDiskIo());
    RSLInfo("CancelDiskIo", LogTag_Int1, (int)m_cfg.CancelDiskIo());

    m_cfg.GetWorkingDir(m_dataDir);

    int ec;
    if (!CreateDirectoryA(m_dataDir, NULL) &&
        (ec = GetLastError()) != ERROR_ALREADY_EXISTS)
    {
        RSLError("Failed to create directory",
                 LogTag_String1, m_dataDir.Str(), LogTag_ErrorCode, ec);
        Logger::CallNotificationsIfDefined(LogLevel_Error, LogID_RSLLIB, "Failed to create data directory");
        return false;
    }

    // Verify that the device to which transaction journals will be written has a physical sector size of 512 bytes. We have interoperability problems with logs
    // written on different sector size disks since the sector size becomes part of the format. Additionally we don't trust devices with different sector sizes
    // to support the reliability guarantees we require (atomic write).
    VerifyDiskSectorSize();

    if (m_dataDir[m_dataDir.Length()-1] != '\\')
    {
        m_dataDir.Append('\\');
    }

    if (m_cfg.AddMemberIdToWorkingDir() == true)
    {
        m_dataDir.AppendF("%s\\", m_memberId.GetValue());
    }

    m_tempDir = m_dataDir;
    m_tempDir.AppendF("Temp\\");

    if (!CreateDirectoryA(m_dataDir, NULL) &&
        (ec = GetLastError()) != ERROR_ALREADY_EXISTS)
    {
        RSLError("Failed to create directory",
                 LogTag_String1, m_dataDir.Str(), LogTag_ErrorCode, ec);

        Logger::CallNotificationsIfDefined(LogLevel_Error, LogID_RSLLIB, "Failed to create data member directory");
        return false;
    }
    if (!CreateDirectoryA(m_tempDir, NULL) &&
        (ec = GetLastError()) != ERROR_ALREADY_EXISTS)
    {
        RSLError("Failed to create directory",
                 LogTag_String1, m_tempDir.Str(), LogTag_ErrorCode, ec);
        Logger::CallNotificationsIfDefined(LogLevel_Error, LogID_RSLLIB, "Failed to create temp directory");
        return false;
    }

    // delete all the files in the temp directory
    WIN32_FIND_DATAA wfd;
    DynString pattern(m_tempDir);
    pattern.Append('*');

    HANDLE hf = FindFirstFileA(pattern, &wfd);
    if (hf != INVALID_HANDLE_VALUE)
    {
        do
        {
            if (!(wfd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
            {
                DynString fileName(m_tempDir);
                fileName.Append(wfd.cFileName);
                DeleteFileA(fileName);
            }
        } while (FindNextFileA(hf, &wfd));
        FindClose(hf);
    }

    ReadDefunctFile();

    // This is for backward compatibility. When user has used old API
    // (in new new API, membersOfInitialConfiguration is always empty)
    // then initial configuration number should be 1
    UInt32 initialConfigNumber = (membersOfInitialConfiguration.Count() > 0) ? 1 : 0;
    if (!RestoreState(_I64_MAX, false, initialConfigNumber))
    {
        Logger::CallNotificationsIfDefined(LogLevel_Error, LogID_RSLLIB, "Failed to restore state");
        return false;
    }

    // This is for backward compatibility. If there is no
    // MemberSet checkpointed and user has used old API (in
    // new new API, membersOfInitialConfiguration is always
    // empty) then use what was passed by the user
    if (m_stateConfiguration->GetNumMembers() == 0 && membersOfInitialConfiguration.Count() > 0)
    {
        RSLWarning("Loading replica set through deprecated API");
        MemberSet *initialMemberSet = new MemberSet(membersOfInitialConfiguration, NULL, 0);
        m_stateConfiguration = new ConfigurationInfo(
            m_stateConfiguration->GetConfigurationNumber(),
            m_stateConfiguration->GetInitialDecree(),
            initialMemberSet);
        m_paxosConfiguration = m_stateConfiguration;
    }

    // Initialize statistics capture.
    ResetStatistics();

    // we need to initialize the queue synchronously right here, since other threads
    // assume it is initialized.
    // The StartExecuteQ thread will only run the loop.
    // In this way, we don't depend on the order of execution of the threads.
    this->m_executeQ.Init(this);
    RunThread(&Legislator::StartExecuteQ, "StartExecuteQ");

    PacketFactory *factory = new PacketFactory(m_cfg.MaxMessageSize(), m_cfg.MaxMessageAlertSize());
    LegislatorNetHandler *handler = new LegislatorNetHandler(this, false);
    m_netlibServer = new NetPacketSvc(32*1024);
    if (m_netlibServer->StartAsServer(m_self->m_node.m_rslPort, handler, handler, NULL, factory, s_ListenOnAllIPs ? INADDR_ANY : m_self->m_node.m_ip) != 0)
    {
        RSLError("Failed to start netlib Server");
        Logger::CallNotificationsIfDefined(LogLevel_Error, LogID_RSLLIB, "Failed to start netlib Server");
        return false;
    }

    handler = new LegislatorNetHandler(this, true);
    m_netlibClient = new NetPacketSvc(32*1024);
    if (m_netlibClient->StartAsClient(handler, handler, NULL, factory, s_ListenOnAllIPs ? INADDR_ANY : m_self->m_node.m_ip) != 0)
    {
        RSLError("Failed to start netlib client");
        Logger::CallNotificationsIfDefined(LogLevel_Error, LogID_RSLLIB, "Failed to start netlib client");
        return false;
    }

    // retry 120 times. wait 1 second between retries (total 2 minutes)
    DWORD32 error;
    if(s_ListenOnAllIPs)
    {
        error = m_pFetchSocket->BindAndListen(m_self->m_node.m_rslLearnPort, 1024, 120, 1);
    }
    else
    {
        error = m_pFetchSocket->BindAndListen(m_self->m_node.m_ip, m_self->m_node.m_rslLearnPort, 1024, 120, 1);
    }

    if (NO_ERROR != error)
    {
        Logger::CallNotificationsIfDefined(LogLevel_Error, LogID_RSLLIB, "Failed to bind on learn port");
        return false;
    }

    // spawn a thread to handle the request;
    RunThread(&Legislator::FetchServerLoop, "FetchServerLoop");

    {
        // Lock already acquired in the outer scope.
        // Unnecessary? Left unchanged for backwards equivalence.
        AutoCriticalSection lock2(&m_lock);

        // If there is a configuration saved in checkpoint
        if (HasConfigurationCheckpointed() == false)
        {
            if (IsBootstrapped())
            {
                RSLError("Machine is already bootstrapped but has no replica set info."
                    " If you're upgrading from Version 1 or 2, use old API and"
                    " pass current replica set information.");
                Logger::CallNotificationsIfDefined(LogLevel_Error, LogID_RSLLIB, "Machine is already bootstrapped but has no replica set info");
                return false;
            }
            RSLInfo("No Replica Set checkpointed");
        }
    }
    RunThread(&Legislator::IPLookup, "IPLookup");
    RunThread(&Legislator::MainLoop, "MainLoop", Priority_Highest);
    RunThread(&Legislator::ExecuteLoop, "ExecuteLoop");

    for (UInt32 i = 0; i < m_cfg.NumReaderThreads(); i++)
    {
        RunThread(&Legislator::FastReadLoop, "FastReadLoop");
    }

    RunThread(&Legislator::HeartBeatLoop, "HeartBeatLoop", Priority_OutOfGC);

    return true;
}

// Verify that the device to which transaction journals will be written has a physical sector size of 512 bytes. We have interoperability problems with logs
// written on different sector size disks since the sector size becomes part of the format. Additionally we don't trust devices with different sector sizes to
// support the reliability guarantees we require (atomic write).
void
Legislator::VerifyDiskSectorSize()
{
    char chDevice = '\0';
    if ((m_dataDir.Length() >= 2) && (m_dataDir[1] == ':'))
    {
        // Absolute path given, device label is the first character.
        chDevice = m_dataDir[0];
    }
    else
    {
        // Relative path given, determine device label from current directory.
        DWORD cchBuffer = GetCurrentDirectory(0, nullptr);
        if (cchBuffer > 0)
        {
            char * pBuffer = (char*)_alloca(cchBuffer);
            if (GetCurrentDirectory(cchBuffer, pBuffer))
            {
                chDevice = pBuffer[0];
            }
        }
    }

    // Proceed only with an alphabetic device name (i.e. 'A' through 'Z'). We might have been given an SMB share path ("//system/share") which should never
    // happen in production but is valid in some tool use cases. We can't extract a physical sector size from shares obviously.
    if (isalpha(chDevice))
    {
        // Open a device handle so we can post the IOCTL for disk geometry to it.
        char szDevicePath[] = "\\\\.\\X:";
        szDevicePath[4] = chDevice;
        HANDLE hDevice = CreateFile(szDevicePath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (hDevice != INVALID_HANDLE_VALUE)
        {
            // DISK_GEOMETRY_EX is a variable sized structure but only for future expansion (i.e. for any given OS version it is one fixed size). Unfortunately
            // there isn't a guaranteed way to get DeviceIoControl to give us the correct size. So we'll use the size we have today (a DISK_GEOMETRY_EX +
            // DISK_PARTITION_INFO + DISK_DETECTION_INFO) and multiply it by two to give us plenty of headroom (realistically it's not going to grow much given
            // the type of data being stored).
            ULONG cbBuffer = sizeof(DISK_GEOMETRY_EX) + sizeof(DISK_PARTITION_INFO) + sizeof(DISK_DETECTION_INFO) * 2;
            BYTE * pbBuffer = new BYTE[cbBuffer];
            ULONG cbUsed;
            if (DeviceIoControl(hDevice,
                                IOCTL_DISK_GET_DRIVE_GEOMETRY_EX,
                                nullptr,
                                0,
                                pbBuffer,
                                cbBuffer,
                                &cbUsed,
                                nullptr))
            {
                DISK_GEOMETRY_EX * pGeometry = (DISK_GEOMETRY_EX*)pbBuffer;
                if (pGeometry->Geometry.BytesPerSector == s_PageSize)
                {
                    RSLInfo("Logging device using supported sector size",
                            LogTag_String1, szDevicePath,
                            LogTag_UInt1, s_PageSize);
                }
                else
                {
                    LogAssert(false, "Unsupported logging device sector size",
                              LogTag_String1, szDevicePath,
                              LogTag_UInt1, pGeometry->Geometry.BytesPerSector);
                }
            }
            else
            {
                RSLError("Unable to query logging device for disk geometry",
                         LogTag_String1, szDevicePath, LogTag_ErrorCode, GetLastError());
            }

            delete [] pbBuffer;
            CloseHandle(hDevice);
        }
        else
        {
            RSLError("Unable to acquire handle to logging device",
                     LogTag_String1, szDevicePath, LogTag_ErrorCode, GetLastError());
        }
    }
    else
    {
        RSLError("Unable to determine logging device from working or current directory",
                 LogTag_String1, m_dataDir.Str());
    }
}

RSLResponseCode
Legislator::Bootstrap(MemberSet * memberSet, UInt32 timeout)
{
    RSLResponseCode ret = StartBootstrap(memberSet);

    if (ret != RSLSuccess)
    {
        return ret;
    }

    Int64 hrTimeout = GetHiResTime() + HRTIME_SECONDS(timeout);
    while (IsBootstrapped() == false)
    {
        if (GetHiResTime() > hrTimeout)
        {
            return RSLBootstrapFailed;
        }

        RSLInfo("Sending Bootstrap Messages");
        BootstrapMsg msg(m_version, m_memberId, *memberSet);

        SendRequestToAll(&msg);

        Sleep(1000);
    }

    RSLInfo("Bootstrap succeeded");
    return RSLSuccess;
}

bool
Legislator::HasConfigurationCheckpointed()
{
    AutoCriticalSection lock(&m_lock);
    return (m_stateConfiguration != NULL) &&
           (m_stateConfiguration->GetNumMembers() > 0);
}

void
Legislator::StopMainLoop()
{
    LogAssert(m_paused != MainThread_Terminate);
    m_paused = MainThread_Terminate;
    SetEvent(this->m_waitHandle);

    // wait for the threads to terminate
    while (m_paused != MainThread_Terminated )
    {
        Sleep(10);
    }

    {
        AutoCriticalSection lock(&m_recvQLock);
        m_voteCache.Clear();
    }
}

void
Legislator::StopHeartBeatLoop()
{
    LogAssert(m_hbPaused != MainThread_Terminate);
    m_hbPaused = MainThread_Terminate;
    SetEvent(this->m_hbWaitHandle);

    // wait for the threads to terminate
    while (m_hbPaused != MainThread_Terminated )
    {
        Sleep(10);
    }
}

void
Legislator::StopIPLoop()
{
    LogAssert(m_ipPaused != MainThread_Terminate);
    m_ipPaused = MainThread_Terminate;
    SetEvent(this->m_resolveWaitHandle);

    // wait for the threads to terminate
    while (m_ipPaused != MainThread_Terminated)
    {
        Sleep(10);
    }
}

void
Legislator::StopExeLoop()
{
    LogAssert(m_exePaused != MainThread_Terminate);
    m_exePaused = MainThread_Terminate;

    // wait for the threads to terminate
    while (m_exePaused != MainThread_Terminated)
    {
        Sleep(10);
    }
}
void
Legislator::StopFastReadLoops()
{
    LogAssert(m_frPaused != MainThread_Terminate);
    m_frPaused = MainThread_Terminate;
    SetEvent(this->m_fastReadWaitHandle);

    // wait for the threads to terminate
    while (m_frPaused != MainThread_Terminated)
    {
        Sleep(10);
    }
}

void
Legislator::StopAcceptLoop()
{
    LogAssert(m_acceptPaused != MainThread_Terminate);
    m_acceptPaused = MainThread_Terminate;
    m_pFetchSocket->Cancel();

    // wait for the threads to terminate
    while (m_acceptPaused != MainThread_Terminated)
    {
        Sleep(10);
    }
}
void
Legislator::StopNet()
{
    m_netlibClient->Stop();
    m_netlibServer->Stop();
}

Legislator::~Legislator()
{
    if (m_netlibServer != NULL)
    {
        delete m_netlibServer;
        m_netlibServer = NULL;
    }

    if (m_netlibClient != NULL)
    {
        delete m_netlibClient;
        m_netlibClient = NULL;
    }

    for (int pos = 0; pos < (int) m_logFiles.size(); pos++)
    {
        delete m_logFiles[pos];
    }
    m_logFiles.clear();

    for (int pos = 0; pos < (int) m_replicas.size(); pos++)
    {
        //m_replicas[pos];
        m_replicas[pos]->DownCount();
    }
    m_replicas.clear();

    if (m_self!=NULL)
    {
        delete m_self;
        m_self = NULL;
    }

    {
        AutoCriticalSection lock(&m_recvQLock);

        m_statusMap.Clear();
        m_voteCache.Clear();
        Message *msg;
        while ((msg = m_recvMsgQ.dequeue()) != NULL)
        {
            delete msg;
        }

        if (m_executeQ.IsInitialized())
        {
            // remove all the requests in the m_executeq.
            for (;;)
            {
                Ptr<Vote> vote;

                m_executeQ.Dequeue(&vote);

                if (vote == NULL)
                {
                    break;
                }

            }
        }
    }

    //cleanup resources
    CloseHandle(this->m_resolveWaitHandle);
    CloseHandle(this->m_waitHandle);
    CloseHandle(this->m_fastReadWaitHandle);
}

void
Legislator::Unload()
{
    if (this->m_spawnThread == NULL)
    {
        this->ShutDown();
    }
    else
    {
        if (m_isInitializeCompleted == false)
        {
            return;
        }

        this->StopMainLoop();

        this->StopHeartBeatLoop();

        this->Stop();

        this->StopIPLoop();

        this->StopExeLoop();

        this->StopFastReadLoops();

        this->StopAcceptLoop();

        this->StopNet();

        this->m_executeQ.Stop();

        //first, we need to wait until there is no thread remaining
        while (this->m_numThreads != 0)
        {
            Sleep(20);
        }
    }
}

bool
Legislator::IsBootstrapped()
{
    // Replica is bootstrapped as soon as first vote gets reproposed
    // (maxAcceptedVote.ballotId > 0) but this is not enought since
    // maxAcceptedVote.ballot = (0,0) when we change configuration
    // (See ChangePaxosConfiguration() for details) but in such case,
    // maxAcceptedVote.decree > 1

    AutoCriticalSection lock(&m_lock);
    return (m_maxAcceptedVote->m_ballot.m_ballotId > 0) ||
           (m_maxAcceptedVote->m_decree > 1);
}

RSLResponseCode
Legislator::StartBootstrap(MemberSet * memberSet)
{
    if (memberSet->Verify(m_version) == false)
    {
        return RSLInvalidParameter;
    }

    AutoCriticalSection lock(&m_lock);

    if (IsBootstrapped())
    {
        RSLInfo("Replica already bootstrapped");
        return RSLAlreadyBootstrapped;
    }

    // Make sure current replica is part of new configuration
    if (memberSet->IncludesMember(m_memberId) == false)
    {
        RSLError(
            "Current MemberId must be part of the replica set during bootstrap",
            LogTag_RSLMemberId, m_memberId.GetValue());

        return RSLBootstrapFailed;
    }

    // If there already is MemberSet saved in the checkpoint
    Ptr<MemberSet> currentSet;
    if (HasConfigurationCheckpointed())
    {
        currentSet = m_stateConfiguration->GetMemberSet();
    }
    else if (m_bootstrapMemberSet != NULL)
    {
        currentSet = m_bootstrapMemberSet;
    }

    if (currentSet != NULL)
    {
        RSLInfo("Comparing member set with previously checkpointed one");
        LogAssert(currentSet->GetNumMembers() > 0);

        // Make sure they have same replica set (memberId, hostname and port)
        const RSLNodeCollection& newNodes = memberSet->GetMemberCollection();
        const RSLNodeCollection& chkNodes = currentSet->GetMemberCollection();
        if (chkNodes.Count() != newNodes.Count())
        {
            RSLError("Member set is different (size differs)");
            return RSLBootstrapFailed;
        }
        for (size_t i = 0; i < chkNodes.Count(); i++)
        {
            const RSLNode& chkNode = chkNodes[i];
            bool found = false;
            for (size_t j = 0; j < newNodes.Count(); j++)
            {
                const RSLNode& newNode = newNodes[j];
                if (MemberId::Compare(chkNode.m_memberIdString, newNode.m_memberIdString) == 0)
                {
                    found = chkNode.m_ip == newNode.m_ip &&
                        chkNode.m_rslPort == newNode.m_rslPort &&
                        lstrcmpi(chkNode.m_hostName, newNode.m_hostName) == 0;
                }
            }
            if (found == false)
            {
                RSLError("Member set is different (old member missing)");
                return RSLBootstrapFailed;
            }
        }
        RSLInfo("Member set matches with previously checkpointed one");
    }

    m_bootstrapMemberSet = new MemberSet(*memberSet);

    return RSLSuccess;
}

void
Legislator::CheckBootstrap()
{
    LogAssert(m_state == PaxosInactive);

    bool persisted = false;
    {
        AutoCriticalSection lock(&m_lock);
        if (!HasConfigurationCheckpointed() && m_bootstrapMemberSet != NULL)
        {
            persisted = PersistMemberSet(m_bootstrapMemberSet);
            m_bootstrapMemberSet = NULL;
        }
    }

    // callback application
    if (persisted)
    {
        CAutoPoolLockExclusive lock(&m_callbackLock);
        m_stateMachine->NotifyConfigurationChanged(NULL);
    }

    if (m_paxosConfiguration->IncludesMember(m_memberId))
    {
        m_replicasLock.LockExclusive();
        for (UInt16 i = 0; i < m_paxosConfiguration->GetNumMembers(); i++)
        {
            const RSLNode &node = *m_paxosConfiguration->GetMemberInfo(i);
            if (m_memberId.Compare(node.m_memberIdString) != 0)
            {
                Replica *replica = new Replica(node);
                if (!m_stateMachine->ResolveNode(&replica->m_node))
                {
                    replica->m_needsResolve = true;
                }
                replica->UpCount();
                m_replicas.push_back(replica);
            }
        }
        SetEvent(m_resolveWaitHandle);
        m_replicasLock.UnlockExclusive();

        StartInitializing();
    }
}

bool
Legislator::PersistMemberSet(const MemberSet *memberSet)
{
    RSLInfo("Persisting new member set");
    // Set state configuration with new member set
    LogAssert(m_stateConfiguration->GetConfigurationNumber() == 0);
    LogAssert(m_stateConfiguration->GetInitialDecree() == 0);
    LogAssert(m_stateConfiguration->GetNumMembers() == 0);
    LogAssert(m_maxAcceptedVote->m_configurationNumber == 0);
    LogAssert(m_maxAcceptedVote->m_decree == 0);
    LogAssert(m_maxAcceptedVote->m_ballot == BallotNumber());

    MemberSet * initialMemberSet = new MemberSet(*memberSet);
    m_stateConfiguration = new ConfigurationInfo(1, // initial configuration number is 1
                                                 1, // initial decree number is 1
                                                 initialMemberSet);
    m_paxosConfiguration = m_stateConfiguration;
    m_maxAcceptedVote = new Vote(m_version, m_memberId,
        m_stateConfiguration->GetInitialDecree(),
        m_stateConfiguration->GetConfigurationNumber(),
        BallotNumber(), m_primaryCookie);
    m_maxAcceptedVote->CalculateChecksum();

    // Create checkpoint with new configuration
    if (SaveCheckpoint(m_maxAcceptedVote, false) == false)
    {
        RSLInfo("Failed to create checkpoint");
        return false;
    }
    RSLDebug("Checkpoint saved");
    // callback the application
    return true;
}

bool
Legislator::Replay(
    const char* dir,
    UInt64 maxDecree,
    __in_opt UInt64 checkpointDecree,
    __in_opt const char* checkpointDirectory,
    RSLStateMachine *sm
    )
/*++

Routine Description:

    Replay application state from the specified log and checkpoint files
    up to the specified decree# (maxDecree).

    If the checkpointDecree and the checkpointDirectory parameters are specified,
    create a new checkpoint for state at decree# = maxDecree with a new decree#
    of checkpointDecree and save the checkpoint the directory
    specified by the checkpointDirectory parameter.

Notes:

Arguments:

Assumptions:

    Creating a new checkpoint of application state at maxDecree# is
    an 'expert mode' operation and requires human intervention. This
    MUST be done with great caution.

    Directory of new checkpoint MUST BE different from the input
    'transaction' log directory. This is to prevent the new checkpoint
    from accidentally being used for an online operation; it is intended
    for offline recovery only.

Return Value:

--*/
{
    RSLConfigParam  configParam;
    ConfigParam     *pSavedParam = 0;
    DynString       savedDataDir;
    DynString       outputDir;
    DynString       tempDir;
    UInt64          lastExecutedDecree;
    UInt64          maxVoteDecree;
    bool            returnValue;

    if (((checkpointDecree == 0) && (checkpointDirectory != 0)) ||
        ((checkpointDecree != 0) && (checkpointDirectory == 0)))
    {
        return false;
    }

    m_stateMachine = sm;
    m_dataDir = dir;
    if (m_dataDir.Length() == 0 || m_dataDir[m_dataDir.Length() - 1] != '\\')
    {
        m_dataDir.Append('\\');
    }

    if (maxDecree == 0)
    {
        return true;
    }

    if (checkpointDirectory != 0)
    {
        outputDir = checkpointDirectory;
        if (outputDir.Length() == 0 || outputDir[outputDir.Length() - 1] != '\\')
        {
            outputDir.Append('\\');
        }

        if (DynString::Compare( outputDir, m_dataDir, false ) == 0)
        {
            //
            //  New checkpoint directory MUST BE different from 'transaction'
            //  log directory.
            //

            return false;
        }
    }

    // This is for backward compatibility.
    UInt32 initialConfigNumber = 0;
    if (sm->m_legislator->m_version < RSLProtocolVersion_4)
    {
        initialConfigNumber = 1;
    }

    if (!RestoreState(maxDecree, true, initialConfigNumber))
    {
        return false;
    }

    if (checkpointDecree == 0)
    {
        return true;
    }

    //
    //  ISSUE-2009-02-27-RSHANKAR:
    //      Checkpoint operation uses information that's really
    //      initialised through RSLStateMachine::Initialize().
    //      However, this shouldn't be called for
    //      RSLStateMachine::Replay().
    //
    //

    if (m_cfg.m_cfg != 0)
    {
        pSavedParam = m_cfg.m_cfg;
        m_cfg.m_cfg = 0;
    }

    StringCchCopy( configParam.m_workingDir,
                   sizeof(configParam.m_workingDir),
                   checkpointDirectory );

    if (m_cfg.Reload( &configParam ) == false)
    {
        return false;
    }

    //
    //  Create a checkpoint for state up to maxDecree in the directory
    //  == outputDirectory with sequence# == checkpointDecree. We also need
    //  to set the m_tempDir here; otherwise, the checkpoint file gets created
    //  in the root directory of the drive from where the executable is run
    //  and if this drive is different from the drive corresponding to the
    //  checkpoint directory, the operation to move the temporary
    //  checkpoint file created to the output checkpoint directory
    //  fails.
    //

    savedDataDir = m_dataDir;
    m_dataDir = outputDir;
    m_tempDir = outputDir;

    //
    //  Prepare to write the state at decree# == maxDecree
    //  as new decree# == checkpointDecree.
    //
    //  ISSUE-2009-02-27-RSHANKAR:
    //      This is somewhat of a hack as we 'know' what the
    //      SaveCheckpoint() method called expects.
    //

    lastExecutedDecree = m_lastExecutedDecree;
    m_lastExecutedDecree = checkpointDecree;

    maxVoteDecree = m_maxAcceptedVote->m_decree;
    m_maxAcceptedVote->m_decree = checkpointDecree + 1;

    m_maxAcceptedVote->CalculateChecksum();
    returnValue = SaveCheckpoint(m_maxAcceptedVote, true);

    m_dataDir = savedDataDir;
    m_tempDir = 0;

    //
    //  'Expert-mode' replay is intended for offline recovery only.
    //  Just in case someone accidentally uses it for online recovery,
    //  restore everything back. Note that the new checkpoint was
    //  written to a different directory than the 'transaction' log
    //  directory and so even if the RSL state machine were started,
    //  we won't be executing from the newly created checkpoint.
    //

    m_lastExecutedDecree = lastExecutedDecree;
    m_maxAcceptedVote->m_decree = maxVoteDecree;
    m_maxAcceptedVote->CalculateChecksum();

    if (pSavedParam != 0)
    {
        delete m_cfg.m_cfg;
        m_cfg.m_cfg = pSavedParam;
    }

    return returnValue;
}

void
Legislator::DeclareShutdown()
{
    m_lock.Enter();
    m_isShutting = true;
    m_lock.Leave();
}

bool
Legislator::IsInShutDown()
{
    return m_isShutting;
}

void
Legislator::Stop()
{
    DeclareShutdown();

    AbortReplicateRequests(RSLShuttingDown);

    CAutoPoolLockExclusive lock(&m_shutdownLock);
    // also abort all the fast read requests.
    RequestCtx *ctx;
    while ((ctx = m_fastReadQ.dequeue()) != NULL)
    {
        m_stateMachine->AbortRequest(RSLShuttingDown, ctx->m_cookie);
        free(ctx->m_requestBuf);
        delete ctx;
    }

    m_stateMachine->ShutDown(RSLShuttingDown);

    Logger::Flush();
}

void
Legislator::ShutDown()
{
    Stop();

    ExitProcess(0);
}

void
Legislator::Pause()
{
    // stop the main thread from running and pause all
    LogAssert(m_paused == MainThread_Run);
    m_paused = MainThread_Pause;
    // wait for the main thread to pause;
    while (m_paused != MainThread_Paused)
    {
        Sleep(10);
    }
    // the main thread is now sleeping;

    m_netlibServer->SuspendReceive();
    m_netlibClient->SuspendReceive();
}

void
Legislator::Resume()
{
    LogAssert(m_paused == MainThread_Paused);
    m_netlibServer->ResumeReceive();
    m_netlibClient->ResumeReceive();
    m_paused = MainThread_Run;
}

void
Legislator::ReadDefunctFile()
{
    DynString filename;
    filename.Set(m_dataDir);
    filename.Append("defunct.txt", strlen("defunct.txt"));

    auto_ptr<APSEQREAD> reader(new APSEQREAD());
    int ec = reader->DoInit(filename, 2, s_PageSize, false /* read/write: no */);
    if (ec != NO_ERROR)
    {
        return;
    }
    UInt32 number;
    ec = reader->GetData(&number, 4);
    if (ec != NO_ERROR)
    {
        return;
    }
    m_highestDefunctConfigurationNumber = number;
}

void
Legislator::SwitchToPaxosInactiveState()
{
    if (m_state == PaxosInactive)
    {
        return;
    }

    RSLDebug("SwitchToPaxosInactiveState");

    m_state = PaxosInactive;

    m_statusMap.Clear();
    m_responseMap.clear();

    m_nextElectionTime = _I64_MAX;
    m_actionTimeout = _I64_MAX;
}

void
Legislator::ChangePaxosConfiguration(Ptr<ConfigurationInfo> configuration)
{
    {
        AutoCriticalSection lock(&m_lock);

        m_paxosConfiguration = configuration;

        m_maxSeenBallot = BallotNumber();
        m_maxBallot = BallotNumber(m_maxBallot.m_ballotId + 1, MemberId());
        m_maxAcceptedVote = new Vote(m_version,
                                     m_memberId,
                                     configuration->GetInitialDecree(),
                                     0,
                                     BallotNumber(),
                                     m_primaryCookie);
        m_lastTimeJoinMessagesSent = 0;

    }

    bool inConfiguration = m_paxosConfiguration->IncludesMember(m_memberId);
    RSLInfo("New Paxos Configuration",
            LogTag_UInt1, PaxosConfiguration(),
            LogTag_UInt2, m_paxosConfiguration->GetNumMembers(),
            LogTag_String1, inConfiguration ? "In" : "Out");

    {
        CAutoPoolLockExclusive lock(&m_replicasLock);

        for (ReplicaIter i = m_replicas.begin(); i != m_replicas.end(); i++)
        {
            Replica *replica = *i;
            replica->DownCount();
        }

        m_replicas.clear();

        for (UInt16 i = 0; i < m_paxosConfiguration->GetNumMembers(); i++)
        {
            const RSLNode &node = *m_paxosConfiguration->GetMemberInfo(i);
            if (m_memberId.Compare(node.m_memberIdString) != 0)
            {
                Replica *replica = new Replica(node);
                if (!m_stateMachine->ResolveNode(&replica->m_node))
                {
                    replica->m_needsResolve = true;
                }
                replica->UpCount();
                m_replicas.push_back(replica);
            }
        }

        SetEvent(m_resolveWaitHandle);
    }
    {
        AutoCriticalSection lock(&m_recvQLock);
        m_voteCache.Clear();
        Message *msg;
        while ((msg = m_recvMsgQ.dequeue()) != NULL)
        {
            delete msg;
        }
    }

    if (!inConfiguration)
    {
        SwitchToPaxosInactiveState();
    }
    else
    {
        StartInitializing();
    }
}

void
Legislator::SendJoinMessagesIfNecessary()
{
    if (PaxosConfiguration() <= m_highestDefunctConfigurationNumber + 1)
    {
        return;
    }

    if (GetHiResTime() - m_lastTimeJoinMessagesSent < m_cfg.JoinMessagesInterval())
    {
        return;
    }

    SendJoinMessage(0, 0, true);
    m_lastTimeJoinMessagesSent = GetHiResTime();
}

void
Legislator::UpdateDefunctInfo(UInt32 defunctConfigurationNumber)
{
    if (defunctConfigurationNumber <= m_highestDefunctConfigurationNumber)
    {
        return;
    }

    RSLDebug("UpdateDefunctInfo",
             LogTag_UInt1, defunctConfigurationNumber);

    if (PaxosConfiguration() <= defunctConfigurationNumber && m_state != PaxosInactive)
    {
        StartInitializing();
        return;
    }

    {
        AutoCriticalSection lock(&m_lock);
        m_highestDefunctConfigurationNumber = defunctConfigurationNumber;
    }

    DynString filename;
    filename.Set(m_dataDir);
    filename.Append("defunct.txt", strlen("defunct.txt"));

    auto_ptr<APSEQWRITE> writer(new APSEQWRITE());
    writer->DoInit(filename.Str());
    writer->Write(&m_highestDefunctConfigurationNumber, 4);
}

void
Legislator::SetSpawnThreadRoutine(SpawnRoutine spawnThread)
{
    m_spawnThread = spawnThread;
}

int
Legislator::GetNativePriority(ThreadPriority priority)
{
    switch (priority & ~Priority_OutOfGC)
    {
    case Priority_Lowest:
        return THREAD_PRIORITY_LOWEST;
    case Priority_BelowNormal:
        return THREAD_PRIORITY_BELOW_NORMAL;
    case Priority_Normal:
        return THREAD_PRIORITY_NORMAL;
    case Priority_AboveNormal:
        return THREAD_PRIORITY_ABOVE_NORMAL;
    case Priority_Highest:
        return THREAD_PRIORITY_HIGHEST;
    }

    return THREAD_PRIORITY_NORMAL;
}

void
Legislator::RunThread(MemFun1 startMethod, const char *name, void *arg, ThreadPriority priority, HANDLE* handle)
{
    LegislatorArgWrapper *wrapper = new LegislatorArgWrapper(this, startMethod, arg);
    wrapper->m_name = name;

    RunThread(wrapper, priority, handle);
}

void
Legislator::RunThread(MemFun startMethod, const char * name, ThreadPriority priority, HANDLE *handle)
{
    LegislatorArgWrapper *wrapper = new LegislatorArgWrapper(this, startMethod);
    wrapper->m_name = name;

    RunThread(wrapper, priority, handle);
}

void
Legislator::RunThread(LegislatorArgWrapper *wrapper, ThreadPriority priority, HANDLE* handle)
{
    HANDLE h = 0;

    if (m_spawnThread == NULL || (priority & Priority_OutOfGC) )
    {
        h = (HANDLE)_beginthreadex(NULL, 0, Legislator::ThreadStartMethod, wrapper, NULL, NULL);

        LogAssert(h != 0);

        if (priority & Priority_OutOfGC)
        {
            priority = (ThreadPriority) (priority & ~Priority_OutOfGC);
        }

        SetThreadPriority(h, GetNativePriority(priority));
    }
    else
    {
        m_spawnThread(ThreadStartMethodCustomized, priority, wrapper);
        h = nullptr;
    }

    if (handle != NULL)
    {
        *handle = h;
    }
    else
    {
        CloseHandle(h);
    }
}

unsigned int
Legislator::ThreadStartMethod(void *arg)
{
    unsigned int result = ThreadStartMethodCustomized(arg);

    _endthreadex(result);

    return result;
}

unsigned int
Legislator::ThreadStartMethodCustomized(void *arg)
{
    // seed the rand() function. Every thread starts with a default seed
    srand((unsigned int) time(NULL));

    LegislatorArgWrapper *wrapper = (LegislatorArgWrapper *) arg;

    InterlockedIncrement(&wrapper->m_legislator->m_numThreads);

    wrapper->MakeCall();

    InterlockedDecrement(&wrapper->m_legislator->m_numThreads);

    delete wrapper;

    return 0;
}

void
Legislator::SetAcceptMessages(bool acceptMessages)
{
    this->m_acceptMessages = acceptMessages;
}

LegislatorNetHandler::LegislatorNetHandler(Legislator *legislator, bool asClient) :
    m_legislator(legislator), m_asClient(asClient)
{}

void
LegislatorNetHandler::ProcessSend(Packet *packet, TxRxStatus status)
{
    m_legislator->ProcessSend(packet, status, m_asClient);
}

void
LegislatorNetHandler::ProcessReceive(Packet *packet)
{
    m_legislator->ProcessReceive(packet, m_asClient);
}

void
LegislatorNetHandler::ProcessConnect(UInt32 ip, UInt16 port, ConnectState state)
{
    m_legislator->ProcessConnect(ip, port, state, m_asClient);
}
