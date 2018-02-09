
#include "message.h"
#include "hirestime.h"
#include "rsldebug.h"
#include "legislator.h"
#include "utils.h"
#include "limits.h"
#include "FingerPrint.h"
#include <strsafe.h>

using namespace std;
using namespace RSLibImpl;

Replica::Replica(const RSLNode &node) :
    m_connected(false),
    m_node(node),
    m_numOutstanding(0),
    m_status(1),
    m_consecutiveFailures(0),
    m_needsResolve(false),
    m_lastRequestVotedDecree(0)
{
    // For backward's compatiblity with old API. The old code assumed that
    // m_rslPort+1 will be used for learning votes and checkpoints.
    // If m_rsllearnPort is not set, assume that the application wants the
    // old behavior
    if (m_node.m_rslLearnPort == 0)
    {
        m_node.m_rslLearnPort = m_node.m_rslPort+1;
    }
}

MemberId::MemberId() : m_len(0)
{
    memset(m_value, 0, sizeof(m_value));
}

MemberId::MemberId(const char* value)
{
    LogAssert(SUCCEEDED(StringCbLengthA(value, sizeof(m_value), &m_len)));
    LogAssert(SUCCEEDED(StringCbCopyA(m_value, sizeof(m_value), value)));
}

MemberId::MemberId(const char* value, size_t len)
{
    LogAssert(len < sizeof(m_value));
    LogAssert(SUCCEEDED(StringCbCopyA(m_value, sizeof(m_value), value)));
    m_len = len;
}

bool 
MemberId::operator <(const MemberId& b) const
{
    return this->Compare(b) < 0;
}

bool 
MemberId::operator >(const MemberId& b) const
{
    return this->Compare(b) > 0;
}

bool 
MemberId::operator ==(const MemberId& b) const
{
    return this->Compare(b) == 0;
}

bool 
MemberId::operator !=(const MemberId& b) const
{
    return this->Compare(b) != 0;
}

bool 
MemberId::operator >=(const MemberId& b) const
{
    return this->Compare(b) >= 0;
}

int
MemberId::Compare(const char* value) const
{
    size_t len;
    LogAssert(SUCCEEDED(StringCbLength(value, sizeof(m_value), &len)));
    return this->Compare(value, len);
}

int
MemberId::Compare(const char* value, size_t len) const
{
    MemberId id(value, len);
    return this->Compare(id);
}

int
MemberId::Compare(const MemberId &memberId) const
{
    if (m_len == memberId.m_len)
    {
        return strncmp(m_value, memberId.m_value, sizeof(m_value));
    }
    else
    {
        return (int) (m_len - memberId.m_len);
    }
}

const char*
MemberId::GetValue() const
{
    return m_value;
}

void 
MemberId::Marshal(MarshalData *marshal, RSLProtocolVersion version) const
{
    if (version <= RSLProtocolVersion_3)
    {
        UInt64 value = RSLNode::ParseMemberIdAsUInt64(m_value);
        marshal->WriteUInt64(value);
    }
    else
    {
        marshal->WriteData((UInt32) MemberId::Size, (void *) m_value);
    }
}
        
bool 
MemberId::UnMarshal(MarshalData *marshal, RSLProtocolVersion version)
{
    HRESULT hresult;

    if (version <= RSLProtocolVersion_3)
    {
        UInt64 value;
        if (!marshal->ReadUInt64(&value))
        {
            return false;
        }

        if (value == 0)
        {
            *m_value = '\0';
        }
        else
        {
            hresult = StringCbPrintfA(m_value, sizeof(m_value), "%I64u", value);
            if (!SUCCEEDED(hresult))
            {
                return false;
            }
        }
    }
    else
    {
        if (!marshal->ReadData(MemberId::Size, (void *) m_value))
        {
            return false;
        }
    }

    hresult = StringCbLengthA(m_value, sizeof(m_value), &m_len);
    if (!SUCCEEDED(hresult))
    {
        return false;
    }

    return true;
}

UInt32
MemberId::GetBaseSize(RSLProtocolVersion version)
{
    if (version <= RSLProtocolVersion_3)
    {
        return 8;
    }
    else
    {
        return Size;
    }
}

int
MemberId::Compare(const char* value1, const char* value2)
{
    MemberId id1(value1);
    MemberId id2(value2);

    return (id1.Compare(id2));
}

PrimaryCookie::PrimaryCookie() : m_len(0), m_data(NULL), m_own(false) {}

PrimaryCookie::PrimaryCookie(void *data, UInt32 len, bool copy) : m_len(len), m_own(copy)
{
    if (m_own)
    {
        m_data = malloc(m_len);
        memcpy(m_data, data, m_len);
    }
}
        
PrimaryCookie::~PrimaryCookie()
{
    if (m_own)
    {
        free(m_data);
    }
}
        
void
PrimaryCookie::Marshal(MarshalData *marshal)
{
    marshal->WriteUInt32(m_len);
    if (m_len)
    {
        marshal->WriteData(m_len, m_data);
    }
}

