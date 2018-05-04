#pragma once

#include "netpacketsvc.h"
#include "netpacket.h"
#include "rsl.h"
#include "message.h"
#include "rslconfig.h"
#include "DynString.h"
#include "PoolLock.h"
#include <map>
#include <vector>
#include "streamio.h"

namespace RSLibImpl
{
    static const UInt32 s_PageSize = 512;
    static const UInt32 s_SystemPageSize = 4096;
    static const UInt32 PAGES_PER_WRITE = 512;
    static const UInt32 s_ChecksumBlockSize = 4 * 1024 * 1024; // 4 megabytes


    inline UInt32 RoundUpToPage(UInt32 x)
    {
        return ((x + (s_PageSize - 1)) & ~(s_PageSize - 1));
    }

    inline UInt32 RoundUpToSystemPage(UInt32 x)
    {
        return ((x + (s_SystemPageSize - 1)) & ~(s_SystemPageSize - 1));
    }

    class StreamReader
    {
    public:
        StreamReader() {}
        virtual ~StreamReader() {}
        virtual DWORD32 Read(void *buffer, UInt32 numBytes, UInt32 *read) = 0;
        virtual UInt64 BytesRead() = 0;

    };

    class SocketStreamReader : public StreamReader
    {
    public:
        SocketStreamReader(StreamSocket *sock);
        DWORD32 Read(void *buffer, UInt32 numBytes, UInt32 *read);

        UInt64 BytesRead() { return m_read; };
        StreamSocket *m_sock;
        UInt64 m_read;
    };

    class DiskStreamReader : public StreamReader
    {
    public:
        DiskStreamReader(APSEQREAD *seqRead);
        DWORD32 Read(void *buf, UInt32 numBytes, UInt32 *read);

        UInt64 BytesRead() { return m_read; }
        APSEQREAD *m_seqRead;

        UInt64 m_read;
    };

    class VoteQueue
    {
    private:
        // a simple class that maintains a ref count on the vote and has a link member
        class VoteLink
        {
        public:

            Ptr<Vote> m_ptr;
            UInt32 m_len;
            Link<VoteLink> link;

            VoteLink(Vote* ptr) : m_ptr(ptr), m_len(ptr->GetMarshalLen()) {}

            ~VoteLink(void) { m_ptr = NULL; }
        };

        Queue<VoteLink> m_queue;
        UInt32 m_size;
        UInt32 m_numElements;

        void EnqueueLink(VoteLink *wrapper)
        {
            m_queue.enqueue(wrapper);
            m_size += wrapper->m_len;
            m_numElements++;
        }

        VoteLink* DequeueLink()
        {
            VoteLink *wrapper = m_queue.dequeue();
            if (wrapper)
            {
                m_size -= wrapper->m_len;
                m_numElements--;
            }
            return wrapper;
        }

    public:

        VoteQueue() : m_size(0), m_numElements(0) {}

        ~VoteQueue()
        {
            LogAssert(m_numElements == 0);
        }

        void Enqueue(Vote *vote)
        {
            VoteLink *wrapper = new VoteLink(vote);
            EnqueueLink(wrapper);
        }

        Vote* Dequeue(Ptr<Vote> *votePtr)
        {
            VoteLink *wrapper = DequeueLink();
            Vote *vote = (wrapper) ? wrapper->m_ptr : NULL;
            if (votePtr)
            {
                *votePtr = vote;
            }
            delete wrapper;
            return vote;
        }

        Vote* Tail()
        {
            VoteLink *wrapper = m_queue.tail;
            Vote *vote = (wrapper) ? wrapper->m_ptr : NULL;
            return vote;
        }

        Vote *Head()
        {
            VoteLink *wrapper = m_queue.head;
            Vote *vote = (wrapper) ? wrapper->m_ptr : NULL;
            return vote;
        }

        void Append(VoteQueue &q)
        {
            m_queue.append(q.m_queue);
            m_size += q.m_size;
            m_numElements += q.m_numElements;
            q.m_queue.clear();
            q.m_size = 0;
            q.m_numElements = 0;
        }

