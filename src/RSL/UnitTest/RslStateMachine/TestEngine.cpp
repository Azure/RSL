#include "TestEngine.h"
#include "apdiskio.h"
#include "rsldebug.h"
#include <process.h>
#include <strsafe.h>

UInt16 g_port = 20000;

class TestEngine::NetHandler : public SendHandler, public ReceiveHandler
{
private:
    bool m_asClient;

public:
    NetHandler(bool asClient) : m_asClient(asClient) {}
    
    void ProcessSend(Packet *packet, TxRxStatus status)
    {
        TestEngine::ProcessSend(packet, status, m_asClient);
    }
    
    void ProcessReceive(Packet *packet)
    {
        TestEngine::ProcessReceive(packet, m_asClient);
    }
};

RSLConfig TestEngine::m_cfg;
RSLConfigParam TestEngine::m_cfgParam;

RSLNode TestEngine::m_testingNode;
UInt16 TestEngine::m_testingNodeClientPort = 0;
TestStateMachine * TestEngine::m_machine = NULL;
StatusResponse TestEngine::m_status = StatusResponse(RSLProtocolVersion_1, MemberId(), 0, 1, BallotNumber());
StatusResponse* TestEngine::m_externalStatus = NULL;
UInt32 TestEngine::m_maxSentBallotId = 0;
RSLNode TestEngine::m_nodeTemplate;
RSLNodeCollection TestEngine::m_nodesInConfig;
UInt64 TestEngine::m_membersIdCounter = 0;

NetPacketSvc * TestEngine::m_netlibServer = NULL;
NetPacketSvc * TestEngine::m_netlibClient = NULL;
std::unique_ptr<StreamSocket> TestEngine::m_pFetchSocket(StreamSocket::CreateStreamSocket()); // TBD: static init will not allow SSL as ssl init happens after the module load
volatile bool TestEngine::m_sendOutstanding = false;

CRITSEC TestEngine::m_lock;
HANDLE TestEngine::m_netlibSendHandle;
HANDLE TestEngine::m_netlibRecvHandle;
HANDLE TestEngine::m_fetchReqHandle;

Queue<Message> TestEngine::m_reqRecvQ;
Queue<Message> TestEngine::m_respRecvQ;
Queue<FetchRequest> TestEngine::m_fetchRecvQ;
Message * TestEngine::m_lReq = NULL;
Message * TestEngine::m_lResp = NULL;

// Private constructor
TestEngine::TestEngine()
{}

void
TestEngine::Init(RSLProtocolVersion version, UInt32 numReplicas,
                 UInt64 testingReplicaMemberId, UInt32 numReaderThreads)
{
    // Vote Factory
    VoteFactory::Init();

    // Initialize the default config param
    m_cfgParam.m_newLeaderGracePeriodSec=60;
    m_cfgParam.m_heartBeatIntervalSec=60;
    m_cfgParam.m_electionDelaySec=60;
    m_cfgParam.m_maxElectionRandomizeSec=1;
    m_cfgParam.m_initializeRetryIntervalSec=60;
    m_cfgParam.m_prepareRetryIntervalSec=60;
    m_cfgParam.m_voteRetryIntervalSec=60;
    m_cfgParam.m_cPQueryRetryIntervalSec=5;
    m_cfgParam.m_maxCheckpointIntervalSec=0;
    m_cfgParam.m_joinMessagesIntervalSec=60;
    m_cfgParam.m_maxLogLenMB=2;
    m_cfgParam.m_sendTimeoutSec=300;
    m_cfgParam.m_receiveTimeoutSec=300;
    m_cfgParam.m_maxCacheLengthMB=50;
    m_cfgParam.m_maxVotesInLog=1000;
    m_cfgParam.m_maxOutstandingPerReplica=10;
    m_cfgParam.m_maxCheckpoints=2;
    m_cfgParam.m_maxLogs=100;
    m_cfgParam.m_logLenRandomize=20;
    m_cfgParam.m_electionRandomize=10;
    m_cfgParam.m_fastReadSeqThreshold=5;
    m_cfgParam.m_inMemoryExecutionQueueSizeMB=10;
    UT_AssertIsTrue(SUCCEEDED(StringCchCopyA(m_cfgParam.m_workingDir, sizeof(m_cfgParam.m_workingDir), ".\\rsltest")));
    m_cfgParam.m_addMemberIdToWorkingDir = true;

    DynString cmd;
    cmd.AppendF("rmdir /s /Q %s 2>NUL", m_cfgParam.m_workingDir);
    system(cmd);
    UT_AssertIsTrue(CreateDirectoryA(m_cfgParam.m_workingDir, NULL));

    // Generate config file
    RSLNodeCollection nodes;
    RSLConfigParam cfgParam;
    GenerateConfigFile(numReplicas, testingReplicaMemberId, numReaderThreads, cfgParam, nodes);

    m_cfg.Reload(&cfgParam);
    UT_AssertAreEqual(numReplicas, nodes.Count());

    // Populate data for replica set
    int testEnginePort = -1;
    for (size_t i=0; i < nodes.Count(); i++)
    {
        // Sets all replicas in current configuration
        m_nodesInConfig.Append(nodes[i]);
       
        // Real replica
        if (nodes[i].GetMemberIdAsUInt64() == testingReplicaMemberId)
        {
            m_testingNode = nodes[i];
        }
        else 
        {
            if (testEnginePort < 0)
            {
                testEnginePort = nodes[i].m_rslPort;
            }
            UT_AssertAreEqual(testEnginePort, nodes[i].m_rslPort);
        }
    }
    // Copy values from real replica
    m_nodeTemplate = m_testingNode;
    m_nodeTemplate.SetMemberIdAsUInt64(0);
    m_nodeTemplate.m_rslPort = (UInt16) testEnginePort;
    m_nodeTemplate.m_rslLearnPort = (UInt16) testEnginePort+1;

    // Init Network listeners
    // Server
    PacketFactory *factory = new PacketFactory();
    NetHandler *handler = new NetHandler(false);
    m_netlibServer = new NetPacketSvc(32*1024);
    int ret = m_netlibServer->StartAsServer((UInt16) testEnginePort, handler, handler, factory);
    UT_AssertAreEqual(0, ret);
    // Client
    handler = new NetHandler(true);
    m_netlibClient = new NetPacketSvc(32*1024);
    ret = m_netlibClient->StartAsClient(handler, handler, factory);
    UT_AssertAreEqual(0, ret);
    // Learning socket
    UT_AssertAreEqual(NO_ERROR, m_pFetchSocket->BindAndListen((UInt16) testEnginePort+1, 1000, 120, 1));
    // Socket thread
    uintptr_t h = _beginthreadex(NULL, 0, HandleFetchRequest, NULL, NULL, NULL);
    LogAssert(h != 0);

    // Init Testing Replica
    m_machine = new TestStateMachine(version);
    RSLNodeCollection replicaCfgs(nodes);

    // Set all the IPs to 0. This will test the ip lookup code
    for (size_t i = 0; i < replicaCfgs.Count(); i++)
    {
        replicaCfgs[i].m_ip = 0;
    }    

    UT_AssertIsTrue(m_machine->Initialize(&cfgParam, nodes, m_testingNode, version, true));
    //// Bootstrap replica
    //MemberSet memberSet(nodes, NULL, 0);
    //BootstrapMsg msg(version, testingReplicaMemberId, memberSet);
    //SendRequest(&msg);

    // wait until you recieve the status query. This will also set
    // the port that RSL is using for client requests.
    ReceiveRequests(Message_StatusQuery);
}

