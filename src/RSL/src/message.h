#pragma once

#include "marshal.h"
#include "rsl.h"
#include "list.h"
#include "streamio.h"
#include "RefCount.h"
#include <vector>

using namespace RSLib;

namespace RSLibImpl
{
    static const UInt16 Message_None = 0;
    static const UInt16 Message_Vote = 1;
    static const UInt16 Message_VoteAccepted = 2;
    static const UInt16 Message_Prepare = 3;
    static const UInt16 Message_PrepareAccepted = 4;
    static const UInt16 Message_NotAccepted = 5;
    static const UInt16 Message_StatusQuery = 6;
    static const UInt16 Message_StatusResponse = 7;
    static const UInt16 Message_FetchVotes = 8;
    static const UInt16 Message_FetchCheckpoint = 9;
    static const UInt16 Message_ReconfigurationDecision = 10;
    static const UInt16 Message_DefunctConfiguration = 11;
    static const UInt16 Message_Join = 12;
    static const UInt16 Message_JoinRequest = 13;
    static const UInt16 Message_Bootstrap = 14;

    static const UInt32 s_MessageMagic = 0xF00DFACE;

    static const UInt32 s_AvgMessageLen = 10*1024*1024;

    // offset in the message where the checksum is written. 2 byts is for the
    // version field and 4 bytes for the length
    static const UInt32 s_ChecksumOffset = 2 + 4;

    struct SIZED_BUFFER
    {
        void* m_buf;
        UInt32 m_len;
    };

    //
    // RSL uses HiRes clock internally which is based on GetTickCount win32 call.
    // GetTickCount returns number of ms since the system start which is orthogonal to the current system date-time.
    // RSL clients are using system time, therefore publicly exposed APIs need to use system time.
    // RslDateTime class records both values.
    //
    class RslDateTime
    {
    public:
        RslDateTime()
        {
            m_hiRes = 0;
            ZeroMemory(&m_system, sizeof(m_system));
        }

        void Set()
        {
            m_hiRes = GetHiResTime();
            GetSystemTime(&m_system);
        }

        Int64 HiResTime() const { return m_hiRes; }
        SYSTEMTIME SystemTime() const { return m_system; }

    private:
        Int64 m_hiRes;
        SYSTEMTIME m_system;
    };

    class Replica : public RefCount 
    {
        public:
        RSLNode m_node;

        volatile bool m_connected;
        volatile long m_numOutstanding;
        volatile int m_status;  // clear this bit every so often
        RslDateTime m_failedAt;
        RslDateTime m_lastRequestSentAt;
        RslDateTime m_lastRequestVotedAt;
        UInt64 m_lastRequestVotedDecree;
        UInt64 m_lastVotePayload;

        UInt32 m_consecutiveFailures;
        bool m_needsResolve;

        Replica(const RSLNode &node);

    };

    class MemberId
    {

    public:
        static const UInt32 Size = 64;


        MemberId();
        explicit MemberId(const char* value);
        MemberId(const char* value, size_t len);

        bool operator <(const MemberId& b) const;
        bool operator >(const MemberId& b) const;
        bool operator ==(const MemberId& b) const;
        bool operator !=(const MemberId& b) const;
        bool operator >=(const MemberId& b) const;

        int Compare(const MemberId& memberId) const;
        int Compare(const char* memberIdStr) const;
        int Compare(const char* memberIdStr, size_t len) const;

        static int Compare(const char* str1, const char* str2);

        void Marshal(MarshalData *marshal, RSLProtocolVersion version) const;
        bool UnMarshal(MarshalData *marshal, RSLProtocolVersion version);

        const char* GetValue() const;

        static UInt32 GetBaseSize(RSLProtocolVersion version);

    private:

        char m_value[Size];

        // this is not marshalled. m_value is null terminated.
        size_t m_len;

        friend class BallotNumber;

    };

    class PrimaryCookie
    {
        public:
        
        UInt32 m_len;
        void *m_data;

        PrimaryCookie();
        PrimaryCookie(void *data, UInt32 len, bool copy);
        ~PrimaryCookie();
        
        void Marshal(MarshalData *marshal);        
        bool UnMarshal(MarshalData *marshal, bool copy);
        UInt32 GetMarshalLen();

