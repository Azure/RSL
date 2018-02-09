#include "TestEngine.h"

FakeLegislator::FakeLegislator(const char* memberid)
{
    Init(MemberId(memberid));
}

FakeLegislator::FakeLegislator(MemberId memberId)
{
    Init(memberId);
}

FakeLegislator::FakeLegislator(const FakeLegislator &other)
{
    Init(other.m_memberId);
    CopyFrom(other);
}

FakeLegislator &
FakeLegislator::operator=(const FakeLegislator &other)
{
    Init(other.m_memberId);
    CopyFrom(other);
    return *this;
}

void
FakeLegislator::Init(MemberId memberId)
{
    m_version = RSLProtocolVersion_1;
    m_memberId = memberId;
    m_primaryCookie = new PrimaryCookie();
    m_ballot = BallotNumber(0, MemberId());
    m_decree = 0;
    m_maxBallot = BallotNumber(0, MemberId());
    m_configNumber = 1;
    m_lastVote = NULL;
}

FakeLegislator::~FakeLegislator()
{
    delete m_primaryCookie;
    m_primaryCookie = NULL;
    SetLastVote(NULL);
    ClearLogs();
}

Message *
FakeLegislator::CreateCopy(Message * msg)
{
    if (msg == NULL)
    {
        return NULL;
    }
    Message * copy = NULL;
    switch (msg->m_msgId)
    {
        case Message_Vote:
            copy = new Vote((RSLProtocolVersion) msg->m_version, (Vote *) msg, m_primaryCookie);
            break;
        case Message_Prepare:
            copy = new PrepareMsg();
            *copy = *((PrepareMsg *) msg);
            break;
        case Message_ReconfigurationDecision:
            copy = new Message();
            *copy = *((Message *) msg);
            break;
        default:
            UT_AssertFail("Copy for this type of message is not available!");
    }
    return copy;
}

//-------------------------------------------------------------------------
// Ballot, Decree and ConfigurationNumber
//-------------------------------------------------------------------------

void
FakeLegislator::SetLastVote(Vote * vote)
{
    bool found = false;
    for (size_t i=0; m_lastVote != NULL && !found && i < m_log.size(); i++)
    {
        found = (m_log[i] == m_lastVote);
    }
    if (!found && m_lastVote != NULL)
    {
        delete m_lastVote;
    }
    m_lastVote = vote;
}

Vote *
FakeLegislator::GetCopyLastVote()
{
    return (Vote *) CreateCopy(m_lastVote);
}

void
FakeLegislator::Reset(StatusResponse &status)
{
    m_version = (RSLProtocolVersion) status.m_version;
    m_configNumber = status.m_configurationNumber;
    m_ballot = status.m_ballot;
    m_decree = status.m_decree;
    m_maxBallot = status.m_maxBallot;
    SetLastVote(NULL);
    ClearLogs();
}

void
FakeLegislator::CopyFrom(const FakeLegislator &fake)
{
    m_version = fake.m_version;
    m_ballot = fake.m_ballot;
    m_decree = fake.m_decree;
    m_maxBallot = fake.m_maxBallot;
    m_configNumber = fake.m_configNumber;
    SetLastVote(NULL);
    m_log.clear();
    for (size_t i=0; i < fake.m_log.size(); i++)
    {
        Message * copy = CreateCopy(fake.m_log[i]);
        m_log.push_back(copy);
        if (fake.m_log[i] == fake.m_lastVote)
        {
            SetLastVote((Vote *) m_log.back());
        }
    }
    if (m_lastVote == NULL)
    {
        SetLastVote((Vote *) CreateCopy(fake.m_lastVote));
    }
}

void
FakeLegislator::SetBallotId(UInt32 id)
{
    m_ballot = BallotNumber(id, m_memberId);
}

void
FakeLegislator::IncrementBallot(UInt32 inc)
{
    m_ballot = BallotNumber(m_maxBallot.m_ballotId + inc, m_memberId);
    m_maxBallot = m_ballot;
}

void
FakeLegislator::GetStatus(StatusResponse * status)
{
    StatusResponse * resp = NewStatusResponse(0, BallotNumber(), -1);
    *status = *resp;
    delete resp;
}

void
FakeLegislator::AcceptPrepare(Message * msg)
{
    //m_version = (RSLProtocolVersion) msg->m_version;
    //m_ballot = msg->m_ballot;
    //m_configNumber = msg->m_configurationNumber;
    //m_decree = msg->m_decree;
    UT_AssertIsTrue(m_maxBallot < msg->m_ballot || m_maxBallot == msg->m_ballot);
    m_maxBallot = msg->m_ballot;
    AppendLog(msg);
}

