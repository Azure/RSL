#pragma once

#include <windows.h>
#include <vector>

#ifdef RSL_EXPORTS
#define RSL_IMPORT_EXPORT   __declspec( dllexport )
#elif RSL_IMPORTS
#define RSL_IMPORT_EXPORT   __declspec( dllimport )
#else
#define RSL_IMPORT_EXPORT
#endif

#define STDCALL     __stdcall

namespace RSLibImpl
{
    class Legislator;
    class PrimaryCookie;
    class MemberSet;
    class CheckpointHeader;
    class APSEQREAD;
    class APSEQWRITE;
}

class TestEngine;

namespace RSLib
{
    typedef void(*NotificationsCallback)(int level, int logId, const char* title, const char *message);

    // log level used to pass to LogEntryCallBack
    typedef enum
    {
        CallBackLogLevel_Debug,
        CallBackLogLevel_Info,
        CallBackLogLevel_Status,
        CallBackLogLevel_Warning,
        CallBackLogLevel_Error,
        CallBackLogLevel_Alert,
        CallBackLogLevel_Assert
    } CallBackLogLevel;

    typedef void(*LogEntryCallback)(const char *file, const char *function, int line, int logId, CallBackLogLevel level, const char *title, const char *fmt, va_list args);

    /*
    * RSL creates and uses a subdirectory called "Logs" under the
    * directory specified by debugLogPath.

    * void (*notificationsCallback)(int level, int logId, const char* title, char*message)
    *   If specified (not NULL), it is a routine to be used
    *   to call back when a notification is sent to userland (i.e. on assert violations). Parameters will
    *   contain the notification level, logId and the message to be traced in the logs
    *   upon return of the routine, RSL will take a minidump and
    *   the process will exit.
    *
    * logEntryCallback
    *   If specified (not NULL), it is a routine called by the Logger for every log entry (debug/error/...).
    *   This allows the higher layer to capture and store the log entries in their own format.
    */

    RSL_IMPORT_EXPORT bool STDCALL RSLInit(const char * debugLogPath, bool logDebugMessages = false, NotificationsCallback callback = NULL, LogEntryCallback logEntryCallback = NULL);

    RSL_IMPORT_EXPORT void STDCALL SetMinidumpEnabled(bool enabled);

    RSL_IMPORT_EXPORT bool STDCALL SetThumbprintsForSsl(const char * pThumbPrintA, const char * pThumbPrintB, bool validateCAChain, bool checkCertificateRevocation);
    RSL_IMPORT_EXPORT bool STDCALL SetSubjectNamesForSsl(const char * pSubjectA, const char * pThumbPrintsParentA, const char * pSubjectB, const char * pThumbPrintsParentB, bool considerIdentitiesWhitelist);

    RSL_IMPORT_EXPORT void STDCALL EnableListenOnAllIPs();

    RSL_IMPORT_EXPORT void STDCALL RSLUnload();

    class RSLCheckpointStreamWriter;
    /*************************************************************************
    * Class: RSLCheckpointStreamReader
    *
    * Class for reading the application state stored on stable storage.
    *************************************************************************/
    class RSL_IMPORT_EXPORT RSLCheckpointStreamReader
    {
    public:

        RSLCheckpointStreamReader();

        ~RSLCheckpointStreamReader();

        /* Gets a pointer to bytes read. When crossing read buffers this
        * pointer goes to a temp buffer. Data is only copied when a read
        * crosses the read buffers. (Currently, the read buffer size is
        * 64K). So, if data is read in multiples of 64K, no copies would
        * be made
        *
        *  Paramaters:
        *      ppvBuffer     returns pointer to buffer with bytes of data.
        *      dwNumBytes    number of bytes requested.
        *      pcdRead       number of bytes read.
        *
        *  Returns NO_ERROR on successs, ERROR_HANDLE_EOF for end of file
        *  and GetLastError() if read fails */

        unsigned int GetDataPointer(void ** ppvBuffer, unsigned long dwNumBytes, unsigned long* pcbRead);

        /* copies the data to pvBuffer. Returns the same error codes as
        * GetDataPointer() */
        unsigned int GetData(void * pvBuffer, unsigned long dwNumBytes, unsigned long *pcbRead);

        /* skip 'dwNumBytes' bytes in the stream. */
        unsigned int Skip(unsigned long dwNumBytes);

        /* seek to position 'offset' in the exposed stream. */
        unsigned int Seek(unsigned long long offset);

        /* Returns the total length of the stream */
        unsigned long long Size();

        unsigned int Init(const char *file);

        char *GetFileName();

        unsigned int Init(const char *file, RSLibImpl::CheckpointHeader *header);

        /* Creates a RSLCheckpointStreamWriter with the header we had
        * on creation of this file */
        RSLCheckpointStreamWriter* CreateWriterForThisReader(const char *file);

        /* Closes the given writer, which has the given name, and
        * was opened with CreateWriterForThisReader.
        * To do so, it closes the writer, and flushes the length
        * into the header, and writes back the header into the file */
        void CloseWriterForThisReader(const char *file, RSLCheckpointStreamWriter *writer);

    private:

        RSLibImpl::CheckpointHeader* m_context;
        RSLibImpl::APSEQREAD *m_reader;
        unsigned long long m_offset;
        unsigned long long m_nextblockOffset;
        unsigned long long m_dataSize;
        char * m_filename;

        const static int MAXFILENAME_SIZE = 64 * 1024;
        const static int CHECKSUM_SIZE = sizeof(unsigned long long);
        char * m_dataBlock;
        unsigned int m_defaultBlockSize;
        unsigned int m_currentBlockSize;
        unsigned int m_dataReadOffset;

        unsigned int ReadNextDataBlock();

        friend class RSLibImpl::Legislator;
        friend class RDSStreamTester;
        friend class CsmCodexReader;
    };


    /*************************************************************************
    * Class: RSLCheckpointStreamWriter
    *
    * Class for writing the application state to stable storage
    *************************************************************************/
    class RSL_IMPORT_EXPORT RSLCheckpointStreamWriter
    {
    public:
        RSLCheckpointStreamWriter();

        ~RSLCheckpointStreamWriter();

        /* Write bytes from 'pbWrite' to internal buffer (and possibly
        * write to disk.
        * Return NO_ERROR on success, GetlastError() on failure */
        unsigned int  Write(const void* pbWrite, unsigned long cbWrite);

        unsigned int  Init(const char *file, RSLibImpl::CheckpointHeader *header);
        unsigned int  Close();

    private:
        unsigned long long BytesIssued();
        RSLibImpl::APSEQWRITE *m_writer;
        unsigned long long m_offset;

