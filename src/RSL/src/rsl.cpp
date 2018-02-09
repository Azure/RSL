
#include "rsl.h"
#include "rsldebug.h"
#include "legislator.h"
#include "apdiskio.h"
#include "utils.h"
#include "fingerprint.h"
#include "ws2tcpip.h"
#include <strsafe.h>
#include <windows.h>
#include "SSLImpl.h"

using namespace RSLib;
using namespace RSLibImpl;

void
RSLNode::SetMemberIdAsUInt64(unsigned long long memberId)
{
    HRESULT hresult = StringCbPrintfA(m_memberIdString, sizeof(m_memberIdString), "%I64u", memberId);
    LogAssert(SUCCEEDED(hresult));
}

UInt64
RSLNode::GetMemberIdAsUInt64()
{
    return RSLNode::ParseMemberIdAsUInt64(m_memberIdString);
}

unsigned long long
RSLNode::ParseMemberIdAsUInt64(const char* memberId)
{
    if (*memberId == NULL)
    {
        return 0;
    }
    char * endPtr = NULL;
    UInt64 value = _strtoui64(memberId, &endPtr, 0);
    LogAssert(*endPtr == NULL);
    return value;
}

void STDCALL RSLib::RSLUnload()
{
    Netlib::Stop();
    Logger::Stop();
}

bool STDCALL RSLib::SetThumbprintsForSsl(const char * pThumbPrintA, const char * pThumbPrintB, bool validateCAChain, bool checkCertificateRevocation)
{
    HRESULT hr = SSLAuth::SetSSLThumbprints("MY", pThumbPrintA, pThumbPrintB, validateCAChain, checkCertificateRevocation);
    return SUCCEEDED(hr);
}

bool STDCALL RSLib::SetSubjectNamesForSsl(const char * pSubjectA, const char * pThumbPrintsParentA, const char * pSubjectB, const char * pThumbPrintsParentB, bool considerIdentitiesWhitelist)
{
    HRESULT hr = SSLAuth::SetSSLSubjectNames(pSubjectA, pThumbPrintsParentA, pSubjectB, pThumbPrintsParentB, considerIdentitiesWhitelist);
    return SUCCEEDED(hr);
}

void STDCALL RSLib::EnableListenOnAllIPs()
{
    Legislator::EnableListenOnAllIPs();
}

void STDCALL RSLib::SetMinidumpEnabled(bool enabled)
{
    Logger::SetMinidumpEnabled(enabled);
}

bool STDCALL RSLib::RSLInit(const char * debugLogPath, bool logDebugMessages, NotificationsCallback notificationsCallback, LogEntryCallback logEntryCallback)
{
    Logger::SetNotificationsCallback(notificationsCallback);
    Logger::SetLogEntryCallback((LogEntryCallbackInternal)logEntryCallback);

    //The application needs to initialize Logger and netlib
    //before initializing the RSL
    WSADATA data;
    int ret = WSAStartup(MAKEWORD(2, 2), &data);
    if (ret != 0)
    {
        return false;
    }

    if (Logger::Init(debugLogPath) == false)
    {
        return false;
    }
    if (debugLogPath != NULL)
    {
        const char* logInfo = (logDebugMessages == true) ? "DISWEA" : "ISWEA";
        if (Logger::CreateFileLogDestination("rsl", "rsl", 10 * 1024 * 1024, 10) < 0 ||
            Logger::AddRule("RSL", logInfo, "rsl") == false ||
            Logger::AddRule("*", "SWEA", "rsl") == false)
        {
            return false;
        }
    }

    if (Netlib::Initialize() == false)
    {
        return false;
    }
    return true;
}

RSLCheckpointStreamReader::RSLCheckpointStreamReader() : m_offset(0), m_dataSize(0),
m_dataBlock(NULL), m_defaultBlockSize(0), m_currentBlockSize(0), m_dataReadOffset(0),
m_context(NULL), m_filename(NULL)
{
    m_reader = new APSEQREAD();
    LogAssert(m_reader);
}

RSLCheckpointStreamReader::~RSLCheckpointStreamReader()
{
    if (m_dataBlock != NULL)
    {
        delete[] m_dataBlock;
    }

    if (m_filename != NULL)
    {
        delete[] m_filename;
    }

    delete m_reader;

    if (m_context != NULL)
    {
        delete m_context;
    }
}

void
RSLCheckpointStreamReader::CloseWriterForThisReader(const char *file, RSLCheckpointStreamWriter *writer)
{
    m_context->SetBytesIssued(writer);
    LogAssert(writer->Close() == NO_ERROR);
    m_context->Marshal(file);
}

RSLCheckpointStreamWriter*
RSLCheckpointStreamReader::CreateWriterForThisReader(const char *file)
{
    if (m_context == NULL)
    {
        return NULL;
    }

    RSLCheckpointStreamWriter* result = new RSLCheckpointStreamWriter();

    if (NO_ERROR != result->Init(file, m_context))
    {
        delete result;
        return NULL;
    }

    return result;
}

unsigned int
RSLCheckpointStreamReader::Init(const char *file)
{
    CheckpointHeader *header = new CheckpointHeader();

    unsigned int result = ERROR_CRC;
    m_context = NULL;

    if (header->UnMarshal(file))
    {
        result = Init(file, header);
    }

    if (result != NO_ERROR)
    {
        delete header;
    }
    else
    {
        m_context = header;
    }

    return result;
}