void
FakeLegislator::AcceptVote(Vote * msg)
{
    UT_AssertIsTrue(m_ballot < msg->m_ballot || m_ballot == msg->m_ballot);
    UT_AssertIsTrue(m_configNumber == msg->m_configurationNumber);
    UT_AssertIsTrue(m_decree == msg->m_decree || m_decree == msg->m_decree-1);

    m_version = (RSLProtocolVersion) msg->m_version;
    m_ballot = msg->m_ballot;
    m_configNumber = msg->m_configurationNumber;
    m_decree = msg->m_decree;
    if (m_maxBallot < msg->m_ballot)
    {
        m_maxBallot = msg->m_ballot;
    }
    AppendLog(msg);
}

void
FakeLegislator::AcceptReconfigurationDecision(Message * msg)
{
    UT_AssertIsTrue(m_ballot == msg->m_ballot);
    UT_AssertIsTrue(m_configNumber == msg->m_configurationNumber);
    UT_AssertIsTrue(m_decree == msg->m_decree || m_decree == msg->m_decree-1);

    m_version = (RSLProtocolVersion) msg->m_version;
    m_configNumber = msg->m_configurationNumber + 1;
    m_ballot = BallotNumber();
    m_decree = msg->m_decree + 1;
    m_maxBallot = BallotNumber(msg->m_ballot.m_ballotId + 1, MemberId());
    AppendLog(msg);
    // Let VoteFactory know about the empy vote (where reqNum=0)
    SetLastVote(VoteFactory::NewEmptyVote(m_version, m_decree, m_configNumber));
}

void
FakeLegislator::AppendLog(Message * msg)
{
    Message * copy = CreateCopy(msg);
    m_log.push_back(copy);
    if (copy->m_msgId == Message_Vote)
    {
        SetLastVote((Vote *) m_log.back());
    }
}

void
FakeLegislator::ClearLogs()
{
    for (size_t i=0; i < m_log.size(); i++)
    {
        if (m_log[i] == m_lastVote)
        {
            m_lastVote = NULL;
        }
        delete m_log[i];
    }
    m_log.clear();
}

//-------------------------------------------------------------------------
// Message Construction
//-------------------------------------------------------------------------

PrepareMsg *
FakeLegislator::NewPrepare()
{
    return new PrepareMsg(m_version, m_memberId, m_decree,
        m_configNumber, m_ballot, m_primaryCookie);
}

PrepareAccepted * 
FakeLegislator::NewPrepareAccepted()
{
    Vote * copy = (Vote *) CreateCopy(m_lastVote);
    UT_AssertIsNotNull(copy);
    return new PrepareAccepted(m_version, m_memberId, m_decree, m_configNumber,
        m_maxBallot, copy);
}


Vote *
FakeLegislator::NewVote()
{
    return VoteFactory::NewVote(m_version, m_memberId, m_decree, m_configNumber,
        m_ballot, m_primaryCookie);
}

Vote * 
FakeLegislator::NewVote(RSLNodeCollection &nodes, void * /*reconfigCookie*/)
{
    MemberSet * newMemberSet = new MemberSet(nodes, NULL, 0);
    Vote * vote = new Vote(m_version, newMemberSet, NULL, m_primaryCookie);
    vote->m_memberId = m_memberId;
    vote->m_decree = m_decree;
    vote->m_ballot = m_ballot;
    vote->m_configurationNumber = m_configNumber;
    return vote;
}

Message * 
FakeLegislator::NewVoteAccepted()
{
    Message * msg = new Message(m_version, Message_VoteAccepted, m_memberId, m_decree, m_configNumber, m_ballot);
    return msg;
}

Message * 
FakeLegislator::NewNotAccepted()
{
    Message * msg = new Message(m_version, Message_NotAccepted, m_memberId, m_decree, m_configNumber, m_maxBallot);
    return msg;
}

Message * 
FakeLegislator::NewStatusQuery()
{
    Message * msg = new Message(m_version, Message_StatusQuery, m_memberId, m_decree, m_configNumber, m_ballot);
    return msg;
}

StatusResponse *
FakeLegislator::NewStatusResponse(UInt64 queryDecree, BallotNumber queryBallot, Int64 lastReceived)
{
    StatusResponse * res = new StatusResponse(m_version, m_memberId, m_decree,
        m_configNumber, m_ballot);
    res->m_maxBallot = m_maxBallot;
    res->m_queryDecree = queryDecree;
    res->m_queryBallot = queryBallot;
    if (lastReceived == -1)
    {
        res->m_lastReceivedAgo = _I64_MAX;
    }
    return res;
}