bool
PrimaryCookie::UnMarshal(MarshalData *marshal, bool copy)
{
    m_data = NULL;
    m_own = copy;
            
    if (!marshal->ReadUInt32(&m_len))
    {
        return false;
    }
    if (m_len > 0)
    {
        if (m_own)
        {
            m_data = malloc(m_len);
            if (marshal->ReadData(m_len, m_data) == false)
            {
                return false;
            }
        }
        else
        {
            if (!marshal->PeekDataPointer(m_len, &m_data))
            {
                return false;
            }
            marshal->ForwardReadPointer(m_len);
        }
    }
    return true;
}

UInt32
PrimaryCookie::GetMarshalLen()
{
    return m_len + 4;
}

BallotNumber::BallotNumber() : m_ballotId(0), m_memberId()
{}

BallotNumber::BallotNumber(UInt32 ballotId, MemberId memberId) :
    m_ballotId(ballotId), m_memberId(memberId)
{}

bool
BallotNumber::operator <(const BallotNumber& b)
{
    if (m_ballotId == b.m_ballotId)
    {
        return m_memberId < b.m_memberId;
    }
    return m_ballotId < b.m_ballotId;
}

bool
BallotNumber::operator >(const BallotNumber& b)
{
    if (m_ballotId == b.m_ballotId)
    {
        return m_memberId > b.m_memberId;
    }
    return m_ballotId > b.m_ballotId;
}

bool
BallotNumber::operator ==(const BallotNumber& b)
{
    return (m_ballotId == b.m_ballotId && m_memberId == b.m_memberId);
}

bool
BallotNumber::operator >=(const BallotNumber &b)
{
    if (m_ballotId == b.m_ballotId)
    {
        return m_memberId >= b.m_memberId;
    }
    return m_ballotId >= b.m_ballotId;
}
    
bool
BallotNumber::operator !=(const BallotNumber& b)
{
    return (m_ballotId != b.m_ballotId || m_memberId != b.m_memberId);
}

void
BallotNumber::Marshal(MarshalData *marshal, RSLProtocolVersion version)
{
    marshal->WriteUInt32(m_ballotId);
    m_memberId.Marshal(marshal, version);
}
   
bool
BallotNumber::UnMarshal(MarshalData *marshal, RSLProtocolVersion version)
{
    if (!marshal->ReadUInt32(&m_ballotId) ||
        !m_memberId.UnMarshal(marshal, version))
    {
        return false;
    }
    return true;
}

UInt32 
BallotNumber::GetBaseSize(RSLProtocolVersion version)
{
    return (4 + MemberId::GetBaseSize(version));
}


void
BallotNumber::LogString(char *buf, size_t size, char** end) const
{
    if (FAILED(StringCchPrintfExA(buf, size, end, NULL, 0,
                                  "0x%I32x;%s",
                                  m_ballotId,
                                  m_memberId.GetValue())))
    {
        *end = buf;
    }
}

Message::Message()
{
    InitMessage((RSLProtocolVersion) 0, Message_None, MemberId(), 0, 0, BallotNumber(), 0);
}

Message::Message(RSLProtocolVersion version, UInt16 msg)
{
    InitMessage(version, msg, MemberId(), 0, 0, BallotNumber(), 0);
}

Message::Message(
    RSLProtocolVersion version,
    UInt16 msg,
    MemberId memberId,
    UInt64 decree,
    UInt32 configurationNumber,
    BallotNumber ballot) 
{
    InitMessage(version, msg, memberId, decree, configurationNumber, ballot, 0);
}

Message::Message(
    RSLProtocolVersion version,
    UInt16 msg,
    MemberId memberId,
    UInt64 decree,
    UInt32 configurationNumber,
    BallotNumber ballot,
    UInt64 payload)
{
    InitMessage(version, msg, memberId, decree, configurationNumber, ballot, payload);
}

void
Message::InitMessage(
    RSLProtocolVersion version,
    UInt16 msg,
    MemberId memberId,
    UInt64 decree,
    UInt32 configurationNumber,
    BallotNumber ballot,
    UInt64 payload)
{
    m_version = version;
    m_unMarshalLen = 0;
    m_checksum = 0;
    m_magic = s_MessageMagic;
    m_msgId = msg;
    m_memberId = memberId;
    m_decree = decree;
    m_configurationNumber = configurationNumber;
    m_ballot = ballot;
    m_remoteIP = 0;
    m_remotePort = 0;
    m_asClient = false;
    m_payload = payload;
}

void
Message::MarshalBuf(char *buf, UInt32 bufLen)
{
    LogAssert(bufLen >= GetMarshalLen());
    FixedMarshalMemoryManager manager(buf, bufLen);
    MarshalData marshal(&manager);
    Marshal(&marshal);
}

UInt32
Message::GetMarshalLen()
{
    return GetBaseSize();
}