char *
RSLCheckpointStreamReader::GetFileName()
{
    return m_filename;
}

unsigned int
RSLCheckpointStreamReader::Init(const char *file, CheckpointHeader *header)
{
    m_offset = header->GetMarshalLen();
    unsigned int blockSize = s_ChecksumBlockSize;
    if (header->m_checksumBlockSize > 0)
    {
        m_defaultBlockSize = header->m_checksumBlockSize;
        blockSize = header->m_checksumBlockSize;
        // Check if block size if multiple of s_PageSize
        LogAssert(blockSize % s_PageSize == 0);
        m_dataBlock = new char[m_defaultBlockSize];
        LogAssert(m_dataBlock);
    }
    int ec = m_reader->DoInit(file, APSEQREAD::c_maxReadsDefault, blockSize);
    if (ec != NO_ERROR)
    {
        return ec;
    }
    if (header->m_checksumBlockSize > 0)
    {
        if (m_reader->FileSize() != header->m_size)
        {
            RSLError("File size different from the size in the header",
                LogTag_UInt641, m_reader->FileSize(), LogTag_UInt642, header->m_size);
            return ERROR_CRC;
        }

        // Calculate user's data size
        unsigned long long total = m_reader->FileSize() - m_offset;
        unsigned long long numberOfFullBlocks = total / m_defaultBlockSize;
        m_dataSize = (numberOfFullBlocks * (m_defaultBlockSize - CHECKSUM_SIZE));
        unsigned long long lastBlockSize = total - (numberOfFullBlocks * m_defaultBlockSize);
        if (lastBlockSize > 0)
        {
            // The size of the checkpoint file is already checked
            // by the checkpoint m_size member, so the last block
            // should never be less or equal to the ckecksum token
            // size. If that happens, it is because there is a bug
            // in the RSLCheckpointStreamWriter code.
            if (lastBlockSize <= CHECKSUM_SIZE)
            {
                RSLError("File size mismatches block alignment",
                    LogTag_UInt641, lastBlockSize);
                return ERROR_CRC;
            }
            m_dataSize += lastBlockSize - CHECKSUM_SIZE;
        }
    }

    // we allow rewriting m_filename, since there rest of this function already can deal with it.
    if (m_filename != NULL)
    {
        delete[] m_filename;
        m_filename = NULL;
    }

    // we allow here file being null, even though DoInit would not allow it.
    // but for the scope of this function, it should be okay.
    if (file != NULL)
    {
        // max length allowed here for the filename buffer is MAXFILENAME_SIZE
        size_t filenameLength = strnlen(file, MAXFILENAME_SIZE - 1);
        if (filenameLength == MAXFILENAME_SIZE - 1)
        {
            return ERROR_INVALID_PARAMETER;
        }

        // copy the filename
        m_filename = new char[filenameLength + 1];
        memcpy_s(m_filename, filenameLength + 1, file, filenameLength);
        m_filename[filenameLength] = '\0';
    }

    m_nextblockOffset = m_offset;
    return m_reader->Reset(m_offset);
}

unsigned int
RSLCheckpointStreamReader::ReadNextDataBlock()
{
    m_dataReadOffset = 0;
    m_currentBlockSize = 0;
    unsigned int dwNumBytes = m_defaultBlockSize;

    m_nextblockOffset += m_defaultBlockSize;

    while (dwNumBytes > 0)
    {
        char * pb;
        DWORD pcbRead;
        unsigned int ec = m_reader->GetDataPointer((void**)&pb, dwNumBytes, &pcbRead);

        // Block has ended
        if (ec == ERROR_HANDLE_EOF)
        {
            if (m_currentBlockSize > 0)
            {
                break;
            }
            return ERROR_HANDLE_EOF;
        }
        if (ec != NO_ERROR)
        {
            return ec;
        }
        // Copy read data to data block buffer
        memcpy(&m_dataBlock[m_currentBlockSize], pb, pcbRead);
        m_currentBlockSize += pcbRead;
        dwNumBytes -= pcbRead;
        LogAssert(m_currentBlockSize <= m_defaultBlockSize);
    }

    // Must have at least checksum token
    if (m_currentBlockSize < CHECKSUM_SIZE)
    {
        RSLError("Data block too short", LogTag_UInt1, m_currentBlockSize);
        return ERROR_CRC;
    }
    m_currentBlockSize -= CHECKSUM_SIZE;
    LogAssert(m_currentBlockSize <= m_defaultBlockSize);

    // Verify Checksum
    unsigned long long pstdChecksum;
    memcpy(&pstdChecksum, &m_dataBlock[m_currentBlockSize], CHECKSUM_SIZE);

    unsigned long long calcChecksum = Utils::CalculateChecksum(m_dataBlock, m_currentBlockSize);
    if (pstdChecksum != calcChecksum)
    {
        return ERROR_CRC;
    }
    LogAssert(m_currentBlockSize > 0);
    return NO_ERROR;
}

