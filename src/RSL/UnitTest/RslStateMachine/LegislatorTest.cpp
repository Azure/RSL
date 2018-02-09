#pragma warning(disable:4512)
#include "legislatortest.h"

int
LegislatorTest::NumReqs(UInt64 decree)
{
    return VoteFactory::NumReqs(decree);
}

int
LegislatorTest::ReqLen(UInt64 decree)
{
    return VoteFactory::ReqLen(decree);
}

char*
LegislatorTest::ReqBuf(UInt64 decree)
{
    return VoteFactory::ReqBuf(decree);
}

// constructs and returns a new vote.
Vote*
LegislatorTest::NewVote(RSLProtocolVersion version, MemberId memberId, UInt64 decree, BallotNumber ballot,
                        int numReqs, int len, void *cookie)
{
    return VoteFactory::NewVote(version, memberId, decree,
        m_self.m_configNumber, ballot, numReqs, len, cookie);
}

// Returns a vote with default # of requests.
Vote*
LegislatorTest::NewVote(RSLProtocolVersion version, MemberId memberId, UInt64 decree, BallotNumber ballot)
{
    return VoteFactory::NewVote(version, memberId, decree,
        m_self.m_configNumber, ballot);
}

Vote*
LegislatorTest::NewVote(RSLProtocolVersion version, MemberId memberId, UInt64 decree, BallotNumber ballot,
                        PrimaryCookie *primaryCookie)
{
    return VoteFactory::NewVote(version, memberId, decree,
        m_self.m_configNumber, ballot, primaryCookie);
}

UInt32
LegislatorTest::NumFastReads()
{
    return VoteFactory::NumFastReads();
}

void 
LegislatorTest::GenerateConfigFile(UInt32 numReplicas, RSLConfigParam &cfg, RSLNodeCollection &nodes, UInt32 numReaderThreads)
{
    TestEngine::GenerateConfigFile(numReplicas, RSLNode::ParseMemberIdAsUInt64(m_legislatorMemberId.GetValue()), numReaderThreads, cfg, nodes);
}

LegislatorTest::LegislatorTest() :
        m_self(MemberId()), m_machine(NULL),
        m_memberId(), m_ip(0), m_port(0), m_legislatorPort(0),
        m_legislatorMemberId(), m_version(RSLProtocolVersion_1)
{
}

BallotNumber
LegislatorTest::LowerBallot(BallotNumber b)
{
    BallotNumber newBallot(b.m_ballotId-1, m_self.m_memberId);
    return newBallot;
}

BallotNumber
LegislatorTest::HighBallot(BallotNumber b)
{
    BallotNumber newBallot(b.m_ballotId+1, m_self.m_memberId);
    return newBallot;
}

void
LegislatorTest::Init()
{

    RSLNode node = TestEngine::GetFakeNode(1);
    m_memberId = MemberId(node.m_memberIdString);
    m_ip = node.m_ip;
    m_port = node.m_rslPort;

    m_self = FakeLegislator(m_memberId);
    m_version = (RSLProtocolVersion) TestEngine::m_status.m_version;
    m_legislatorMemberId = MemberId(TestEngine::m_testingNode.m_memberIdString);
    m_machine = TestEngine::m_machine;

    TestEngine::SetExternalStatusUpdate(&m_status);
}

void
LegislatorTest::SendMessage(Message *msg, bool asClient, UInt16 port)
{
    TestEngine::SendMessage(msg, asClient, port);
}

void
LegislatorTest::SendBuffer(void *buf, UInt32 length, bool asClient)
{
    TestEngine::SendBuffer(buf, length, asClient);
}

void
LegislatorTest::SendResponse(Message *msg)
{
    SendMessage(msg, false);
}

void
LegislatorTest::SendResponse(UInt16 msgId, UInt64 decree, BallotNumber ballot)
{
    Message *msg = GenerateMessage(msgId, decree, ballot);
    SendResponse(msg);
    delete msg;
}

Message*
LegislatorTest::ReceiveResponse(UInt16 messageId, bool verify)
{
    return TestEngine::ReceiveResponse(messageId, verify);
}