bool
Message::UnMarshalBuf(char *buf, UInt32 bufLen)
{
    FixedMarshalMemoryManager manager(buf, bufLen);
    MarshalData marshal(&manager);
    marshal.SetMarshaledLength(bufLen);
    return UnMarshal(&marshal);
}

void
Message::Marshal(MarshalData *marshal)
{
    LogAssert(m_version != 0);
    marshal->WriteUInt16((UInt16) m_version);
    marshal->WriteUInt32(GetMarshalLen());
    marshal->WriteUInt64(m_checksum);
    marshal->WriteUInt32(m_magic);
    marshal->WriteUInt16(m_msgId);
    m_memberId.Marshal(marshal, m_version);
    marshal->WriteUInt64(m_decree);
    if (m_version >= RSLProtocolVersion_3)
    {
        marshal->WriteUInt32(m_configurationNumber);
    }

    m_ballot.Marshal(marshal, m_version);

    if (m_version >= RSLProtocolVersion_6)
    {
        marshal->WriteUInt64(m_payload);
    }
}

bool
Message::Peek(MarshalData *marshal)
{
    UInt32 readPtr = marshal->GetReadPointer();
    bool ret = UnMarshal(marshal);
    marshal->SetReadPointer(readPtr);
    return ret;
}

bool
Message::UnMarshal(MarshalData  *marshal)
{
    UInt16 version;
    if (!marshal->ReadUInt16(&version))
    {
        return false;
    }

    if (!Message::IsVersionValid(version))
    {
        RSLError("Unknown message version", LogTag_RSLMsgVersion, version);
        return false;
    }

    m_version = (RSLProtocolVersion) version;

    if (!marshal->ReadUInt32(&m_unMarshalLen) ||
        !marshal->ReadUInt64(&m_checksum) ||
        !marshal->ReadUInt32(&m_magic) ||
        !marshal->ReadUInt16(&m_msgId) ||
        !m_memberId.UnMarshal(marshal, m_version) ||
        !marshal->ReadUInt64(&m_decree))
    {
        return false;
    }

    if (m_version >= RSLProtocolVersion_3)
    {
        if (!marshal->ReadUInt32(&m_configurationNumber))
        {
            return false;
        }
    }
    else
    {
        m_configurationNumber = 1;
    }

    if (!m_ballot.UnMarshal(marshal, m_version))
    {
        return false;
    }

    if (m_version >= RSLProtocolVersion_6)
    {
        if (!marshal->ReadUInt64(&m_payload))
        {
            return false;
        }
    }
    else
    {
        m_payload = 0;
    }

    if (m_unMarshalLen < GetBaseSize())
    {
        RSLError("Invalid message length", LogTag_RSLMsgLen, m_unMarshalLen);
        return false;
    }

    if (m_magic != s_MessageMagic)
    {
        RSLError("Bad magic number", LogTag_U32X1, m_magic);
        return false;
    }

    return true;
}

void
Message::CalculateChecksum(char *buf, UInt32 len)
{
    LogAssert(len == GetMarshalLen());
    UInt32 writeOffset = s_ChecksumOffset;
    UInt32 dataOffset = writeOffset + sizeof(m_checksum);
    m_checksum = Utils::CalculateChecksum(buf+dataOffset, len-dataOffset);
    FixedMarshalMemoryManager manager(buf+writeOffset, len-writeOffset);
    MarshalData marshal(&manager);
    marshal.WriteUInt64(m_checksum);
}

bool
Message::VerifyChecksum(char *buf, UInt32 len)
{
    LogAssert(len == m_unMarshalLen);
    UInt32 dataOffset = s_ChecksumOffset + sizeof(m_checksum);
    UInt64 checksum = Utils::CalculateChecksum(buf+dataOffset, len-dataOffset);
    if (m_checksum != checksum)
    {
        RSLError("Incorrect message checksum",
                 LogTag_U64X1, m_checksum, LogTag_U64X2, checksum);
        return false;
    }
    return true;
}

bool
Message::IsLowerDecree(UInt64 curDecree, BallotNumber &curBallot)
{
    return (m_decree < curDecree || m_ballot < curBallot);
}

bool
Message::IsHigherDecree(UInt64 curDecree, BallotNumber &curBallot)
{
    return ((m_decree > curDecree && m_ballot >= curBallot) ||
            (m_decree == curDecree && m_ballot > curBallot));
}

bool
Message::IsNextDecree(UInt64 curDecree, BallotNumber &curBallot)
{
    return ((m_decree == curDecree && m_ballot >= curBallot) ||
            (m_decree == curDecree+1 && m_ballot == curBallot));
}

bool
Message::IsSameDecree(UInt64 curDecree, BallotNumber &curBallot)
{
    return (m_decree == curDecree && m_ballot == curBallot);
}