unsigned int
RSLCheckpointStreamReader::GetDataPointer(void ** ppvBuffer, unsigned long dwNumBytes,
    unsigned long * pcbRead)
{
    unsigned int ec = NO_ERROR;
    // No block handling, use backwards compatilible code
    if (m_dataBlock == NULL)
    {
        return m_reader->GetDataPointer(ppvBuffer, dwNumBytes, pcbRead);
    }

    // End of last block, read next then
    if (m_dataReadOffset == m_currentBlockSize)
    {
        ec = ReadNextDataBlock();
        if (ec != NO_ERROR)
        {
            return ec;
        }
    }
    // Point to the right position on the buffer
    *ppvBuffer = &m_dataBlock[m_dataReadOffset];
    unsigned long bytesRead = m_currentBlockSize - m_dataReadOffset;
    if (bytesRead > dwNumBytes)
    {
        bytesRead = dwNumBytes;
    }
    m_dataReadOffset += bytesRead;
    *pcbRead = bytesRead;

    LogAssert(m_dataReadOffset <= m_currentBlockSize);
    return NO_ERROR;
}

unsigned int
RSLCheckpointStreamReader::GetData(void * pvBuffer, unsigned long dwNumBytes, unsigned long * pcbRead)
{
    char * pb;
    unsigned int ec = GetDataPointer((void**)&pb, dwNumBytes, pcbRead);
    if (!ec)
    {
        memcpy(pvBuffer, pb, *pcbRead);
    }
    return ec;
}

unsigned int
RSLCheckpointStreamReader::Seek(unsigned long long offset)
{
    unsigned int ec = NO_ERROR;

    unsigned long long numblocks = offset / (m_defaultBlockSize - CHECKSUM_SIZE);
    unsigned int offsetInBlock = (unsigned int)(offset % (m_defaultBlockSize - CHECKSUM_SIZE));

    // here we can optimize and avoid reading a new block if the seek is within the current block
    if (m_dataBlock == NULL || m_nextblockOffset != m_offset + (1 + numblocks) * m_defaultBlockSize)
    {
        m_nextblockOffset = m_offset + numblocks * m_defaultBlockSize;

        m_reader->Reset(m_nextblockOffset);
        m_currentBlockSize = 0;

        ec = ReadNextDataBlock();
    }

    m_dataReadOffset = offsetInBlock;

    return ec;
}

unsigned int
RSLCheckpointStreamReader::Skip(unsigned long dwNumBytes)
{
    unsigned int ec = NO_ERROR;
    // No block handling, use backwards compatilible code
    if (m_dataBlock == NULL)
    {
        return m_reader->Skip(dwNumBytes);
    }

    while (dwNumBytes > 0)
    {
        LogAssert(m_dataReadOffset <= m_currentBlockSize);
        // End of last block, read next then
        if (m_dataReadOffset == m_currentBlockSize)
        {
            ec = ReadNextDataBlock();
            if (ec != NO_ERROR)
            {
                return ec;
            }
        }
        unsigned long bytesRead = m_currentBlockSize - m_dataReadOffset;
        if (bytesRead > dwNumBytes)
        {
            bytesRead = dwNumBytes;
        }
        m_dataReadOffset += bytesRead;
        dwNumBytes -= bytesRead;
    }

    LogAssert(m_dataReadOffset <= m_currentBlockSize);
    LogAssert(dwNumBytes == 0);
    return NO_ERROR;
}

unsigned long long
RSLCheckpointStreamReader::Size()
{
    // No block handling, use backwards compatilible code
    if (m_dataBlock == NULL)
    {
        return m_reader->FileSize() - m_offset;
    }

    // It was calculated and checked in Init()
    return m_dataSize;
}

RSLCheckpointStreamWriter::RSLCheckpointStreamWriter() : m_available(0), m_buffer(NULL),
m_written(0), m_defaultBlockSize(0), m_dataWrittenOffset(0), m_checksum(0)
{
    m_writer = new APSEQWRITE();
    LogAssert(m_writer);
}

RSLCheckpointStreamWriter::~RSLCheckpointStreamWriter()
{
    delete m_writer;
}

unsigned int
RSLCheckpointStreamWriter::Init(const char *file, CheckpointHeader *header)
{
    unsigned int blockSize = s_ChecksumBlockSize;
    if (header->m_checksumBlockSize > 0)
    {
        m_defaultBlockSize = header->m_checksumBlockSize;
        blockSize = header->m_checksumBlockSize;
        // Check if block size if multiple of s_PageSize
        LogAssert(blockSize % s_PageSize == 0);
    }
    unsigned int ec = m_writer->DoInit(file, blockSize);
    if (ec != NO_ERROR)
    {
        return ec;
    }
    unsigned long long offset = header->GetMarshalLen();
    while (offset > 0)
    {
        unsigned long long bytesToWrite = offset;
        ec = m_writer->GetAvailable(&m_buffer, &m_available);
        if (ec != NO_ERROR)
        {
            return ec;
        }
        if (bytesToWrite > m_available)
        {
            bytesToWrite = m_available;
        }
        ec = m_writer->CommitAvailable((unsigned int)bytesToWrite);
        if (ec != NO_ERROR)
        {
            return ec;
        }
        offset -= bytesToWrite;
    }

    ec = m_writer->GetAvailable(&m_buffer, &m_available);
    m_written = 0;
    return ec;
}