        void Clear()
        {
            VoteLink *wrapper;
            while ((wrapper = DequeueLink()) != NULL)
            {
                delete wrapper;
            }
        }

        UInt32 NumElements()
        {
            return m_numElements;
        }

        UInt32 SizeInBytes()
        {
            return m_size;
        }
    };

    typedef std::vector<StatusResponse*> StatusList;
    typedef std::vector<Replica*>::iterator ReplicaIter;
    typedef std::map<MemberId, bool> ResponseMap;
    typedef void (Legislator::*MemFun)();
    typedef void (Legislator::*MemFun1)(void *arg);

    class StatusMap
    {
    public:
        Queue<StatusResponse> m_queue;
        UInt32 m_numResponses;

        StatusMap();
        ~StatusMap();

        void Insert(StatusResponse *resp);
        UInt32 Size();

        void Clear();

        void FindHigherLogged(UInt64 minDecreeInLog, UInt64 decree, BallotNumber &ballot, StatusList *list);
        void FindCheckpoint(UInt64 decree, StatusList *list);
        Int64 FindMinReceivedAgo();
    };

    class LogFile
    {
    public:

        HANDLE m_hFile;
        HANDLE m_overlapEvent;

        std::vector<UInt64> m_decreeOffsets;
        UInt64 m_dataLen;
        char m_fileName[MAX_PATH + 1];
        UInt64 m_minDecree;

        LogFile();
        ~LogFile();
        DWORD32 Open(const char *dir, UInt64 decree);

        bool Write(SIZED_BUFFER* bufs, UInt32 count);
        bool Read(void* buf, UInt32 numBytes, UInt64 offset, HANDLE event);
        void AddMessage(Message *msg);
        DWORD32 SetWritePointer();
        UInt64 GetOffset(UInt64 decree);
        bool HasDecree(UInt64 decree);
        UInt32 GetLengthOfDecree(UInt64 decree);

        UInt64 MaxDecree();

    private:
        bool IssueWriteFileGather(FILE_SEGMENT_ELEMENT *segments, UInt64 offset,
            DWORD bytesToWrite);
    };

    class MemberSet : public RefCount
    {
    public:

        MemberSet();
        MemberSet(const MemberSet& memberset);
        MemberSet(const RSLNodeCollection &members, void *cookie, UInt32 cookieLength);
        ~MemberSet();

        void Copy(const MemberSet *memberset);

        void Marshal(MarshalData *marshal, RSLProtocolVersion version);
        bool UnMarshal(MarshalData *marshal, RSLProtocolVersion version);
        UInt32 GetMarshalLen(RSLProtocolVersion version);

        size_t GetNumMembers() const;
        RSLNode *GetMemberInfo(UInt16 whichMember);
        const RSLNodeCollection& GetMemberCollection() const;
        RSLNodeArray *GetMemberArray_Deprecated();     // TODO: Remove once STL is fully removed from the RSL API signatures.

        bool IncludesMember(MemberId memberId) const;
        void SetConfigurationCookie(void *cookie, UInt32 cookieLength);

        void *GetConfigurationCookie(UInt32 *length) const;

        bool Verify(RSLProtocolVersion version) const;

    private:
        RSLNodeArray m_members_Deprecated;   // TODO: STL vector, should be removed once STL is fully removed from the RSL API signatures.
        RSLNodeCollection m_members;

        void *m_cookie;
        UInt32 m_cookieLength;

        // declaration only
        MemberSet& operator=(const MemberSet &copy);

        void FreeCookie();
    };

    class ConfigurationInfo : public RefCount
    {
    public:
        ConfigurationInfo() : m_configurationNumber(0), m_initialDecree(0) { }
        ConfigurationInfo(UInt32 configurationNumber, UInt64 initialDecree, MemberSet *memberSet) :
            m_configurationNumber(configurationNumber), m_initialDecree(initialDecree), m_memberSet(memberSet)
        { }