void
Message::LogString(char *buf, size_t len, char** end) const
{
    if (FAILED(StringCchPrintfExA(buf, len, end, NULL, 0,
                                  "0x%hx;0x%I64x;0x%I32x;",
                                  m_msgId, m_decree, m_configurationNumber)))
    {
        *end = buf;
        return;
    }
    len-= *end - buf;
    m_ballot.LogString(*end, len, end);
}

UInt32
Message::GetBaseSize() const
{
    UInt32 size = 
        2 + // version
        4 + // length
        8 + // checksum
        4 + // magic
        2 + // messageid
        MemberId::GetBaseSize(m_version) + // memberid
        8 + // decree
        BallotNumber::GetBaseSize(m_version); // ballot

    if (m_version >= RSLProtocolVersion_3)
    {
        size = size + 4; // 4 bytes for configuration number
    }

    if (m_version >= RSLProtocolVersion_6)
    {
        size = size + 8; // 8 bytes for payload
    }

    return size;
}

bool
Message::ReadFromSocket(StreamSocket *socket, UInt32 maxMessageSize)
{
    const int HeaderSize = 6;

    UInt32 bytesRead;
    UInt16 version = 0;
    UInt32 length = 0;
    char header[HeaderSize];

    int ec = socket->Read(&header, HeaderSize, &bytesRead);
    if (ec != NO_ERROR || bytesRead != HeaderSize)
    {
        RSLInfo("Read message version from socket failed",
                LogTag_ErrorCode, ec, LogTag_UInt1, bytesRead);
        return false;
    }

    MarshalData marshal(header, HeaderSize, false);

    if (!marshal.ReadUInt16(&version) ||
        !marshal.ReadUInt32(&length) ||
        !Message::IsVersionValid(version))
    {
        RSLError("Unknown message version", LogTag_RSLMsgVersion, version);
        return false;
    }

    if (length > maxMessageSize)
    {
        RSLError("Discarding large message", LogTag_UInt1, length);
        return false;
    }

    StandardMarshalMemoryManager memory(length);
    char *msgBuf = (char *) memory.GetBuffer();
    memcpy(msgBuf, &header, HeaderSize);
    ec = socket->Read(msgBuf + HeaderSize, length - HeaderSize, &bytesRead);
    if (ec != NO_ERROR || bytesRead != length - HeaderSize)
    {
        RSLInfo("Read message body from socket failed",
                LogTag_ErrorCode, ec, LogTag_UInt1, bytesRead);
        return false;
    }
    if (!UnMarshalBuf(msgBuf, length))
    {
        RSLError("Failed to unmarshal message");
        return false;
    }

    return true;
}

bool
Message::IsVersionValid(UInt16 version)
{
    return (version == (UInt16) RSLProtocolVersion_1 ||
            version == (UInt16) RSLProtocolVersion_2 ||
            version == (UInt16) RSLProtocolVersion_3 ||
            version == (UInt16) RSLProtocolVersion_4 ||
            version == (UInt16) RSLProtocolVersion_5 ||
            version == (UInt16) RSLProtocolVersion_6);
}

Vote::Vote() :
    Message((RSLProtocolVersion) 0, Message_Vote), 
    m_isReconfiguration(false), m_isExclusiveVote(false),m_numRequests(0),
    m_reconfigurationCookie(NULL), m_relinquishPrimary(false)
{}

Vote::Vote(
    RSLProtocolVersion version,
    MemberSet *membersInNewConfiguration,
    void *reconfigCookie,
    PrimaryCookie *cookie) :
    Message(version, Message_Vote), m_isReconfiguration(true), m_isExclusiveVote(false),m_numRequests(0),
    m_reconfigurationCookie(reconfigCookie), m_relinquishPrimary(false)
{
    Init(membersInNewConfiguration, cookie);
}

Vote::Vote(
    RSLProtocolVersion version,
    MemberId memberId,
    UInt64 decree,
    UInt32 configurationNumber,
    BallotNumber ballot,
    PrimaryCookie *cookie) :
    
    Message(version, Message_Vote, memberId, decree, configurationNumber, ballot),
    m_isReconfiguration(false), m_isExclusiveVote(false), m_numRequests(0), m_reconfigurationCookie(NULL),
    m_relinquishPrimary(false)
{
    Init(NULL, cookie);
}

Vote::Vote(
    RSLProtocolVersion version,
    MemberId memberId,
    UInt64 decree,
    UInt32 configurationNumber,
    BallotNumber ballot,
    PrimaryCookie *cookie,
    bool relinquishPrimary) :

    Message(version, Message_Vote, memberId, decree, configurationNumber, ballot),
    m_isReconfiguration(false), m_isExclusiveVote(false), m_numRequests(0), m_reconfigurationCookie(NULL),
    m_relinquishPrimary(relinquishPrimary)
{
    Init(NULL, cookie);
}

