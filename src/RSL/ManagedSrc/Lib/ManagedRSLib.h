/*
.........1.........2.........3.........4.........5.........6.........7.........
ManagedRSLib.h
    This is a managed class that will be used to build RSL based appications
    written in managed code, it provides access to all the RSL features and
    it encapsulates all the marshaling that is needed to interact with the
    unmanaged RSLib.

    For a better reference of every class/method in this library please 
    read rsl.h (//depot/dev/main/private/shared/rsl/rsl.h)
*/
#pragma once

// Ideally, we should wrap these includes around unmanaged. But, then the linker complains about
// multiple definitions for STL code.
// #pragma unmanaged
#include "winsock2.h"
#include "rsl.h"
#include "NetCommon.h"
#include "LibFuncs.h"
#include "BufferPool.h"

// #pragma managed
#include <vcclr.h>
// TODO: Validate Removal of deprecated loader-lock mitigation mechanism, per http://msdn2.microsoft.com/en-us/library/ms173267(VS.80).aspx
//#define _CRT_VCCLRIT_NO_DEPRECATE 
//#include <_vcclrit.h>
#include <strsafe.h>

using namespace RSLibImpl;
using namespace RSLib;
using namespace System;
using namespace System::IO;
using namespace System::Net;
using namespace System::Net::Sockets;
using namespace System::Runtime::InteropServices;

//Managed wrapper for the RSL library 
namespace ManagedRSLib
{
    
    class RSLStateMachineWrapper;

    
    //RSLResponse
    public enum class RSLResponse 
    {
        RSLFastReadStale = RSLib::RSLFastReadStale,
        RSLNotPrimary    = RSLib::RSLNotPrimary,
        RSLShuttingDown  = RSLib::RSLShuttingDown,
        RSLSuccess       = RSLib::RSLSuccess,
        RSLAlreadyBootstrapped = RSLib::RSLAlreadyBootstrapped,
        RSLBootstrapFailed = RSLib::RSLBootstrapFailed,
        RSLInvalidParameter = RSLib::RSLInvalidParameter
    };

    //
    public ref class ManagedRSLCheckpointStream : public Stream
    {
        private:
        RSLCheckpointStreamWriter *m_oWriter;
        RSLCheckpointStreamReader* m_oReader;
        ManagedRSLCheckpointStream^ m_oReaderThisWasCreatedFrom;
        System::String^ m_fileName;

        long long m_llPos;
        
        public:
        ManagedRSLCheckpointStream(System::String^ fileName, bool reader, ManagedRSLCheckpointStream^ readerStr);
        ManagedRSLCheckpointStream(System::String^ fileName, bool reader);
        ManagedRSLCheckpointStream(RSLCheckpointStreamReader *reader);
        ManagedRSLCheckpointStream(RSLCheckpointStreamWriter *writer);

        virtual void Close() override;

        virtual int Read(array<unsigned char>^ buffer, int offset, int count) override;

        virtual int ReadByte() override;

        virtual property long long Position
        {
            long long get() override;
            void set(long long value) override;
        }

        virtual property long long Length
        {
            long long get()  override;
        }

        virtual property System::String^ FileName
        {
            System::String^ get();
        }

        virtual property bool CanRead
        {
            bool get() override;
        }

        virtual property bool CanSeek
        {
            bool get() override;
        }
        
        virtual property  bool CanWrite
        {
            bool get() override;
        }
        
        virtual property bool CanTimeout
        {
            bool get() override;
        }

        virtual long long Seek(long long offset, SeekOrigin origin) override;

        virtual void SetLength(long long value) override;

        virtual void Write(array<unsigned char>^ buffer, int offset, int count) override;

        virtual void WriteByte(unsigned char value) override;

        virtual void Flush() override; 
    };

    //RSLProtocolVersion
    public enum class ManagedRSLProtocolVersion
    {
        // First Protocol Version
        ManagedRSLProtocolVersion_1 = RSLib::RSLProtocolVersion_1,
    
        // This version supports passing primary cookie as part of the
        // message. AcceptMessageFromReplica() will always be called with
        // (NULL, 0) Without this version
        ManagedRSLProtocolVersion_2 = RSLib::RSLProtocolVersion_2,
    
