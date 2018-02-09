#include "rslconfig.h"
#include "rsldebug.h"
#include "hirestime.h"
#include "ws2tcpip.h"
#include "DynString.h"
#include <strsafe.h>

using namespace RSLib;
using namespace RSLibImpl;

const char* ConfigParam::c_RSLSection = "RSL";

ConfigParam::ConfigParam()
{}

#define Get(_x, _y, _min, _max) do { \
    if (param->_x < _min || param->_x > _max) \
    { \
        RSLError("Configuration Error", LogTag_String1, #_y); \
        return false; \
    } \
    _y = param->_x; \
} while (0)

#define GetMax(_x, _y, _max) do { \
    if (param->_x > _max) { RSLError("Configuration Error", LogTag_String1, #_y); return false; }\
    _y = param->_x; \
} while (0)

bool
ConfigParam::Init(RSLConfigParam *param)
{
    Get(m_newLeaderGracePeriodSec, m_newLeaderGracePeriod, 1, 3600);
    Get(m_heartBeatIntervalSec, m_heartBeatInterval, 1, 3600);
    Get(m_electionDelaySec, m_electionDelay, 1, 3600);
    Get(m_maxElectionRandomizeSec, m_maxElectionRandomize, 0, INT_MAX);
    Get(m_initializeRetryIntervalSec, m_initializeRetryInterval, 1, 3600);
    Get(m_prepareRetryIntervalSec, m_prepareRetryInterval, 1, 3600);
    Get(m_voteRetryIntervalSec, m_voteRetryInterval, 1, 3600); 
    Get(m_voteMaxOutstandingIntervalSec, m_voteMaxOutstandingInterval, 1, 3600);
    Get(m_cPQueryRetryIntervalSec, m_cPQueryRetryInterval, 1, 3600);
    Get(m_maxCheckpointIntervalSec, m_maxCheckpointInterval, 0, _I64_MAX);
    Get(m_joinMessagesIntervalSec, m_joinMessagesInterval, 1, 3600);
    Get(m_maxLogLenMB, m_maxLogLen, 1, 100*1024);
    Get(m_sendTimeoutSec, m_sendTimeout, 1, 3600);
    Get(m_receiveTimeoutSec, m_receiveTimeout, 1, 3600);
    Get(m_maxCacheLengthMB, m_maxCacheLength, 1, 1024);
    Get(m_maxVotesInLog, m_maxVotesInLog, 1, INT_MAX);
    Get(m_maxOutstandingPerReplica, m_maxOutstandingPerReplica, 1, INT_MAX);
    Get(m_maxCheckpoints, m_maxCheckpoints, 1, INT_MAX);
    GetMax(m_maxCheckpointSizeGB, m_maxCheckpointSize, INT_MAX);
    GetMax(m_minCheckpointSpaceFileCount, m_minCheckpointSpaceFileCount, INT_MAX);
    Get(m_maxLogs, m_maxLogs, 1, INT_MAX);
    GetMax(m_logLenRandomize, m_logLenRandomize, 100);
    GetMax(m_electionRandomize, m_electionRandomize, 100);
    GetMax(m_fastReadSeqThreshold, m_fastReadSeqThreshold, INT_MAX);
    GetMax(m_inMemoryExecutionQueueSizeMB, m_inMemoryExecutionQueueSize, INT_MAX);
    GetMax(m_numReaderThreads, m_numReaderThreads, 100);
    Get(m_maxMessageSizeMB, m_maxMessageSize, 1, INT_MAX);
    GetMax(m_maxMessageAlertSizeMB, m_maxMessageAlertSize, INT_MAX);
    memcpy(m_workingDir, param->m_workingDir, ELEMENTCOUNT(param->m_workingDir));
    m_addMemberIdToWorkingDir = param->m_addMemberIdToWorkingDir;
    m_allowPrimaryPromotionWhileCatchingUp = param->m_allowPrimaryPromotionWhileCatchingUp;

    // validate that m_workingDir is not empty
    if (m_workingDir[0] == '\0')
    {
        RSLError("Configuration Error", LogTag_String1, m_workingDir); 
        return false;
    }

    if (m_inMemoryExecutionQueueSize == 0)
    {
        m_inMemoryExecutionQueueSize = m_maxCacheLength;
    }
    
    if (m_numReaderThreads == 0)
    {
        SYSTEM_INFO info;
        // get the processor info.
        GetNativeSystemInfo(&info);
        m_numReaderThreads = info.dwNumberOfProcessors;
    }
    
    // remove any trailing '\' from the working directory
    size_t dirLen = strlen(m_workingDir);
    if (m_workingDir[dirLen-1] == '\\')
    {
        m_workingDir[dirLen-1] = '\0';
    }
    
    m_newLeaderGracePeriod = HRTIME_SECONDS(m_newLeaderGracePeriod);
    m_heartBeatInterval = HRTIME_SECONDS(m_heartBeatInterval);
    m_electionDelay = HRTIME_SECONDS(m_electionDelay);
    m_maxElectionRandomize = HRTIME_SECONDS(m_maxElectionRandomize);
    m_initializeRetryInterval = HRTIME_SECONDS(m_initializeRetryInterval);
    m_prepareRetryInterval = HRTIME_SECONDS(m_prepareRetryInterval);
    m_voteRetryInterval = HRTIME_SECONDS(m_voteRetryInterval);
    m_cPQueryRetryInterval = HRTIME_SECONDS(m_cPQueryRetryInterval);
    if (m_maxCheckpointInterval > 0 &&
        m_maxCheckpointInterval < _I64_MAX/HRTIME_SECOND)
    {
        m_maxCheckpointInterval = HRTIME_SECONDS(m_maxCheckpointInterval);
    }
    else
    {
        m_maxCheckpointInterval = _I64_MAX;
    }
    m_joinMessagesInterval = HRTIME_SECONDS(m_joinMessagesInterval);

    m_sendTimeout *= 1000;
    m_receiveTimeout *= 1000;
    m_maxCacheLength *= 1024*1024;
    m_maxLogLen *= 1024*1024;
    m_inMemoryExecutionQueueSize *= 1024*1024;
    m_maxCheckpointSize *= 1024 * 1024 * 1024;

    m_maxMessageSize *= 1024*1024;
    m_maxMessageSize += 1024; // Add 1K for rsl message headers.
    if (m_maxMessageAlertSize > 0)
    {
        m_maxMessageAlertSize *= 1024 * 1024;
        m_maxMessageAlertSize += 1024;
    }

    m_cancelDiskIo = param->m_cancelDiskIo;

    return true;
}
    
RSLConfig::RSLConfig() : m_cfg(NULL)
{}

RSLConfig::~RSLConfig()
{
    delete m_cfg;
}

void
RSLConfig::Lock()
{
    m_lock.Enter();
}

void
RSLConfig::UnLock()
{
    m_lock.Leave();
}

bool
RSLConfig::Reload(RSLConfigParam *param)
{
    AutoCriticalSection lock(&m_lock);
    // make sure the replicas have not changed.
    ConfigParam *cfg = new ConfigParam();
    if (!cfg->Init(param))
    {
        return false;
    }
    delete m_cfg;
    m_cfg = cfg;
    return true;
}

Int64
RSLConfig::NewLeaderGracePeriod()
{
    AutoCriticalSection lock(&m_lock);
    return m_cfg->m_newLeaderGracePeriod;
}

Int64
RSLConfig::HeartBeatInterval()
{
    AutoCriticalSection lock(&m_lock);
    return m_cfg->m_heartBeatInterval;
}

Int64
RSLConfig::ElectionDelay()
{
    AutoCriticalSection lock(&m_lock);
    return m_cfg->m_electionDelay;
}

void
RSLConfig::ChangeElectionDelay(Int64 delayInSecs)
{
    AutoCriticalSection lock(&m_lock);
    m_cfg->m_electionDelay = HRTIME_SECONDS(delayInSecs);
}

Int64
RSLConfig::MaxElectionRandomize()
{
    AutoCriticalSection lock(&m_lock);
    return m_cfg->m_maxElectionRandomize;
}

Int64
RSLConfig::InitializeRetryInterval()
{
    AutoCriticalSection lock(&m_lock);
    return m_cfg->m_initializeRetryInterval;
}

Int64
RSLConfig::PrepareRetryInterval()
{
    AutoCriticalSection lock(&m_lock);
    return m_cfg->m_prepareRetryInterval;
}

Int64
RSLConfig::VoteRetryInterval()
{
    AutoCriticalSection lock(&m_lock);
    return m_cfg->m_voteRetryInterval;
}

Int64
RSLConfig::VoteMaxOutstandingInterval()
{
    AutoCriticalSection lock(&m_lock);
    return m_cfg->m_voteMaxOutstandingInterval;
}

Int64
RSLConfig::CPQueryRetryInterval()
{
    AutoCriticalSection lock(&m_lock);
    return m_cfg->m_cPQueryRetryInterval;
}

Int64
RSLConfig::MaxCheckpointInterval()
{
    AutoCriticalSection lock(&m_lock);
    return m_cfg->m_maxCheckpointInterval;
}

Int64
RSLConfig::JoinMessagesInterval()
{
    AutoCriticalSection lock(&m_lock);
    return m_cfg->m_joinMessagesInterval;
}

Int64
RSLConfig::MaxLogLen()
{
    AutoCriticalSection lock(&m_lock);
    return m_cfg->m_maxLogLen;
}

UInt32
RSLConfig::SendTimeout()
{
    AutoCriticalSection lock(&m_lock);
    return m_cfg->m_sendTimeout;
}

UInt32
RSLConfig::ReceiveTimeout()
{
    AutoCriticalSection lock(&m_lock);
    return m_cfg->m_receiveTimeout;
}

UInt32
RSLConfig::MaxCacheLength()
{
    AutoCriticalSection lock(&m_lock);
    return m_cfg->m_maxCacheLength;
}

UInt32
RSLConfig::MaxVotesInLog()
{
    AutoCriticalSection lock(&m_lock);
    return m_cfg->m_maxVotesInLog;
}

UInt32
RSLConfig::MaxOutstandingPerReplica()
{
    AutoCriticalSection lock(&m_lock);
    return m_cfg->m_maxOutstandingPerReplica;
}

UInt32
RSLConfig::MaxCheckpoints()
{
    AutoCriticalSection lock(&m_lock);
    return m_cfg->m_maxCheckpoints;
}

UInt32
RSLConfig::MaxLogs()
{
    AutoCriticalSection lock(&m_lock);
    return m_cfg->m_maxLogs;
}

UInt32
RSLConfig::LogLenRandomize()
{
    AutoCriticalSection lock(&m_lock);
    return m_cfg->m_logLenRandomize;
}

UInt32
RSLConfig::ElectionRandomize()
{
    AutoCriticalSection lock(&m_lock);
    return m_cfg->m_electionRandomize;
}

UInt32
RSLConfig::FastReadSeqThreshold()
{
    AutoCriticalSection lock(&m_lock);
    return m_cfg->m_fastReadSeqThreshold;
}

UInt32
RSLConfig::InMemoryExecutionQueueSize()
{
    AutoCriticalSection lock(&m_lock);
    return m_cfg->m_inMemoryExecutionQueueSize;
}

UInt32
RSLConfig::NumReaderThreads()
{
    AutoCriticalSection lock(&m_lock);
    return m_cfg->m_numReaderThreads;
}


UInt32
RSLConfig::MaxMessageSize()
{
    AutoCriticalSection lock(&m_lock);
    return m_cfg->m_maxMessageSize;
}

UInt32
RSLConfig::MinCheckpointSpaceFileCount()
{
    AutoCriticalSection lock(&m_lock);
    return m_cfg->m_minCheckpointSpaceFileCount;
}

UInt32
RSLConfig::MaxMessageAlertSize()
{
    AutoCriticalSection lock(&m_lock);
    return m_cfg->m_maxMessageAlertSize;
}

UInt64
RSLConfig::MaxCheckpointSize()
{
    AutoCriticalSection lock(&m_lock);
    return m_cfg->m_maxCheckpointSize;
}
    
void
RSLConfig::GetWorkingDir(DynString &str)
{
    AutoCriticalSection lock(&m_lock);
    str.AppendF(m_cfg->m_workingDir);
}

bool
RSLConfig::AddMemberIdToWorkingDir()
{
    AutoCriticalSection lock(&m_lock);
    return m_cfg->m_addMemberIdToWorkingDir;
}

bool
RSLConfig::AllowPrimaryPromotionWhileCatchingUp()
{
    AutoCriticalSection lock(&m_lock);
    return m_cfg->m_allowPrimaryPromotionWhileCatchingUp;
}

bool
RSLConfig::CancelDiskIo()
{
    AutoCriticalSection lock(&m_lock);
    return m_cfg->m_cancelDiskIo;
}