Vote::Vote(
    RSLProtocolVersion version,
    Vote *other,
    PrimaryCookie *cookie) :
    Message(version, Message_Vote, other->m_memberId, other->m_decree,
            other->m_configurationNumber, other->m_ballot),
     m_numRequests(0), m_isReconfiguration(other->m_isReconfiguration),
    m_isExclusiveVote(other->m_isExclusiveVote),m_reconfigurationCookie(other->m_reconfigurationCookie),
    m_relinquishPrimary(false)
{
    Init(other->m_membersInNewConfiguration, cookie);
    for (RequestCtx *ctx = other->m_requests.head; ctx; ctx = ctx->link.next)
    {
        AddRequest((char *) ctx->m_requestBuf, (UInt32) ctx->m_bufLen, ctx->m_cookie);
    }
}

void
Vote::Init(MemberSet *membersInNewConfiguration, PrimaryCookie *cookie)
{
    LogAssert(!m_isReconfiguration || m_version >= RSLProtocolVersion_3);

    UInt32 minSize = GetBaseSize() + cookie->GetMarshalLen();
    if (m_version >= RSLProtocolVersion_3)
    {
        minSize += (!m_isReconfiguration) ? 1 : 1 + membersInNewConfiguration->GetMarshalLen(m_version);
    }
    MarshalMemoryManager *memory = AddMemory(minSize);
    m_marshal.push_back(memory);
    
    memory->SetValidLength(Message::GetMarshalLen());
    
    MarshalData writeMarshal(memory);
    
    if (m_version >= RSLProtocolVersion_2)
    {
        memory->SetReadPointer(memory->GetValidLength());
        cookie->Marshal(&writeMarshal);
        LogAssert(m_primaryCookie.UnMarshal(&writeMarshal, false));
        memory->SetReadPointer(0);
    }
    if (m_version >= RSLProtocolVersion_3)
    {
        writeMarshal.WriteUInt8(m_isReconfiguration ? 1 : 0);
        if (m_isReconfiguration)
        {
            m_membersInNewConfiguration = membersInNewConfiguration;
            m_membersInNewConfiguration->Marshal(&writeMarshal, m_version);
        }
    }
    if (m_version >= RSLProtocolVersion_5)
    {
        writeMarshal.WriteUInt8(m_relinquishPrimary ? 1 : 0);
    }
}

bool Vote::GetRelinquishPrimary()
{
    return m_relinquishPrimary;
}

Vote::~Vote()
{
    RequestCtx *ctx;
    while ((ctx = m_requests.dequeue()) != NULL)
    {
        delete ctx;
    }
    for (size_t i = 0; i < m_marshal.size(); i++)
    {
        VirtualFree(m_marshal[i]->GetBuffer(), 0, MEM_RELEASE);
        delete m_marshal[i];
    }
}

MarshalMemoryManager*
Vote::AddMemory(UInt32 minSize, bool exact)
{
    UInt32 len = (exact) ? minSize : max(minSize, 32*1024);
    UInt32 bufSize = RoundUpToSystemPage(len);
    void *buf = VirtualAlloc(NULL, bufSize, MEM_COMMIT, PAGE_READWRITE);
    LogAssert(buf);
    MarshalMemoryManager *memory = new FixedMarshalMemoryManager(buf, bufSize);
    return memory;
}

UInt32
Vote::GetMarshalLen()
{
    UInt32 len = 0;
    for (size_t i = 0; i < m_marshal.size(); i++)
    {
        len += m_marshal[i]->GetValidLength();
    }
    return len;
}

void
Vote::CalculateChecksum()
{
    ReMarshalHeader();
    LogAssert(m_marshal.size() > 0);
    UInt32 dataOffset =  s_ChecksumOffset + sizeof(m_checksum);
    m_checksum = FingerPrint64::GetInstance()->GetFingerPrint(
        ((BYTE *) m_marshal[0]->GetBuffer()) + dataOffset,
        m_marshal[0]->GetValidLength()-dataOffset);
    for (size_t i = 1; i < m_marshal.size(); i++)
    {
        m_checksum = FingerPrint64::GetInstance()->GetFingerPrint(
            m_checksum,
            m_marshal[i]->GetBuffer(),
            m_marshal[i]->GetValidLength());
    }
}

void
Vote::Marshal(MarshalData *marshal)
{
    Message::Marshal(marshal);
    UInt32 off = Message::GetMarshalLen();
    for (size_t i = 0; i < m_marshal.size(); i++)
    {
        // copy the marhaled buffer
        marshal->WriteData(m_marshal[i]->GetValidLength()-off, (BYTE *)m_marshal[i]->GetBuffer()+off);
        off = 0;
    }
}