unsigned int
RSLCheckpointStreamWriter::Write(const void * pbWrite, unsigned long cbWrite)
{
    unsigned int ec = NO_ERROR;
    // No block handling, use backwards compatilible code
    if (m_defaultBlockSize == 0)
    {
        return m_writer->Write(pbWrite, cbWrite);
    }

    unsigned int dataOnlyBlockSize = m_defaultBlockSize - CHECKSUM_SIZE;
    while (cbWrite > 0)
    {
        LogAssert(m_dataWrittenOffset <= dataOnlyBlockSize);
        if (m_available == m_written || m_dataWrittenOffset == dataOnlyBlockSize)
        {
            ec = m_writer->CommitAvailable(m_written);
            if (ec != NO_ERROR)
            {
                return ec;
            }
            // we have written m_defaultBlockSize. Now write the checksum
            if (m_dataWrittenOffset == dataOnlyBlockSize)
            {
                ec = m_writer->Write(&m_checksum, CHECKSUM_SIZE);
                if (ec != NO_ERROR)
                {
                    return ec;
                }
                m_dataWrittenOffset = 0;
            }
            ec = m_writer->GetAvailable(&m_buffer, &m_available);
            if (ec != NO_ERROR)
            {
                return ec;
            }
            m_written = 0;
        }

        // Write data to data block
        DWORD bytesToWrite = cbWrite;
        if (bytesToWrite > m_available - m_written)
        {
            bytesToWrite = m_available - m_written;
        }
        if (bytesToWrite > dataOnlyBlockSize - m_dataWrittenOffset)
        {
            bytesToWrite = dataOnlyBlockSize - m_dataWrittenOffset;
        }

        memcpy((char*)m_buffer + m_written, pbWrite, bytesToWrite);
        if (m_dataWrittenOffset == 0)
        {
            LogAssert(m_written == 0);
            m_checksum = FingerPrint64::GetInstance()->GetFingerPrint(
                m_buffer,
                bytesToWrite);
        }
        else
        {
            m_checksum = FingerPrint64::GetInstance()->GetFingerPrint(
                m_checksum,
                ((char *)m_buffer) + m_written,
                bytesToWrite);
        }

        m_dataWrittenOffset += bytesToWrite;
        m_written += bytesToWrite;
        cbWrite -= bytesToWrite;
        pbWrite = (const void *)(((char *)pbWrite) + bytesToWrite);
    }

    LogAssert(m_dataWrittenOffset <= dataOnlyBlockSize);
    return NO_ERROR;
}

unsigned int
RSLCheckpointStreamWriter::Close()
{
    unsigned int ec = NO_ERROR;
    // No block handling, use backwards compatilible code
    if (m_defaultBlockSize == 0)
    {
        ec = m_writer->Flush();
        m_writer->DoDispose();
        return ec;
    }

    // If there is data to be written
    if (m_dataWrittenOffset > 0)
    {
        ec = m_writer->CommitAvailable(m_written);
        if (ec != NO_ERROR)
        {
            return ec;
        }
        ec = m_writer->Write(&m_checksum, CHECKSUM_SIZE);
        if (ec != NO_ERROR)
        {
            return ec;
        }
        m_dataWrittenOffset = 0;
    }

    m_writer->DoDispose();
    return NO_ERROR;
}

unsigned long long
RSLCheckpointStreamWriter::BytesIssued()
{
    // No block handling, use backwards compatilible code
    if (m_defaultBlockSize == 0)
    {
        m_writer->Flush();
        return m_writer->BytesIssued();
    }

    unsigned int ec = m_writer->Flush();
    if (ec != NO_ERROR)
    {
        return 0;
    }
    unsigned long long size = m_writer->BytesIssued();
    if (m_dataWrittenOffset > 0)
    {
        size += m_written + CHECKSUM_SIZE;
    }
    return size;
}

RSLPrimaryCookie::RSLPrimaryCookie() : m_data(NULL) {}

RSLPrimaryCookie::~RSLPrimaryCookie()
{
    delete m_data;
}

void
RSLPrimaryCookie::Set(void *data, UInt32 len)
{
    m_data = new PrimaryCookie(data, len, true);
};

typedef std::vector<RSLStateMachine*>::iterator RSLStateMachineIter;

std::vector<RSLStateMachine*> sm_rslStateMachines; // all the state machines in this process

RSLStateMachine::RSLStateMachine() : m_legislator(NULL)
{
    sm_rslStateMachines.push_back(this);
}

unsigned long long
RSLStateMachine::GetHighResolutionCurrentTickCount()
{
    return GetHiResTime();
}

void RSLStateMachine::RelinquishPrimary()
{
    m_legislator->RelinquishPrimary();
}

bool RSLStateMachine::IsInStableState()
{
    return m_legislator->IsInStableState();
}

bool
RSLStateMachine::Initialize(RSLConfigParam *cfg, const RSLNodeArray &nodes, const RSLNode &self,
    RSLProtocolVersion version, bool serializeFastReadsWithReplicate,
    SpawnRoutine spawnRoutine, bool IndependentHeartbeat)
{
    //
    // Convert RSLNodeArray to RSLNodeCollection
    //
    RSLNodeCollection collection;
    for (size_t i = 0; i < nodes.size(); i++)
    {
        collection.Append(nodes[i]);
    }

    return Initialize(cfg, collection, self, version, serializeFastReadsWithReplicate, spawnRoutine, IndependentHeartbeat);
}

bool
RSLStateMachine::Initialize(RSLConfigParam *cfg, const RSLNodeCollection &nodes, const RSLNode &self,
    RSLProtocolVersion version, bool serializeFastReadsWithReplicate,
    SpawnRoutine spawnRoutine, bool IndependentHeartbeat)
{
    m_legislator = new Legislator(IndependentHeartbeat);
    LogAssert(m_legislator);

    m_legislator->SetSpawnThreadRoutine(spawnRoutine);
    return m_legislator->Start(cfg, nodes, self, version, serializeFastReadsWithReplicate, this);
}

