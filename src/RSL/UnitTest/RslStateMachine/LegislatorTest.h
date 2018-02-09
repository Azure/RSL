#pragma once
#include "TestEngine.h"

class LegislatorTest
{
public:

    // states that legislator can be in. This is a superset of
    // Legislator::State. This state distinguishes between
    // primary and secondary, while the legislator::State doesn't.
    // xxxPrepared states are states where
    // m_maxBallot > m_maxAcceptedVote->m_maxBallot; i.e the replica
    // has gotten a prepare message.
    enum State
    {
        Primary             = TestEngine::Primary,
        Secondary           = TestEngine::Secondary,
        SecondaryPrepared   = TestEngine::SecondaryPrepared,
        Prepare             = TestEngine::Prepare,
        Initialize          = TestEngine::Initialize,
        InitializePrepared  = TestEngine::InitializePrepared,
        State_Count         = TestEngine::State_Count
    };

    static const int c_BufSize = VoteFactory::c_BufSize;
    static const UInt32 s_MaxVoteLen = VoteFactory::s_MaxVoteLen;
    static const UInt32 s_MaxCacheLength = VoteFactory::s_MaxCacheLength;

    LegislatorTest();

    // This must be caled before calling any methods. (except GenerateConfig).
    void Init();

    // The following 2 methods are for sending mesages to the replica.
    // If asClient is true, message/pkt is sent to the replica's server port,
    // otherwise it is sent to the client port. If port is not specified,
    // message is sent to m_legislator's port. To send the message to another
    // legislator, specify the port of the legislator.
    void SendMessage(Message *msg, bool asClient, UInt16 port = 0);
    void SendBuffer(void *buf, UInt32 length, bool asClient);

    // Wrappers around SendMessage() API. 
    void SendRequest(Message *msg);
    // Constructs the appropriate message with the decree and ballot specified
    // and sends the request.
    void SendRequest(UInt16 msgId, UInt64 decree, BallotNumber ballot);

    // Wrappers around SendMessage() API
    void SendResponse(Message *msg);
    // Constructs the appropriate message with the decree and ballot specified
    // and sends the response.
    void SendResponse(UInt16 msgId, UInt64 decree, BallotNumber ballot);

    UInt32 MaxSentBallotId();
    // Waits for a response from the legislator. Asserts if the response
    // doesn't arrive with m_cfg.ReceiveTimeout. If it receives the message,
    // it asserts that its m_msgId == messageId. If verify is true, calls
    // VerifyMessage(). The message is saved in m_lResp. Caller should NOT
    // delete the message.
    Message* ReceiveResponse(UInt16 messageId, bool verify=true);
    // Wrapper around the above ReceiveResponse(). Also verifies the decree and ballot
    Message* ReceiveResponse(UInt16 msgId, UInt64 decree, BallotNumber ballot);

    // Analogous to ReceiveResponse(), except that it checks the request.
    Message* ReceiveRequest(UInt16 messageId, bool verify=true);
    Message* ReceiveRequest(UInt16 messageId, UInt64 decree, BallotNumber ballot);

    // Waits for a fetch request from the replica (maximum wait time is m_cfg.ReceiveTimeout).
    FetchRequest* ReceiveFetchRequest(UInt16 msgId);

    // Wrapper around SendRequest() and ReceiveResponse(). Caller should NOT
    // delete the message returned (also saved as m_lResp).
    Message* SendAndReceive(Message *req, UInt16 messageId);

    // Returns a message object. The message object type is based on msgId.
    // So, if msgId == Message_Vote, a Vote* is returned.
    Message* GenerateMessage(UInt16 msgId, UInt64 decree, BallotNumber ballot);

    // Waits until the statusresponse returned by the replica has
    // the following properties.
    void WaitForState(UInt32 state, BallotNumber ballot, UInt64 decree, BallotNumber maxBallot);
    void WaitForCheckpoint(UInt64 checkpoint);
    
    void GetStatus(StatusResponse *status);

    // constructs a status response and sends it to the replica.
    void SendStatusResponse(UInt64 decree, BallotNumber ballot, UInt64 queryDecree,
                            BallotNumber queryBallot, Int64 lastReceived);

    // dequeues the next fetch request from the queue and verifies that the
    // request's decree and ballot match. Sends all votes between
    // reqDecree and decree.
    void HandleLearnVotes(UInt64 reqDecree, BallotNumber reqBallot, UInt64 decree, BallotNumber ballot,
                          bool waitForClose = false);

    void HandleLearnVotes(UInt64 reqDecree, BallotNumber reqBallot,
                          vector< pair<UInt64, BallotNumber> > v, bool waitForClose=false);
    