UInt16
TestEngine::GetGPort()
{
    return g_port;
}

void
TestEngine::GenerateConfigFile(UInt32 numReplicas, UInt64 testingReplicaMemberId,
                               UInt32 numReaderThreads, RSLConfigParam &cfg, RSLNodeCollection &nodes)
{
    nodes.Clear();
    for (UInt32 i=1; i <= numReplicas; i++)
    {
        RSLNode node;
        LogAssert(SUCCEEDED(StringCchCopyA(node.m_hostName, sizeof(node.m_hostName), "127.0.0.1")));
        node.m_ip = inet_addr(node.m_hostName);
        node.SetMemberIdAsUInt64(i);
        node.m_rslPort = (node.GetMemberIdAsUInt64() == testingReplicaMemberId) ? g_port : g_port+10;
        nodes.Append(node);
    }
    GenerateConfigFile(nodes, numReaderThreads, cfg);
}

void
TestEngine::GenerateConfigFile(RSLNodeCollection &nodes, UInt32 numReaderThreads, RSLConfigParam &cfg)
{
    m_cfgParam.m_numReaderThreads = numReaderThreads;

    // Make a unique directory for this instance of legislator.
    DynString dir(m_cfgParam.m_workingDir);
    DynString originalDir(dir);

    dir.AppendF("\\%d", (int) g_port);
    StringCchCopyA(m_cfgParam.m_workingDir, sizeof(m_cfgParam.m_workingDir), dir.Str());
    
    DynString cmd;
    cmd.AppendF("rmdir /s /Q %s 2>NUL", dir.Str());
    system(cmd);
    UT_AssertIsTrue(CreateDirectoryA(dir, NULL));

    cfg = m_cfgParam;
    StringCchCopyA(m_cfgParam.m_workingDir, sizeof(m_cfgParam.m_workingDir), originalDir.Str());
    
    for (size_t i = 0; i < nodes.Count(); i++)
    {
        DynString dest;
        dest.AppendF("%s\\%s\\", dir.Str(), nodes[i].m_memberIdString);
        UT_AssertIsTrue(CreateDirectoryA(dest, NULL));

        // Update m_membersIdCounter
        m_membersIdCounter = max(m_membersIdCounter, nodes[i].GetMemberIdAsUInt64() + 1);

    }
    // Update g_port
    g_port += 20;
}

void
TestEngine::GetNodesInCofiguration(RSLNodeCollection &list)
{
    list.Clear();
    for (size_t i=0; i < m_nodesInConfig.Count(); i++)
    {
        list.Append(m_nodesInConfig[i]);
    }
}

void
TestEngine::UpdateNodesInConfiguration(const RSLNodeCollection &list)
{
    m_nodesInConfig.Clear();
    for (size_t i = 0; i < list.Count(); i++)
    {
        if (MemberId::Compare(list[i].m_memberIdString, m_testingNode.m_memberIdString) == 0)
        {
            m_nodesInConfig.Append(m_testingNode);
        }
        else
        {
            m_nodesInConfig.Append(list[i]);
        }
    }
}

void
TestEngine::UpdateMajority(FakeLegislatorCollection &legislators, RSLNodeCollection &newNodes, Vote * lastVote)
{
    // Update known configuration
    UpdateNodesInConfiguration(newNodes);

    // Update the majority
    if (legislators.size() > 0)
    {
        // Make all new fake replicas know about past logged messages
        FakeLegislator fakeLegCopy = *legislators[0];
        GetSyncedFakeLegislatorsMajority(legislators);
        CallForAll(&legislators, CopyFrom(fakeLegCopy));
    }
    else
    {
        // Just update the majority
        GetSyncedFakeLegislatorsMajority(legislators);
        if (lastVote != NULL)
        {
            CallForAll(&legislators, AppendLog(lastVote));
        }
    }
}

bool
TestEngine::ContainsNode(RSLNodeCollection &nodes, const char* memberId)
{
    for (size_t i = 0; i < nodes.Count(); i++)
    {
        if (MemberId::Compare(memberId, nodes[i].m_memberIdString) == 0)
        {
            return true;
        }
    }
    return false;
}


Legislator *
TestEngine::GetLegislator(RSLStateMachine * stateMachine)
{
    if (stateMachine == NULL)
    {
        stateMachine = m_machine;
    }
    UT_AssertIsNotNull(stateMachine);
    UT_AssertIsNotNull(stateMachine->m_legislator);
    return stateMachine->m_legislator;
}

void 
TestEngine::GetConfigurationInfo(RSLStateMachine * stateMachine, RSLNodeCollection * nodeArray,
                                 UInt32 * configNumber, UInt32 * defunctConfigNumber)
{
    Legislator * legislator = GetLegislator(stateMachine);

    Ptr<MemberSet> memberSet = new MemberSet();
    UInt32 actualConfigNumber;
    legislator->GetConfiguration(memberSet, &actualConfigNumber);
    const RSLNodeCollection& actualNodeArray = memberSet->GetMemberCollection();

    for (size_t i = 0; i < actualNodeArray.Count(); i++)
    {
        RSLNode * node = &((actualNodeArray)[i]);
        nodeArray->Append(*node);
    }
    if (configNumber != NULL)
    {
        *configNumber = actualConfigNumber;
    }
    if (defunctConfigNumber != NULL)
    {
        *defunctConfigNumber = legislator->GetHighestDefunctConfigurationNumber();
    }
}

void
TestEngine::CheckConfiguration(RSLNodeCollection &expectedNodeArray, UInt32 expectedConfigNumber,
                               UInt32 expectedHighestDefunct)
{
    AssertEmptyQueues();

    // Gather current data
    RSLNodeCollection actualNodeArray;
    UInt32 actualConfigNumber;
    UInt32 actualHighestDefunct;
    GetConfigurationInfo(NULL, &actualNodeArray, &actualConfigNumber, &actualHighestDefunct);

    // Default values
    if (expectedConfigNumber == 0)
    {
        expectedConfigNumber = actualConfigNumber;
        expectedHighestDefunct = actualHighestDefunct;
    }
    else if (expectedHighestDefunct == 0)
    {
        expectedHighestDefunct = expectedConfigNumber-1;
    }

    UT_AssertAreEqual(expectedConfigNumber, actualConfigNumber);
    UT_AssertAreEqual(expectedHighestDefunct, actualHighestDefunct);
    CompareNodeArrays(expectedNodeArray, actualNodeArray);
}