bool
RSLStateMachine::Initialize(RSLConfigParam *cfg, const RSLNode &self, RSLProtocolVersion version,
    bool serializeFastReadsWithReplicate,
    SpawnRoutine spawnRoutine, bool IndependentHeartbeat)
{
    m_legislator = new Legislator(IndependentHeartbeat);
    LogAssert(m_legislator);

    m_legislator->SetSpawnThreadRoutine(spawnRoutine);
    return m_legislator->Start(cfg, self, version, serializeFastReadsWithReplicate, this);
}

void
RSLStateMachine::ChangeElectionDelay(UInt32 delayInSecs)
{
    m_legislator->ChangeElectionDelay(delayInSecs);
}

RSLResponseCode
RSLStateMachine::Bootstrap(RSLMemberSet* members, UInt32 timeout)
{
    if (members == NULL)
    {
        RSLError("RSLMemberSet cannot be NULL");
        return RSLInvalidParameter;
    }
    return m_legislator->Bootstrap(members->m_memberSet, timeout);
}

void
RSLStateMachine::Unload()
{
    for (RSLStateMachineIter i = sm_rslStateMachines.begin(); i != sm_rslStateMachines.end(); i++)
    {
        RSLStateMachine *sm = *i;
        sm->UnloadThisOne();
    }

    sm_rslStateMachines.clear();
}

void
RSLStateMachine::UnloadThisOne()
{
    Legislator *legislator = m_legislator;
    m_legislator = NULL;

    if (legislator != NULL)
    {
        RSLStateMachineIter it = std::find(sm_rslStateMachines.begin(), sm_rslStateMachines.end(), this);
        if (it != sm_rslStateMachines.end())
        {
            sm_rslStateMachines.erase(it);
        }

        legislator->Unload();
        delete legislator;
    }
}

bool
RSLStateMachine::Replay(const char* directory,
    UInt64 maxSeqNo)
{
    m_legislator = new Legislator(false);
    LogAssert(m_legislator);
    return m_legislator->Replay(directory, maxSeqNo, 0, 0, this);
}

bool
RSLStateMachine::Replay(const char* directory,
    unsigned long long maxSeqNo,
    unsigned long long checkpointSeqNo,
    const char* checkpointDirectory)
{
    m_legislator = new Legislator(false);
    LogAssert(m_legislator);
    return m_legislator->Replay(directory, maxSeqNo, checkpointSeqNo, checkpointDirectory, this);
}


RSLResponseCode
RSLStateMachine::ReplicateRequest(void* request, size_t len, void* cookie)
{
    return m_legislator->ReplicateRequest(request, len, cookie);
}

RSLResponseCode
RSLStateMachine::ReplicateRequestExclusiveVote(void* request, size_t len, void* cookie)
{
    return m_legislator->ReplicateRequestExclusiveVote(request, len, cookie);
}

RSLResponseCode
RSLStateMachine::ReplicateRequestExclusiveVote(void* request, size_t len, void* cookie, bool isLastDecree)
{
    return m_legislator->ReplicateRequestExclusiveVote(request, len, cookie, isLastDecree);
}

void
RSLStateMachine::AttemptPromotion()
{
    m_legislator->AttemptPromotion();
}

void
RSLStateMachine::RelinquishPrimaryStatus()
{
    m_legislator->RelinquishPrimary();
}

RSLResponseCode
RSLStateMachine::FastReadRequest(UINT64 maxSeenSeqNo, void* request, size_t len, void* cookie)
{
    return m_legislator->FastReadRequest(maxSeenSeqNo, request, len, cookie, 0);
}

RSLResponseCode
RSLStateMachine::FastReadRequest(UINT64 maxSeenSeqNo, void* request, size_t len, void* cookie, UInt32 timeout)
{
    return m_legislator->FastReadRequest(maxSeenSeqNo, request, len, cookie, timeout);
}

UINT64
RSLStateMachine::GetCurrentSequenceNumber()
{
    return m_legislator->GetCurrentSequenceNumber();
}

UINT64
RSLStateMachine::GetHighestPassedSequenceNumber()
{
    return m_legislator->GetHighestPassedSequenceNumber();
}

unsigned short
RSLStateMachine::GetReplicasInformation(unsigned short numEntries, ReplicaHealth *replicaSetHealth)
{
    return m_legislator->GetReplicasInformation(numEntries, replicaSetHealth);
}

bool
RSLStateMachine::GetReplicaInformation(ReplicaHealth *replicaHealth)
{
    return m_legislator->GetReplicaInformation(replicaHealth);
}

void
RSLStateMachine::GetCurrentPrimary(RSLNode *node)
{
    m_legislator->GetCurrentPrimary(node);
}

void
RSLStateMachine::AllowSaveState(bool yes)
{
    m_legislator->AllowSaveState(yes);
}

void
RSLStateMachine::SetVotePayload(unsigned long long payload)
{
    m_legislator->SetVotePayload(payload);
}

bool
RSLStateMachine::IsAnyRequestPendingExecution()
{
    return m_legislator->IsAnyRequestPendingExecution();
}

RSLResponseCode
RSLStateMachine::CopyStateFromReplica(void *cookie)
{
    return m_legislator->CopyStateFromReplica(cookie);
}

