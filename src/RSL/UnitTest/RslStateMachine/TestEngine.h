#pragma once
#include "legislator.h"
//#include "hirestime.h"
//#include "list.h"
//#include "limits.h"
#include <vector>
#include "rslconfig.h"
#include "Util.h"
#include "VoteFactory.h"
#include "TestStateMachine.h"

using namespace RSLibImpl;
using namespace std;

enum TestCorruptionEnum
{
    TestCorruption_None,
    TestCorruption_Incomplete,
    TestCorruption_Zero
};

class FetchRequest
{
    public:
    
    StreamSocket *m_sock;
    Message *m_message;
    
    Link<FetchRequest> link;
    
    FetchRequest(StreamSocket *sock, Message *msg) : m_sock(sock), m_message(msg) {}
    
    ~FetchRequest()
    {
        delete m_sock;
        delete m_message;
    }
};

typedef vector<MemberId> MemberIdList;

class FakeLegislator;
class FakeLegislatorCollection;

class TestEngine
{
public:
    static RSLConfig m_cfg;
    static RSLConfigParam m_cfgParam;
    static RSLNode m_testingNode;
    static UInt16 m_testingNodeClientPort;
    static TestStateMachine * m_machine;
    static StatusResponse m_status;
    static StatusResponse *m_externalStatus;
    static UInt32 m_maxSentBallotId;
    static RSLNode m_nodeTemplate;
    static RSLNodeCollection m_nodesInConfig;
private:
    static UInt64 m_membersIdCounter;

    static NetPacketSvc * m_netlibServer;
    static NetPacketSvc * m_netlibClient;
    static std::unique_ptr<StreamSocket> m_pFetchSocket;
    static volatile bool m_sendOutstanding;

    static CRITSEC m_lock;
    static HANDLE m_netlibSendHandle;
    static HANDLE m_netlibRecvHandle;
    static HANDLE m_fetchReqHandle;

    static Queue<Message> m_reqRecvQ;
    static Queue<Message> m_respRecvQ;
    static Queue<FetchRequest> m_fetchRecvQ;
    static Message * m_lReq;
    static Message * m_lResp;

    TestEngine();

    class NetHandler;

public:
    // states that legislator can be in. This is a superset of
    // Legislator::State. This state distinguishes between
    // primary and secondary, while the legislator::State doesn't.
    // xxxPrepared states are states where
    // m_maxBallot > m_maxAcceptedVote->m_maxBallot; i.e the replica
    // has gotten a prepare message.
    enum State
    {
        PaxosInactive,
        Primary,
        Secondary,
        SecondaryPrepared,
        Prepare,
        Initialize,
        InitializePrepared,
        State_Count
    };

    static void Init(RSLProtocolVersion version, UInt32 numReplicas,
                     UInt64 testingReplicaMemberId, UInt32 numReaderThreads);
    
    // Return nodes in current configuration
    static void GetNodesInCofiguration(RSLNodeCollection &list);
    static void UpdateNodesInConfiguration(const RSLNodeCollection &list);
    static void UpdateMajority(FakeLegislatorCollection &legislators, RSLNodeCollection &newNodes, Vote * lastVote=NULL);
    static bool ContainsNode(RSLNodeCollection &nodes, const char* memberId);
    static Legislator * GetLegislator(RSLStateMachine * stateMachine=NULL);
    static void GetConfigurationInfo(RSLStateMachine * stateMachine, RSLNodeCollection * nodeArray,
        UInt32 * configNumber, UInt32 * defunctConfigNumber);
    static void CheckConfiguration(RSLNodeCollection &expectedNodeArray, UInt32 expectedConfigNumber=0,
        UInt32 expectedHighestDefunct=0);
    static void CompareNodeArrays(RSLNodeCollection &expectedNodeArray, RSLNodeCollection &actualNodeArray);

    // Return a RSLNode populated with m_fakeNodeTemplate
    // if memberId is non-zero, it populates node's m_memberId
    // otherwise, choose an brand new memberId out of the current configuration
    static RSLNode GetFakeNode(UInt64 id=0);

    static MemberId TestingNodeMemberId();

    static StatusResponse * GetStatus(StatusResponse * status);
    static void SetExternalStatusUpdate(StatusResponse *externalStatus);

    static void GetSyncedFakeLegislatorsMajority(FakeLegislatorCollection &list, StatusResponse * status=NULL, bool all=false);

    static void ApplyConfigurationChangedResults(StatusResponse * status);