Message*
LegislatorTest::ReceiveResponse(UInt16 msgId, UInt64 decree, BallotNumber ballot)
{
    Message* msg = ReceiveResponse(msgId);
    UT_AssertAreEqual(decree, msg->m_decree);
    UT_AssertIsTrue(ballot == msg->m_ballot);
    return msg;
}

void
LegislatorTest::SendRequest(Message *msg)
{
    SendMessage(msg, true);
}

UInt32
LegislatorTest::MaxSentBallotId()
{
    return TestEngine::MaxSentBallotId();
}

void
LegislatorTest::SendRequest(UInt16 msgId, UInt64 decree, BallotNumber ballot)
{
    Message *msg = GenerateMessage(msgId, decree, ballot);
    SendRequest(msg);
    delete msg;
}

    
Message*
LegislatorTest::ReceiveRequest(UInt16 messageId, bool verify)
{
    return TestEngine::ReceiveRequests(messageId, verify);
}

Message*
LegislatorTest::ReceiveRequest(UInt16 msgId, UInt64 decree, BallotNumber ballot)
{
    Message* msg = ReceiveRequest(msgId);
    UT_AssertAreEqual(decree, msg->m_decree);
    UT_AssertIsTrue(ballot == msg->m_ballot);
    return msg;
}

FetchRequest*
LegislatorTest::ReceiveFetchRequest(UInt16 messageId)
{
    return TestEngine::ReceiveFetchRequest(messageId);
}

Message*
LegislatorTest::SendAndReceive(Message *req, UInt16 messageId)
{
    SendRequest(req);
    return ReceiveResponse(messageId);
}

Message*
LegislatorTest::GenerateMessage(UInt16 msgId, UInt64 decree, BallotNumber ballot)
{
    Message *msg = NULL;
    switch (msgId)
    {
        case Message_Vote:
            msg = NewVote(m_self.m_version, m_self.m_memberId, decree, ballot);
            break;

        case Message_PrepareAccepted:
        {
            Ptr<Vote> vote = NewVote(m_self.m_version, m_self.m_memberId, decree,
                                     BallotNumber(ballot.m_ballotId -1, ballot.m_memberId));
            msg = new PrepareAccepted(m_self.m_version, m_self.m_memberId, decree,
                                      m_self.m_configNumber, ballot, vote);
            break;
        }
        
        case Message_StatusResponse:
            msg = new StatusResponse(m_self.m_version, m_self.m_memberId, decree,
                                     m_self.m_configNumber, ballot);
            break;

        case Message_Prepare:
            msg = new PrepareMsg(m_self.m_version, m_self.m_memberId, decree,
                                 m_self.m_configNumber, ballot, m_self.m_primaryCookie);
            break;
            
        case Message_VoteAccepted:
        case Message_NotAccepted:
        case Message_StatusQuery:
        case Message_FetchVotes:
        case Message_FetchCheckpoint:
            msg = new Message(m_self.m_version, msgId, m_self.m_memberId, decree,
                              m_self.m_configNumber, ballot);
            break;

        default:
            UT_AssertFail();
    }
    return msg;
}
    

void
LegislatorTest::WaitForState(UInt32 state, BallotNumber ballot,
                             UInt64 decree, BallotNumber maxBallot)
{
    TestEngine::WaitForState(state, ballot, decree, m_self.m_configNumber, maxBallot);
}

void
LegislatorTest::WaitForCheckpoint(UInt64 checkpoint)
{
    TestEngine::WaitForCheckpoint(checkpoint);
}

void
LegislatorTest::MoveToPrimary()
{
    TestEngine::MoveToPrimary();
}

void
LegislatorTest::MoveToSecondary()
{
    TestEngine::MoveToSecondary();
}

void
LegislatorTest::MoveToSecondaryPrepared()
{
    TestEngine::MoveToSecondaryPrepared();
}

void
LegislatorTest::MoveToPreparing()
{
    TestEngine::MoveToPreparing();
}

void
LegislatorTest::MoveToInitializing()
{
    TestEngine::MoveToInitializing();
}