void
TestEngine::CompareNodeArrays(RSLNodeCollection &expectedNodeArray, RSLNodeCollection &actualNodeArray)
{
    UT_AssertIsTrue(expectedNodeArray.Count() == actualNodeArray.Count());
    for (size_t i = 0; i < expectedNodeArray.Count(); i++)
    {
        RSLNode * expectedNode = &expectedNodeArray[i];
        bool found = false;
        for (size_t j = 0; !found && j < actualNodeArray.Count(); j++)
        {
            if (MemberId::Compare(expectedNode->m_memberIdString, actualNodeArray[j].m_memberIdString) == 0)
            {
                RSLNode * actualNode = &actualNodeArray[j];
                UT_AssertIsTrue(expectedNode->m_rslPort == actualNode->m_rslPort);
                found = true;
            }
        }
        UT_AssertIsTrue(found);
    }
}

RSLNode
TestEngine::GetFakeNode(UInt64 id)
{
    RSLNode node = m_nodeTemplate;
    if (id == 0)
    {
        id = m_membersIdCounter++;
    }
    node.SetMemberIdAsUInt64(id);
    return node;
}

StatusResponse *
TestEngine::GetStatus(StatusResponse * status)
{
    RSLProtocolVersion version = (RSLProtocolVersion) m_status.m_version;
    UInt64 decree = rand();
    UInt32 configNumber;
    Ptr<MemberSet> memberSet = new MemberSet();
    m_machine->m_legislator->GetPaxosConfiguration(memberSet, &configNumber);
    BallotNumber ballot = BallotNumber();

    Message req(version, Message_StatusQuery, MemberId(), decree, configNumber, ballot);
    StatusResponse * resp = (StatusResponse *) SendAndReceive(&req, Message_StatusResponse);

    // Make sure response matches request
    UT_AssertIsTrue(resp->m_queryBallot == req.m_ballot);
    UT_AssertAreEqual(resp->m_queryDecree, req.m_decree);
    // Compare last status
    UT_AssertIsTrue(resp->m_decree >= m_status.m_decree);
    if (resp->m_configurationNumber == m_status.m_configurationNumber)
    {
        UT_AssertIsTrue(resp->m_ballot >= m_status.m_ballot);
    }
    UT_AssertIsTrue(resp->m_configurationNumber >= m_status.m_configurationNumber);
    UT_AssertIsTrue(resp->m_maxBallot >= m_status.m_maxBallot);

    if (status != NULL)
    {
        *status = *resp;
    }
    if (m_externalStatus != NULL && status == &m_status)
    {
        *m_externalStatus = m_status;
    }
    return status;
}

void
TestEngine::SetExternalStatusUpdate(StatusResponse *externalStatus)
{
    m_externalStatus = externalStatus;
}

void
TestEngine::GetSyncedFakeLegislatorsMajority(FakeLegislatorCollection &list, StatusResponse * status, bool all)
{
    StatusResponse localStatus;
    GetStatus(&localStatus);
    if (status != NULL)
    {
        *status = localStatus;
    }

    // Check whether replica is in current configuration
    bool replicaInCurrentConfig = false;
    for (size_t i=0; i < m_nodesInConfig.Count() && !replicaInCurrentConfig; i++)
    {
        replicaInCurrentConfig = (MemberId::Compare(m_nodesInConfig[i].m_memberIdString, m_testingNode.m_memberIdString) == 0);
    }

    // Calc majority
    size_t majoritySize = all ? m_nodesInConfig.Count() : CalcMajority(m_nodesInConfig.Count());
    // If replica is in current config, it will take part on communication
    // so we only need majority-1 fake replicas
    if (replicaInCurrentConfig)
    {
        majoritySize--;
    }

    list.clear();
    for (size_t i = 0; i < m_nodesInConfig.Count() && list.size() < majoritySize; i++)
    {
        if (MemberId::Compare(m_nodesInConfig[i].m_memberIdString, m_testingNode.m_memberIdString) != 0)
        {
            FakeLegislator fakeLeg(MemberId(m_nodesInConfig[i].m_memberIdString));
            fakeLeg.Reset(localStatus);
            list.append(fakeLeg);
        }
    }
    UT_AssertIsTrue(majoritySize == list.size());
}

void
TestEngine::ApplyConfigurationChangedResults(StatusResponse * status)
{
    status->m_ballot = BallotNumber(0, MemberId());
    status->m_decree += 2;
    status->m_configurationNumber++;
    status->m_maxBallot = BallotNumber(status->m_maxBallot.m_ballotId + 1, MemberId());
}

void
TestEngine::WaitForState(UInt32 state, BallotNumber ballot, UInt64 decree,
        UInt32 configurationNumber, BallotNumber maxBallot)
{
    int count = 0;
    while (count++ < c_WaitIterations)
    {
        StatusResponse s;
        GetStatus(&s);
        if (s.m_state == state && s.m_ballot == ballot &&
            s.m_decree == decree && s.m_maxBallot == maxBallot &&
            s.m_configurationNumber == configurationNumber)
        {
            return;
        }
        Sleep(c_SleepTime);
    }
    UT_AssertFail();
}

void
TestEngine::WaitForState(UInt32 state, StatusResponse &status)
{
    WaitForState(state, status.m_ballot, status.m_decree,
        status.m_configurationNumber, status.m_maxBallot);
}


void
TestEngine::WaitForCheckpoint(UInt64 checkpoint)
{
    int count = 0;
    while (count++ < c_WaitIterations)
    {
        StatusResponse s;
        GetStatus(&s);
        if (s.m_checkpointedDecree == checkpoint)
        {
            return;
        }
        Sleep(c_SleepTime);
    }
    UT_AssertFail();
}

void
TestEngine::AssertEmptyQueues()
{
    Sleep(200);
    m_lock.Enter();
    UT_AssertIsNull(m_respRecvQ.head);
    UT_AssertIsNull(m_reqRecvQ.head);
    m_lock.Leave();
}

//------------------------------------------------------------------------
// Move to methods
//------------------------------------------------------------------------

void
TestEngine::MoveToState(UInt32 state)
{
    switch (state)
    {
        case PaxosInactive:
            UT_AssertFail("Use MoveToConfiguration() instead!");
            break;

        case Primary:
            MoveToPrimary();
            break;
            
        case Secondary:
            MoveToSecondary();
            break;
            
        case SecondaryPrepared:
            MoveToSecondaryPrepared();
            break;
        
        case Prepare:
            MoveToPreparing();
            break;
        
        case Initialize:
            MoveToInitializing();
            break;

        case InitializePrepared:
            MoveToInitializingPrepared();
            break;
            
        default:
            UT_AssertFail();
    }
}