    static void WaitForCheckpoint(UInt64 checkpoint);
    static void WaitForState(UInt32 state, BallotNumber ballot, UInt64 decree,
        UInt32 configurationNumber, BallotNumber maxBallot);
    static void WaitForState(UInt32 state, StatusResponse &status);

    // Checks that the request and response queues are empty.
    static void AssertEmptyQueues();

    static UInt16 GetGPort();
    static void GenerateConfigFile(UInt32 numReplicas, UInt64 id,
                                   UInt32 numReaderThreads, RSLConfigParam &cfg, RSLNodeCollection &replicas);
    static void GenerateConfigFile(RSLNodeCollection &nodes, UInt32 numReaderThreads, RSLConfigParam &cfg);
private:

    //------------------------------------------------------------------------
    // Move to methods
    //------------------------------------------------------------------------

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
    
public:

    enum ReconfigStage {
        BeforeSendReconfigVote,
        BeforeSendDecision,
        BeforeReceiveStatusQuery,
        BeforeReceiveJoinMessages,
        BeforeSendStatusResponse,
        BeforePrepareAgain,
        BeforeSendLastVote
    };

    static void MoveToState(UInt32 state);
    static void MoveToPrimary(bool waitForPrimaryCallback=false);
    static void MoveToSecondary();
    static void MoveToSecondaryPrepared();
    static void MoveToPreparing();
    static void MoveToInitializing();
    static void MoveToInitializingPrepared();
    static void EnsureVersion(RSLProtocolVersion version);
    static void MoveToDefaultConfig();
    static void MoveToDefaultConfigUpTo(ReconfigStage stage, FakeLegislatorCollection &legislators);
    static void MoveToPaxosInactive();
    static void IncrementHighestDefunctConfiguration(UInt32 increment);
    static void MoveToConfiguration(RSLNodeCollection &nodes);

private:
    // Internal methods to move the replica between two specific states.
    static void PreparingToPrimary();
    static void StableToSecondary();
    static void PreparingToSecondary();
    static void InitializingToSecondary();
    static void InitializingToPreparing();

    //------------------------------------------------------------------------
    // Communication
    //------------------------------------------------------------------------

    // The following 3 methods are for sending mesages to the replica.
    // If asClient is true, message/pkt is sent to the replica's server port,
    // otherwise it is sent to the client port. If port is not specified,
    // message is sent to m_legislator's port. To send the message to another
    // legislator, specify the port of the legislator.
public:
    static void SendMessage(Message *msg, bool asClient, UInt16 port=0);
    static void SendBuffer(void *buf, UInt32 length, bool asClient);
private:
    static void SendPacket(Packet *pkt, bool asClient, UInt16 port=0);
    // Wrappers around SendMessage() API. 
public:
    static void SendRequest(Message * msg);
    static void SendResponse(Message * msg);

    static Message * SendAndReceive(Message * msg, UInt16 msgId);

    // Waits for a response from the legislator. Asserts if the response
    // doesn't arrive with m_cfg.ReceiveTimeout. If it receives the message,
    // it asserts that its m_msgId == messageId. If verify is true, calls
    // VerifyMessage(). The message is saved in m_lResp. Caller should NOT
    // delete the message.
private:
    static Message * ReceiveMessage(UInt16 msgId, Queue<Message> * queue, Message ** lastMsg, bool verify);
    // Wrapper around the above ReceiveMessage()
public:
    static void VerifyMessage(Message *msg, int msgId);
    static Message * ReceiveResponse(UInt16 msgId, bool verify=true);
    static Message * ReceiveRequests(size_t numberOfRequests, UInt16 msgId, bool verify=true);
    static Message * ReceiveRequests(UInt16 msgId, bool verify=true);

private:
    static void ProcessSend(Packet *packet, TxRxStatus status, bool asClient);
    static void ProcessReceive(Packet *pkt, bool asClient);
    static Message * UnMarshalMessage(IMarshalMemoryManager *memory);

public:
    static void HandleLearnVotes(vector<Message *> log, int idx, bool waitForClose=false);
    static void HandleLearnVotes(FetchRequest *req, vector<Message *> log, int idx, bool waitForClose=false);
    static void HandleLearnVotes(vector<Message *> log, bool waitForClose=false);
    static FetchRequest * ReceiveFetchRequest(UInt16 messageId);
    static UInt32 Fetch(Message *msg, const char* file);
    static unsigned int __stdcall HandleFetchRequest(void *arg);
    static UInt32 MaxSentBallotId() { return m_maxSentBallotId; }
};


//
// Represents a fake legislator
//
class FakeLegislator
{
public:
    PrimaryCookie * m_primaryCookie;
    vector<Message*> m_log;