        const static int CHECKSUM_SIZE = sizeof(unsigned long long);
        void * m_buffer;
        unsigned long m_available;
        unsigned long m_written;
        unsigned int m_defaultBlockSize;
        unsigned int m_dataWrittenOffset;
        unsigned long long m_checksum;

        friend class RSLibImpl::Legislator;
        friend class RDSStreamTester;
        friend class RSLibImpl::CheckpointHeader;
    };

    enum RSLResponseCode {
        RSLSuccess,
        RSLFastReadStale,
        RSLNotPrimary,
        RSLShuttingDown,
        RSLAlreadyBootstrapped,
        RSLBootstrapFailed,
        RSLInvalidParameter,
    };


    /*
    * The type describing a custom Thread Spawn Routine
    *
    */
    enum ThreadPriority
    {
        Priority_Normal = 0,
        Priority_AboveNormal = 1,
        Priority_BelowNormal = 2,
        Priority_Highest = 3,
        Priority_Lowest = 4,
        Priority_OutOfGC = 16,
    };

    typedef void(*SpawnRoutine)(unsigned int(__stdcall *)(void*), ThreadPriority, void*);

    /* The version of protocol that RSL should use internally to talk to
    other replicas.  This should be passed as part of the Initialize
    method to RSL. Once the protocol is upgraded to a higher version,
    rolling back to the code version that does not understand the new
    protocol might not be possible.

    All versions listed here are compatible. A replica can send/receive
    messages with any of the versions listed here. Applications should
    periodically upgrade the version to the latest version to have the
    latest features/bug fixes.
    */
    enum RSLProtocolVersion
    {
        // First Protocol Version
        RSLProtocolVersion_1 = 1,

        // This version supports passing primary cookie as part of the
        // message. AcceptMessageFromReplica() will always be called with
        // (NULL, 0) Without this version
        RSLProtocolVersion_2 = 2,

        // This version introduces replica set management.
        // It allows replicas to be added, removed, replaced
        RSLProtocolVersion_3 = 3,

        // This version introduces checkpoint checksum and
        // bootstrap mechanism, which allows user to create
        // a replica set out of a group of available replicas
        // by issuing a command from a given replica
        RSLProtocolVersion_4 = 4,

        // This version introduces relinguishPrimary flag.
        RSLProtocolVersion_5 = 5,

        // This version introduces payload on votes from secondaries.
        RSLProtocolVersion_6 = 6,
    };

    /* a simple wrapper around the Ip and port */
    class RSL_IMPORT_EXPORT RSLNode
    {
    public:
        static const size_t MaxMemberIdLength = 63;

        // memberId of the rsl host (Note: must be unique among
        // replicas within the same set. MemberIdString must
        // be ASCII encoded and is case-sensitive.
        char m_memberIdString[MaxMemberIdLength + 1];

        // DNS name of the machine. Set this if you want RSL
        // to resolve the name to IP dynamically. This field is
        // not used by RSL if the statemachine overrides ResolveNode()
        // callback or if m_ip is set.
        // However, this information is preserved by RSL and
        // the application can use this field to store the DNS name if
        // it wants to resolve the name itself during the callback.
        char m_hostName[256];

        // IP address of the machine name. If this field is set,
        // RSL does not use the m_hostName field. However, the
        // ResolveNode() callback is still called. Application is
        // free to change m_ip in the ResolveNode() callback.
        // If neither m_hostName nor m_ip are set, application must
        // override ResolveNode(). Otherwise, communication to this
        // node will fail.
        // Note that m_ip is populated from inet_addr(). The inet_addr () function converts
        // the Internet host address cp from numbers-and-dots notation into binary data in network byte order.
        // On the i386 the host byte order is Least Significant Byte first (little endian),
        // whereas the network byte order, as used on the Internet, is Most Significant Byte first (big endian).
        // For example, if m_hostName="127.0.0.1", then the memory representation of m_ip is exactly "7f 00 00 01".
        // Then, in big endian OS, you will see that m_ip = 0x100007f = 0x0100007f.
        unsigned int m_ip;

        // Port number for paxos messages. Must be set
        unsigned short m_rslPort;       //  Host byte order.

                                        // Port number for learning . If not set, defaults
                                        // to m_rslPort+1
        unsigned short m_rslLearnPort;  //  Host byte order.

                                        // DEPRECATED
        unsigned short  m_appPort;

        // DEPRECATED.
        unsigned int m_priority;

        RSLNode() : m_ip(0), m_rslPort(0), m_rslLearnPort(0), m_appPort(0), m_priority(0)
        {
            m_hostName[0] = '\0';
            m_memberIdString[0] = '\0';
        }

        // For backward's compatibility with old versions. In
        // versions < RSLProtocolVersion_3, memberId was a 64 bit
        // integer. This allows setting the memberid as an integer
        // Internally, it is converted and stored as a decimal string
        void SetMemberIdAsUInt64(unsigned long long memberId);

        // For backward's compatibility with old versions. Retreives
        // the memberid as a 64 bit integer. It is valid to call
        // this method only if SetMemberIdAsUInt64() was called to
        // set the memberid
        unsigned long long GetMemberIdAsUInt64();

        // For backward's compatibility with old versions. Parses
        // the memberId string
        static unsigned long long ParseMemberIdAsUInt64(const char* memberId);
    };

    class RSL_IMPORT_EXPORT ReplicaHealth
    {
    public:
        // memberId of the rsl host (Note: must be unique among
        // replicas within the same set. MemberIdString must
        // be ASCII encoded and is case-sensitive.
        char m_memberId[RSLNode::MaxMemberIdLength + 1];

        bool m_isPrimary;
        bool m_connected;
        long m_numOutstanding;
        SYSTEMTIME m_failedAt;
        SYSTEMTIME m_lastRequestSentAt;
        SYSTEMTIME m_lastRequestVotedAt;
        unsigned long long m_lastRequestVotedDecree;
        unsigned int m_consecutiveFailures;
        bool m_needsResolve;
        unsigned long long m_lastVotePayload;

        ReplicaHealth()
        {
            m_memberId[0] = '\0';
        }
    };

    class RSL_IMPORT_EXPORT RSLNodeCollection
    {
    public:
        RSLNodeCollection();
        RSLNodeCollection(const RSLNodeCollection& other);
        ~RSLNodeCollection();

        RSLNodeCollection& operator=(const RSLNodeCollection& other);

        size_t Count() const;
        void Append(const RSLNode& node);
        void Remove(size_t index);
        RSLNode& operator[](size_t index) const;
        void Clear();

    private:
        void EnsureSize();
        void CopyFrom(const RSLNodeCollection& other);

    private:
        size_t m_size;
        size_t m_count;
        RSLNode* m_array;