        // This version introduces replica set management.
        // It allows replicas to be added, removed or replaced
        ManagedRSLProtocolVersion_3 = RSLib::RSLProtocolVersion_3,

        // This version introduces checkpoint checksum and
        // bootstrap mechanism, which allows user to create
        // a replica set out of a group of available replicas
        // by issuing a command from a given replica
        ManagedRSLProtocolVersion_4 = RSLib::RSLProtocolVersion_4,
	
        // This version introduces relinguishPrimary flag
        ManagedRSLProtocolVersion_5 = RSLib::RSLProtocolVersion_5,
	
        // This version introduces vote payload
        ManagedRSLProtocolVersion_6 = RSLib::RSLProtocolVersion_6,
    };

    public ref class ManagedRSLConfigParam
    {
    internal:
        RSLConfigParam* m_configParam;

    public:
        ManagedRSLConfigParam() { m_configParam = new RSLConfigParam(); }
        !ManagedRSLConfigParam() { delete m_configParam; m_configParam = NULL; }
        ~ManagedRSLConfigParam() { delete m_configParam; m_configParam = NULL; }

        property long long NewLeaderGracePeriodSec {
            long long get() { return m_configParam->m_newLeaderGracePeriodSec; }
            void set(long long value) { m_configParam->m_newLeaderGracePeriodSec = value; }
        }

        property long long HeartBeatIntervalSec {
            long long get() { return m_configParam->m_heartBeatIntervalSec; }
            void set(long long value) { m_configParam->m_heartBeatIntervalSec = value; }
        }

        property long long ElectionDelaySec {
            long long get() { return m_configParam->m_electionDelaySec; }
            void set(long long value) { m_configParam->m_electionDelaySec = value; }
        }

        property long long MaxElectionRandomizeSec {
            long long get() { return m_configParam->m_maxElectionRandomizeSec; }
            void set(long long value) { m_configParam->m_maxElectionRandomizeSec = value; }
        }

        property long long InitializeRetryIntervalSec {
            long long get() { return m_configParam->m_initializeRetryIntervalSec; }
            void set(long long value) { m_configParam->m_initializeRetryIntervalSec = value; }
        }

        property long long PrepareRetryIntervalSec {
            long long get() { return m_configParam->m_prepareRetryIntervalSec; }
            void set(long long value) { m_configParam->m_prepareRetryIntervalSec = value; }
        }

        property long long VoteRetryIntervalSec {
            long long get() { return m_configParam->m_voteRetryIntervalSec; }
            void set(long long value) { m_configParam->m_voteRetryIntervalSec = value; }
        }

        property long long VoteMaxOutstandingIntervalSec {
            long long get() { return m_configParam->m_voteMaxOutstandingIntervalSec; }
            void set(long long value) { m_configParam->m_voteMaxOutstandingIntervalSec = value; }
        }

        property long long CPQueryRetryIntervalSec {
            long long get() { return m_configParam->m_cPQueryRetryIntervalSec; }
            void set(long long value) { m_configParam->m_cPQueryRetryIntervalSec = value; }
        }

        property long long MaxCheckpointIntervalSec {
            long long get() { return m_configParam->m_maxCheckpointIntervalSec; }
            void set(long long value) { m_configParam->m_maxCheckpointIntervalSec = value; }
        }

        property long long JoinMessagesIntervalSec {
            long long get() { return m_configParam->m_joinMessagesIntervalSec; }
            void set(long long value) { m_configParam->m_joinMessagesIntervalSec = value; }
        }

        property long long MaxLogLenMB {
            long long get() { return m_configParam->m_maxLogLenMB; }
            void set(long long value) { m_configParam->m_maxLogLenMB = value; }
        }

        property unsigned int SendTimeoutSec {
            unsigned int get() { return m_configParam->m_sendTimeoutSec; }
            void set(unsigned int value) { m_configParam->m_sendTimeoutSec = value; }
        }

        property unsigned int ReceiveTimeoutSec {
            unsigned int get() { return m_configParam->m_receiveTimeoutSec; }
            void set(unsigned int value) { m_configParam->m_receiveTimeoutSec = value; }
        }

        property unsigned int MaxCacheLengthMB {
            unsigned int get() { return m_configParam->m_maxCacheLengthMB; }
            void set(unsigned int value) { m_configParam->m_maxCacheLengthMB = value; }
        }

        property unsigned int MaxVotesInLog {
            unsigned int get() { return m_configParam->m_maxVotesInLog; }
            void set(unsigned int value) { m_configParam->m_maxVotesInLog = value; }
        }

        property unsigned int MaxOutstandingPerReplica {
            unsigned int get() { return m_configParam->m_maxOutstandingPerReplica; }
            void set(unsigned int value) { m_configParam->m_maxOutstandingPerReplica = value; }
        }

        property unsigned int MaxCheckpoints {
            unsigned int get() { return m_configParam->m_maxCheckpoints; }
            void set(unsigned int value) { m_configParam->m_maxCheckpoints = value; }
        }

        property unsigned int MaxLogs {
            unsigned int get() { return m_configParam->m_maxLogs; }
            void set(unsigned int value) { m_configParam->m_maxLogs = value; }
        }

        property unsigned int LogLenRandomize {
            unsigned int get() { return m_configParam->m_logLenRandomize; }
            void set(unsigned int value) { m_configParam->m_logLenRandomize = value; }
        }

        property unsigned int ElectionRandomize {
            unsigned int get() { return m_configParam->m_electionRandomize; }
            void set(unsigned int value) { m_configParam->m_electionRandomize = value; }
        }

        property unsigned int FastReadSeqThreshold {
            unsigned int get() { return m_configParam->m_fastReadSeqThreshold; }
            void set(unsigned int value) { m_configParam->m_fastReadSeqThreshold = value; }
        }

        property unsigned int InMemoryExecutionQueueSizeMB {
            unsigned int get() { return m_configParam->m_inMemoryExecutionQueueSizeMB; }
            void set(unsigned int value) { m_configParam->m_inMemoryExecutionQueueSizeMB = value; }
        }

        property unsigned int NumReaderThreads {
            unsigned int get() { return m_configParam->m_numReaderThreads; }
            void set(unsigned int value) { m_configParam->m_numReaderThreads = value; }
        }

        property unsigned int MaxMessageSizeMB {
            unsigned int get() { return m_configParam->m_maxMessageSizeMB; }
            void set(unsigned int value) { m_configParam->m_maxMessageSizeMB = value; }
        }

        property bool AllowPrimaryPromotionWhileCatchingUp {
            bool get() { return m_configParam->m_allowPrimaryPromotionWhileCatchingUp; }
            void set(bool value) { m_configParam->m_allowPrimaryPromotionWhileCatchingUp = value; }
        }

        property String^ WorkingDir {
            String^ get() {
                return gcnew String(m_configParam->m_workingDir);
            }
            void set(String^ value) {
                IntPtr pStr = Marshal::StringToHGlobalAnsi(value);
                if (FAILED(StringCchCopyA(
                        m_configParam->m_workingDir,
                        sizeof(m_configParam->m_workingDir),
                        (PSTR) pStr.ToPointer())))
                {
                    throw gcnew System::ArgumentException("value");
                }
                Marshal::FreeHGlobal(pStr);
            }
        }

        property bool UseGlobalAcceptMessagesFlag {
            bool get() { return m_configParam->m_useGlobalAcceptMessagesFlag; }
            void set(bool value) { m_configParam->m_useGlobalAcceptMessagesFlag = value; }
        }
    };