void
TestEngine::MoveToPrimary(bool waitForPrimaryCallback)
{
    AssertEmptyQueues();

    // get the current status
    StatusResponse resp;
    GetStatus(&resp);
    
    switch (resp.m_state)
    {
        case Legislator::StablePrimary:
        case Legislator::StableSecondary:
            MoveToInitializing();
            InitializingToPreparing();
            PreparingToPrimary();
            break;

        case Legislator::Preparing:
            PreparingToPrimary();
            break;

        case Legislator::Initializing:
            InitializingToPreparing();
            PreparingToPrimary();
            break;

        default:
            UT_AssertFail();
    }
    UT_AssertAreEqual(Message_Vote, m_lReq->m_msgId);

    if (waitForPrimaryCallback)
    {
        FakeLegislatorCollection legislators;
        GetSyncedFakeLegislatorsMajority(legislators);
        CallForAll(&legislators, SendVoteAccepted());
        WaitUntil(m_machine->m_isPrimary == true);
    }
    else
    {
        // If the replica was a primary before, wait until the replica
        // notifies it is not primary anymore.
        WaitUntil(m_machine->m_isPrimary == false);
    }
}


void
TestEngine::MoveToSecondary()
{
    AssertEmptyQueues();

    // get the current status
    StatusResponse resp;
    GetStatus(&resp);
    switch (resp.m_state)
    {
        case Legislator::StablePrimary:
        case Legislator::StableSecondary:
            StableToSecondary();
            break;

        case Legislator::Preparing:
            PreparingToSecondary();
            break;

        case Legislator::Initializing:
            InitializingToSecondary();
            break;

        default:
            UT_AssertFail();
    }
    // If the replica was a primary before, wait until it
    // notifies.
    WaitUntil(m_machine->m_isPrimary == false);
}


void
TestEngine::MoveToSecondaryPrepared()
{
    MoveToSecondary();
    FakeLegislatorCollection legislators;
    GetSyncedFakeLegislatorsMajority(legislators, &m_status);
    UT_AssertIsTrue(legislators.size() > 0);
    FakeLegislator * worker = legislators[0];

    // send a prepare message. Replica should accept the message
    worker->IncrementBallot(10);
    Message * msg = worker->NewPrepare();

    Message *resp = SendAndReceive(msg, Message_PrepareAccepted);
    UT_AssertAreEqual(msg->m_decree, resp->m_decree);
    UT_AssertIsTrue(msg->m_ballot == resp->m_ballot);
    GetStatus(&m_status);
    delete msg;
}


void
TestEngine::MoveToPreparing()
{
    // get the current status
    MoveToInitializingPrepared();
    InitializingToPreparing();
    WaitUntil(m_machine->m_isPrimary == false);
}


void
TestEngine::MoveToInitializing()
{
    // send a higher vote
    MoveToSecondary();

    FakeLegislatorCollection legislators;
    GetSyncedFakeLegislatorsMajority(legislators, &m_status);
    UT_AssertIsTrue(legislators.size() > 0);
    FakeLegislator * worker = legislators[0];

    worker->IncrementBallot(10);
    worker->m_decree++;
    Message * msg = worker->NewVote();
    SendRequest(msg);
    delete msg;
    
    ReceiveRequests(Message_StatusQuery);
    GetStatus(&m_status);
    WaitUntil(m_machine->m_isPrimary == false);
}


void
TestEngine::MoveToInitializingPrepared()
{
    MoveToSecondaryPrepared();

    FakeLegislatorCollection legislators;
    GetSyncedFakeLegislatorsMajority(legislators, &m_status);
    UT_AssertIsTrue(legislators.size() > 0);
    FakeLegislator * worker = legislators[0];

    // send a status response with a higher ballot than ours
    worker->IncrementBallot(10);
    worker->m_decree++;
    Message * msg = worker->NewVote();
    SendRequest(msg);
    delete msg;
    
    ReceiveRequests(Message_StatusQuery);
    GetStatus(&m_status);
} 

void
TestEngine::EnsureVersion(RSLProtocolVersion version)
{
    MoveToSecondary();

    FakeLegislatorCollection legislators;
    GetSyncedFakeLegislatorsMajority(legislators, &m_status);
    UT_AssertIsTrue(legislators.size() > 0);
    FakeLegislator * worker = legislators[0];

    worker->m_version = version;
    worker->IncrementBallot(10);

    PrepareMsg * msg = worker->NewPrepare();
    SendAndReceive(msg, Message_PrepareAccepted);
    delete msg;

    Vote * vote = worker->NewVote();
    SendAndReceive(vote, Message_VoteAccepted);
    delete vote;

    worker->m_decree++;
    vote = worker->NewVote();
    SendAndReceive(vote, Message_VoteAccepted);
    delete vote;

    WaitUntil(GetStatus(&m_status)->m_version >= version);
    m_machine->m_version = (RSLProtocolVersion) GetStatus(&m_status)->m_version;
    AssertEmptyQueues();
}

void 
TestEngine::MoveToDefaultConfig()
{
    // Default config
    RSLNodeCollection nodes;
    nodes.Append(GetFakeNode(1));
    nodes.Append(m_testingNode);
    nodes.Append(GetFakeNode(3));

    // Move to new config
    MoveToConfiguration(nodes);
}

void 
TestEngine::MoveToDefaultConfigUpTo(ReconfigStage stage, FakeLegislatorCollection &legislators)
{
    // Default config
    RSLNodeCollection nodes;
    nodes.Append(GetFakeNode(1));
    nodes.Append(m_testingNode);
    nodes.Append(GetFakeNode(3));
    // Check current config
    CheckConfiguration(nodes);

    GetSyncedFakeLegislatorsMajority(legislators);
    UT_AssertIsTrue(legislators.size() > 0);
    FakeLegislator * fakeLeg = legislators[0];

    // Prepapre
    fakeLeg->SendPrepare(&legislators);
    ReceiveResponse(Message_PrepareAccepted);
    // Pass vote
    fakeLeg->SendLastVote(&legislators);
    ReceiveResponse(Message_VoteAccepted);

    // Place the change
    if (stage <= BeforeSendReconfigVote) return;
    fakeLeg->SendChangeConfig(nodes, NULL, &legislators);
    ReceiveResponse(Message_VoteAccepted);
    
    // Send decision
    if (stage <= BeforeSendDecision) return;
    fakeLeg->SendReconfigurationDecision(&legislators);
    
    // Wait
    StatusResponse status;
    fakeLeg->GetStatus(&status);
    WaitForState(Legislator::Initializing, status);

    // Update majority
    Vote * lastVote = fakeLeg->GetCopyLastVote();
    UpdateMajority(legislators, nodes, lastVote);
    fakeLeg = legislators[0];
    delete lastVote;

    // Receive status query 
    if (stage <= BeforeReceiveStatusQuery) return;
    Message * query = ReceiveRequests(Message_StatusQuery);
    UInt64 queryDecree = query->m_decree;
    BallotNumber queryBallot = query->m_ballot;

    // Receive join messages for B and C
    if (stage <= BeforeReceiveJoinMessages) return;
    ReceiveRequests(Message_Join);
    // No messages left
    AssertEmptyQueues();

    // Send status response
    if (stage <= BeforeSendStatusResponse) return;
    CallForAll(&legislators, SendStatusResponse(queryDecree, queryBallot, 0));

    // Prepapre
    if (stage <= BeforePrepareAgain) return;
    fakeLeg->SendPrepare(&legislators);
    ReceiveResponse(Message_PrepareAccepted);

    // Send last vote
    if (stage <= BeforeSendLastVote) return;
    fakeLeg->SendLastVote(&legislators);
    ReceiveResponse(Message_VoteAccepted);
}