bool
Vote::UnMarshal(MarshalData *marshal)
{
    LogAssert(m_marshal.size() == 0);
    UInt32 readPtr = marshal->GetReadPointer();
    if (!Message::UnMarshal(marshal))
    {
        return false;
    }
    marshal->SetReadPointer(readPtr);
    
    if (m_msgId != Message_Vote)
    {
        RSLError("Vote message expected", LogTag_RSLMsg, this);
        return false;
    }
    
    MarshalMemoryManager* marshalMemory = AddMemory(m_unMarshalLen, true);
    m_marshal.push_back(marshalMemory);

    MarshalData writeMarshal(marshalMemory);
    writeMarshal.SetMarshaledLength(m_unMarshalLen);
    
    if (marshal->ReadData(m_unMarshalLen, marshalMemory->GetBuffer()) == false)
    {
        RSLError("Incomplete vote message", LogTag_UInt1, m_unMarshalLen,
                 LogTag_UInt2, marshal->GetMarshaledLength());
        return false;
    }
    
    // ignore the header
    writeMarshal.ForwardReadPointer(GetBaseSize());
    if (m_version >= RSLProtocolVersion_2)
    {
        if (!m_primaryCookie.UnMarshal(&writeMarshal, false))
        {
            RSLError("failed to unmarshal primary cookie", LogTag_RSLMsg, this);
            return false;
        }
    }
    if (m_version >= RSLProtocolVersion_3)
    {
        UInt8 isReconfiguration;
        if (!writeMarshal.ReadUInt8(&isReconfiguration))
        {
            RSLError("Could not read is-reconfiguration byte in vote", LogTag_RSLMsg, this);
            return false;
        }
        m_isReconfiguration = (isReconfiguration != 0);
        if (m_isReconfiguration)
        {
            m_membersInNewConfiguration = new MemberSet();
            if (!m_membersInNewConfiguration->UnMarshal(&writeMarshal, m_version))
            {
                RSLError("Invalid configuration info in vote message", LogTag_RSLMsg, this);
                return false;
            }
        }
    }

    if (m_version >= RSLProtocolVersion_5)
    {
        UInt8 relinquishPrimary;
        if (!writeMarshal.ReadUInt8(&relinquishPrimary))
        {
            RSLError("Could not read relinquishPrimary byte in vote", LogTag_RSLMsg, this);
            return false;
        }
        m_relinquishPrimary = (relinquishPrimary != 0);
    }

    // now parse the buffer
    while (writeMarshal.TestReadRemaining(1))
    {
        LogAssert(!m_isReconfiguration);
        // read the length
        UInt32 reqLen;
        if (!writeMarshal.ReadUInt32(&reqLen))
        {
            RSLError("Failed to read Length",
                     LogTag_UInt1, reqLen, LogTag_UInt2, m_numRequests);
            return false;
        }
        void *reqBuf;
        if (reqLen == 0 || !writeMarshal.PeekDataPointer(reqLen, &reqBuf))
        {
            RSLError("Incorrect length",
                     LogTag_UInt1, reqLen, LogTag_UInt2, m_numRequests);
            return false;
        }
        writeMarshal.ForwardReadPointer(reqLen);
        RequestCtx *ctx = new RequestCtx(m_decree, reqBuf, reqLen, NULL);
        LogAssert(ctx);
        m_requests.enqueue(ctx);
        m_numRequests++;
    }
    return true;
}

void
Vote::AddRequest(char *req, UInt32 len, void *cookie)
{
    LogAssert(!m_isReconfiguration);
    // this should not be a vote that was unmarshaled before.
    LogAssert(m_marshal.size() != 0);
    MarshalMemoryManager *memory = m_marshal.back();
    UInt32 spaceNeeded = len + 4;
    if (spaceNeeded + memory->GetValidLength() > memory->GetBufferLength())
    {
        UInt32 extra = memory->GetValidLength() % s_SystemPageSize;
        MarshalMemoryManager *newMemory = AddMemory(spaceNeeded+extra);

        // copy systempagesize worth of data from the last buffer to new buffer
        if (extra)
        {
            BYTE *buffer = (BYTE *) memory->GetBuffer() + memory->GetValidLength() - extra;            
            memcpy(newMemory->GetBuffer(), buffer, extra);
            newMemory->SetValidLength(extra);
            
            for (RequestCtx *ctx = m_requests.head; ctx; ctx = ctx->link.next)
            {
                BYTE *requestBuf = (BYTE *) ctx->m_requestBuf;
                if (requestBuf >= buffer && requestBuf < buffer + extra)
                {
                    ctx->m_requestBuf = (BYTE *) newMemory->GetBuffer() + (requestBuf - buffer);
                }
            }
            
            memory->SetValidLength(memory->GetValidLength() - extra);
        }
        if (memory->GetValidLength() == 0)
        {
            m_marshal.pop_back();
            VirtualFree(memory->GetBuffer(), 0, MEM_RELEASE);
            delete memory;
        }
        m_marshal.push_back(newMemory);
        memory = newMemory;
    }
    MarshalData writeMarshal(memory);
    writeMarshal.WriteUInt32(len);
    writeMarshal.WriteData(len, req);
    void *data = (char *) memory->GetBuffer() + memory->GetValidLength() - len;
    RequestCtx *ctx = new RequestCtx(0, data, len, cookie);
    LogAssert(ctx);
    m_requests.enqueue(ctx);
    m_numRequests++;
}