        static const size_t s_Increment = 5;
    };

    // !!! DEPRECATED: RSLNodeArray is deprecated as part of effort to remove STL from RSL API signatures
    // !!! Plese use RSLNodeCollection instead.
    typedef std::vector<RSLNode> RSLNodeArray;

    class RSL_IMPORT_EXPORT RSLPrimaryCookie
    {
    public:
        RSLPrimaryCookie();
        ~RSLPrimaryCookie();

        void Set(void *data, unsigned int len);

    private:
        RSLibImpl::PrimaryCookie *m_data;
        friend class RSLibImpl::Legislator;
    };

    class RSLCheckpointUtility;

    class RSL_IMPORT_EXPORT RSLMemberSet
    {
    public:

        RSLMemberSet();
        RSLMemberSet(const RSLNodeCollection &members, void *cookie, unsigned int cookieLength);

        // !!! DEPRECATED:RSLNodeArray is deprecated as part of effort to remove STL from RSL API signatures
        // !!! Plese use RSLMemberSet constructor that has RSLNodeCollection instead.
        RSLMemberSet(const RSLNodeArray &members, void *cookie, unsigned int cookieLength);

        ~RSLMemberSet();

        size_t GetNumMembers() const;
        const RSLNode *GetMemberInfo(unsigned short whichMember) const;
        void GetMemberCollection(__out RSLNodeCollection& nodes) const;

        // !!! DEPRECATED: GetMemberArray is deprecated as part of effort to remove STL from RSL API signatures
        // !!! Plese use GetMemberCollection instead.
        const RSLNodeArray *GetMemberArray() const;

        bool IncludesMember(const char* memberId) const;
        void SetConfigurationCookie(void *cookie, unsigned int cookieLength);

        void *GetConfigurationCookie(unsigned int *length) const;

        static const unsigned int s_MaxMemberSetCookieLength = 8192;

    private:

        RSLMemberSet(const RSLMemberSet &other);
        RSLMemberSet& operator=(const RSLMemberSet &other);

        RSLibImpl::MemberSet* m_memberSet;

        friend class RSLibImpl::MemberSet;
        friend class RSLStateMachine;
        friend class RSLCheckpointUtility;
    };

    struct RSL_IMPORT_EXPORT RSLConfigParam
    {
        RSLConfigParam();

        long long m_newLeaderGracePeriodSec;
        long long m_heartBeatIntervalSec;
        long long m_electionDelaySec;
        long long m_maxElectionRandomizeSec;
        long long m_initializeRetryIntervalSec;
        long long m_prepareRetryIntervalSec;
        long long m_voteRetryIntervalSec;
        long long m_voteMaxOutstandingIntervalSec;
        long long m_cPQueryRetryIntervalSec;
        long long m_maxCheckpointIntervalSec;
        long long m_joinMessagesIntervalSec;
        long long m_maxLogLenMB;
        unsigned int m_sendTimeoutSec;
        unsigned int m_receiveTimeoutSec;
        unsigned int m_maxCacheLengthMB;
        unsigned int m_maxVotesInLog;
        unsigned int m_maxOutstandingPerReplica;
        unsigned int m_maxCheckpoints;

        //Give an alert when checkpoint file size exceed m_maxCheckpointSizeGB
        //if m_maxCheckpointSizeGB is set to zero, no aler will be given
        unsigned int m_maxCheckpointSizeGB;

        //Give an alert when space left in the checkpoint directory is smaller
        //than m_minCheckpointSpaceFileCount * Last_Checkpointfile_Size
        //if m_minCheckpointSpaceFileCount is set to 0, no alert will be given
        unsigned int m_minCheckpointSpaceFileCount;

        unsigned int m_maxLogs;
        unsigned int m_logLenRandomize;
        unsigned int m_electionRandomize;
        unsigned int m_fastReadSeqThreshold;
        unsigned int m_inMemoryExecutionQueueSizeMB;
        unsigned int m_numReaderThreads;
        unsigned int m_maxMessageSizeMB;

        //Give an alert when max message size exceed m_maxMessageAlertSizeMB
        //if m_maxMessageAlertSizeMB is set to 0, no alert will be given
        unsigned int m_maxMessageAlertSizeMB;

        // if m_allowPrimaryPromotionWhileCatchingUp is true
        // a replica may become primary while still executing requests learnt from the previous primary
        // if m_allowPrimaryPromotionWhileCatchingUp is false (default),
        // a replica must have executed every single request from the previous primary before even attempting
        // to become a primary (i.e. even before getting a call to CanBecomePrimary)
        bool m_allowPrimaryPromotionWhileCatchingUp;

        //  Only the leaf directory of specified path is created; intermediate
        //  pathname components must be created by caller.
        char   m_workingDir[MAX_PATH + 1];
        bool m_addMemberIdToWorkingDir;

        // Call CancelIoEx to attempt to cancel unwanted disk IO before waiting for its completion.
        bool m_cancelDiskIo;

        // Use global accept messages flag set by SetAcceptMessages rather than calling AcceptMessageFromReplica 
        bool m_useGlobalAcceptMessagesFlag;
    };

    // Filled in by GetStatisticsSnapshot. Returns incremental statistics
    // gathered since the last call. Set m_cbSizeOfThisStruct to
    // sizeof(RSLStats) prior to calling GetStatisticsSnapshot.
    struct RSL_IMPORT_EXPORT RSLStats
    {
        size_t              m_cbSizeOfThisStruct;

        unsigned long long  m_cCaptureTimeInMicroseconds;

        unsigned long long  m_cDecreesExecuted;

        // Track log reads (non-cached execution and handling learn vote requests).
        unsigned long long  m_cLogReads;
        unsigned long long  m_cLogReadBytes;
        unsigned long long  m_cLogReadMicroseconds;
        unsigned int        m_cLogReadMaxMicroseconds;

        // Track log writes (primary or secondary).
        unsigned long long  m_cLogWrites;
        unsigned long long  m_cLogWriteBytes;
        unsigned long long  m_cLogWriteMicroseconds;
        unsigned int        m_cLogWriteMaxMicroseconds;

        // Primary only; track time to pass a vote from sending the vote until a
        // quorum of accepts are received).
        unsigned long long  m_cVotingTimeMicroseconds;
        unsigned int        m_cVotingTimeMaxMicroseconds;
    };

    /*************************************************************************
    * Class: RSLStateMachine
    *
    * the base class for all state machines. A concrete derived class
    * must implement LoadState(), ExecuteReplicatedRequest(), AbortRequest(),
    * SaveState(), NotifyStatus(), ExecuteFastReadRequest(),
    * NotifyConfigurationChanged(),AddChangeConfiguration(), and ShutDown().
    *************************************************************************/
    class RSL_IMPORT_EXPORT RSLStateMachine
    {
    public:

        RSLStateMachine();

        virtual ~RSLStateMachine() { }

        /* Initialize must be called before using RSLStateMachine.
        *
        * When Initialize returns successfully, LoadState will have
        * completed and it is safe to call other object methods.
        * Initialize can fail for various reasons (for example,
        * the checkpoint or logs is corrupt). If the RSL fails to
        * initialize, it returns false. In this case, no other methods
        * can be called.
        */

        /* Initializes replica.
        *
        * DEPRECATED: this method should only becalled by systems
        *   running as versions 1, 2 or 3, or in upgrade process to
        *   version 4. Begining with version 4, new systems should
        *   use overloaded version of Initialize method which does
        *   not take a initial replica set. Instead, replica set
        *   should be informed through Bootstrap method issued from
        *   one of the replica in that set.
        *
        * Version 4 upgrade notes:
        *   In order to move to version 4, replica must have issued
        *   at least 1 healthy version 3 checkpoint. This is due
        *   the fact that beginning with version 3, replica saves
        *   replica set information in checkpoint files, and that
        *   is required by version 4 in order to Initialize without
        *   a list of replicas set explicitly informed.
        *
        * PARAMETERS:
        *
        * RSLConfigParam *cfg
        *   Passes a set of parameters that tune replicas behavior.
        *   For further details, see RSLConfigParam comments.
        *
        * RSLNodeArray& membersOfInitialConfiguration
        *   Contains a non-empty list of all replicas.
        *   Note: the application should ensure that only replica's
        *         member Id are unique within the replica set that
        *         constitute the replicated system.
        *
        * RSLNode& self
        *   Contains information regarding the replica it self.
        *   See RSLNode for details.
        *
        * RSLProtocolVersion version
        *   Version which replica should behavior as. The allowed
        *   values are versions 1, 2, 3 or 4. Once system is
        *   upgraded to version 4, the application can start
        *   calling new version of this method.
        *
        * bool serializeFastReadsWithReplicates
        *   If it is false, ExecuteFastReadRequest callbacks are not
        *   serialized with ExecuteReplicateRequest callbacks. This
        *   parallelizes execution of write and read requests, which
        *   might increase throughput, but the application needs to
        *   handle locking itself.
        */
        bool Initialize(RSLConfigParam *cfg,
            const RSLNodeCollection& membersOfInitialConfiguration,
            const RSLNode& self,
            RSLProtocolVersion version,
            bool serializeFastReadsWithReplicates,
            SpawnRoutine spawnRoutine = NULL,
            bool IndependentHeartbeat = false);

        /*
        * This RSLNode wants to give up the leadership and trigger the
        * new election immediately.
        */
        void RelinquishPrimary();

        /*
        * Check if the RSL instance is in stable state or not.
        * The RSL instance is stable means that it is primary or secondary.
        *
        */
        bool IsInStableState();

        // !!! DEPRECATED: RSLNodeArray is deprecated as part of effort to remove STL from RSL API signatures
        // !!! Plese use Initialize that has RSLNodeCollection in the signature instead.
        bool Initialize(RSLConfigParam *cfg,
            const RSLNodeArray& membersOfInitialConfiguration,
            const RSLNode& self,
            RSLProtocolVersion version,
            bool serializeFastReadsWithReplicates,
            SpawnRoutine spawnRoutine = NULL,
            bool IndependentHeartbeat = false);

        /* Initializes replica.
        *
        * IMPORTANT: this method must be only called by new systems
        *   or fully upgraded to version 4. In case of new systems,
        *   replica set should be informed through Bootstrap method.
        *
        * For the first time, it starts as inactive and it remains
        * inactive until bootstrap operation is issued. In such state,
        * is not part of any replica set.
        *
        * Once replica was bootstrapped (or had an unfinshed attempt to
        * bootstrap), replica shall have the list of replicas that it
        * is part of. Thus this method will cause replica to contact
        * other replicas of the known replica set in order to catch up
        * any progress that might have happen when replica was down.
        *
        * Bootstrap is accomplished by the application through Bootstrap
        * method which should be called by one of the replicas.
        * For further details, check Bootstrap method.
        *
        * PARAMETERS:
        *
        * RSLConfigParam *cfg
        *   Passes a set of parameters that tune replicas behavior.
        *   For further details, see RSLConfigParam comments.
        *
        * RSLNode& self
        *   Contains information regarding the replica it self.
        *   See RSLNode for details.
        *
        * RSLProtocolVersion version
        *   Version which replica should behavior as.
        *   Note: althought RSL is compatible with previous versions,
        *         the application must follow version upgrade guidelines.
        *         Check Initialize overloaded methods for details.
        *
        * bool serializeFastReadsWithReplicates
        *   If it is false, ExecuteFastReadRequest callbacks are not
        *   serialized with ExecuteReplicateRequest callbacks. This
        *   parallelizes execution of write and read requests, which
        *   might increase throughput, but the application needs to
        *   handle locking itself.
        *
        * void (*spawnRoutine)(unsigned int (*)(void*), void*)
        *   If specified (not NULL), it is a routine to be used
        *   to create threads for rsl. Therefore, spawnRoutine can
        *   be used to create threads in an appdomain, for example.
        *   If not specified (or if value is NULL), RSL will then
        *   use beginthreadex to do so.
        *   The first argument of spawnRoutine is the procedure to
        *   be executed by the new thread.
        *   The second argument of spawnRoutine is the argument to
        *   be passed to that procedure
        *
        */
        bool Initialize(RSLConfigParam *cfg,
            const RSLNode& self,
            RSLProtocolVersion version,
            bool serializeFastReadsWithReplicates,
            SpawnRoutine spawnRoutine = NULL,
            bool IndependentHeartbeat = false);