void 
TestEngine::MoveToPaxosInactive()
{
    RSLNodeCollection nodes;
    nodes.Append(GetFakeNode());
    nodes.Append(GetFakeNode());
    nodes.Append(GetFakeNode());
    MoveToConfiguration(nodes);
}

void
TestEngine::IncrementHighestDefunctConfiguration(UInt32 increment)
{
    StatusResponse status;
    FakeLegislatorCollection legislators;
    GetSyncedFakeLegislatorsMajority(legislators, &status);
    UT_AssertIsTrue(legislators.size() > 0);
    FakeLegislator * fakeLeg = legislators[0];
    UInt32 configNumber = status.m_configurationNumber;
    UInt32 highestDefunct = GetLegislator()->GetHighestDefunctConfigurationNumber();

    // Send notification
    fakeLeg->SendDefunctConfiguration(highestDefunct + increment);
    // Wait for results
    WaitUntil(highestDefunct < GetLegislator()->GetHighestDefunctConfigurationNumber());

    // Verify changes
    TestEngine::GetStatus(&status);
    UT_AssertIsTrue(configNumber == status.m_configurationNumber);
    UT_AssertIsTrue(highestDefunct + increment == GetLegislator()->GetHighestDefunctConfigurationNumber());
}

void
TestEngine::MoveToConfiguration(RSLNodeCollection &nodes)
{
    //// Check known config against what replica actually has
    //CheckConfiguration(m_nodesInConfig, currentConfigNumber);

    Ptr<MemberSet> memberSet = new MemberSet();
    UInt32 currentConfigNumber;
    m_machine->m_legislator->GetPaxosConfiguration(memberSet, &currentConfigNumber);
    UInt32 highestDefunctConfig = GetLegislator()->GetHighestDefunctConfigurationNumber();
    UpdateNodesInConfiguration(memberSet->GetMemberCollection());

    StatusResponse status;
    FakeLegislatorCollection legislators;
    GetStatus(&status);
    UT_AssertIsTrue(status.m_configurationNumber == currentConfigNumber);

    bool testingNodeInOldConfig = ContainsNode(m_nodesInConfig, m_testingNode.m_memberIdString);
    bool testingNodeInNewConfig = ContainsNode(nodes, m_testingNode.m_memberIdString);
    UT_AssertIsTrue(testingNodeInOldConfig || status.m_state == Legislator::PaxosInactive);

    //---------------------------------
    // Submit change
    //---------------------------------

    // Check if replica is the only one in current config
    if (testingNodeInOldConfig && m_nodesInConfig.Count() == 1)
    {
        UT_AssertIsTrue(status.m_state == Legislator::StablePrimary);
        // Issue config change
        RSLResponseCode res = m_machine->SubmitChangeConfig(nodes);
        UT_AssertIsTrue(res == RSLSuccess);
        // Sets expected status values
        ApplyConfigurationChangedResults(&status);
    }
    else
    {
        // Not alone
        GetSyncedFakeLegislatorsMajority(legislators);
        // Pick a primary
        UT_AssertIsTrue(legislators.size() > 0);
        FakeLegislator * primary = legislators[0];

        // Make sure replica is either Stable or PaxosInactive
        if (status.m_state == Legislator::Initializing)
        {
            UT_AssertIsTrue(testingNodeInOldConfig);
            CallForAll(&legislators, SendStatusResponse(status.m_decree, status.m_ballot, 0));
            WaitForState(Legislator::StableSecondary, status);
        }
        // Prepapre
        primary->SendPrepare(&legislators, testingNodeInOldConfig);
        if (testingNodeInOldConfig)
        {
            TestEngine::ReceiveResponse(Message_PrepareAccepted);
        }
        // Pass vote
        primary->SendLastVote(&legislators, testingNodeInOldConfig);
        if (testingNodeInOldConfig)
        {
            TestEngine::ReceiveResponse(Message_VoteAccepted);
        }
        // Place the change
        primary->SendChangeConfig(nodes, NULL, &legislators, testingNodeInOldConfig);
        if (testingNodeInOldConfig)
        {
            TestEngine::ReceiveResponse(Message_VoteAccepted);
        }
        // Send decision
        primary->SendReconfigurationDecision(&legislators, testingNodeInOldConfig);

        // Save the expected status
        primary->GetStatus(&status);

        // Replica was out of the config set, so it needs to be invited and learn votes as well
        if (testingNodeInOldConfig == false)
        {
            // Send a join to the real replica
            primary->SendJoin();
            // Handle learn votes
            HandleLearnVotes(primary->m_log, false);
        }
    }

    // LastVote
    Vote * lastVote = VoteFactory::NewEmptyVote((RSLProtocolVersion) status.m_version,
        status.m_decree, status.m_configurationNumber);

    // Wait for results
    Legislator::State newState = Legislator::PaxosInactive;
    if (testingNodeInNewConfig)
    {
        newState = Legislator::Initializing;
        if (nodes.Count() == 1)
        {
            // Replica is alone in new configuration
            newState = Legislator::StablePrimary;
            status.m_maxBallot = BallotNumber(status.m_maxBallot.m_ballotId+1, MemberId(m_testingNode.m_memberIdString));
            status.m_ballot = status.m_maxBallot;
        }
    }

    WaitForState(newState, status);

    //---------------------------------
    // Join new config
    //---------------------------------

    // Update majority
    UpdateMajority(legislators, nodes, lastVote);
    delete lastVote;

    // Drain messages
    UInt64 queryDecree = 0;
    BallotNumber queryBallot = BallotNumber();
    if (newState == Legislator::Initializing)
    {
        // Receive Status query
        Message * query = ReceiveRequests(Message_StatusQuery);
        queryDecree = query->m_decree;
        queryBallot = query->m_ballot;
    }

    // Receive join requests (if needed)
    if (currentConfigNumber > highestDefunctConfig)
    {
        size_t numOfRequests = testingNodeInNewConfig ? nodes.Count() - 1 : nodes.Count();
        ReceiveRequests(numOfRequests, Message_Join);
    }
    // No messages left
    AssertEmptyQueues();
    
    if (newState == Legislator::Initializing)
    {
        UT_AssertIsTrue(queryDecree != 0);
        CallForAll(&legislators, SendStatusResponse(queryDecree, queryBallot, -1));

        // Wait until replica become preparing
        status.m_maxBallot = BallotNumber(status.m_maxBallot.m_ballotId+1, MemberId(m_testingNode.m_memberIdString));
        WaitForState(Legislator::Preparing, status);

        // Accept prepare
        Message * prep = ReceiveRequests(Message_Prepare);
        CallForAll(&legislators, AcceptPrepare(prep));
        CallForAll(&legislators, SendPrepareAccepted());
        // Receive empty vote
        Vote * vote = (Vote *) ReceiveRequests(Message_Vote);
        CallForAll(&legislators, AcceptVote(vote));
        CallForAll(&legislators, SendVoteAccepted());
        UT_AssertAreEqual(0, vote->m_numRequests);

        // Update expected state and status
        newState = Legislator::StablePrimary;
        status.m_ballot = status.m_maxBallot;
    }
    else if (newState == Legislator::PaxosInactive)
    {
        // Pick a primary
        UT_AssertIsTrue(legislators.size() > 0);
        FakeLegislator * primary = legislators[0];
        // Let the replica now that old config is defunct
        primary->SendDefunctConfiguration(currentConfigNumber);
    }

    // No messages left
    AssertEmptyQueues();

    // Wait for changes
    WaitForState(newState, status);
    // Check new config
    CheckConfiguration(nodes, currentConfigNumber+1);
}