        void Marshal(MarshalData *marshal, RSLProtocolVersion version);
        bool UnMarshal(MarshalData *marshal, RSLProtocolVersion version);
        UInt32 GetMarshalLen(RSLProtocolVersion version);

        UInt32 GetConfigurationNumber() const { return m_configurationNumber; }
        UInt64 GetInitialDecree() const { return m_initialDecree; }
        MemberSet *GetMemberSet() { return m_memberSet; }
        size_t GetNumMembers() { return m_memberSet->GetNumMembers(); }
        const RSLNodeCollection& GetMemberCollection() { return m_memberSet->GetMemberCollection(); }
        const RSLNode *GetMemberInfo(UInt16 whichMember) { return m_memberSet->GetMemberInfo(whichMember); }
        bool IncludesMember(MemberId memberId) { return m_memberSet->IncludesMember(memberId); }

        void UpdateMemberSet(MemberSet *memberset)
        {
            m_memberSet = memberset;
        }

    private:
        UInt32 m_configurationNumber;
        UInt64 m_initialDecree;
        Ptr<MemberSet> m_memberSet;
    };

    class CheckpointHeader
    {
    public:
        CheckpointHeader() :
            m_version(RSLProtocolVersion_1), m_unMarshalLen(0), m_checksum(0),
            m_memberId(), m_lastExecutedDecree(0), m_stateSaved(true), m_size(0),
            m_checksumBlockSize(0)
        {}

        UInt32 GetMarshalLen();
        void Marshal(const char* file);
        void Marshal(MarshalData *marshal);
        bool UnMarshal(const char* file);
        bool UnMarshal(MarshalData *marshal);
        bool UnMarshal(StreamReader *reader);
        void SetBytesIssued(RSLCheckpointStreamWriter * writer);

        static void GetCheckpointFileName(DynString &file, UInt64 decree);

        RSLProtocolVersion m_version;
        UInt32 m_unMarshalLen;
        UInt64 m_checksum;
        MemberId m_memberId; // member that produced this checkpoint
        UInt64 m_lastExecutedDecree;  // decree executed at this checkpoint
        BallotNumber m_maxBallot;
        Ptr<ConfigurationInfo> m_stateConfiguration;
        Ptr<Vote> m_nextVote;
        bool m_stateSaved;                // Indicates whether user data was saved too
        unsigned long long m_size;        // The whole chekcpoint file size
        unsigned int m_checksumBlockSize; // The size of each block (user data + checksum token)
    };

    class Legislator;

    // This object keeps track of the votes that have passed and need
    // to be executed. Since the size of the executeQ is throttled,
    // there might be occasions where some votes are not in this
    // queue, but only in the log file. The ReadLoop() method
    // periodically reads the votes from the log file and inserts
    // them in the m_toExecuteQ.

    class ExecuteQueue
    {
    public:

        ExecuteQueue();
        ~ExecuteQueue();

        void Stop();
        void Init(Legislator* legislator);
        void Run();

        // If must is true, the vote is inserted in m_toExecuteQ even
        // if it is full. This is needed for cases where the legislator
        // is the primary. Since the vote has a cookie in it and we
        // don't save the cookie in the log file, it has to be in
        // memory.
        void Enqueue(Vote* vote, bool must);
        Vote* Dequeue(Ptr<Vote> *votePtr);
        Vote* Head(Ptr<Vote> *votePtr);
        bool IsInMemory();
        // Called by the Execute Thread to wait until another
        // vote is inserted or read from the log file
        void WaitExecuteThread(DWORD timeout);

        bool IsInitialized()
        {
            return (m_legislator != NULL);
        }

        UInt64 GetLastPassedDecree()
        {
            return m_lastPassedDecree;
        }

    private:
        // Votes ready to be executed. All votes in this
        // queue are contiguous.
        VoteQueue m_toExecuteQ;

        enum ThreadState
        {
            Thread_Run,
            Thread_Terminate,
            Thread_Terminated,
        };

        ThreadState m_paused;