    public ref class ManagedReplicaHealth
    {
        String^ m_memberId;
        bool m_isPrimary;
        bool m_isConnected;
        long m_numOutstanding;
        System::DateTime m_failedAt;
        System::DateTime m_lastRequestSentAt;
        System::DateTime m_lastRequestVotedAt;
        long m_consecutiveFailures;
        unsigned long long m_lastVotePayload;
        bool m_needsResolve;

    internal:
        ManagedReplicaHealth(ReplicaHealth *replica)
        {
            m_memberId = gcnew String(replica->m_memberId);
            m_isPrimary = replica->m_isPrimary;
            m_isConnected = replica->m_connected;
            m_numOutstanding = replica->m_numOutstanding;
            m_consecutiveFailures = replica->m_consecutiveFailures;
            m_lastVotePayload = replica->m_lastVotePayload;

            if (replica->m_consecutiveFailures == 0) 
            { 
                m_failedAt = System::DateTime::MinValue;
            }
            else 
            {
                m_failedAt = ConvertSystemTimeToDateTime(replica->m_failedAt);
            }

            m_lastRequestSentAt = ConvertSystemTimeToDateTime(replica->m_lastRequestSentAt);
            m_lastRequestVotedAt = ConvertSystemTimeToDateTime(replica->m_lastRequestVotedAt);

            m_needsResolve = replica->m_needsResolve;
        }

        System::DateTime ConvertSystemTimeToDateTime(SYSTEMTIME systime)
        {
            if (systime.wYear == 0 || systime.wMonth == 0 || systime.wDay == 0)
            {
                // systime not initialized yet, return MinValue
                return System::DateTime::MinValue;
            }

            try
            {
                System::DateTime dateTime(systime.wYear, systime.wMonth, systime.wDay, systime.wHour, systime.wMinute, systime.wSecond, systime.wMilliseconds);
                return dateTime;
            }
            catch (...)
            {
                return System::DateTime::MinValue;
            }
        }

    public:
        ManagedReplicaHealth() { }

        /*
         * The member Id this object describes the health for
         */
        property String^ MemberId 
        { 
            String^ get() { return m_memberId; };
            void set(String^ value) { m_memberId = value; }
        }

        /*
         * Does RSL consider this replica "connected" to the quorum?
         */
        property bool IsConnected 
        { 
            bool get() { return m_isConnected;}
            void set(bool value) { m_isConnected = value;}
        }

        /*
         * Does this instance of RSL consider this replica as Primary?
         */
        property bool IsPrimary
        { 
            bool get() { return m_isPrimary;}
            void set(bool value) { m_isPrimary = value;}
        }

        /*
         * Number of outstanding messages to that replica
         */
        property long NumOutstanding 
        { 
            long get() { return m_numOutstanding;}
            void set(long value) { m_numOutstanding=value;}
        }

        /*
         * Last time the primary decided this replica was failed
         */
        property System::DateTime FailedAt 
        {
            System::DateTime get() { return m_failedAt; }
            void set(System::DateTime value) { m_failedAt = value; }
        }

        /*
         * Number of consecutive failures on talking to this replica
         */
        property long ConsecutiveFailures 
        { 
            long get() { return m_consecutiveFailures;}
            void set(long value) { m_consecutiveFailures = value; }
        }

        /*
         * Last time the primary successfully sent a decree to this replica
         */
        property System::DateTime LastRequestSentAt 
        { 
            System::DateTime get() { return m_lastRequestSentAt;}
            void set(System::DateTime value) { m_lastRequestSentAt = value; }
        }

        /*
         * Last time the primary received a vote from this replica
         */
        property System::DateTime LastRequestVotedAt 
        { 
            System::DateTime get() { return m_lastRequestVotedAt;}
            void set(System::DateTime value) { m_lastRequestVotedAt = value; }
        }

        /* 
         * Last payload sent by this replica in a vote
         */
        property unsigned long long LastVotePayload
        {
            unsigned long long get() { return m_lastVotePayload; }
            void set(unsigned long long value) { m_lastVotePayload = value; }
        }

        /*
         * True if the primary is trying to reconnect to this replica. 
         * False if the primary considers the replica connected, or gave up completely.
         */
        property bool NeedsResolve 
        { 
            bool get() { return m_needsResolve; }
            void set(bool value) { m_needsResolve = value; }
        }
    };