void
TestEngine::PreparingToPrimary()
{
    StatusResponse status;
    FakeLegislatorCollection legislators;
    GetSyncedFakeLegislatorsMajority(legislators, &status);

    UT_AssertAreEqual(Legislator::Preparing, status.m_state);
    UT_AssertIsTrue(status.m_maxBallot != status.m_ballot);

    // send a prepareAccepted meessage. This will cause the
    // replica to move to primary and re-propose the last vote.

    UT_AssertIsTrue(legislators.size() > 0);
    FakeLegislator * worker = legislators[0];
    // Make sure they have last vote
    Vote * vote = worker->NewVote();
    CallForAll(&legislators, AppendLog(vote));
    delete vote;
    // Send acceptence response
    CallForAll(&legislators, SendPrepareAccepted());

    // Accepts vote
    ReceiveRequests(Message_Vote);

    // wait until the replica logs the last vote.
    WaitForState(Legislator::StablePrimary, m_lReq->m_ballot, m_lReq->m_decree, m_lReq->m_configurationNumber, m_lReq->m_ballot);
    GetStatus(&m_status);
}

void
TestEngine::StableToSecondary()
{
    StatusResponse status;
    FakeLegislatorCollection legislators;
    GetSyncedFakeLegislatorsMajority(legislators, &status);
    UT_AssertIsTrue(legislators.size() > 0);
    FakeLegislator * worker = legislators[0];

    UT_AssertIsTrue(Legislator::StableSecondary == status.m_state ||
                    Legislator::StablePrimary == status.m_state);

    worker->IncrementBallot(10);
    Message * msg = worker->NewVote();
    // send vote
    Message *resp = SendAndReceive(msg, Message_VoteAccepted);
    UT_AssertAreEqual(msg->m_decree, resp->m_decree);
    UT_AssertIsTrue(msg->m_ballot == resp->m_ballot);
    GetStatus(&m_status);
    delete msg;
}


void
TestEngine::PreparingToSecondary()
{
    StatusResponse status;
    FakeLegislatorCollection legislators;
    GetSyncedFakeLegislatorsMajority(legislators, &status);
    UT_AssertIsTrue(legislators.size() > 0);
    FakeLegislator * worker = legislators[0];

    UT_AssertAreEqual(Legislator::Preparing, status.m_state);
    UT_AssertIsTrue(status.m_maxBallot != status.m_ballot);

    // Send a vote with same decree an higher ballot.
    worker->IncrementBallot(10);
    Message * msg = worker->NewVote();
    // send vote
    Message *resp = SendAndReceive(msg, Message_VoteAccepted);
    UT_AssertAreEqual(msg->m_decree, resp->m_decree);
    UT_AssertIsTrue(msg->m_ballot == resp->m_ballot);
    GetStatus(&m_status);
    delete msg;
}

void
TestEngine::InitializingToSecondary()
{
    StatusResponse status;
    FakeLegislatorCollection legislators;
    GetSyncedFakeLegislatorsMajority(legislators, &status);
    UT_AssertIsTrue(legislators.size() > 0);
    FakeLegislator * worker = legislators[0];

    // Insert an empty vote into worker's log
    worker->AppendLog(worker->NewVote());
    
    // send status response with a higher decree number
    worker->IncrementBallot(10);
    worker->AppendLog(worker->NewVote());

    // Send responses
    for (size_t i=0; i < legislators.size(); i++)
    {
        legislators[i]->m_ballot = worker->m_ballot;
        legislators[i]->m_maxBallot = worker->m_maxBallot;
        legislators[i]->SendStatusResponse(status.m_decree, status.m_ballot, 0);
    }

    // Handle learn votes request
    HandleLearnVotes(worker->m_log);

    // After learning, replica sends out status query requests
    Message * msg = ReceiveRequests(Message_StatusQuery);
    UT_AssertAreEqual(msg->m_decree, worker->m_decree);
    UT_AssertIsTrue(msg->m_ballot == worker->m_ballot);
    
    CallForAll(&legislators, SendStatusResponse(msg->m_decree, msg->m_ballot, 0));

    // wait until the legislator receives this message
    worker->GetStatus(&status);
    WaitForState(Legislator::StableSecondary, status);
    GetStatus(&m_status);
}


void
TestEngine::InitializingToPreparing()
{
    StatusResponse status;
    FakeLegislatorCollection legislators;
    GetSyncedFakeLegislatorsMajority(legislators);

    UT_AssertIsTrue(legislators.size() > 0);
    FakeLegislator * worker = legislators[0];
    // send status response with the same decree number
    CallForAll(&legislators, SendStatusResponse(worker->m_decree, worker->m_ballot, -1));

    Message * prep = ReceiveRequests(Message_Prepare);
    UT_AssertIsTrue(worker->m_ballot != m_lReq->m_ballot);
    worker->AcceptPrepare(prep);
    
    worker->GetStatus(&status);
    WaitForState(Legislator::Preparing, status);
    GetStatus(&m_status);
}