    void HandleLearnVotes(FetchRequest *req, vector< pair<UInt64, BallotNumber> > v, bool waitForClose=false);
    // Returns a ballotNumber which has a lower ballotId than b and memberId == m_memberID
    BallotNumber LowerBallot(BallotNumber b);
    
    // Returns a ballotNumber which has higher ballotId than b and memberId == m_memberID
    BallotNumber HighBallot(BallotNumber b);

    // Sends possible combinations of responses with decree in [d-1,d+1] and
    // b in [b.ballotId-1, b.ballotID+1]. 
    void MsgCombinations(UInt16 msgId, UInt64 d, BallotNumber b);
    // Sends possible combinations of requests with decree in [d-1,d+1] and
    // b in [b.ballotId-1, b.ballotID+1]. Expects the response for every
    // request to have rspId, decree == d1 and ballot == b1
    void MsgCombinations(UInt16 msgId, UInt64 d, BallotNumber b, UInt16 rspId, UInt64 d1, BallotNumber b1);

    // Sends the fetch message to the replica and saves the response to the file.
    // Returns the size of the saved file.
    UInt32 Fetch(Message *msg, const char *file);

    // Checks that the request and response queues are empty.
    void AssertEmptyQueues();

    // Moves the replica to the appropriate state. After this method returns,
    // the latest state of the replica is aved in m_state. If the replica
    // sends any requests/responses while changing states, those are
    // saved in m_lReq and m_lResp.
    // When this method returns, the replica is in the appropriate state,
    // waiting for a request/response. For example, in the primary state,
    // it has reproposed the last vote and it waiting for the response.
    // Similarly in Initializing, it has sent the StatusQuery and it
    // waiting for a response.
    // When the replica is moved to a XXXprepared state, the difference
    // between m_maxBallot.m_ballotId and m_maxAcceptedVote->m_ballot.m_ballotId
    // is 10. This way, test cases can send ballots between maxBallot
    // and maxAcceptedVote->m_ballot
    
    void MoveToState(UInt32 state);
    void MoveToPrimary();
    void MoveToSecondary();
    void MoveToSecondaryPrepared();
    void MoveToPreparing();
    void MoveToInitializing();
    void MoveToInitializingPrepared();

    static int NumReqs(UInt64 decree);
    static int ReqLen(UInt64 decree);
    static char* ReqBuf(UInt64 decree);
    Vote* NewVote(RSLProtocolVersion version, MemberId memberId, UInt64 decree, BallotNumber ballot,
                         int numReqs, int len, void *cookie);
    Vote* NewVote(RSLProtocolVersion version, MemberId memberId, UInt64 decree, BallotNumber ballot);
    Vote* NewVote(RSLProtocolVersion version, MemberId memberId, UInt64 decree,
                         BallotNumber ballot, PrimaryCookie *primaryCookie);

    static UInt32 NumFastReads();

    void GenerateConfigFile(UInt32 numReplicas, RSLConfigParam &cfg, RSLNodeCollection &nodes, UInt32 numReaderThreads=0);
    
    FakeLegislator m_self;
    TestStateMachine * m_machine;
    RSLProtocolVersion m_version;
    MemberId m_memberId;
    UInt32 m_ip;
    UInt16 m_port;
    UInt16 m_legislatorPort;
    MemberId m_legislatorMemberId;

    StatusResponse m_status;

    void TestAsyncFileRead();
    // Following are the test cases.
    void TestInvalidMessages();
    void TestNotAcceptedMsg(UInt32 state);
    void TestNotAcceptedMsgInInitialize(UInt32 state);
    void TestVoteMsg(UInt32 state);
    void TestVoteMsgInPrepared(UInt32 state);
    void TestVoteMsgInInitialize(UInt32 state);
    void TestVoteAcceptedMsg(UInt32 state);
    void TestVoteAcceptedMsgInPrimary();
    void TestPrepareMsg(UInt32 state);
    void TestPrepareMsgInInitialize(UInt32 state);
    void TestPrepareAcceptedMsg(UInt32 state);
    void TestStatusResponseMsg(UInt32 state);
    void TestStatusResponseMsgInInitialize(UInt32 state);
    void TestExecutionQueueOnDisk();
    void TestCanBecomePrimary();
    void TestAcceptMessageFromReplica();
    void TestReadNextMessage();
    void TestFastRead(UInt32 state);
    void TestRestoreFromCheckpoint();
    void TestRestore(TestCorruptionEnum test = TestCorruption_None);
    void TestSaveCheckpointAtRestore();
    void TestRestoreFromLoadState();
    void TestCheckpointing();
    void TestCheckpointingInPrimary();
    void TestCopyCheckpoint();
    void TestReplay();
    void TestSingleReplica();
};