        private:
        bool m_own;
    };
    
    class BallotNumber
    {
        public:
    
        UInt32 m_ballotId;
        MemberId m_memberId;

        BallotNumber();
        BallotNumber(UInt32 sequenceNumber, MemberId memberId);
        bool operator <(const BallotNumber& b);
        bool operator >(const BallotNumber& b);
        bool operator ==(const BallotNumber& b);
        bool operator !=(const BallotNumber& b);
        bool operator >=(const BallotNumber& b);
        void Marshal(MarshalData *marshal, RSLProtocolVersion version);
        bool UnMarshal(MarshalData *marshal, RSLProtocolVersion version);

        void LogString(char *buf, size_t size, char **end) const;

        static UInt32 GetBaseSize(RSLProtocolVersion version);

    };

    class Message
    {
        public:

        Message();
        Message(RSLProtocolVersion version, UInt16 msg);
        
        Message(
            RSLProtocolVersion version,
            UInt16 msg,
            MemberId memberId,
            UInt64 decree,
            UInt32 configurationNumber,
            BallotNumber ballot,
            UInt64 payload);

        Message(
            RSLProtocolVersion version,
            UInt16 msg,
            MemberId memberId,
            UInt64 decree,
            UInt32 configurationNumber,
            BallotNumber ballot);

        virtual ~Message() 
        {
            m_replica = NULL;
        }

        // The following fields are marshaled/unMarshaled
        RSLProtocolVersion m_version;
        UInt32 m_unMarshalLen; // length including this message base to unmarshal the message.
        UInt64 m_checksum;
        UInt32 m_magic;
        UInt16 m_msgId;
        MemberId m_memberId;
        UInt64 m_decree;
        UInt32 m_configurationNumber;
        BallotNumber m_ballot;
        UInt64 m_payload;

        // There fields are not marshaled
        UInt32 m_remoteIP;
        UInt16 m_remotePort;

        Ptr<Replica> m_replica;
        bool m_asClient;
        Link<Message> link;

        void InitMessage(
            RSLProtocolVersion version,
            UInt16 msg,
            MemberId memberId,
            UInt64 decree,
            UInt32 configurationNumber,
            BallotNumber ballot,
            UInt64 payload);

        void MarshalBuf(char *buf, UInt32 bufLen);
        bool UnMarshalBuf(char *buf, UInt32 bufLen);
        bool Peek(MarshalData *marshal);
        bool ReadFromSocket(StreamSocket *socket, UInt32 maxMessageSize);

        bool IsLowerDecree(UInt64 curDecree, BallotNumber &curBallot);
        bool IsHigherDecree(UInt64 curDecree, BallotNumber &curBallot);
        bool IsNextDecree(UInt64 curDecree, BallotNumber &curBallot);
        bool IsSameDecree(UInt64 curDecree, BallotNumber &curBallot);

        void CalculateChecksum(char *buf, UInt32 len);
        bool VerifyChecksum(char *buf, UInt32 len);
        void LogString(char *buf, size_t size, char** end) const;
    
        virtual void Marshal(MarshalData *marshal);
        virtual bool UnMarshal(MarshalData *marshal);
        virtual UInt32 GetMarshalLen();
        UInt32 GetBaseSize () const;

        static bool IsVersionValid(UInt16 version);
    };

    class RequestCtx;

    class Vote : public Message, public RefCount
    {
        public:
    
        Vote();
        Vote(RSLProtocolVersion version,
             MemberSet *membersInNewConfiguration,
             void* reconfigurationCookie,
             PrimaryCookie *cookie);

        Vote(RSLProtocolVersion version,
             MemberId memberId,
             UInt64 decree,
             UInt32 configurationNumber,
             BallotNumber ballot,
             PrimaryCookie *cookie);

        Vote(RSLProtocolVersion version,
             MemberId memberId,
             UInt64 decree,
             UInt32 configurationNumber,
             BallotNumber ballot,
             PrimaryCookie *cookie,
             bool relinquishPrimary);

        Vote(RSLProtocolVersion version, Vote *other, PrimaryCookie *cookie);
        
        ~Vote();

        void Marshal(MarshalData *marshal);
        bool UnMarshal(MarshalData *marshal);
        UInt32 GetMarshalLen();