void
LegislatorTest::MoveToInitializingPrepared()
{
    TestEngine::MoveToInitializingPrepared();
}  

void
LegislatorTest::MoveToState(UInt32 state)
{
    TestEngine::MoveToState(state);
}  

void
LegislatorTest::GetStatus(StatusResponse *status)
{
    TestEngine::GetStatus(status);
}

void
LegislatorTest::SendStatusResponse(UInt64 decree, BallotNumber ballot, UInt64 queryDecree,
                                   BallotNumber queryBallot, Int64 lastReceived)
{
    StatusResponse *msg;
    msg = (StatusResponse *) GenerateMessage(Message_StatusResponse, decree, ballot);
    
    msg->m_queryDecree = queryDecree;
    msg->m_queryBallot = queryBallot;

    if (lastReceived == -1)
    {
        msg->m_lastReceivedAgo = _I64_MAX;
    }
    
    SendResponse(msg);
    delete msg;
}

void
LegislatorTest::HandleLearnVotes(UInt64 reqDecree, BallotNumber reqBallot, UInt64 decree,
                                 BallotNumber ballot, bool waitForClose)
{
    UInt64 d = reqDecree;
    vector< pair<UInt64, BallotNumber> > v;
    while (d <= decree)
    {
        v.push_back(make_pair(d, reqBallot));
        d++;
    }
    if (reqBallot < ballot)
    {
        v.push_back(make_pair(decree, ballot));
    }
    HandleLearnVotes(reqDecree, reqBallot, v, waitForClose);
}

void
LegislatorTest::HandleLearnVotes(UInt64 reqDecree, BallotNumber reqBallot,
                                 vector< pair<UInt64, BallotNumber> > v, bool waitForClose)
{
    // wait for a fetch request
    FetchRequest *req = ReceiveFetchRequest(Message_FetchVotes);
    UT_AssertIsNotNull(req);
    Message *msg = req->m_message;
    UT_AssertAreEqual(reqDecree, msg->m_decree);
    UT_AssertIsTrue(reqBallot == msg->m_ballot);

    HandleLearnVotes(req, v, waitForClose);

    delete req;
}

void
LegislatorTest::HandleLearnVotes(FetchRequest *req, vector< pair<UInt64, BallotNumber> > v, bool waitForClose)
{

    vector<Message *> votes;
    for (size_t i = 0; i < v.size(); i++)
    {
        Vote* vote = VoteFactory::NewVote(m_self.m_version, m_self.m_memberId, v[i].first, m_self.m_configNumber, v[i].second);
        votes.push_back(vote);
    }
    TestEngine::HandleLearnVotes(req, votes, 0, waitForClose);
    for (size_t i = 0; i < votes.size(); i++)
    {
        delete votes[i];
    }
}

void
LegislatorTest::MsgCombinations(UInt16 msgId, UInt64 d, BallotNumber b)
{
    MsgCombinations(msgId, d, b, 0, d, b);
}

void
LegislatorTest::MsgCombinations(UInt16 msgId, UInt64 d, BallotNumber b,
                                        UInt16 rspId, UInt64 d1, BallotNumber b1)
{
    StatusResponse initStatus;
    GetStatus(&initStatus);

    for (int i = -1; i <= 1; i++)
    {
        for (int j = -1; j <= 1; j++)
        {
            UInt64 decree = d+i;
            BallotNumber ballot(b.m_ballotId+j, b.m_memberId);
            // Even if we are sending a response, Send it as a request
            // anyways. The replica should ignore the request if it
            // is supposed to be a response.
            SendRequest(msgId, decree, ballot);
            if (rspId != 0)
            {
                ReceiveResponse(rspId, d1, b1);
                
            }
            else
            {
                SendResponse(msgId, decree, ballot);
            }
        }
    }
    WaitForState(initStatus.m_state, initStatus.m_ballot, initStatus.m_decree, initStatus.m_maxBallot);
}

UInt32
LegislatorTest::Fetch(Message *msg, const char *file)
{
    return TestEngine::Fetch(msg, file);
}

void
LegislatorTest::AssertEmptyQueues()
{
    TestEngine::AssertEmptyQueues();
}

