#pragma once

#include "basic_types.h"
#include "logging.h"
#include "dynstring.h"
#include "rsl.h"

#include <vector>

using namespace RSLib;

namespace RSLibImpl
{
    struct ConfigParam
    {
        ConfigParam();
        bool Init(RSLConfigParam *param);
        
        static const char* c_RSLSection;
    
        Int64 m_newLeaderGracePeriod;
        Int64 m_heartBeatInterval;
        Int64 m_electionDelay;
        Int64 m_maxElectionRandomize;
        Int64 m_initializeRetryInterval;
        Int64 m_prepareRetryInterval;
        Int64 m_voteRetryInterval;
        Int64 m_voteMaxOutstandingInterval;
        Int64 m_cPQueryRetryInterval;
        Int64 m_maxCheckpointInterval;
        Int64 m_joinMessagesInterval;
        Int64 m_maxLogLen;
        UInt32 m_sendTimeout;
        UInt32 m_receiveTimeout;
        UInt32 m_maxCacheLength;
        UInt32 m_maxVotesInLog;
        UInt32 m_maxOutstandingPerReplica;
        UInt32 m_maxCheckpoints;
        UInt64 m_maxCheckpointSize;
        UInt32 m_minCheckpointSpaceFileCount;
        UInt32 m_maxLogs;
        UInt32 m_logLenRandomize;
        UInt32 m_electionRandomize;
        UInt32 m_fastReadSeqThreshold;
        UInt32 m_inMemoryExecutionQueueSize;
        UInt32 m_numReaderThreads;
        UInt32 m_maxMessageSize;
        UInt32 m_maxMessageAlertSize;
        bool m_allowPrimaryPromotionWhileCatchingUp;

        char   m_workingDir[MAX_PATH+1];
        bool m_addMemberIdToWorkingDir;
        bool m_cancelDiskIo;

        bool m_useGlobalAcceptMessagesFlag;
    };
    
    class RSLConfig
    {
        public:

        static const UInt32 s_MaxMessageLen = 100; // 100 MB

        RSLConfig();
        ~RSLConfig();

        bool Reload(RSLConfigParam *param);
        void Lock();
        void UnLock();

        Int64 NewLeaderGracePeriod();
        Int64 HeartBeatInterval();
        Int64 ElectionDelay();
		
        Int64 MaxElectionRandomize();
        Int64 InitializeRetryInterval();
        Int64 PrepareRetryInterval();
        Int64 VoteRetryInterval();
        Int64 VoteMaxOutstandingInterval();
        Int64 CPQueryRetryInterval();
        Int64 MaxCheckpointInterval();
        Int64 JoinMessagesInterval();
        Int64 MaxLogLen();
        UInt32 SendTimeout();
        UInt32 ReceiveTimeout();
        UInt32 MaxCacheLength();
        UInt32 MaxVotesInLog();
        UInt32 MaxOutstandingPerReplica();
        UInt32 MaxCheckpoints();
        UInt64 MaxCheckpointSize();
        UInt32 MinCheckpointSpaceFileCount();
        UInt32 MaxLogs();
        UInt32 LogLenRandomize();
        UInt32 ElectionRandomize();
        UInt32 FastReadSeqThreshold();
        UInt32 InMemoryExecutionQueueSize();
        UInt32 NumReaderThreads();
        UInt32 MaxMessageSize();
        UInt32 MaxMessageAlertSize();
        bool AllowPrimaryPromotionWhileCatchingUp();

        void   GetWorkingDir(DynString &str);
        bool AddMemberIdToWorkingDir();
        void ChangeElectionDelay(Int64 delayInSecs);
        bool CancelDiskIo();

        bool UseGlobalAcceptMessagesFlag();

        CRITSEC m_lock;
        ConfigParam *m_cfg;
    
    };

};