    public ref class ManagedRSLNode
    {
    internal:
        RSLNode* m_node;

        ManagedRSLNode(const RSLNode * node);

    public:

        static const size_t MaxMemberIdLength = RSLNode::MaxMemberIdLength;

        ManagedRSLNode() { m_node = new RSLNode(); }
        !ManagedRSLNode() { delete m_node; m_node = NULL; }
        ~ManagedRSLNode() { delete m_node; m_node = NULL; }

        property String^ MemberId {
            String^ get(); 
            void set(String^ value);
        }

        property String^ HostName {
            String^ get() {
                return gcnew String(m_node->m_hostName);
            }
            void set(String^ value) {
                
                if (value == nullptr)
                {
                    throw gcnew System::ArgumentNullException("value");
                }
                
                IntPtr pStr = Marshal::StringToHGlobalAnsi(value);
                if (FAILED(StringCbCopyA(
                        m_node->m_hostName,
                        sizeof(m_node->m_hostName),
                        (PSTR) pStr.ToPointer())))
                {
                    throw gcnew System::ArgumentException("value");
                }
                Marshal::FreeHGlobal(pStr);
            }
        }

        property IPAddress^ Ip {
            IPAddress^ get() { return gcnew IPAddress(m_node->m_ip); }
            void set(IPAddress^ value) 
            { 
                if (value->AddressFamily != AddressFamily::InterNetwork)
                {
                    throw gcnew System::ArgumentException("Invalid IP family. Only IP V4 family is supported");
                }
                array<System::Byte>^ ipBytes = value->GetAddressBytes();
                m_node->m_ip = BitConverter::ToUInt32(ipBytes, 0);
            }
        }

        property unsigned short RslPort {
            unsigned short get() { return m_node->m_rslPort; }
            void set(unsigned short value) { m_node->m_rslPort = value; }
        }

        property unsigned short RslLearnPort {
            unsigned short get() { return m_node->m_rslLearnPort; }
            void set(unsigned short value) { m_node->m_rslLearnPort = value; }
        }

    };