    MemberId m_memberId;
    RSLProtocolVersion m_version;
    UINT32 m_configNumber;
    BallotNumber m_ballot;
    UINT64 m_decree;
    BallotNumber m_maxBallot;
    Vote * m_lastVote;

    FakeLegislator(const char* memberId);
    FakeLegislator(MemberId memberId=MemberId());
    FakeLegislator(const FakeLegislator &other);
    FakeLegislator & operator=(const FakeLegislator &other);
    void Init(MemberId memberId);
    ~FakeLegislator();
    Message * CreateCopy(Message * msg);

    //-------------------------------------------------------------------------
    // Ballot, Decree and ConfigurationNumber
    //-------------------------------------------------------------------------
public:
    void SetLastVote(Vote * vote);
    Vote * GetCopyLastVote();
    void Reset(StatusResponse &status);
    void CopyFrom(const FakeLegislator &fake);

    void SetBallotId(UInt32 ballotId);
    void IncrementBallot(UInt32 inc=1);
    void GetStatus(StatusResponse *status);

    void AcceptPrepare(Message * msg);
    void AcceptVote(Vote * vote);
    void AcceptReconfigurationDecision(Message * msg);

    //Vote * GetLastVote();
    void AppendLog(Message * msg);
    void ClearLogs();

    //-------------------------------------------------------------------------
    // Message Construction
    //-------------------------------------------------------------------------
public:
    // Prepapre
    PrepareMsg * NewPrepare();
    PrepareAccepted * NewPrepareAccepted();
    // Vote
    Vote * NewVote();
    Message * NewVoteAccepted();
    Message * NewNotAccepted();
    // Learning
    Message * NewFetchVotes();
    Message * NewFetchCheckpoint(UInt64 checkpointedDecree);
    // Reconfiguration
    Vote * NewVote(RSLNodeCollection &nodes, void * reconfigCookie);
    Message * NewReconfigurationDecision();
    Message * NewDefunctConfiguration(UInt32 highestDefunctConfigurationNumber);
    JoinMessage * NewJoin(UInt32 configNumber, UInt64 minDecreeInLog, UInt64 checkpointedDecree);
    JoinMessage * NewJoin();
    Message * NewJoinRequest();
    // Status
    Message * NewStatusQuery();
    StatusResponse * NewStatusResponse(UInt64 queryDecree, BallotNumber queryBallot, Int64 lastReceived);

    //-------------------------------------------------------------------------
    // Send and Receive
    //-------------------------------------------------------------------------
public:
    // Requests
    Message * SendPrepare(FakeLegislatorCollection * targets=NULL, bool sendToReplica=true);
    Vote * SendVote(FakeLegislatorCollection * targets=NULL, bool sendToReplica=true);
    Vote * SendLastVote(FakeLegislatorCollection * targets=NULL, bool sendToReplica=true);
    Vote * SendChangeConfig(RSLNodeCollection &nodes, void * reconfigCookie,
        FakeLegislatorCollection * targets=NULL, bool sendToReplica=true);
    void SendStatusQuery();
    void SendFetchVotes();
    void SendFetchCheckpoint(UInt64 checkpointedDecree);
    Message * SendReconfigurationDecision(FakeLegislatorCollection * targets=NULL, bool sendToReplica=true);
    void SendDefunctConfiguration(UInt32 highestDefunctConfigurationNumber);
    void SendJoin();
    void SendJoin(UInt32 configNumber, UInt64 minDecreeInLog, UInt64 checkpointedDecree);
    void SendJoinRequest();

    // Responses
    void SendPrepareAccepted();
    void SendPrepareNotAccepted();
    void SendVoteAccepted();
    void SendVoteNotAccepted();
    void SendStatusResponse(UInt64 queryDecree, BallotNumber queryBallot, Int64 lastReceived);
    void SendLog();
};

class FakeLegislatorCollection
{
private:
    vector<FakeLegislator *> m_legislators;
    // Managed fake legislators (will be automatically
    // deleted when this object is deleted)
    class Link
    {
    public:
        FakeLegislator m_leg;
        Link * m_next;
        Link() { m_next = NULL; }
    };
    Link * m_head;

public:
    FakeLegislatorCollection(FakeLegislator * first, ...);
    FakeLegislatorCollection();
    ~FakeLegislatorCollection();

    FakeLegislator * getByMemberId(MemberId memberId);

    FakeLegislator * operator[] (size_t idx);
    void append(FakeLegislator & fake);
    void clear();
    size_t size();
};