//------------------------------------------------------------------------
// Communication
//------------------------------------------------------------------------


unsigned int
TestEngine::HandleFetchRequest(void * /*arg*/)
{
    for (;;)
    {
        std::unique_ptr<StreamSocket> pSock(StreamSocket::CreateStreamSocket());
        UT_AssertIsNotNull(pSock.get());
        int ec = m_pFetchSocket->Accept(pSock.get(), m_cfg.ReceiveTimeout(), m_cfg.SendTimeout());
        if (ec != NO_ERROR)
        {
            UT_AssertAreEqual(NO_ERROR, ec);
            return 0;
        }
       
        Message *msg = new Message();
        bool success = msg->ReadFromSocket(pSock.get(), 10*1000*1000);
        UT_AssertIsTrue(success);

        UT_AssertIsTrue(Message_FetchVotes == msg->m_msgId || 
                        Message_FetchCheckpoint == msg->m_msgId ||
                        Message_StatusQuery == msg->m_msgId);

        FetchRequest *req = new FetchRequest(pSock.release(), msg);
        {
            AutoCriticalSection lock(&m_lock);
            m_fetchRecvQ.enqueue(req);
        }
    }
}

FetchRequest*
TestEngine::ReceiveFetchRequest(UInt16 messageId)
{
    Int64 start = GetHiResTime();
    FetchRequest *req = NULL;
    while (GetHiResTime() - start < HRTIME_MSECONDS(m_cfg.ReceiveTimeout()))
    {
        m_lock.Enter();
        req = m_fetchRecvQ.dequeue();
        m_lock.Leave();
        if (req)
        {
            UT_AssertAreEqual(req->m_message->m_msgId, messageId);
            break;
        }
        WaitForSingleObject(m_fetchReqHandle, 10);
    }

    UT_AssertIsNotNull(req);
    return req;
}

UInt32
TestEngine::Fetch(Message *msg, const char *file)
{
    MarshalData marshal;
    msg->Marshal(&marshal);
    
	std::unique_ptr<StreamSocket> pSock(StreamSocket::CreateStreamSocket());
    UT_AssertAreEqual(NO_ERROR, pSock->Connect(m_testingNode.m_ip, m_testingNode.m_rslPort+1, m_cfg.ReceiveTimeout(), m_cfg.SendTimeout()));
    
    UT_AssertAreEqual(NO_ERROR, pSock->Write(marshal.GetMarshaled(), marshal.GetMarshaledLength()));

    APSEQWRITE writer;
    UT_AssertAreEqual(NO_ERROR, writer.DoInit(file));

    UInt32 recv;
    StandardMarshalMemoryManager memory(10000);
    while (pSock->Read(memory.GetBuffer(), memory.GetBufferLength(), &recv) == NO_ERROR)
    {
        UT_AssertAreEqual(NO_ERROR, writer.Write(memory.GetBuffer(), recv));
    }
    writer.DoDispose();
    return (UInt32) writer.BytesIssued();
}


void
TestEngine::ProcessSend(Packet * /*packet*/, TxRxStatus status, bool asClient)
{
    (void) asClient;
    UT_AssertAreEqual(TxSuccess, status);
    UT_AssertAreEqual(true, m_sendOutstanding);
    m_sendOutstanding = false;
    SetEvent(m_netlibSendHandle);
}


void
TestEngine::ProcessReceive(Packet *pkt, bool asClient)
{
    UT_AssertAreEqual(m_testingNode.m_ip, pkt->GetServerIp());
    UT_AssertAreEqual(m_testingNode.m_ip, pkt->GetClientIp());
    
    Message *msg = UnMarshalMessage(&pkt->m_MemoryManager);
    UT_AssertIsNotNull(msg);

    AutoCriticalSection lock(&m_lock);
    if (asClient)
    {
        m_respRecvQ.enqueue(msg);
    }
    else
    {
        if (m_testingNodeClientPort == 0)
        {
            m_testingNodeClientPort = pkt->GetClientPort();
        }
        else
        {
            UT_AssertAreEqual(m_testingNodeClientPort, pkt->GetClientPort());
        }
        m_reqRecvQ.enqueue(msg);
    }
    SetEvent(m_netlibRecvHandle);
    delete pkt;
}


Message*
TestEngine::UnMarshalMessage(IMarshalMemoryManager *memory)
{
    MarshalData marshal(memory);
    Message msgHdr;
    Message *msg = NULL;

    UT_AssertIsTrue(msgHdr.Peek(&marshal));

    switch (msgHdr.m_msgId)
    {
        case Message_Vote:
            msg = new Vote();
            break;

        case Message_PrepareAccepted:
            msg = new PrepareAccepted();
            break;

        case Message_StatusResponse:
            msg = new StatusResponse();
            break;

        case Message_Prepare:
            msg = new PrepareMsg();
            break;
            
        case Message_Join:
            msg = new JoinMessage();
            break;
  
        case Message_Bootstrap:
            msg = new BootstrapMsg();
            break;

        case Message_VoteAccepted:
        case Message_NotAccepted:
        case Message_StatusQuery:
        case Message_DefunctConfiguration:
        case Message_JoinRequest:
        case Message_ReconfigurationDecision:
            msg = new Message();
            break;

        default:
            UT_AssertFail();
    }
    UT_AssertIsNotNull(msg);
    UT_AssertIsTrue(msg->UnMarshal(&marshal));
    return msg;
}

void
TestEngine::SendMessage(Message *msg, bool asClient, UInt16 port)
{
    if (msg->m_ballot.m_ballotId > m_maxSentBallotId)
    {
        m_maxSentBallotId = msg->m_ballot.m_ballotId;
    }
    
    // votes have to have correct checksum
    if (msg->m_msgId == Message_Vote)
    {
        ((Vote *) msg)->CalculateChecksum();
    }
    UInt32 len = msg->GetMarshalLen();
    // allocate a packet of that length;
    Packet *pkt = new Packet();
    
    MarshalData marshal(&pkt->m_MemoryManager);
    msg->Marshal(&marshal);
    UT_AssertAreEqual(len, marshal.GetMarshaledLength());

    SendPacket(pkt, asClient, port);
    delete pkt;
}

void
TestEngine::SendBuffer(void *buf, UInt32 length, bool asClient)
{
    Packet *pkt = new Packet();
    pkt->m_MemoryManager.ResizeBuffer(length);
    memcpy(pkt->m_MemoryManager.GetBuffer(), buf, length);
    pkt->m_MemoryManager.SetValidLength(length);
    SendPacket(pkt, asClient);
    delete pkt;
}