        // These are votes that have been passed, but can't
        // be executed until we read some votes from the log.
        // The sum of the sizes of m_toExecuteQ and m_cacheQ
        // does not exceed the maximum configured size.
        // If a vote can't be inserted into the m_toExecuteQ,
        // its inserted in this queue.
        VoteQueue m_cacheQ;

        UInt64 m_lastPassedDecree;
        UInt64 m_lastInsertedDecree;
        Legislator *m_legislator;
        // TRUE if the thread is currently reading from log
        bool m_fIOWait;

        HANDLE m_executeWaitHandle;
        HANDLE m_readWaitHandle;
        HANDLE m_overlapEvent;

        static void ReadExecuteFromDiskLoop(void *arg);
        // Periocally reads votes from the log file (if any).
        void ReadLoop();

        LogFile* CheckRead();
    };

    class CheckpointSavedNotification
    {
    public:

        UInt64 m_checkpointedDecree;
        DynString m_destFileName;
        void *m_cookie;

        CheckpointSavedNotification(UInt64 checkpointedDecree, DynString destFileName, void *cookie)
        {
            this->m_checkpointedDecree = checkpointedDecree;
            this->m_destFileName.Append(destFileName);
            this->m_cookie = cookie;
        }
    };

    class LegislatorArgWrapper;