    public ref class ManagedRSLMemberSet
    {
    internal:
        RSLMemberSet * m_rslMemberSet;

    public:
        ManagedRSLMemberSet() { m_rslMemberSet = new RSLMemberSet(); }
        
        ManagedRSLMemberSet(
            array<ManagedRSLNode^>^ gc_members,
            array<System::Byte>^ gc_cookie,
            int offset,
            int length);
        
        !ManagedRSLMemberSet() { delete m_rslMemberSet; m_rslMemberSet = NULL; }
        ~ManagedRSLMemberSet() { delete m_rslMemberSet; m_rslMemberSet = NULL; }
        
        property array<ManagedRSLNode^>^ MemberArray { array<ManagedRSLNode^>^ get(); }
        property array<System::Byte>^ ConfigurationCookie { array<System::Byte>^ get(); }

        ManagedRSLNode^ GetMemberInfo(System::String^ gc_memberId);

    };

    public interface class IManagedRSLCheckpointCreator
    {
        void SaveState(ManagedRSLCheckpointStream^ gc_writer);
    };

    public ref class ManagedRSLCheckpointUtility sealed
    {
    public:
        static void SaveCheckpoint(
            String^ directoryName, 
            ManagedRSLProtocolVersion version,
            unsigned long long lastExecutedSequenceNumber, 
            unsigned int configurationNumber,
            ManagedRSLMemberSet^ gc_memberSet, 
            IManagedRSLCheckpointCreator^ gc_checkpointCreator);

        static bool ChangeReplicaSet(
            String^ checkpointName, 
            ManagedRSLMemberSet^ memberSet);

        static bool MigrateToVersion3(
            String^ checkpointName, 
            ManagedRSLMemberSet^ memberSet);

        static bool MigrateToVersion4(
            String^ checkpointName, 
            ManagedRSLMemberSet^ memberSet);

        static bool GetReplicaSet(
            String^ checkpointName, 
            ManagedRSLMemberSet^ memberSet);

        static String^ GetLatestCheckpoint(String^ rslDirectory);


    private:
        ManagedRSLCheckpointUtility();
    };