        /* Issues Bootstrap operation.
        *
        * IMPORTANT: This method must be called for a new version 4
        *   and up system only. It is not thread safe and must NOT
        *   be called together with any other method.
        *
        * The system is considered boostrapped when a majority of
        * its replicas have replica set information and a primary
        * has been elected. That means there is a majority ready,
        * and so progress can be made.
        *
        * In order to accomplish such state, one of the replicas must
        * have Bootstrap method called. This method will cause calling
        * replica to perform a checkpoint and contact each replica in
        * the specified replica set in order to inform them of replica
        * set they are part of. Each invited replica also issues a
        * checkpoint. A checkpoint is required so replicas can start
        * by its own in case it shutdowns.
        *
        * NotifyConfigurationChanged() callback is called on all
        * the replicas that were bootstrapped as part of this call.
        *
        * After message is received, replicas will proceed with regular
        * initialization and shall elect a primary. This method returns
        * once that happens or it times out. In case it times out, the
        * only guarantee the application has is that a majority was not
        * verified to be ready. The application can safelly call this
        * method again if desired. See return codes for details.
        *
        * PARAMETERS:
        *
        * RSLMemberSet* memberSet
        *   Replica set to start a system with. Should contain a non-empty
        *   list of RSLNodes where each node specify a replica with a unique
        *   member id, together with hostname, ip and port info.
        *
        * unsigned int timeout
        *   Time in seconds to wait for bootstrap operation to finish.
        *
        * RETURN CODES:
        *
        *   RSLSuccess
        *     In case replica successfully persisted informed replica
        *     set, and verified that a majority is ready (i.e. a primary
        *     was elected).
        *
        *   RSLAlreadyBootstrapped
        *     In case a majority has been recognized as ready. That can
        *     happen after a previous call succeded or timed out before
        *     a primary was elected, or another replica invited current
        *     replica, or in case the system was upgraded.
        *
        *   RSLBootstrapFailed
        *     In case it times out, or in case an attempt to bootstrap
        *     failed and user specified a different set of replicas.
        *     Check debug log files for details.
        *
        *   RSLInvalidParameter
        *     In case there is a problem with specified replica set.
        *     Check debug log files for details.
        */
        RSLResponseCode Bootstrap(RSLMemberSet* memberSet, unsigned int timeout);

        /*
        * Dynamically changes the election delay for the replica.
        * This is useful in periods when we know there will be extremely large decrees.
        * The reason is that, rsl secondaries "suspect" from the primary if no HB is
        * seen, and a HB cannot be received while a large decree is being learnt.
        */
        void ChangeElectionDelay(unsigned int delayInSecs);

        /* Uninitializes all RSLStateMachines from the process. If it is a managed
        * AppDomain, using RSLManagedWrapper, then the AppDomain can be unloaded then.
        * Otherwise, for native process, Unload works as ShoutDown.
        */
        static void Unload();

        /*
        * Returns a 64 bits number describing the current tickcount as calculated by RSL.
        */
        static unsigned long long GetHighResolutionCurrentTickCount();

        /* Initializes the state machine in "read-only" mode. Replays the checkpoints
        * and logs upto (and including) maxSeqNo. Any request after maxSeqNo
        * is not executed. When Replay() returns sucessfully, LoadState() and
        * ExecuteReplicatedRequest() have already been called for the previous requests.
        * No more calls to ExcuteRequest() or AbortRequest() will be made.
        *
        * It is not valid to call any method except GetCurrentSequenceNumber()
        * when RSL is initialized with this method. Specifically, Initialize() method
        * need not be called.
        *
        */
        bool Replay(const char* directory,
            unsigned long long maxSeqNo);

        /*
        * 'Expert-mode' Replay(): will cause a checkpoint to be written
        * for state up to maxSeqNo in the location specified by the checkpointDirectory
        * parameter using the sequence number specified by the checkpointSeqNo parameter.
        * The checkpointDirectory CANNOT be the same as directory; otherwise, the method
        * fails.
        *
        * WARNING!!!:
        *      This mode of recovery must be used with extreme caution. It is meant for
        *      offline recovery ONLY.
        */

        bool Replay(const char* directory,
            unsigned long long maxSeqNo,
            unsigned long long checkpointSeqNo,
            const char* checkpointDirectory);

        /* AttemptPromotion() will make this secondary attempt to become a primary.
        */
        void AttemptPromotion();

        /* RelinquishPrimaryStatus() will relinquish primary status
        */
        void RelinquishPrimaryStatus();

        /* ReplicateRequest() injects a replicated command into the RSL
        * machinery. If the replica returns RSLSuccess, the
        * implementation guarantees that it will subsequently call
        * ExecuteReplicatedRequest() or AbortRequest()
        * with cookie . After a successful call (returning RSLSuccess)
        * the RSL owns cookie, and it must not be deleted before the
        * matching call to ExecuteReplicatedRequest()/AbortRequest().
        *
        * ExecuteReplicatedRequest() will never be called directly from within
        * ReplicateRequest(). ReplicateRequest() may be called concurrently
        * by multiple threads and will not block for an extended
        * period. It is an error to call ReplicateRequest() before LoadState()
        * has completed.
        *
        * If the replica is not a primary, this returns RSLNotPrimary.
        * If the replica is shutting down, this return RSLShuttingDown
        */
        RSLResponseCode ReplicateRequest(void* request, size_t len, void* cookie);

        /* This function has similar fuctionality as ReplicateRequest() except
        * that it executes this request on a vote which is exclusive.No other
        * request is batched with this request.
        */
        RSLResponseCode ReplicateRequestExclusiveVote(void* request, size_t len, void* cookie);

        /* This function has similar fuctionality as ReplicateRequest() except
        * that it executes this request on a vote which is exclusive.No other
        * request is batched with this request. If isLastDecree is true, this
        * primary is stating he wants to terminate its primariness and fail over
        * in favour of some other replica immediatelly.
        */
        RSLResponseCode ReplicateRequestExclusiveVote(void* request, size_t len, void* cookie, bool isLastDecree);

        /* FastReadRequest injects a fast-read command into the RSL
        * machinery. If the replica returns RSLSuccess, the
        * implementation guarantees that it will subsequently call
        * ExecuteFastReadRequest with the cookie or
        * AbortRequest with cookie. After a successful call (returning
        * RSLSuccess) the RSL owns cookie, and it must not be deleted before
        * the matching call to ExecuteReplicatedRequest/AbortRequest.
        *
        * ExecuteReplicatedRequest will never be called directly from within
        * FastReadRequest. FastReadRequest may be called concurrently by
        * multiple threads and will not block for an extended period. It
        * is an error to call FastReadRequest before LoadState() has
        * completed.
        *
        * Returns RSLFastReadState if the replica is stale with respect
        * to maxSeenSeqNo. This may return RSLFastReadStale even if
        * maxSeenSeqNo < GetCurrentSequenceNumber(), for example if this
        * replica has fallen far behind the most recently chosen decree.
        * Returns RSLShuttingDown if the replica is shutting down.
        */
        RSLResponseCode FastReadRequest(unsigned long long maxSeenSeqNo, void* request, size_t len, void* cookie);

        /* timeout is in milliseconds. If timeout is not 0, the fast read
        * request is queued until the timeout elapses or maxSeenSeqNo is executed.
        */
        RSLResponseCode FastReadRequest(unsigned long long maxSeenSeqNo, void* request, size_t len, void* cookie, unsigned int timeout);