JoinMessage *
FakeLegislator::NewJoin(UInt32 configNumber, UInt64 minDecreeInLog, UInt64 checkpointedDecree)
{
    JoinMessage * msg = new JoinMessage(m_version, m_memberId, m_decree, configNumber);
    msg->m_minDecreeInLog = minDecreeInLog;
    msg->m_checkpointedDecree = checkpointedDecree;
    msg->m_learnPort = TestEngine::m_nodeTemplate.m_rslPort+1;
    return msg;
}

JoinMessage *
FakeLegislator::NewJoin()
{
    return NewJoin(m_configNumber, 0, 0);
}

Message *
FakeLegislator::NewJoinRequest()
{
    Message * msg = new Message(m_version, Message_JoinRequest,
        m_memberId, m_decree, m_configNumber, m_ballot);
    return msg;
}

Message * 
FakeLegislator::NewFetchVotes()
{
    Message * msg = new Message(m_version, Message_FetchVotes, m_memberId, m_decree, m_configNumber, m_ballot);
    return msg;
}

Message * 
FakeLegislator::NewFetchCheckpoint(UInt64 checkpointedDecree)
{
    Message * msg = new Message(m_version, Message_FetchCheckpoint, m_memberId, checkpointedDecree, 1, BallotNumber());
    return msg;
}

Message * 
FakeLegislator::NewReconfigurationDecision()
{
    Message * msg = new Message(m_version, Message_ReconfigurationDecision,
        m_memberId, m_decree, m_configNumber, m_ballot);
    return msg;
}

Message * 
FakeLegislator::NewDefunctConfiguration(UInt32 highestDefunctConfigurationNumber)
{
    Message * msg = new Message(m_version, Message_DefunctConfiguration,
        m_memberId, 0, highestDefunctConfigurationNumber, BallotNumber());
    return msg;
}

//Message *
//FakeLegislator::NewMessage(UInt16 msgId)
//{
//    switch (msgId) {
//        //case Message_None:
//        case Message_Vote:              return NewVote()
//        case Message_VoteAccepted:      return NewVoteAccepted()
//        case Message_Prepare:           return NewPrepare()
//        case Message_PrepareAccepted:   return NewPrepareAccepted()
//        case Message_NotAccepted:       return NewNotAccepted()
//        case Message_StatusQuery:       return NewStatusQuery()
//        //case Message_StatusResponse:
//        case Message_FetchVotes:        return NewFetchVotes()
//        case Message_FetchCheckpoint:   return NewFetchCheckpoint()
//        case Message_ReconfigurationDecision:   return NewReconfigurationDecision()
//        case Message_DefunctConfiguration:      return NewDefunctConfiguration()
//        case Message_Join:              return NewJoin()
//        case Message_JoinRequest:       return NewJoinRequest()
//    }
//    UT_AssertFail("No default implementation for messageId=%d", msgId);
//    return NULL;
//}

//-------------------------------------------------------------------------
// Send and Receive
//-------------------------------------------------------------------------

#define CallForAllAndSelf(_list,_func) \
    for (size_t _i=0; _list != NULL && _i < (*_list).size(); _i++) \
    { \
        if ((*_list)[_i]->m_memberId != this->m_memberId) \
        { \
            (*_list)[_i]->_func; \
        } \
    } \
    this->_func;


Message *
FakeLegislator::SendPrepare(FakeLegislatorCollection * targets, bool sendToReplica)
{
    IncrementBallot();
    Message * msg = NewPrepare();
    if (sendToReplica)
    {
        TestEngine::SendRequest(msg);
    }
    CallForAllAndSelf(targets, AcceptPrepare(msg));
    return msg;
}

Vote *
FakeLegislator::SendVote(FakeLegislatorCollection * targets, bool sendToReplica)
{
    m_decree++;
    Vote * vote = NewVote();
    if (sendToReplica)
    {
        TestEngine::SendRequest(vote);
    }
    CallForAllAndSelf(targets, AcceptVote(vote));
    return vote;
}

Vote * 
FakeLegislator::SendLastVote(FakeLegislatorCollection * targets, bool sendToReplica)
{
    Vote * vote = (Vote *) CreateCopy(m_lastVote);
    if (vote == NULL)
    {
        vote = NewVote();
    }
    UT_AssertIsTrue(vote->m_decree == m_decree);
    vote->m_memberId = m_memberId;
    vote->m_ballot = m_ballot;
    vote->m_configurationNumber = m_configNumber;
    if (sendToReplica)
    {
        TestEngine::SendRequest(vote);
    }
    CallForAllAndSelf(targets, AcceptVote(vote));
    return vote;
}


Vote *
FakeLegislator::SendChangeConfig(RSLNodeCollection &nodes, void * reconfigCookie,
                                 FakeLegislatorCollection * targets, bool sendToReplica)
{
    m_decree++;
    Vote * msg = NewVote(nodes, reconfigCookie);
    if (sendToReplica)
    {
        TestEngine::SendRequest(msg);
    }
    CallForAllAndSelf(targets, AcceptVote(msg));
    return msg;
}