    public enum class NotificationLevel 
    {
        Debug = RSLibImpl::LogLevel_Debug,
        Info = RSLibImpl::LogLevel_Info,
        Status = RSLibImpl::LogLevel_Status,
        Warning = RSLibImpl::LogLevel_Warning,
        Error = RSLibImpl::LogLevel_Error,
        Assert = RSLibImpl::LogLevel_Assert,
        Alert = RSLibImpl::LogLevel_Alert
    };

    public enum class NotificationLogID
    {
        Logging = RSLibImpl::LogID_Logging,
        Common = RSLibImpl::LogID_Common,
        NetLib = RSLibImpl::LogID_Netlib,
        RSLLib = RSLibImpl::LogID_RSLLIB,
        NetlibCorruptPacket = RSLibImpl::LogID_NetlibCorruptPacket,
    };

    public ref class ManagedRSLStats
    {
    public:
        unsigned long long  captureTimeInMicroseconds;
        unsigned long long  decreesExecuted;
        unsigned long long  logReads;
        unsigned long long  logReadBytes;
        unsigned long long  logReadMicroseconds;
        unsigned int        logReadMaxMicroseconds;
        unsigned long long  logWrites;
        unsigned long long  logWriteBytes;
        unsigned long long  logWriteMicroseconds;
        unsigned int        logWriteMaxMicroseconds;
        unsigned long long  votingTimeMicroseconds;
        unsigned int        votingTimeMaxMicroseconds;
    };

    public ref class ManagedRSLUtils
    {
    public:
        static System::UInt64 CalculateChecksum(array<System::Byte>^ blob);
    };