        /* Starts replica set change process.
        *
        * IMPORTANT: calling to this method causes system to momently pause.
        *
        * This method should be called when replica set needs to be
        * changed. Changes include: addition, removal or replacement
        * of a replica, or replica information update (like IP,
        * hostname, and ports).
        *
        * Like ReplicaRequest, this method issues a change in the
        * system that needs to be accepted by the majority, and
        * follows similar rules that apply to any system state change.
        * Thus, even if replicas shutdown, they will eventually
        * catch up the configuration change.
        *
        * PARAMETERS:
        *
        *   RSLMemberSet* memberSet
        *     New replica set. Should contain a non-empty list of RSLNodes
        *     where each node specify a replica with a unique member id,
        *     together with hostname, ip and port info.
        *
        *   void* cookie
        *     Application's cookie which is passed on
        *     NotifyConfigurationChanged and AbortChangeConfiguration.
        *     Note: this buffer will be serialized and sent across
        *           the network.
        *
        * RETURN CODES:
        *
        *   RSLSuccess
        *     In case configuration change process was started with success.
        *
        *   RSLNotPrimary
        *     In case replica is not current primary.
        *
        *   RSLShuttingDown
        *     In case replica is in shutdown process.
        *
        *   RSLInvalidParameter
        *     In case there is a problem with specified replica set.
        *     Check debug log files for details.
        *
        * CALLBACKS:
        *
        *   NotifyConfigurationChanged
        *     Notify replica that configuration has been changed.
        *     This is called on each reaplica from both old and new
        *     configuration.
        *
        *   AbortChangeConfiguration
        *     Notify replica which issued configuration change that
        *     the attempt to change configuration has failed.
        */
        RSLResponseCode ChangeConfiguration(RSLMemberSet* memberSet, void* cookie);

        /* Get current replica set list and its configuration number.
        * Returns false in case an error has ocurred. Returns true
        * and an empty memberset if the replica has not been
        * bootstrapped yet.
        *
        * PARAMETERS:
        *
        * RSLMemberSet * membersSet
        *   Replica set list object to be filled. Must be non-null.
        *
        * unsigned int * configuratioNumber (optional)
        *   Configuration number to be filled.
        */
        bool GetConfiguration(RSLMemberSet * membersSet, unsigned int * configuratioNumber = NULL);

        /* GetCurrentSequenceNumber() returns the sequence number of the
        * latest replicated command which has been executed on this
        * replica.
        *
        * if GetCurrentSequenceNumber() is called multiple times from
        * within a call to ExecuteReplicatedRequest() it is guaranteed to return the
        * same value each time. In ExecuteReplicatedRequest()
        * the current sequence number is the sequence number of the
        * command being executed. Otherwise it is the sequence number of
        * the most recently executed replicated command.
        *
        * If GetCurrentSequenceNumber() is called from anywhere except
        * ExecuteReplicatedRequest() its return value may increase at any
        * time, so this should be used as a hint, however it is
        * guaranteed non-decreasing on a particular replica even over
        * server restarts.
        *
        * GetCurrentSequenceNumber() may be called concurrently by multiple
        * threads and will not block for an extended period. It is an
        * error to call GetCurrentSequenceNumber() before LoadState() has
        * completed.
        */
        unsigned long long GetCurrentSequenceNumber();

        /* GetHighestPassedSequenceNumber() returns the sequence number of the
        * latest decree which has passed.
        *
        * Its return value may increase at any time, so this should be used as
        * a hint or a timestamp.
        *
        * GetHighestPassedSequenceNumber() may be called concurrently by multiple
        * threads and will not block for an extended period. It is an
        * error to call GetHighestPassedSequenceNumber() before LoadState() has
        * completed.
        */
        unsigned long long GetHighestPassedSequenceNumber();

        unsigned short GetReplicasInformation(unsigned short numEntries, ReplicaHealth *replicaSetHealth);

        bool GetReplicaInformation(ReplicaHealth *replicaHealth);

        /* Returns the memberId of the current known primary. If the
        * replica does not know who the primary is (eg. the system is
        * bootstrapped and hasn't elected a primary yet), it returns 0.
        * The memberId returned is not guaranteed to be the real primary
        * - other replicas might elect another primary without this
        * replica's knowledge. However, in a stable system primary rarely
        * changes, so the memberId returned will most probably be the
        * real primary. When the replica learns about a new primary, it
        * does not notify the applicaton. Application should always get
        * the primary using this method
        */
        void GetCurrentPrimary(RSLNode *node);

        /* Tells RSL to either stop or restart saving state automatically.
        * If yes is false, RSL will stop saving state to disk no matter
        * how large the log files get. Also, RSL will stop saving state
        * when TrySaveState() returns false. Application must call
        * AllowCheckpointing(true) to restart saving state.
        * This setting is *not* persisted across process restarts. After
        * a process restart, if the application does not want to create
        * checkpoint, it should call AllowCheckpointing(false). The
        * default behavior is to checkpoint whenever the log file gets
        * to a certain size.
        */
        void AllowSaveState(bool yes);

        /* Tells RSL what is the pauload to add into the votes sent by
        * this replica as secondary.
        * If never set, a 0 is used by default.
        * This payload is visible to the primary as part of the replicas health.
        * This can be set at any time by the secondaries, and will be sent to the primary upon every vote accepted.
        */
        void SetVotePayload(unsigned long long payload);

        /* Gets from RSL whether at this point we have any command pending execution.
        * This is useful on CanBecomePrimary callback, to not even try to become
        * primary until the queue is fully drained and all commands have been executed
        */
        bool IsAnyRequestPendingExecution();

        /* Tells RSL to copy the state from other replica.
        * The state is copied if another replica has a more recent
        * persisted state.
        * The StateCopied() callback is invoked to notify the
        * machine of the status of this request.
        *
        * PARAMETERS
        *
        * cookie - should be non-null. The cookie is passed back
        * to StateCopied()
        */
        RSLResponseCode CopyStateFromReplica(void *cookie);

        /* Obtain a snapshot of various RSL statistics in the time period since
        * the last call was made (or since the state machine was initialized if
        * no prior call has been made).
        *
        * PARAMETERS
        *
        * pStats - pointer to RSLStats structure to be filled in by call. The
        * only field that needs to be initialized by the caller is
        * m_cbSizeOfThisStruct (used as a versioning mechanism by the method).
        */
        void GetStatisticsSnapshot(RSLStats * pStats);

        void SetAcceptMessages(bool acceptMessages);

        /* These methods are defined only for testing purpose */
        void Pause();
        void Resume();
        void UnloadThisOne();

    protected:

        /* The state machine should initialize all its data structures and
        * read in the checkpoint from reader. No calls to ExecuteReplicatedRequest()
        * or SaveState() will be made before LoadState() returns. It is an
        * error to call ReplicateRequest(), FastReadRequest() or
        * GetCurrentSequenceNumber() before LoadState() has completed.
        *
        * LoadState() will be called when the process is
        * starting and no other callbacks will occur until
        * LoadState has returned success.
        * LoadState() should return failure if it thinks that the
        * checkpoint is corrupt. RSL will attempt to call LoadState()
        * again with previous checkpoints. Startup fails if
        * LoadState() returns failure with all available checkpoints.
        *
        * The parameter reader being NULL indicates that there is no checkpoint
        * file to read.
        */

        /*
        * DEPRECATED: use LoadState() instead
        * virtual void StartUp(RSLCheckpointStreamReader* reader) = 0;
        */

        virtual bool LoadState(RSLCheckpointStreamReader* reader) = 0;

        /* BeforeExecuteReplicatedRequests is called once before all replicated
        * requests from one decree are executed with series of calls to ExecuteReplicatedRequest.
        * AfterExecuteReplicatedRequests is called once after all replicated
        * requests from one decree are executed with series of calls to ExecuteReplicatedRequest.
        * One or more calls to ExecuteReplicatedRequest are made between calls to
        * BeforeExecuteReplicatedRequests and AfterExecuteReplicatedRequests
        */
        virtual void BeforeExecuteReplicatedRequests()
        {
        }

        virtual void AfterExecuteReplicatedRequests()
        {
        }

        /* ExecuteReplicatedRequest() is called when any state machine write
        * command needs to be executed. The machine must change the state
        * deterministically before returning.If cookie is non-NULL then
        * the command was initiated by the local process with a call to
        * ReplicateRequest(). When cookie is NULL this is a replicated
        * command on a secondary, a replay during restart, etc. A given
        * command will be executed with cookie non-NULL at most once on
        * any replica in the system, for all time.
        *
        * If cookie is non-NULL, then ExecuteReplicatedRequest() owns cookie
        * and must not let it go out of scope without deleting it.
        *
        * Callback Synchronization: Exclusive
        *
        * A call to ExecuteReplicatedRequest() will never
        * overlap with any other call to ExecuteReplicatedRequest() or
        * SaveState. This method will not be called until after LoadState()
        * has returned.
        *
        * The state machine can set saveState to true to cause the
        * system to create a checkpoint before the next decree is
        * executed. RSL will call SaveState() sometime after this method
        * returns even on the master replica. SaveState is guaranteed to
        * be called before the Sequence Number is incremented. In some
        * cases, more requests can in executed in the same sequence
        * number before a call to SaveState() is made. For example, if
        * the request is part of a bigger replicated request (RSL
        * combines multiple request into bigger one internal request for
        * replication), then the state can be saved only after the last
        * request is executed
        */

        /*
        * DEPRECATED: use ExecuteReplicatedRequest/ExecuteFastReadRequest instead
        *
        * virtual void ExecuteRequest(bool isFastRead, void* request, size_t len, void* cookie) = 0;
        */

        virtual void ExecuteReplicatedRequest(void* request, size_t len, void* cookie, bool *saveState) = 0;

        /* ExecuteFastReadRequest is called when any state machine read command needs
        * to be executed. The command must be
        * read-only and the machine should respond with an error leaving
        * the state unchanged if the command would mutate state.
        *
        * ExecuteFastReadRequest owns cookie and must not let it go out
        * of scope without deleting it.
        *
        * Callback Synchronization: Exclusive (if 'serializeFastReadsWithReplicate'
        * is true. Shared if 'serializeFastReadsWithReplicate' is false)
        *
        * Multiple calls to ExecuteFastReadRequest will not overlap with
        * ExecuteReplicateRequest if 'serializeFastReadsWithReplicate'
        * was set to true during initialization. But multiple callbacks
        * may overlap with each other, and with saveState.
        * ExecuteFastReadRequest will not be called until after
        * LoadState() has returned. */

        virtual void ExecuteFastReadRequest(void* request, size_t len, void* cookie) = 0;

        /* AbortRequest is currently called only when a replica loses its
        * primary status or if the replica is shutting down. It is called
        * with status set to RSLNotPrimary for every command which has
        * been injected using ReplicatedRequest but has not yet been
        * executed. After AbortRequest has been called for all such
        * in-flight commands, NotifyStatus will be called with isPrimary
        * set to false.
        *
        * AbortRequest owns cookie and must not let it go out of scope
        * without deleting it.
        *
        * Callback Synchronization: Exclusive (if 'serializeFastReadsWithReplicate'
        * is true. Shared if 'serializeFastReadsWithReplicate' is false)

        * If serializeFastReadsWithReplicate is false, calls to
        * AbortRequest for read requests can overlap with other
        * callbacks. If serializeFastReadsWithReplicate is true then call
        * to AbortRequest will never overlap with any call to
        * ExecuteReplicatedRequest, but it may overlap with
        * multiple calls to ExecuteFastReadRequest or with
        * SaveState.
        */
        virtual void AbortRequest(RSLResponseCode status, void* cookie) = 0;

        /* this method is called spontaneously by the replication
        * machinery whenever the state needs to be checkpointed.
        *
        * Callback Synchronization: Shared
        *
        * A call to SaveState will never overlap with any call to
        * ExecuteReplicatedRequest Multiple calls to
        * ExecuteFastReadRequest with may overlap with each other, and
        * with SaveState. SaveState will not be called until after
        * LoadState has returned.
        */
        virtual void SaveState(RSLCheckpointStreamWriter* writer) = 0;

        /* Try version of SaveState(). Application can return false if
        * it is unable to save its state to disk. Once TrySaveState()
        * returns false, it is application's responsibility to call
        * AllowSaveState() to re-enable periodic checkpointing.
        * RSL will stop saving state to disk no matter how large the
        * log files get until AllowSaveState(true) is called
        *
        * Callback Synchronization: Shared
        */
        virtual bool TrySaveState(RSLCheckpointStreamWriter *writer)
        {
            SaveState(writer);
            return true;
        }

        /* this method is called whenever the replica acquires or loses
        * primary status. If isPrimary is false, it is guaranteed that
        * all pending commands injected using ReplicatedRequest() have
        * already triggered their matching ExecuteReplicatedRequest() or
        * AbortRequest() callbacks, so there are no "dangling" cookie
        * objects.
        *
        * If isPrimary is true, NotifyStatus() might be called before all
        * the replicated commands (passed by the previous primary) have
        * been executed. RSL calls NotifyPrimaryRecovered() once all the
        * replicated commands issued by the earlier primary has been
        * executed.
        *
        * On startup it should be assumed that the replica is not primary
        * until a call to NotifyStatus is received with isPrimary set to
        * true.
        *
        * Callback Synchronization: Exclusive
        *
        * NotifyStatus will not overlap with any call to
        * ExecuteReplicatedRequest however it may overlap with any number
        * of calls to ExecuteFastReadRequest, or a call to SaveState.
        */
        virtual void NotifyStatus(bool isPrimary) = 0;