void
Vote::ReMarshalHeader()
{
    LogAssert(m_marshal.size() != 0);
    
    FixedMarshalMemoryManager memory(m_marshal[0]->GetBuffer(), GetBaseSize());
    MarshalData marshal(&memory);
    Message::Marshal(&marshal);
}

UInt32
Vote::GetNumBuffers()
{
    return (UInt32) m_marshal.size();
}

void
Vote::GetBuffers(SIZED_BUFFER *buffers, UInt32 count)
{
    ReMarshalHeader();
    LogAssert(count >= m_marshal.size());
    for (size_t i = 0; i < m_marshal.size(); i++)
    {
        buffers[i].m_buf = m_marshal[i]->GetBuffer();
        buffers[i].m_len = RoundUpToPage(m_marshal[i]->GetValidLength());
    }
};

//////////////////////
// JoinMessage
//////////////////////

JoinMessage::JoinMessage() :
    Message((RSLProtocolVersion) 0, Message_Join),
    m_learnPort(0), m_minDecreeInLog(0), m_checkpointedDecree(0), m_checkpointSize(0)
{
}

JoinMessage::JoinMessage(RSLProtocolVersion version,
                         MemberId memberId,
                         UInt64 decree,
                         UInt32 configurationNumber) :
    Message(version, Message_Join, memberId, decree, configurationNumber, BallotNumber()),
    m_learnPort(0), m_minDecreeInLog(0), m_checkpointedDecree(0), m_checkpointSize(0)
{
}
        
UInt32
JoinMessage::GetMarshalLen()
{
    return GetBaseSize() + 2 + 8 + 8 + 8;
}

void
JoinMessage::Marshal(MarshalData *marshal)
{
    Message::Marshal(marshal);
    marshal->WriteUInt16(m_learnPort);
    marshal->WriteUInt64(m_minDecreeInLog);
    marshal->WriteUInt64(m_checkpointedDecree);
    marshal->WriteUInt64(m_checkpointSize);
}

bool
JoinMessage::UnMarshal(MarshalData *marshal)
{
    if (!Message::UnMarshal(marshal))
    {
        return false;
    }

    if (m_msgId != Message_Join)
    {
        RSLError("Join message expected", LogTag_RSLMsg, this);
        return false;
    }

    if (!marshal->ReadUInt16(&m_learnPort) ||
        !marshal->ReadUInt64(&m_minDecreeInLog) ||
        !marshal->ReadUInt64(&m_checkpointedDecree) ||
        !marshal->ReadUInt64(&m_checkpointSize))
    {
        RSLError("Invalid join message", LogTag_RSLMsg, this);
        return false;
    }

    return true;
}

PrepareMsg::PrepareMsg() : Message((RSLProtocolVersion) 0, Message_Prepare)
{}

PrepareMsg::PrepareMsg(
    RSLProtocolVersion version,
    MemberId memberId,
    UInt64 decree,
    UInt32 configurationNumber,
    BallotNumber ballot,
    PrimaryCookie *cookie) :
    Message(version, Message_Prepare, memberId, decree, configurationNumber, ballot),
    m_primaryCookie(cookie->m_data, cookie->m_len, true)
{}

PrepareMsg::~PrepareMsg() {}

UInt32
PrepareMsg::GetMarshalLen()
{
    UInt32 cookieLen = (m_version >= RSLProtocolVersion_2) ? m_primaryCookie.GetMarshalLen() : 0;
    return (cookieLen + GetBaseSize());
}

void
PrepareMsg::Marshal(MarshalData *marshal)
{
    Message::Marshal(marshal);
    if (m_version >= RSLProtocolVersion_2)
    {
        m_primaryCookie.Marshal(marshal);
    }
}

bool
PrepareMsg::UnMarshal(MarshalData *marshal)
{
    if (!Message::UnMarshal(marshal))
    {
        return false;
    }
    if (m_msgId != Message_Prepare)
    {
        RSLError("Prepare message expected", LogTag_RSLMsg, this);
        return false;
    }
    if (m_version >= RSLProtocolVersion_2)
    {
        if (!m_primaryCookie.UnMarshal(marshal, true))
        {
            RSLError("failed to unmarshal primary cookie", LogTag_RSLMsg, this);
            return false;
        }
    }
    return true;
}

PrepareAccepted::PrepareAccepted() : Message((RSLProtocolVersion) 0, Message_PrepareAccepted)
{}

PrepareAccepted::PrepareAccepted(
    RSLProtocolVersion version,
    MemberId memberId,
    UInt64 decree,
    UInt32 configurationNumber,
    BallotNumber ballot,
    Vote *vote) :
    Message(version, Message_PrepareAccepted, memberId, decree, configurationNumber, ballot), m_vote(vote)
{}

PrepareAccepted::~PrepareAccepted()
{
    m_vote = NULL;
}