    public ref class ManagedRSLStateMachine abstract : public IDisposable, public IManagedRSLCheckpointCreator
    {
        private:

        RSLStateMachineWrapper* m_oMRSLMachine;
        ManagedRSLNode^ m_lastAcceptedNode;
        array<System::Byte>^ m_lastAcceptedBuffer;

        RSLResponse CreateRequest(
            System::UInt64 maxSeenSeqNo,
            array<System::Byte>^ gc_request, 
            int offset,
            int count,
            System::Object^ gc_cookie,
            bool isFast,
            bool isLastRequest,
            System::UInt32 timeout);

        internal:

        void UnmarshalExecuteRequest(
            bool isFastRead,
            void* request, 
            unsigned int len,
            void* cookie,
            bool* saveState);
        
        void UnmarshalAbortRequest(
            RSLResponseCode status,
            void* cookie);


        void UnmarshallNotifyConfigurationChanged(
            void* cookie);

        void UnmarshallAbortChangeConfiguration(
            RSLResponseCode status,
            void* cookie);

        bool UnmarshalAcceptMessageFromReplica(
            RSLNode *node,
            void * data,
            unsigned int len);

        void UnmarshalStateSaved(
            unsigned long long seqNo, 
            const char* fileName);

        void UnmarshalStateCopied(
            unsigned long long seqNo, 
            const char* fileName, 
            void *cookie);

        delegate void NotificationsCallbackWrapper(int level, int logId, const char* title,const char*message);

        public:

        delegate void NotificationsCallbackDelegate(NotificationLevel level, NotificationLogID logID, System::String^ title, System::String^ message);

        static void Init(System::String^ logPath);
        static void Init(System::String^ logPath, bool minidumpEnabled);
        static void Init(System::String^ logPath, System::String^ thumbPrintA, System::String^ thumbPrintB, bool listenOnAllIPs, bool validateCAChain, bool checkCertificateRevocation);
        static void Init(System::String^ logPath, System::String^ thumbPrintA, System::String^ thumbPrintB, bool listenOnAllIPs, bool validateCAChain, bool checkCertificateRevocation, bool minidumpEnabled);
        static void Init(System::String^ logPath, System::String^ thumbPrintA, System::String^ thumbPrintB, System::String^ subjectA, System::String^ thumbPrintsParentA, System::String^ subjectB, System::String^ thumbPrintsParentB, bool considerIdentitiesWhitelist, bool listenOnAllIPs, bool validateCAChain, bool checkCertificateRevocation);
        static void Init(System::String^ logPath, System::String^ thumbPrintA, System::String^ thumbPrintB, System::String^ subjectA, System::String^ thumbPrintsParentA, System::String^ subjectB, System::String^ thumbPrintsParentB, bool considerIdentitiesWhitelist, bool listenOnAllIPs, bool validateCAChain, bool checkCertificateRevocation, bool minidumpEnabled);
        static void ReplaceThumbprints(System::String^ thumbPrintA, System::String^ thumbPrintB, bool validateCAChain, bool checkCertificateRevocation);
        static void ReplaceSubjects(System::String^ subjectA, System::String^ thumbPrintsParentA, System::String^ subjectB, System::String^ thumbPrintsParentB, bool considerIdentitiesWhitelist);
        static void ReplaceThumbprintsAndSubjects(System::String^ thumbPrintA, System::String^ thumbPrintB, System::String^ subjectA, System::String^ thumbPrintsParentA, System::String^ subjectB, System::String^ thumbPrintsParentB, bool considerIdentitiesWhitelist, bool validateCAChain, bool checkCertificateRevocation);

        virtual bool LoadState(ManagedRSLCheckpointStream^ gc_reader) = 0;

        virtual void ExecuteReplicatedRequest(
            array<System::Byte>^ gc_request,
            System::Object^ gc_cookie,
            System::Boolean %saveState) = 0;

        virtual void ExecuteFastReadRequest(
            array<System::Byte>^ gc_request,
            System::Object^ gc_cookie) = 0;

        virtual void AbortRequest(
            RSLResponse status,
            System::Object^ gc_cookie) = 0;

        virtual void SaveState(ManagedRSLCheckpointStream^ gc_writer) = 0;
        
        virtual bool TrySaveState(ManagedRSLCheckpointStream^ gc_writer)
        {
            SaveState(gc_writer);
            return true;
        }

        virtual void NotifyStatus(bool isPrimary) = 0;

        virtual void NotifyConfigurationChanged(System::Object^ gc_cookie) = 0;

        virtual void AbortChangeConfiguration(RSLResponse status, System::Object^ gc_cookie) = 0;

        virtual void NotifyPrimaryRecovered() = 0;

        virtual bool CanBecomePrimary(array<System::Byte>^ %data) = 0;

        /* When secondary replicas gets a new replicate request from the
         * primary, it calls back the state machine. This callback allow
         * replicas to decide whether they want to be part of the quorum
         * or not. Every accept/prepare message has a primaryCookie -
         * this is the cookie set by the primary in CanBecomePrimary(). If
         * the replica does not want to accept this message, it can return
         * false. In that case, RSL will not respond to the accept/prepare
         * request.
         */
        virtual bool AcceptMessageFromReplica(ManagedRSLNode^ node, array<System::Byte>^ data) = 0;
        
        virtual void ShutDown(RSLResponse status) = 0;

        virtual void AttemptPromotion();
        virtual void RelinquishPrimaryStatus();

        virtual void StateSaved(
            System::UInt64 seqNo, 
            System::String^ fileName) = 0;

        virtual void StateCopied(
            System::UInt64 seqNo, 
            System::String^ fileName, 
            System::Object ^ gc_cookie) = 0;

        ManagedRSLStateMachine();

        !ManagedRSLStateMachine();
        ~ManagedRSLStateMachine();
        
        bool Initialize(
            ManagedRSLConfigParam^ cfg,
            array<ManagedRSLNode^>^ nodes,
            ManagedRSLNode^ selfNode,
            ManagedRSLProtocolVersion version,
            bool serializeFastReadsWithReplicates);

        bool Initialize(
            ManagedRSLConfigParam^ cfg,
            ManagedRSLNode^ selfNode,
            ManagedRSLProtocolVersion version,
            bool serializeFastReadsWithReplicates);

        RSLResponse Bootstrap(
            ManagedRSLMemberSet^ gc_memberSet,
            int timeout);

        void ChangeElectionDelay(
            unsigned int delayInSecs);

        static void Unload();

        bool Replay(
            System::String^ directory,
            System::UInt64 maxSeqNo);
            
        RSLResponse ReplicateRequest(
            array<System::Byte>^ gc_request,
            System::Object^ gc_cookie);

        RSLResponse ReplicateRequest(
            array<System::Byte>^ gc_request,
            System::Object^ gc_cookie, 
            bool isLastRequest);

        RSLResponse ReplicateRequest(
            array<System::Byte>^ gc_request,
            int offset,
            int count,
            System::Object^ gc_cookie);
                    
        RSLResponse FastReadRequest(
            System::UInt64 maxSeenSeqNo, 
            array<System::Byte>^ gc_request,
            int offset,
            int count,
            System::Object^ gc_cookie);

        RSLResponse FastReadRequest(
            System::UInt64 maxSeenSeqNo, 
            array<System::Byte>^ gc_request,
            System::Object^ gc_cookie);

        RSLResponse FastReadRequest(
            System::UInt64 maxSeenSeqNo, 
            array<System::Byte>^ gc_request,
            int offset,
            int count,
            System::Object^ gc_cookie,
            System::UInt32 timeout);

        RSLResponse ChangeConfiguration(
            ManagedRSLMemberSet^ gc_memberSet,
            System::Object^ gc_cookie);

        bool GetConfiguration(
            ManagedRSLMemberSet^ gc_memberSet,
            unsigned int % gc_configNumber);

        /*
         * When invoked on the primary it Retrieves the view from the primary of the given memberId's health.
         */
        ManagedReplicaHealth^ GetReplicaHealth(System::String^ memberId);

        /*
         * When invoked on the primary, it retrieves the health of the secondary nodes (not the primary) on an array.
         * This information can be used to understand how RSL primary is seeing the cluster's health.
         */
        property array<ManagedReplicaHealth^>^ MemberSetHealth { array<ManagedReplicaHealth^>^ get(); }

        System::UInt64 GetCurrentSequenceNumber();

        System::UInt64 GetHighestPassedSequenceNumber();

        void GetCurrentPrimary(ManagedRSLNode ^node);

        bool IsAnyRequestPendingExecution();

        void AllowSaveState(bool yes);

        void SetVotePayload(unsigned long long payload);

        RSLResponse CopyStateFromReplica(System::Object^ gc_cookie);

        void UnloadThisOne();
        void Pause();
        void Resume();

        static property NotificationsCallbackDelegate^ NotificationsCallback
        {
            NotificationsCallbackDelegate^ get() { return s_notificationscallback; }
            void set(NotificationsCallbackDelegate^ value) { s_notificationscallback = value; }
        }

        ManagedRSLStats^ GetStatisticsSnapshot();

        void SetAcceptMessages(bool acceptMessages);

        private:

        static NotificationsCallbackDelegate^ s_notificationscallback = nullptr;
    };
    