void
FakeLegislator::SendStatusQuery()
{
    Message * msg = NewStatusQuery();
    TestEngine::SendRequest(msg);
    delete msg;
}

void
FakeLegislator::SendFetchVotes()
{
    Message * msg = NewFetchVotes();
    TestEngine::SendRequest(msg);
    delete msg;
}

void
FakeLegislator::SendFetchCheckpoint(UInt64 checkpointedDecree)
{
    Message * msg = NewFetchCheckpoint(checkpointedDecree);
    TestEngine::SendRequest(msg);
    delete msg;
}

Message *
FakeLegislator::SendReconfigurationDecision(FakeLegislatorCollection * targets, bool sendToReplica)
{
    Message * msg = NewReconfigurationDecision();
    if (sendToReplica)
    {
        TestEngine::SendRequest(msg);
    }
    CallForAllAndSelf(targets, AcceptReconfigurationDecision(msg));
    return msg;
}

void
FakeLegislator::SendDefunctConfiguration(UInt32 highestDefunctConfigurationNumber)
{
    Message * msg = NewDefunctConfiguration(highestDefunctConfigurationNumber);
    TestEngine::SendRequest(msg);
    delete msg;
}

void
FakeLegislator::SendJoin()
{
    SendJoin(m_configNumber, 0, 0);
}

void
FakeLegislator::SendJoin(UInt32 configNumber, UInt64 minDecreeInLog, UInt64 checkpointedDecree)
{
    Message * msg = NewJoin(configNumber, minDecreeInLog, checkpointedDecree);
    TestEngine::SendRequest(msg);
    delete msg;
}

void
FakeLegislator::SendJoinRequest()
{
    Message * msg = NewJoinRequest();
    TestEngine::SendRequest(msg);
    delete msg;
}

void
FakeLegislator::SendPrepareAccepted()
{
    Message * msg = NewPrepareAccepted();
    TestEngine::SendResponse(msg);
    delete msg;
}

void
FakeLegislator::SendPrepareNotAccepted()
{
    Message * msg = NewNotAccepted();
    TestEngine::SendResponse(msg);
    delete msg;
}

void
FakeLegislator::SendVoteAccepted()
{
    Message * msg = NewVoteAccepted();
    TestEngine::SendResponse(msg);
    delete msg;
}

void
FakeLegislator::SendVoteNotAccepted()
{
    Message * msg = NewNotAccepted();
    TestEngine::SendResponse(msg);
    delete msg;
}

void
FakeLegislator::SendStatusResponse(UInt64 queryDecree, BallotNumber queryBallot, Int64 lastReceived)
{
    Message * msg = NewStatusResponse(queryDecree, queryBallot, lastReceived);
    TestEngine::SendResponse(msg);
    delete msg;
}

void
FakeLegislator::SendLog()
{
    UT_AssertFail("Not implemented!");
}


//-------------------------------------------------------------------------
// FakeLegislatorCollection
//-------------------------------------------------------------------------


FakeLegislatorCollection::FakeLegislatorCollection(FakeLegislator * first, ...)
{
    va_list ptr;
    va_start(ptr, first);
    while (first != NULL) {
        m_legislators.push_back(first);
        first = va_arg(ptr, FakeLegislator *);
    }
    va_end(ptr);
    m_head = NULL;
}

FakeLegislatorCollection::FakeLegislatorCollection()
{
    m_head = NULL;
}


FakeLegislatorCollection::~FakeLegislatorCollection()
{
    clear();
}

FakeLegislator *
FakeLegislatorCollection::getByMemberId(MemberId memberId)
{
    for (size_t i =0; i < m_legislators.size(); i++)
    {
        if (m_legislators[i]->m_memberId == memberId)
        {
            return m_legislators[i];
        }
    }
    return NULL;
}

FakeLegislator *
FakeLegislatorCollection::operator[] (size_t idx)
{
    return m_legislators[idx];
}

void
FakeLegislatorCollection::append(FakeLegislator & fake)
{
    Link * newLink = new Link();
    newLink->m_leg = fake;
    newLink->m_next = m_head;
    m_head = newLink;
    m_legislators.push_back(&newLink->m_leg);
}

void
FakeLegislatorCollection::clear()
{
    m_legislators.clear();
    while (m_head != NULL)
    {
        Link * kill = m_head;
        m_head = m_head->m_next;
        delete kill;
    }    
    m_head = NULL;
}

size_t
FakeLegislatorCollection::size()
{
    return m_legislators.size();
}