        /* Replica should implement this method to get notified
        * by replica set changes.
        *
        * This method informs that a replica set change has
        * occured. It is called for both replicas in old and
        * new replica set, and should be used by the primary
        * replica that issued the replica set change as the
        * confirmation of the change.
        *
        * Callback Synchronization: Exclusive
        *
        * PARAMETERS:
        *
        * void *cookie
        *   Object passed by the primary when ChangeConfiguration
        *   was called.
        */

        virtual void NotifyConfigurationChanged(void *cookie) = 0;

        /* This method informs replica that replica set change
        * was aborted.
        *
        * Callback Synchronization: Exclusive
        *
        * PARAMETERS:
        *
        * RSLResponseCode status
        *   Indicates the reason why the change was aborted.
        *   If ChangeConfiguration was called on an non-primary
        *   its value will be RSLNotPrimary. If replica is
        *   shutdown process, its value is RSLShuttingDown.
        *
        * void *cookie
        *   Object passed by the primary when ChangeConfiguration
        *   was called.
        */
        virtual void AbortChangeConfiguration(RSLResponseCode status, void* cookie) = 0;

        /* This method is called after NotifyStatus() once this primary
        * executed all the commands passed by the previous primaries.
        * State Machines that rely on their states to be current when
        * they become primary should wait for this call.
        *
        * Note: if the replica becomes a primary, but then loses primary
        * status before it has executed all the previous commands, this
        * method will not be called. In that scenario, only NotifyStatus
        * will be called twice (once when it becomes primary and again
        * when it becomes secondary).
        *
        * Callback Synchronization: Exclusive
        *
        * NotifyPrimaryRecoved will not overlap with any call to
        * ExecuteReplicatedRequest, ExecuteFastReadRequest, or
        * SaveState.
        */
        virtual void NotifyPrimaryRecovered() {}

        /* Before trying to become the primary, RSL calls back the state
        * machine. Applications can override CanBecomePrimary to return
        * false on some replicas and thus prevent them from being
        * eligible to act as the primary.
        *
        * Optionally, if it does want to become primary, it can set some
        * opaque data in the cookie. (cookie has a Set(void* data, UINt32
        * len) to set the data. Then once this replica is trying to elect
        * itself primary, it sends that cookie to every replica with the
        * accept/prepare request, and those replicas can then decide
        * whether or not they want to support that election. Before
        * accepting a request from the primary, the state machine on the
        * remote replica will get a callback in AcceptMessageFromReplica
        * as passed this data as a parameter.
        *
        * Callback Synchronization: None
        *
        * This callback is not synchronized with any other callbacks besides
        * itself.

        * NOTE: this callback happens on the main RSL thread which
        * does all the message processing. Blocking this thread
        * blocks progress of RSL.
        */
        virtual bool CanBecomePrimary() { return true; }
        virtual bool CanBecomePrimary(RSLPrimaryCookie * /*cookie*/) { return CanBecomePrimary(); }

        /* When secondary replicas gets a new replicate request from the
        * primary, it calls back the state machine. This callback allow
        * replicas to decide whether they want to be part of the quorum
        * or not. Every accept/prepare message has a primaryCookie -
        * this is the cookie set by the primary in CanBecomePrimary(). If
        * the replica does not want to accept this message, it can return
        * false. In that case, RSL will not respond to the accept/prepare
        * request.
        *
        * Callback Synchronization: None
        *
        * This callback is not synchronized with any other callbacks besides
        * itself.
        *
        * NOTE: this callback happens on the main RSL thread which
        * does all the message processing. Blocking this thread
        * blocks progress of RSL.
        */
        virtual bool AcceptMessageFromReplica(RSLNode * /*node*/, void * /*data*/, unsigned int  /*len*/) { return true; }

        /* ShutDown is called by the RSL before it exits, e.g. because the
        * state machine has fallen behind and needs to re-load from a
        * saved checkpoint, or because the state machine is being cleanly
        * stopped e.g. in order to change the replica set.
        *
        * Callback Synchronization: Exclusive
        *
        */
        virtual void ShutDown(RSLResponseCode status) = 0;

        /* ResolveNode is called whenever RSL needs to contact a remote
        * replica. RSL does not need to resolve node often as it
        * keeps connections alive between the replicas. This method
        * is called.whenever the connection breaks and needs to be
        * reestablished. It is okay to block in this callback -
        * this callback happens on a separate resolve thread.
        *
        * Callback Synchronization: None
        *
        * ResolveNode is not synchronized with other callbacks. Two
        * calls to ResolveNode() or other callbacks might overlap
        */
        virtual bool ResolveNode(RSLNode *node);

        /* Notification that the state has been saved to disk. When SaveState()
        * returns, the state might have been saved to a temporary file or partly in
        * memory. This notification confirms that the state has been saved to the
        * file. fileName is the full pathname of the file where the state is saved.
        * seqNo is the sequence number of the last executed command when the
        * state was saved.
        *
        * Callback Synchronization: Shared
        */
        virtual void StateSaved(unsigned long long /*seqNo*/, const char* /*fileName*/) {}

        /* Notification that the state has been copied from another replica.
        * If cookie is non-null, it means that the CopyStateFromReplica() request
        * was previously submitted. This callback notifies the machine of the status
        * of that request - if seqNo is greater than 0, then the state was
        * successfully copied. fileName gives the full pathName of the file that
        * has the copied state. SeqNo is null if it failed to copy the state from
        * another replica - either no other replica is online or they don't have
        * a persisted state more recent than this replica's
        * The primary never persists its state. Instead it periodically copies
        * the state from other replicas. This notification is also called whenever
        * the primary copies the state from another replica. The cookie is null in
        * this case
        * Callback Synchronization: Shared
        *
        */
        virtual void StateCopied(unsigned long long /*seqNo*/, const char* /*fileName*/, void * /*cookie*/) {}

    private:

        RSLibImpl::Legislator *m_legislator;

        friend class RSLibImpl::Legislator;
        friend class TestEngine;
    };

    /*************************************************************************
    * Class: RSLUtils
    *
    * Class that exposes RSL utility functions
    *************************************************************************/
    class RSL_IMPORT_EXPORT RSLUtils
    {
    public:
        static unsigned long long CalculateChecksum(const void* blob, const size_t dataSize);
    };
} // namespace RSLIB