void
TestEngine::SendPacket(Packet *pkt, bool asClient, UInt16 port)
{
    UT_AssertIsFalse(m_sendOutstanding);
    m_sendOutstanding = true;
    TxRxStatus status;
    if (asClient)
    {
        if (port == 0)
        {
            port = m_testingNode.m_rslPort;
        }
        pkt->SetServerAddr(m_testingNode.m_ip, port);
        status = m_netlibClient->Send(pkt, m_cfg.SendTimeout());
        UT_AssertAreEqual(TxSuccess, status);
    }
    else
    {
        if (port == 0)
        {
            port = m_testingNodeClientPort;
        }
        pkt->SetClientAddr(m_testingNode.m_ip, port);
        status = m_netlibServer->Send(pkt, m_cfg.SendTimeout());
        UT_AssertAreEqual(TxSuccess, status);
    }
    while (m_sendOutstanding == true)
    {
        WaitForSingleObject(m_netlibSendHandle, 10);
    }
}


void
TestEngine::SendRequest(Message * msg)
{
    SendMessage(msg, true);
}


void
TestEngine::SendResponse(Message * msg)
{
    SendMessage(msg, false);
}


Message *
TestEngine::SendAndReceive(Message * msg, UInt16 msgId)
{
    SendRequest(msg);
    return ReceiveResponse(msgId);
}

Message *
TestEngine::ReceiveMessage(UInt16 msgId, Queue<Message> * queue, Message ** lastMsg, bool verify)
{
    Int64 start = GetHiResTime();
    Message *msg = NULL;
    while (GetHiResTime() - start < HRTIME_MSECONDS(m_cfg.ReceiveTimeout()))
    {
        m_lock.Enter();
        msg = queue->dequeue();
        m_lock.Leave();

        if (msg)
        {
            if (verify)
            {
                VerifyMessage(msg, msgId);
            }
            delete *lastMsg;
            *lastMsg = msg;
            break;
        }
        WaitForSingleObject(m_netlibRecvHandle, 10);
    }
    UT_AssertIsNotNull(msg);
    return msg;
}

void
TestEngine::VerifyMessage(Message *msg, int msgId)
{
    // TODO
    //UT_AssertAreEqual(m_version, msg->m_version);
    UT_AssertAreEqual(s_MessageMagic, msg->m_magic);
    UT_AssertAreEqual(msgId, msg->m_msgId);
    UT_AssertIsTrue(msg->m_memberId.Compare(m_testingNode.m_memberIdString) == 0);
    
    // verify checksum
    if (msgId == Message_Vote || msgId == Message_ReconfigurationDecision)
    {
        if (msgId == Message_Vote)
        {
            VoteFactory::VerifyVote((Vote *) msg);
        }
        
        MarshalData marshal;
        msg->Marshal(&marshal);
        UT_AssertIsTrue(msg->VerifyChecksum((char *) marshal.GetMarshaled(),
                                            marshal.GetMarshaledLength()));
    }
    else
    {
        UT_AssertAreEqual(0, msg->m_checksum);
    }

    if (msgId == Message_Vote || msgId == Message_Prepare)
    {
        UT_AssertIsTrue(msg->m_ballot.m_memberId.Compare(m_testingNode.m_memberIdString) == 0);
    }
    if (msgId == Message_PrepareAccepted)
    {
        PrepareAccepted *accepted = (PrepareAccepted *) msg;
        UT_AssertIsTrue(accepted->m_ballot >= accepted->m_vote->m_ballot);
        UT_AssertIsTrue(accepted->m_decree >= accepted->m_vote->m_decree);
    }
    if (msgId == Message_StatusResponse)
    {
        StatusResponse *resp = (StatusResponse *) msg;
        UInt32 state = resp->m_state;
        UT_AssertIsTrue(state == Legislator::PaxosInactive ||
                        state == Legislator::StablePrimary ||
                        state == Legislator::StableSecondary ||
                        state == Legislator::Preparing ||
                        state == Legislator::Initializing);
    }
}

Message *
TestEngine::ReceiveResponse(UInt16 msgId, bool verify)
{
    return ReceiveMessage(msgId, &m_respRecvQ, &m_lResp, verify);
}

Message *
TestEngine::ReceiveRequests(size_t numberOfRequests, UInt16 msgId, bool verify)
{
    Message * msg = NULL;
    for (size_t i=0; i < numberOfRequests; i++)
    {
        msg = ReceiveMessage(msgId, &m_reqRecvQ, &m_lReq, verify);
    }
    if (numberOfRequests > 0)
    {
        UT_AssertIsTrue(msg != NULL);
    }
    return msg;
}

Message *
TestEngine::ReceiveRequests(UInt16 msgId, bool verify)
{
    size_t numberOfRequests = m_nodesInConfig.Count() - 1;
    return ReceiveRequests(numberOfRequests, msgId, verify);
}

void
TestEngine::HandleLearnVotes(vector<Message *> log, bool waitForClose)
{
    HandleLearnVotes(log, -1, waitForClose);
}

void
TestEngine::HandleLearnVotes(vector<Message *> log, int idx, bool waitForClose)
{
    // wait for a fetch request
    FetchRequest *req = ReceiveFetchRequest(Message_FetchVotes);
    UT_AssertIsNotNull(req);
    HandleLearnVotes(req, log, idx, waitForClose);
    delete req;
}

void
TestEngine::HandleLearnVotes(FetchRequest *req, vector<Message *> log, int idx, bool waitForClose)
{
    Message *msg = req->m_message;

    if (idx == -1)
    {
        // Seek required decree
        for (idx=0; idx < (int) log.size(); idx++)
        {
            if (log[idx]->m_decree == msg->m_decree)
            {
                break;
            }
        }
    }
    UT_AssertIsTrue(idx < (int) log.size());

    // Send votes
    int ec = NO_ERROR;
    StandardMarshalMemoryManager memory(s_AvgMessageLen);
    char *buf = (char *) memory.GetBuffer();
    UInt32 received = 0;
    for (; idx < (int) log.size() && ec == NO_ERROR; idx++)
    {
        msg = log[idx];
        if (msg->m_msgId == Message_Vote)
        {
            Vote * vote = (Vote *) msg;
            vote->CalculateChecksum();
            vote->MarshalBuf(buf, s_AvgMessageLen);
            ec = req->m_sock->Write(buf, RoundUpToPage(vote->GetMarshalLen()));
        }
        else
        {
            DWORD toWrite = RoundUpToPage(msg->GetMarshalLen()); 
            msg->MarshalBuf(buf, toWrite);
            msg->CalculateChecksum(buf, msg->GetMarshalLen());
            ec = req->m_sock->Write(buf, RoundUpToPage(msg->GetMarshalLen()));
        }
    }
    if (waitForClose)
    {
        // wait until the thread closes the connection
        ec = req->m_sock->Read(buf, 1, &received);
        UT_AssertAreEqual(0, received);
    }
}

MemberId 
TestEngine::TestingNodeMemberId()
{
    return MemberId(m_testingNode.m_memberIdString);
}