    //
    class RSLStateMachineWrapper : public RSLStateMachine
    {
        private:
        gcroot<ManagedRSLStateMachine^> m_pManagedSM;

        protected:
        bool LoadState(RSLCheckpointStreamReader* reader);

        void ExecuteReplicatedRequest(
            void* request,
            size_t len,
            void* cookie,
            bool *saveState);
        
        void ExecuteFastReadRequest(
            void* request,
            size_t len,
            void* cookie);

        void AbortRequest(
            RSLResponseCode status,
            void* cookie);

        void SaveState(RSLCheckpointStreamWriter* writer);
        bool TrySaveState(RSLCheckpointStreamWriter* writer);

        void NotifyStatus(bool isPrimary);

        void NotifyConfigurationChanged(void *cookie);

        void AbortChangeConfiguration(RSLResponseCode status, void* cookie);

        void NotifyPrimaryRecovered();

        bool CanBecomePrimary(RSLPrimaryCookie *cookie);

        bool AcceptMessageFromReplica(RSLNode *node, void * data, unsigned int len);
    
        void ShutDown(RSLResponseCode status);

        void StateSaved(unsigned long long seqNo, const char* fileName);

        void StateCopied(unsigned long long seqNo, const char* fileName, void *cookie);

        public:
        
        RSLStateMachineWrapper(ManagedRSLStateMachine^ oManagedSM) : m_pManagedSM(oManagedSM) {};

        ~RSLStateMachineWrapper();
    };
};