void RSLStateMachine::GetStatisticsSnapshot(RSLStats * pStats)
{
    m_legislator->GetStatisticsSnapshot(pStats);
}

bool
RSLStateMachine::GetConfiguration(RSLMemberSet *members, UInt32 *number)
{
    if (members == NULL)
    {
        RSLError("RSLMemberSet cannot be NULL");
        return false;
    }
    m_legislator->GetConfiguration(members->m_memberSet, number);
    return true;
}

RSLResponseCode
RSLStateMachine::ChangeConfiguration(RSLMemberSet* members, void *cookie)
{
    if (members == NULL)
    {
        RSLError("RSLMemberSet cannot be NULL");
        return RSLInvalidParameter;
    }
    return m_legislator->ChangeConfiguration(members->m_memberSet, cookie);
}

bool
RSLStateMachine::ResolveNode(RSLNode *node)
{
    LogAssert(node->m_rslPort != 0);
    if (node->m_ip != 0)
    {
        return true;
    }

    struct addrinfo* aInfo;
    // Use this Hints to enforce IPv4
    addrinfo hints;
    ZeroMemory(&hints, sizeof(hints));
    hints.ai_family = PF_INET; // This indicates IPv4 only
    hints.ai_socktype = 0;
    hints.ai_protocol = 0;

    int irc = getaddrinfo(node->m_hostName, NULL, &hints, &aInfo);
    if (irc != 0 || aInfo == NULL)
    {
        RSLError("Machine Name Lookup Failed",
            LogTag_String1, node->m_hostName, LogTag_LastError, WSAGetLastError());
        return false;
    }

    if (aInfo->ai_addr->sa_family != PF_INET)
    {
        RSLError("Machine Name Lookup Returned Unexpected Family",
            LogTag_String1, node->m_hostName, LogTag_String2, aInfo->ai_addr->sa_family);

        freeaddrinfo(aInfo);
        return false;
    }

    struct sockaddr_in *inAddr = (struct sockaddr_in *) aInfo->ai_addr;
    node->m_ip = inAddr->sin_addr.s_addr;
    freeaddrinfo(aInfo);
    return true;
}

void
RSLStateMachine::Pause()
{
    m_legislator->Pause();
}

void
RSLStateMachine::Resume()
{
    m_legislator->Resume();
}

//////////////////////
// MemberSet
//////////////////////

RSLMemberSet::RSLMemberSet() : m_memberSet(new MemberSet())
{
}

RSLMemberSet::RSLMemberSet(const RSLNodeCollection &members, void *cookie, UInt32 cookieLength) :
    m_memberSet(new MemberSet(members, cookie, cookieLength))
{
}

RSLMemberSet::RSLMemberSet(const RSLNodeArray &members, void *cookie, UInt32 cookieLength)
{
    //
    // Convert RSLNodeArray to RSLNodeCollection
    //
    RSLNodeCollection collection;
    for (size_t i = 0; i < members.size(); i++)
    {
        collection.Append(members[i]);
    }

    m_memberSet = new MemberSet(collection, cookie, cookieLength);
}

RSLMemberSet::~RSLMemberSet()
{
    delete m_memberSet;
}

size_t
RSLMemberSet::GetNumMembers() const
{
    return m_memberSet->GetNumMembers();
}

const RSLNode *
RSLMemberSet::GetMemberInfo(UInt16 whichMember) const
{
    return m_memberSet->GetMemberInfo(whichMember);
}

void
RSLMemberSet::GetMemberCollection(
    __out RSLNodeCollection& nodes
) const
{
    nodes = m_memberSet->GetMemberCollection();
}

const RSLNodeArray *
RSLMemberSet::GetMemberArray() const
{
    return m_memberSet->GetMemberArray_Deprecated();
}

bool
RSLMemberSet::IncludesMember(const char* memberId) const
{
    MemberId id(memberId);
    return m_memberSet->IncludesMember(id);
}

void
RSLMemberSet::SetConfigurationCookie(void *cookie, UInt32 cookieLength)
{
    m_memberSet->SetConfigurationCookie(cookie, cookieLength);
}

void *
RSLMemberSet::GetConfigurationCookie(UInt32 *length) const
{
    return m_memberSet->GetConfigurationCookie(length);
}

MemberSet::MemberSet() : m_cookie(NULL), m_cookieLength(0)
{
}

MemberSet::MemberSet(const MemberSet &memberset) :
    m_members(memberset.m_members), m_cookie(NULL), m_cookieLength(0)
{
    SetConfigurationCookie(memberset.m_cookie, memberset.m_cookieLength);
}

MemberSet::MemberSet(const RSLNodeCollection &members, void *cookie, UInt32 cookieLength)
    : m_members(members), m_cookie(NULL), m_cookieLength(0)
{
    SetConfigurationCookie(cookie, cookieLength);
}

MemberSet::~MemberSet()
{
    FreeCookie();
}

void
MemberSet::Copy(const MemberSet *memberset)
{
    m_members = memberset->m_members;
    SetConfigurationCookie(memberset->m_cookie, memberset->m_cookieLength);
}