        void ReMarshalHeader();
        void CalculateChecksum();
        
        void AddRequest(char* request, UInt32 len, void* cookie);
        
        PrimaryCookie m_primaryCookie;

        bool m_isReconfiguration;
        
        Queue<RequestCtx> m_requests;
        UInt32 m_numRequests;
        
        Ptr<MemberSet> m_membersInNewConfiguration;
        
        std::vector<MarshalMemoryManager *> m_marshal;

        // these fields are not marshalled
        void* m_reconfigurationCookie;
        RslDateTime m_receivedAt;
        bool m_isExclusiveVote;
        bool m_relinquishPrimary;

        MarshalMemoryManager* AddMemory(UInt32 minSize, bool exact = false);

        void GetBuffers(SIZED_BUFFER *buffers, UInt32 size);
        UInt32 GetNumBuffers();
        bool GetRelinquishPrimary();

    private:
        void Init(MemberSet *membersInNewConfiguration, PrimaryCookie *cookie);
        
    };

    class JoinMessage : public Message
    {
    public:
        JoinMessage();

        JoinMessage(RSLProtocolVersion version,
                    MemberId memberId,
                    UInt64 decree,
                    UInt32 configurationNumber);
        
        void Marshal(MarshalData *marshal);
        bool UnMarshal(MarshalData *marshal);
        UInt32 GetMarshalLen();

        UInt16 m_learnPort;
        UInt64 m_minDecreeInLog;
        UInt64 m_checkpointedDecree;
        UInt64 m_checkpointSize;

    };

    class PrepareMsg: public Message
    {
        public:

        PrepareMsg();
        PrepareMsg(RSLProtocolVersion version,
                   MemberId memberId,
                   UInt64 decree,
                   UInt32 configurationNumber,
                   BallotNumber ballot,
                   PrimaryCookie *cookie);
        
        ~PrepareMsg();
        
        void Marshal(MarshalData *marshal);
        bool UnMarshal(MarshalData *marshal);
        UInt32 GetMarshalLen();

        PrimaryCookie m_primaryCookie;
    };
    
        
    class PrepareAccepted : public Message
    {
        public:

        PrepareAccepted();
	
        PrepareAccepted(RSLProtocolVersion version,
                        MemberId memberId,
                        UInt64 decree,
                        UInt32 configurationNumber,
                        BallotNumber ballot,
                        Vote *vote);

        ~PrepareAccepted();
    
        void Marshal(MarshalData *marshal);
        bool UnMarshal(MarshalData *marshal);
        UInt32 GetMarshalLen();

        Ptr<Vote> m_vote;
    };

    class StatusResponse : public Message
    {
        public:
    
        StatusResponse();
        StatusResponse(RSLProtocolVersion version,
                       MemberId memberId,
                       UInt64 decree,
                       UInt32 configurationNumber,
                       BallotNumber ballot);

        void Marshal(MarshalData *marshal);
        bool UnMarshal(MarshalData *marshal);
        UInt32 GetMarshalLen();

        UInt64 m_queryDecree;
        BallotNumber m_queryBallot;
    
        Int64 m_lastReceivedAgo;
        UInt64 m_minDecreeInLog;
        UInt64 m_checkpointedDecree;
        UInt64 m_checkpointSize;

        BallotNumber m_maxBallot;
        UInt32 m_state;
    
    };

    class BootstrapMsg: public Message
    {
        public:
        BootstrapMsg();
        BootstrapMsg(RSLProtocolVersion version,
                     MemberId memberId,
                     MemberSet &memberSet);
        ~BootstrapMsg();

        void Marshal(MarshalData *marshal);
        bool UnMarshal(MarshalData *marshal);
        UInt32 GetMarshalLen();

        Ptr<MemberSet> m_memberSet;
    };

    class RequestCtx
    {
        public:

        RequestCtx(UInt64 decree, void *buf, size_t len, void *cookie) :
            m_requestBuf(buf), m_bufLen(len), m_cookie(cookie),
            m_decree(decree), m_timeout(0)
        {}
    
        void *m_requestBuf;
        size_t m_bufLen;
        void *m_cookie;
        UInt64 m_decree;
        Int64 m_timeout;
        Link<RequestCtx> link;
        Link<RequestCtx> m_timeoutLink;
    };
};