    __declspec(align(16)) class Legislator
    {
    public:
        void* operator new(size_t s)
        {
            return _aligned_malloc(s, 16);
        }

        void operator delete(void* p)
        {
            _aligned_free(p);
        }

        enum State
        {
            PaxosInactive,
            StablePrimary,
            StableSecondary,
            Preparing,
            Initializing
        };

        enum MainThreadState
        {
            MainThread_Run,
            MainThread_Pause,
            MainThread_Paused,
            MainThread_Terminate,
            MainThread_Terminated,
        };

        Legislator(bool IndependentHeartbeat);
        ~Legislator();

        void SetSpawnThreadRoutine(SpawnRoutine spawnRoutine);

        bool Start(
            RSLConfigParam *cfg,
            const RSLNodeCollection &membersOfInitialConfiguration,
            const RSLNode &self,
            RSLProtocolVersion version,
            bool serializeFastReadsWithReplicate,
            RSLStateMachine *sm);

        bool Start(
            RSLConfigParam *cfg,
            const RSLNode &self,
            RSLProtocolVersion version,
            bool serializeFastReadsWithReplicate,
            RSLStateMachine *sm);

        bool StartImpl(
            RSLConfigParam *cfg,
            const RSLNodeCollection &membersOfInitialConfiguration,
            const RSLNode &self,
            RSLProtocolVersion version,
            bool serializeFastReadsWithReplicate,
            RSLStateMachine *sm);

        void ChangeElectionDelay(long long delayInSecs);

        RSLResponseCode Bootstrap(MemberSet * memberSet, UInt32 timeout);
        bool HasConfigurationCheckpointed();
        bool IsBootstrapped();
        bool PersistMemberSet(const MemberSet * memberSet);
        RSLResponseCode StartBootstrap(MemberSet * memberSet);
        void CheckBootstrap();

        bool Replay(const char* dir,
            UInt64 maxDecree,
            __in_opt UInt64 checkpointDecree,
            __in_opt const char* checkpointDirectory,
            RSLStateMachine *sm);

        void AttemptPromotion();

        RSLResponseCode ReplicateRequest(void* request, size_t len, void* cookie);
        RSLResponseCode ReplicateRequestExclusiveVote(void* request, size_t len, void* cookie);
        RSLResponseCode ReplicateRequestExclusiveVote(void* request, size_t len, void* cookie, bool isLastVote);

        RSLResponseCode FastReadRequest(
            UINT64 maxSeenSeqNo,
            void* request,
            size_t len,
            void* cookie,
            UInt32 timeout);

        RSLResponseCode ChangeConfiguration(MemberSet *configuration, void* cookie);
        void AllowSaveState(bool yes);
        void SetVotePayload(UInt64 payload);
        bool IsAnyRequestPendingExecution();
        void GetConfiguration(MemberSet * configuration, UInt32 *number = NULL);

        RSLResponseCode CopyStateFromReplica(void *replica);

        static DWORD32 GetFileNumbers(DynString &dir, char *szBuf, std::vector<UInt64> &numbers);

        void GetStatisticsSnapshot(RSLStats * pStats);

        UInt64 GetCurrentSequenceNumber();
        UInt64 GetHighestPassedSequenceNumber();
        void GetCurrentPrimary(RSLNode *node);

        void Pause();
        void Resume();
        void Unload();

        bool IsInShutDown();

        // record the vote accepted by the given replica
        void RecordVoteAcceptedFromReplica(Message *msg);

        // fills up the given structure with the health information for the replicas.
        // returns n>0 if the given structure doesn't contain enough slots (and n is the number of required slots)
        // returns 0 if the copy was completed. Filled up slots has m_memberId!=NULL. Slots are filled in sequence.
        UInt16 GetReplicasInformation(UInt16 numEntries, ReplicaHealth *replicaSetHealth);

        // fills up the given structure with the health information for the replica.
        // returns true if we know about replicaHealth->m_memberId and we can collect the information
        // returns false otherwise
        bool GetReplicaInformation(ReplicaHealth *replicaHealth);

        void ProcessSend(Packet *packet, TxRxStatus status, bool asClient);
        void ProcessReceive(Packet *packet, bool asClient);
        void ProcessConnect(UInt32 ip, UInt16 port, ConnectHandler::ConnectState state,
            bool asClient);

        // Debug methods
        static bool ForDebuggingPurposesUpdateCheckpointFile(
            RSLProtocolVersion version,
            const char* fileName,
            const MemberSet *rslmemberset);

        static bool ForDebuggingPurposesUpdateLogFile(
            DynString& dir,
            UInt64 logDecree,
            std::map<UInt64, RSLNodeCollection*> &configs);

        static void EnableListenOnAllIPs();

        void GetPaxosConfiguration(Ptr<MemberSet> &configuration, UInt32 *number = NULL);
        UInt32 GetHighestDefunctConfigurationNumber();
        void SetNextElectionTime(UInt32 secondsFromNow);
        void RelinquishPrimary();
        bool IsInStableState();

        void SetAcceptMessages(bool acceptMessages);

    private:

        static Message *UnMarshalMessage(IMarshalMemoryManager *memory);

        void FillCounters(Replica *replica, ReplicaHealth *health, bool isPrimary);

        void DeclareShutdown();

        void Stop();
        void MainLoop();
        void HeartBeatLoop();
        void ExecuteLoop();
        void FastReadLoop();
        void ReadExecuteFromDiskLoop();
        void StartExecuteQ();

        void FetchServerLoop();
        void HandleFetchRequest(void *ctx);
        void CopyCheckpointFromSecondary(void *ctx);
        void IPLookup();

        void Timer();
        void CheckNotifyStatus(bool curStatus);
        void HandleMessage(Message *msg);
        bool VerifyMessage(Message *msg);
        void HandleBootstrapMessage(BootstrapMsg * msg);
        void HandleNewVotes(VoteQueue &localQueue);
        void HandleReconfigurationDecisionMsg(Message *decision);
        void HandleJoinMsg(JoinMessage *joinMessage);
        void HandleJoinRequestMsg(Message *msg);

        void HandleVoteAcceptedMsg(Message *msg);
        void HandlePrepareMsg(PrepareMsg *msg);
        void HandlePrepareAcceptedMsg(PrepareAccepted *msg);
        void HandleNotAcceptedMsg(Message *msg);
        void HandleStatusResponseMsg(StatusResponse *query);

        void HandleStatusQueryMsg(Message *msg, StreamSocket *sock);

        void HandleFetchVotesMsg(Message *msg, StreamSocket *sock);
        void HandleFetchCheckpointMsg(Message *msg, StreamSocket *sock);
        void LearnVotesAndTransition(StatusResponse *resp);
        bool LearnVotes(UInt32 ip, UInt16 port);
        static bool ReadNextMessage(StreamReader *stream, IMarshalMemoryManager *memory,
            Message **msg, bool restore);
        ConfigurationInfo* ReadConfiguration(StreamReader *reader);
        Replica *GetReplica(MemberId &memberId, unsigned int remoteIp);

        static bool VerifyZeroStream(StreamReader *stream);

        void LogAndAcceptVote(Vote *vote);
        void ExecuteVote(Vote *vote, bool *shouldSave);

        void AddToExecutionQueue(Vote *vote);
        void SetMaxBallot(BallotNumber &ballot);

        void StartInitializing();
        void StartBeingStablePrimary();
        void StartBeingStableSecondary();
        void StartPreparing();
        void MoveToSecondaryOrPrepare();
        void SendNextVote(Vote *vote);
        void ReSendCurrentVote();
        void ReSendPrepareMsg();
        void SendStatusRequestMessage();
        void SendAndLogReconfigurationDecision(UInt64 decree, BallotNumber& ballot);
        void SendDefunctConfigurationMessage(UInt32 ip, UInt16 port, bool asClient);
        void SendJoinRequestMessage(UInt32 configurationNumber, UInt32 ip, UInt16 port, bool asClient);
        void SendJoinMessage(UInt32 ip, UInt16 port, bool asClient);

        void SendMessageToReplica(Replica *replica, Message *msg);
        void SendMessageToIpPort(UInt32 ip, UInt16 port, Message *msg, bool asClient);

        void SendMessage(UInt32 ip, UInt16 port, Replica *replica, Message *msg, bool asClient);

        void SendResponse(UInt32 ip, UInt16 port, Message *msg);
        void ReSendRequest(Message *msg, Int64 failedReplicaTimeout);
        void SendRequestToAll(Message *msg);
        void SendRequestToReplica(Replica *replica, Message *msg);

        void StartLearning(UInt32 ip, UInt16 port);
        void LogVote(Vote *vote);
        void LogPrepare(PrepareMsg *msg);
        void LogReconfigurationDecision(Message *decision);
        void AddMessageToLog(Message *msg);

        void GetCheckpointFileName(DynString &file, UInt64 decree);
        DWORD32 SendFile(char *file, UInt64 offset, Int64 length, StreamSocket *sock);

        UInt64 GetLastWriteFileTime(const char * fileName);
        bool RestoreState(UInt64 maxDecree, bool readOnly, UInt32 initialConfigNumber);

        void CopyStateThread(void *cookie);

        bool IsCheckpointing();
        bool CopyCheckpoint(UInt32 ip, UInt16 port, UInt64 checkpointedDecree, UInt64 size, void *cookie, bool notifyStateMachine = true);
        bool SaveCheckpoint(Vote *vote, bool saveState = true);
        bool VerifyCheckpoint(const char* file);

        void CheckpointDone(UInt64 decree, const char *tempFile, const char* file, UInt64 size);
        UInt64 GetCopyCheckpointAt();
        void SetCopyCheckpointAt(UInt64 val);
        UInt64 GetSaveCheckpointAt();
        void SetSaveCheckpointAt(UInt64 val);

        void CleanupLogsAndCheckpoint();

        __declspec(noreturn) void ShutDown();

        void AbortReplicateRequests(RSLResponseCode status);

        bool AmPrimary() { return m_state == StablePrimary; }

        bool IsExecuteQueueReady();

        UInt32 PaxosConfiguration() { return m_paxosConfiguration->GetConfigurationNumber(); }
        UInt32 QuorumSize();

        void ReadDefunctFile();
        void UpdateDefunctInfo(UInt32 defunctConfigurationNumber);
        void SwitchToPaxosInactiveState();
        void ChangePaxosConfiguration(Ptr<ConfigurationInfo> configuration);
        void SendJoinMessagesIfNecessary();

        static int GetNativePriority(ThreadPriority priority);

        void RunThread(MemFun1 startMethod, const char *name, void *arg, ThreadPriority priority = Priority_Normal, HANDLE *handle = NULL);
        void RunThread(MemFun startMethod, const char *name, ThreadPriority priority = Priority_Normal, HANDLE *handle = NULL);
        void RunThread(LegislatorArgWrapper *wrapper, ThreadPriority priority, HANDLE* handle);

        static unsigned int __stdcall ThreadStartMethod(void *arg);
        static unsigned int __stdcall ThreadStartMethodCustomized(void *arg);

        void StopIPLoop();
        void StopMainLoop();
        void StopHeartBeatLoop();
        void StopExeLoop();
        void StopFastReadLoops();
        void StopAcceptLoop();
        void StopNet();

        void ResetStatistics();

        void VerifyDiskSectorSize();

    private:

        // this is used to count the number of active threads for this legislator.
        mutable volatile long m_numThreads;

        // this is used to know if the legislator was properly initialized (if Initialize returned true)
        bool m_isInitializeCompleted;

        // this is used as a delegate to start threads.
        // if not present (default), then beginthread will be used
        SpawnRoutine m_spawnThread;

        // these variables either don't change or are
        // changed/accessed only by the main thread. No
        // need to take locks here.
        MemberId m_memberId;
        RSLStateMachine *m_stateMachine;
        DynString m_dataDir;
        DynString m_tempDir;
        bool m_serializeFastReadsWithReplicate;

        static bool s_ListenOnAllIPs;

        CheckpointSavedNotification *m_checkpointSavedNotification;

        // The field m_replicas can be accessed by multiple threads.
        // Modifications to it are made holding the m_replicasLock.  Note
        // that this lock is held exclusive only when m_replicas itself is
        // changing, not when fields of the individual Replica objects in
        // m_replicas are changing.

        std::vector<Replica*> m_replicas;
        Replica *m_self;
        NetPacketSvc *m_netlibServer;
        NetPacketSvc *m_netlibClient;
        std::unique_ptr<StreamSocket> m_pFetchSocket;
        // don't need lock here.
        Ptr<Vote> m_oldFreshestVote;
        Ptr<Vote> m_outstandingVote;
        StatusMap m_statusMap;
        BallotNumber m_maxSeenBallot;

        ResponseMap m_responseMap;
        PrepareMsg *m_lastPrepareMsg;

        State m_state;

        Int64 m_electionDelay;
        Int64 m_nextElectionTime;
        Int64 m_prepareTimeout;
        Int64 m_actionTimeout;
        Int64 m_cpTimeout;
        Int64 m_nextCheckpointTime;
        Int64 m_lastTimeJoinMessagesSent;

        UInt64 m_maxLogLen;
        volatile UInt64 m_copyCheckpointAtDecree;
        volatile UInt64 m_saveCheckpointAtDecree;

        CRITSEC m_lock;
        CRITSEC m_checkpointSavedNotificationLock;
        CRITSEC m_logfileAccessLock;
        CRITSEC m_hbLock;
        CRITSEC m_recvQLock;
        CRITSEC m_fastReadCallbackQLock;
        CPoolLock  m_callbackLock;
        CPoolLock  m_shutdownLock;
        CPoolLock  m_replicasLock;

        HANDLE m_waitHandle;
        HANDLE m_hbWaitHandle;
        HANDLE m_fastReadWaitHandle;
        HANDLE m_resolveWaitHandle;

        // The following 2 items are protected by the m_recvQLock;

        VoteQueue m_voteCache;
        Queue<Message> m_recvMsgQ;

        // these can be accessed by other threads
        // these are changed under the m_lock atomically.
        BallotNumber m_maxBallot;
        Ptr<Vote> m_maxAcceptedVote;

        UInt64 m_votePayload;

        std::vector<LogFile *> m_logFiles;
        Ptr<ConfigurationInfo> m_paxosConfiguration;
        Ptr<MemberSet> m_bootstrapMemberSet;
        UInt32 m_highestDefunctConfigurationNumber;

        bool m_isPrimary;
        bool m_isShutting;
        bool m_IndependentHeartbeat;
        bool m_IsTransitioningToPrimary;

        // call stateMachine->NotifyPrimaryRecovered() after this vote
        // has been executed
        Ptr<Vote> m_callbackAfterVote;

        VoteQueue m_votesToSend;

        // Requests are dequeued and executed atomically
        // under shared m_executeLock
        Queue<RequestCtx> m_fastReadQ;
        Queue<RequestCtx> m_fastReadTimeoutQ;
        Queue<RequestCtx> m_fastReadCallbackQ;
        UInt32 m_fastReadCallbackQSize;

        UInt64 m_minFastReadReq;

        // votes are dequeued and executed atomically
        // under exclusive m_executeLock
        // votes are inserted only by the main thread.
        ExecuteQueue m_executeQ;

        volatile UInt64 m_lastExecutedDecree;

        UInt64 m_checkpointedDecree;
        UInt64 m_checkpointSize;
        HANDLE m_copyCheckpointThread;
        bool m_checkpointAllowed;
        bool m_mustCheckpoint;
        volatile bool m_isSavingCheckpoint;

        volatile MainThreadState m_paused;
        volatile MainThreadState m_hbPaused;
        volatile MainThreadState m_ipPaused;
        volatile MainThreadState m_exePaused;
        volatile MainThreadState m_acceptPaused;
        volatile MainThreadState m_frPaused;
        volatile unsigned int m_frPausedCount;
        volatile bool m_relinquishPrimary;
        // m_forceReelection is used to indicate that we need re-elect the primary without any delay;
        volatile bool m_forceReelection;

        RSLConfig m_cfg;
        RSLProtocolVersion m_version;
        PrimaryCookie *m_primaryCookie;

        Ptr<ConfigurationInfo> m_stateConfiguration;

        // Current statistics being gathereed. These are initialized in StartImpl then reset on every call to GetStatisticsSnapshot(). Protected by m_statsLock.
        RSLStats m_stats;
        CRITSEC m_statsLock;

        friend class ExecuteQueue;

        bool m_acceptMessages;
    };