void
MemberSet::Marshal(MarshalData *marshal, RSLProtocolVersion version)
{
    UInt16 numMembers = (UInt16)GetNumMembers();
    marshal->WriteUInt16(numMembers);

    for (UInt16 whichMember = 0; whichMember < numMembers; ++whichMember)
    {
        const RSLNode *node = GetMemberInfo(whichMember);

        MemberId id(node->m_memberIdString);
        id.Marshal(marshal, version);
        marshal->WriteUInt32(node->m_ip);
        marshal->WriteUInt16(node->m_rslPort);
        if (version > RSLProtocolVersion_3)
        {
            marshal->WriteUInt16(node->m_rslLearnPort);
        }
        else
        {
            marshal->WriteUInt16(node->m_appPort);
        }

        size_t hostNameLength;
        LogAssert(SUCCEEDED(StringCchLengthA(node->m_hostName, sizeof(node->m_hostName), &hostNameLength)));
        marshal->WriteUInt16((UInt16)hostNameLength);
        marshal->WriteData((UInt32)hostNameLength, (void *)node->m_hostName);
    }

    marshal->WriteUInt32(m_cookieLength);
    marshal->WriteData(m_cookieLength, m_cookie);
}

bool
MemberSet::UnMarshal(MarshalData *marshal, RSLProtocolVersion version)
{
    FreeCookie();

    UInt16 numMembers;
    if (!marshal->ReadUInt16(&numMembers))
    {
        return false;
    }

    RSLNodeCollection members;
    for (UInt16 whichMember = 0; whichMember < numMembers; ++whichMember)
    {
        RSLNode node;

        MemberId id;
        if (!id.UnMarshal(marshal, version))
        {
            return false;
        }

        HRESULT success = StringCbCopyA(node.m_memberIdString, sizeof(node.m_memberIdString), id.GetValue());

        if (!SUCCEEDED(success))
        {
            return false;
        }

        if (!marshal->ReadUInt32(&node.m_ip))
        {
            return false;
        }
        if (!marshal->ReadUInt16(&node.m_rslPort))
        {
            return false;
        }

        if (version > RSLProtocolVersion_3)
        {
            if (!marshal->ReadUInt16(&node.m_rslLearnPort))
            {
                return false;
            }
        }
        else
        {
            if (!marshal->ReadUInt16(&node.m_appPort))
            {
                return false;
            }
        }

        UInt16 hostNameLength;
        if (!marshal->ReadUInt16(&hostNameLength))
        {
            return false;
        }
        LogAssert(hostNameLength < sizeof(node.m_hostName));
        if (!marshal->ReadData(hostNameLength, (void *)node.m_hostName))
        {
            return false;
        }
        node.m_hostName[hostNameLength] = '\0';

        members.Append(node);
    }

    m_members = members;

    if (!marshal->ReadUInt32(&m_cookieLength))
    {
        return false;
    }
    if (m_cookieLength > RSLMemberSet::s_MaxMemberSetCookieLength)
    {
        RSLInfo("Cookie in member set too long", LogTag_UInt1, m_cookieLength);
        return false;
    }

    if (m_cookieLength != 0)
    {
        m_cookie = malloc(m_cookieLength);
        LogAssert(m_cookie);

        if (!marshal->ReadData(m_cookieLength, m_cookie))
        {
            return false;
        }
    }

    return true;
}

UInt32
MemberSet::GetMarshalLen(RSLProtocolVersion version)
{
    size_t numMembers = GetNumMembers();
    UInt32 nodeMarshalLen =
        MemberId::GetBaseSize(version) + // memberid
        4 + // ip
        2 + // rslport
        2 + // rsllearnpport
        2; // hostname length
    size_t marshalLen = nodeMarshalLen * numMembers + 6 + m_cookieLength;
    for (size_t whichMember = 0; whichMember < numMembers; ++whichMember)
    {
        RSLNode * node = &m_members[whichMember];
        size_t hostNameLength;
        LogAssert(SUCCEEDED(StringCchLengthA(node->m_hostName, sizeof(node->m_hostName), &hostNameLength)));
        marshalLen += hostNameLength;
    }

    return (UInt32)marshalLen;
}

size_t
MemberSet::GetNumMembers() const
{
    return m_members.Count();
}

RSLNode *
MemberSet::GetMemberInfo(UInt16 whichMember)
{
    return &m_members[whichMember];
}

const RSLNodeCollection&
MemberSet::GetMemberCollection() const
{
    return m_members;
}

RSLNodeArray *
MemberSet::GetMemberArray_Deprecated()
{
    m_members_Deprecated.clear();
    for (size_t i = 0; i < m_members.Count(); i++)
    {
        m_members_Deprecated.push_back(m_members[i]);
    }

    return &m_members_Deprecated;
}

bool
MemberSet::IncludesMember(MemberId memberId) const
{
    size_t numMembers = GetNumMembers();
    for (size_t whichMember = 0; whichMember < numMembers; ++whichMember)
    {
        if (memberId.Compare(m_members[whichMember].m_memberIdString) == 0)
        {
            return true;
        }
    }

    return false;
}