UInt32
PrepareAccepted::GetMarshalLen()
{
	return (m_vote->GetMarshalLen() + GetBaseSize());
}

void
PrepareAccepted::Marshal(MarshalData *marshal)
{
	Message::Marshal(marshal);
	m_vote->Marshal(marshal);
}

bool
PrepareAccepted::UnMarshal(MarshalData *marshal)
{
	if (!Message::UnMarshal(marshal))
	{
		return false;
	}
	if (m_msgId != Message_PrepareAccepted)
	{
		RSLError("Prepare message expected", LogTag_RSLMsg, this);
		return false;
	}
	// unmarshal the vote
	m_vote = new Vote();
	LogAssert(m_vote);
	if (!m_vote->UnMarshal(marshal))
	{
		RSLError("failed to unmarshal vote", LogTag_RSLMsg, this);
		return false;
	}
	return true;
}

StatusResponse::StatusResponse() :
    Message((RSLProtocolVersion) 0, Message_StatusResponse), m_queryDecree(0), m_lastReceivedAgo(0),
    m_minDecreeInLog(0), m_checkpointedDecree(0), m_checkpointSize(0), m_state(0)
{}

StatusResponse::StatusResponse(
    RSLProtocolVersion version,
    MemberId memberId,
    UInt64 decree,
    UInt32 configurationNumber,
    BallotNumber ballot) :
    Message(version, Message_StatusResponse, memberId, decree, configurationNumber, ballot),
    m_queryDecree(0), m_lastReceivedAgo(0), m_minDecreeInLog(0), m_checkpointedDecree(0),
    m_checkpointSize(0), m_state(0)
{}

void
StatusResponse::Marshal(MarshalData *marshal)
{
    Message::Marshal(marshal);
    marshal->WriteUInt64(m_queryDecree);
    m_queryBallot.Marshal(marshal, m_version);
    marshal->WriteUInt64(m_lastReceivedAgo);
    marshal->WriteUInt64(m_minDecreeInLog);
    marshal->WriteUInt64(m_checkpointedDecree);
    marshal->WriteUInt64(m_checkpointSize);
    m_maxBallot.Marshal(marshal, m_version);
    marshal->WriteUInt32(m_state);
}

bool
StatusResponse::UnMarshal(MarshalData *marshal)
{
    if (!Message::UnMarshal(marshal))
    {
        return false;
    }
    if (m_msgId != Message_StatusResponse)
    {
        RSLError("StatusResponse message expected", LogTag_RSLMsg, this);
        return false;
    }

    if (!marshal->ReadUInt64(&m_queryDecree) ||
        !m_queryBallot.UnMarshal(marshal, m_version) ||
        !marshal->ReadUInt64((UInt64 *) &m_lastReceivedAgo) ||
        !marshal->ReadUInt64(&m_minDecreeInLog) ||
        !marshal->ReadUInt64(&m_checkpointedDecree) ||
        !marshal->ReadUInt64(&m_checkpointSize) ||
        !m_maxBallot.UnMarshal(marshal, m_version) ||
        !marshal->ReadUInt32(&m_state))
    {
        return false;
    }
    return true;
}

UInt32
StatusResponse::GetMarshalLen()
{
    return (8 + BallotNumber::GetBaseSize(m_version) + 8 + 8 + 8 + 8 + BallotNumber::GetBaseSize(m_version) + 4 + GetBaseSize());
}

BootstrapMsg::BootstrapMsg() :
    Message((RSLProtocolVersion) 0, Message_Bootstrap), m_memberSet(NULL)
{
    m_memberSet = new MemberSet();
}

BootstrapMsg::BootstrapMsg(RSLProtocolVersion version, MemberId memberId, MemberSet &memberSet) :
    Message(version, Message_Bootstrap, memberId, 0, 0, BallotNumber()), m_memberSet(NULL)
{
    m_memberSet = new MemberSet();
    m_memberSet->Copy(&memberSet);
}

BootstrapMsg::~BootstrapMsg()
{
    //delete m_memberSet;
    m_memberSet = NULL;
}

void
BootstrapMsg::Marshal(MarshalData *marshal)
{
    Message::Marshal(marshal);
    m_memberSet->Marshal(marshal, m_version);
}

bool
BootstrapMsg::UnMarshal(MarshalData *marshal)
{
    if (!Message::UnMarshal(marshal))
    {
        return false;
    }
    if (m_msgId != Message_Bootstrap)
    {
        RSLError("Bootstrap message expected", LogTag_RSLMsg, this);
        return false;
    }
    if (m_memberSet->UnMarshal(marshal, m_version) == false)
    {
        RSLError("failed to unmarshal member set", LogTag_RSLMsg, this);
        return false;
    }
    return true;
}

UInt32
BootstrapMsg::GetMarshalLen()
{
    return
        GetBaseSize() +                 // Message size
        m_memberSet->GetMarshalLen(m_version);   // MemberSet m_memberSet
}