    class LegislatorArgWrapper
    {
    private:

        MemFun m_handler;
        MemFun1 m_handler1;
        Legislator *m_legislator;
        const char *m_name;
        void *m_arg;
        bool m_hasArg;

        LegislatorArgWrapper(Legislator *legislator, MemFun handler) :
            m_legislator(legislator), m_handler(handler), m_handler1(NULL),
            m_arg(NULL), m_hasArg(false)
        {}

        LegislatorArgWrapper(Legislator *legislator, MemFun1 handler, void *arg) :
            m_legislator(legislator), m_handler(NULL), m_handler1(handler),
            m_arg(arg), m_hasArg(true)
        {}

        void MakeCall()
        {
            if (m_hasArg)
            {
                MemFun1 handler = m_handler1;
                (m_legislator->*handler)(m_arg);
            }
            else
            {
                MemFun handler = m_handler;
                (m_legislator->*handler)();
            }
        }

        friend class Legislator;
    };

    class LegislatorPacket : public Packet
    {
    public:
        Ptr<Replica> m_replica;
        LegislatorPacket(UInt32 maxSize = 0) : Packet(maxSize)
        {
        }
        LegislatorPacket(Buffer *buffer, UInt32 maxSize = 0) : Packet(buffer, maxSize)
        {
        }
        ~LegislatorPacket()
        {
            m_replica = NULL;
        }
    };

    class LegislatorNetHandler : public SendHandler, public ReceiveHandler, public ConnectHandler
    {
    public:

        LegislatorNetHandler(Legislator *legislator, bool asClient);
        void ProcessSend(Packet *packet, TxRxStatus status);
        void ProcessReceive(Packet *packet);
        void ProcessConnect(UInt32 ip, UInt16 port, ConnectState state);

    private:
        Legislator *m_legislator;
        bool m_asClient;

    };
};