bool
MemberSet::Verify(RSLProtocolVersion version) const
{
    if (m_members.Count() == 0)
    {
        RSLError("Replica Set cannot be empty");
        return false;
    }

    for (size_t i = 0; i < m_members.Count(); i++)
    {
        for (size_t j = i + 1; j < m_members.Count(); j++)
        {
            // Same member Id
            if (MemberId::Compare(m_members[i].m_memberIdString, m_members[j].m_memberIdString) == 0)
            {
                RSLError(
                    "Replica Set contains a duplicated entry",
                    LogTag_RSLMemberId, m_members[i].m_memberIdString);
                return false;
            }
        }

        if (m_members[i].m_rslPort == 0)
        {
            RSLError("Replica port cannot be zero");
            return false;
        }

        if (m_members[i].m_memberIdString == NULL)
        {
            RSLError("Empty memberId invalid");
            return false;
        }

        if (version < RSLProtocolVersion_4)
        {
            char * endPtr = NULL;
            UInt64 value = _strtoui64(m_members[i].m_memberIdString, &endPtr, 0);
            if (*endPtr != NULL)
            {
                RSLError(
                    "MemberId must be a 64 bit number",
                    LogTag_RSLMemberId, m_members[i].m_memberIdString);
                return false;
            }
        }
    }
    return true;
}
void
MemberSet::SetConfigurationCookie(void *cookie, UInt32 cookieLength)
{
    FreeCookie();

    m_cookieLength = cookieLength;
    if (m_cookieLength != 0)
    {
        m_cookie = malloc(m_cookieLength);
        LogAssert(m_cookie);
        memcpy(m_cookie, cookie, cookieLength);
    }
}

void *
MemberSet::GetConfigurationCookie(UInt32 *length) const
{
    if (length)
    {
        *length = m_cookieLength;
    }
    return m_cookie;
}

void
MemberSet::FreeCookie()
{
    if (m_cookie != NULL)
    {
        free(m_cookie);
        m_cookie = NULL;
        m_cookieLength = 0;
    }
}


RSLConfigParam::RSLConfigParam()
{
    m_allowPrimaryPromotionWhileCatchingUp = false;
    m_newLeaderGracePeriodSec = 15;
    m_heartBeatIntervalSec = 2;
    m_electionDelaySec = 10;
    m_maxElectionRandomizeSec = 1;
    m_initializeRetryIntervalSec = 1;
    m_prepareRetryIntervalSec = 1;
    m_voteRetryIntervalSec = 3;
    m_voteMaxOutstandingIntervalSec = 480;
    m_cPQueryRetryIntervalSec = 5;
    m_maxCheckpointIntervalSec = 0;
    m_joinMessagesIntervalSec = 1;
    m_maxLogLenMB = 100;
    m_sendTimeoutSec = 5;
    m_receiveTimeoutSec = 5;
    m_maxCacheLengthMB = 50;
    m_maxVotesInLog = 10000;
    m_maxOutstandingPerReplica = 10;
    m_maxCheckpoints = 4;
    m_maxCheckpointSizeGB = 0;
    m_minCheckpointSpaceFileCount = 0;
    m_maxLogs = 10;
    m_logLenRandomize = 20;
    m_electionRandomize = 10;
    m_fastReadSeqThreshold = 0;
    m_numReaderThreads = 1;
    m_inMemoryExecutionQueueSizeMB = 0;
    m_maxMessageSizeMB = RSLConfig::s_MaxMessageLen;
    m_maxMessageAlertSizeMB = 0;
    m_workingDir[0] = '\0';
    m_addMemberIdToWorkingDir = true;
    m_cancelDiskIo = true;
};

RSLNodeCollection::RSLNodeCollection() :
    m_size(0),
    m_count(0),
    m_array(nullptr)
{
    m_size = s_Increment;
    m_array = new RSLNode[m_size];
    LogAssert(m_array != nullptr);
}

RSLNodeCollection::RSLNodeCollection(const RSLNodeCollection& other) :
    m_size(0),
    m_count(0),
    m_array(nullptr)
{
    CopyFrom(other);
}

RSLNodeCollection::~RSLNodeCollection()
{
    delete[] m_array;
    m_array = nullptr;
}

RSLNodeCollection& RSLNodeCollection::operator=(RSLNodeCollection const& other)
{
    CopyFrom(other);

    return *this;
}

size_t RSLNodeCollection::Count() const
{
    return m_count;
}

void RSLNodeCollection::Append(const RSLNode& node)
{
    EnsureSize();
    LogAssert(m_count < m_size);

    m_array[m_count] = node;
    m_count++;
}

void RSLNodeCollection::Remove(size_t index)
{
    LogAssert(index < m_count);
    LogAssert(m_count > 0);

    m_count--;
    if (m_count == 0)
    {
        return;
    }

    for (size_t i = index; i < m_count; i++)
    {
        m_array[i] = m_array[i + 1];
    }
}

RSLNode& RSLNodeCollection::operator[](size_t index) const
{
    LogAssert(index < m_size);

    return m_array[index];
}

void RSLNodeCollection::Clear()
{
    m_count = 0;
}

void RSLNodeCollection::EnsureSize()
{
    if (m_size > m_count)
    {
        return;
    }

    LogAssert(m_size == m_count);

    RSLNode* pOldArray = m_array;
    const size_t newSize = m_size + s_Increment;
    RSLNode* pNewArray = new RSLNode[newSize];
    for (size_t i = 0; i < m_size; i++)
    {
        pNewArray[i] = m_array[i];
    }
    delete[] pOldArray;

    m_size = newSize;
    m_array = pNewArray;
}

void RSLNodeCollection::CopyFrom(const RSLNodeCollection& other)
{
    Clear();

    for (size_t i = 0; i < other.Count(); i++)
    {
        Append(other[i]);
    }
}

UInt64 RSLUtils::CalculateChecksum(const void* blob, const size_t dataSize)
{
    return Utils::CalculateChecksum(blob, dataSize);
}
