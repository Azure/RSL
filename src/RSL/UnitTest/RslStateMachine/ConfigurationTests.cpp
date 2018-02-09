#pragma warning(disable:4512)
#include "ConfigurationTests.h"
#include "rsldebug.h"
#include "legislator.h"
#include "apdiskio.h"
#include "rslutil.h"
#include <stdarg.h>
#include <process.h>
#include <windows.h>

RSLNode *A, *B, *C, *D, *E , *F;

char *TestEngineStateToStr[] = {"Inact","Prim","Sec","SecPrep","Prep","Init","InitPrep"};
char *LegislatorStateToStr[] = {"Inact","Prim","Sec","Prep","Init"};

void InitConfigurationTests()
{
    TestEngine::EnsureVersion(RSLProtocolVersion_3);
    TestEngine::MoveToDefaultConfig();

    A = new RSLNode();
    B = new RSLNode();
    C = new RSLNode();
    D = new RSLNode();
    E = new RSLNode();
    F = new RSLNode();

    UInt64 memberId = TestEngine::m_testingNode.GetMemberIdAsUInt64();
    *A = TestEngine::m_testingNode;
    *B = TestEngine::GetFakeNode(memberId-1);
    *C = TestEngine::GetFakeNode(memberId+1);
    *D = TestEngine::GetFakeNode(memberId+2);
    *E = TestEngine::GetFakeNode(memberId+3);
    *F = TestEngine::GetFakeNode(memberId+4);
}

//----------------------------------------------------------------------------
// Simple tests
//----------------------------------------------------------------------------

void TestAddNewReplica_asPrimary()
{
    RSLNodeCollection initStateArray = BuildArray(A, B, C, NULL);
    RSLNodeCollection finalStateArray = BuildArray(A, B, C, D, NULL);
    ReplicaIssuesConfigChange(initStateArray, Legislator::StablePrimary, finalStateArray);
}

void TestAddNewReplica_asSecondary()
{
    RSLNodeCollection initStateArray = BuildArray(A, B, C, NULL);
    RSLNodeCollection finalStateArray = BuildArray(A, B, C, D, NULL);
    ReplicaFollowsConfigChange(TestEngine::Secondary, initStateArray, Legislator::StableSecondary, finalStateArray);
}

//----------------------------------------------------------------------------
// Matrix
//----------------------------------------------------------------------------

//------------------------------------
// Replicas issues config change
//------------------------------------

void ReplicaIssuesConfigChange_AllFinalStates(RSLNodeCollection oldNodes, RSLNodeCollection newNodes)
{
    if (IsTestingNodeInConfig(newNodes))
    {
        ReplicaIssuesConfigChange(oldNodes, Legislator::StablePrimary, newNodes);
        if (newNodes.Count() > 1)
        {
            ReplicaIssuesConfigChange(oldNodes, Legislator::StableSecondary, newNodes);
        }
    }
    else
    {
        ReplicaIssuesConfigChange(oldNodes, Legislator::PaxosInactive, newNodes);
    }
}

void ReplicaIssuesConfigChange(RSLNodeCollection &oldNodes, Legislator::State finalState, RSLNodeCollection &newNodes)
{
    PrintChange("Replica", TestEngine::Primary, oldNodes, finalState, newNodes);

    UT_AssertIsTrue(IsTestingNodeInConfig(oldNodes));

    TestEngine::MoveToConfiguration(oldNodes);
    if (oldNodes.Count() > 1)
    {
        TestEngine::MoveToPrimary(true);
    }
    else
    {
        WaitUntil(TestEngine::m_machine->m_isPrimary == true);
    }

    StatusResponse status;
    FakeLegislatorCollection legislators;
    TestEngine::GetSyncedFakeLegislatorsMajority(legislators, &status);
    UInt32 configNumber = status.m_configurationNumber;

    // Check current state
    TestEngine::CheckConfiguration(oldNodes, configNumber);
    // Perform change
    ReplicaIssuesConfigChangeImpl(legislators, newNodes);
    // Join new config
    JoinNewConfig(true, finalState, legislators, newNodes);
    // Check new config
    TestEngine::CheckConfiguration(newNodes, configNumber+1);
}

void ReplicaIssuesConfigChangeImpl(FakeLegislatorCollection &legislators, RSLNodeCollection &newNodes)
{
    UT_AssertIsTrue(TestEngine::m_machine->m_isPrimary);

    // Calculates expected status at the end of the change
    StatusResponse status;
    TestEngine::GetStatus(&status);
    UInt64 newDecree = ++status.m_decree;
    TestEngine::ApplyConfigurationChangedResults(&status);

    // Pass a decree
    TestEngine::m_machine->SubmitRequest(newDecree);
    // Receive and accept vote
    Vote * vote = (Vote *) TestEngine::ReceiveRequests(Message_Vote);
    CallForAll(&legislators, AcceptVote(vote));
    CallForAll(&legislators, SendVoteAccepted());

    // Place the change
    RSLResponseCode res = TestEngine::m_machine->SubmitChangeConfig(newNodes);
    UT_AssertIsTrue(res == RSLSuccess);
    // Receive and accept reconfiguration vote
    vote = (Vote *) TestEngine::ReceiveRequests(Message_Vote);
    CallForAll(&legislators, AcceptVote(vote));
    CallForAll(&legislators, SendVoteAccepted());

    // Receive and accept reconfiguration decision
    Message * msg = TestEngine::ReceiveRequests(Message_ReconfigurationDecision);
    CallForAll(&legislators, AcceptReconfigurationDecision(msg));

    // Wait for results
    UInt32 newState = Legislator::PaxosInactive;
    if (IsTestingNodeInConfig(newNodes))
    {
        newState = Legislator::Initializing;
        if (newNodes.Count() == 1)
        {
            newState = Legislator::StablePrimary;
            status.m_maxBallot = BallotNumber(status.m_maxBallot.m_ballotId+1, TestEngine::TestingNodeMemberId());
            status.m_ballot = status.m_maxBallot;
        }
    }
    TestEngine::WaitForState(newState, status);
}

//------------------------------------
// Replicas follows config change
//------------------------------------

#define LengthOf(_x) (sizeof(_x)/sizeof(_x[0]))

void ReplicaFollowsConfigChange_AllStates(RSLNodeCollection oldNodes, RSLNodeCollection newNodes)
{
    TestEngine::State ALL_INITIAL_STATES[] = {
        TestEngine::Primary,
        TestEngine::Secondary,
        TestEngine::SecondaryPrepared,
        TestEngine::Prepare
        //TestEngine::Initialize,
        //TestEngine::InitializePrepared
    };
    TestEngine::State PAXOS_INACTIVE_ONLY[] = { TestEngine::PaxosInactive };

    bool testtingNodeInOldConfig = IsTestingNodeInConfig(oldNodes);
    bool testtingNodeInNewConfig = IsTestingNodeInConfig(newNodes);

    TestEngine::State * initalState = testtingNodeInOldConfig ? ALL_INITIAL_STATES : PAXOS_INACTIVE_ONLY;
    int initalStateLength = testtingNodeInOldConfig ? LengthOf(ALL_INITIAL_STATES) : LengthOf(PAXOS_INACTIVE_ONLY);

    for (int i=0; i < initalStateLength; i++)
    {
        if (testtingNodeInNewConfig)
        {
            ReplicaFollowsConfigChange(initalState[i], oldNodes, Legislator::StablePrimary, newNodes);
            if (newNodes.Count() > 1)
            {
                ReplicaFollowsConfigChange(initalState[i], oldNodes, Legislator::StableSecondary, newNodes);
            }
        }
        else
        {
            ReplicaFollowsConfigChange(initalState[i], oldNodes, Legislator::PaxosInactive, newNodes);
        }
    }
}

void ReplicaFollowsConfigChange(TestEngine::State initialState, RSLNodeCollection &oldNodes,
                                Legislator::State finalState, RSLNodeCollection &newNodes)
{
    PrintChange("FakeLeg", initialState, oldNodes, finalState, newNodes);

    bool testingNodeInOldConfig = IsTestingNodeInConfig(oldNodes);
    UT_AssertIsTrue(testingNodeInOldConfig || IsTestingNodeInConfig(newNodes));

    TestEngine::MoveToConfiguration(oldNodes);
    if (initialState != Legislator::PaxosInactive)
    {
        TestEngine::MoveToState(initialState);
    }

    StatusResponse status;
    FakeLegislatorCollection legislators;
    TestEngine::GetSyncedFakeLegislatorsMajority(legislators, &status);
    UInt32 configNumber = status.m_configurationNumber;

    // Check current state
    TestEngine::CheckConfiguration(oldNodes, configNumber);
    // Perform change
    ReplicaFollowsConfigChangeImpl(testingNodeInOldConfig, legislators, newNodes);
    // Join new config
    JoinNewConfig(testingNodeInOldConfig, finalState, legislators, newNodes);
    // Check new config
    TestEngine::CheckConfiguration(newNodes, configNumber+1);
}

void ReplicaFollowsConfigChangeImpl(bool testingNodeInOldConfig, FakeLegislatorCollection &legislators, RSLNodeCollection &newNodes)
{
    UT_AssertIsTrue(legislators.size() > 0);
    // Pick a primary
    FakeLegislator * primary = legislators[0];
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
    primary->SendChangeConfig(newNodes, NULL, &legislators, testingNodeInOldConfig);
    if (testingNodeInOldConfig)
    {
        TestEngine::ReceiveResponse(Message_VoteAccepted);
    }

    // Send decision
    primary->SendReconfigurationDecision(&legislators, testingNodeInOldConfig);

    // Save the expected status
    StatusResponse status;
    primary->GetStatus(&status);

    // Replica was out of the config set, so it needs to be invited and learn votes as well
    if (testingNodeInOldConfig == false)
    {
        // Send a join to the real replica
        primary->SendJoin();
        // Handle learn votes
        TestEngine::HandleLearnVotes(primary->m_log, false);
    }

    // Wait for results
    UInt32 newState = Legislator::PaxosInactive;
    if (IsTestingNodeInConfig(newNodes))
    {
        newState = Legislator::Initializing;
        if (newNodes.Count() == 1)
        {
            newState = Legislator::StablePrimary;
            status.m_maxBallot = BallotNumber(status.m_maxBallot.m_ballotId+1, TestEngine::TestingNodeMemberId());
            status.m_ballot = status.m_maxBallot;
        }
    }
    TestEngine::WaitForState(newState, status);
}

//------------------------------------
// Join new config
//------------------------------------


void JoinNewConfig(bool testingNodeInOldConfig, Legislator::State finalState,
                   FakeLegislatorCollection &legislators, RSLNodeCollection &newNodes)
{
    bool testingNodeInNewConfig = IsTestingNodeInConfig(newNodes);
    switch (finalState)
    {
        case Legislator::StablePrimary:
            UT_AssertIsTrue(testingNodeInNewConfig);
            break;
        case Legislator::StableSecondary:
            UT_AssertIsTrue(testingNodeInNewConfig && newNodes.Count() > 1);
            break;
        case Legislator::PaxosInactive:
            UT_AssertIsTrue(testingNodeInOldConfig && !testingNodeInNewConfig);
            break;
        default:
            UT_AssertFail("Final State not supported!");
    }

    StatusResponse status;
    TestEngine::GetStatus(&status);

    // LastVote
    Vote * lastVote = VoteFactory::NewEmptyVote((RSLProtocolVersion) status.m_version,
        status.m_decree, status.m_configurationNumber);

    // Update majority
    TestEngine::UpdateMajority(legislators, newNodes, lastVote);
    delete lastVote;

    // Drain messages
    UInt64 queryDecree = 0;
    BallotNumber queryBallot = BallotNumber();
    if (testingNodeInNewConfig && newNodes.Count() > 1)
        //if (newState == Legislator::Initializing)
    {
        // Receive Status query
        Message * query = TestEngine::ReceiveRequests(Message_StatusQuery);
        queryDecree = query->m_decree;
        queryBallot = query->m_ballot;
    }

    // Receive join requests
    size_t numOfRequests = testingNodeInNewConfig ? newNodes.Count() - 1 : newNodes.Count();
    TestEngine::ReceiveRequests(numOfRequests, Message_Join);
    // No messages left
    TestEngine::AssertEmptyQueues();

    if (testingNodeInNewConfig && newNodes.Count() > 1)
        //if (newState == Legislator::Initializing)
    {
        UT_AssertIsTrue(queryDecree != 0);

        if (finalState == Legislator::StablePrimary)
        {
            CallForAll(&legislators, SendStatusResponse(queryDecree, queryBallot, -1));

            // Wait until replica become preparing
            status.m_maxBallot = BallotNumber(status.m_maxBallot.m_ballotId+1, TestEngine::TestingNodeMemberId());
            TestEngine::WaitForState(Legislator::Preparing, status);

            // Accept prepare
            Message * prep = TestEngine::ReceiveRequests(Message_Prepare);
            CallForAll(&legislators, AcceptPrepare(prep));
            CallForAll(&legislators, SendPrepareAccepted());
            // Receive empty vote
            Vote * vote = (Vote *) TestEngine::ReceiveRequests(Message_Vote);
            CallForAll(&legislators, AcceptVote(vote));
            CallForAll(&legislators, SendVoteAccepted());
            UT_AssertAreEqual(0, vote->m_numRequests);

            // Update expected status
            status.m_ballot = status.m_maxBallot;
        }
        else
        {
            CallForAll(&legislators, SendStatusResponse(queryDecree, queryBallot, 0));

            // Pick a primary
            UT_AssertIsTrue(legislators.size() > 0);
            FakeLegislator * primary = legislators[0];

            // Wait for change results
            primary->GetStatus(&status);
            TestEngine::WaitForState(Legislator::StableSecondary, status);

            // Prepapre
            primary->SendPrepare(&legislators);
            TestEngine::ReceiveResponse(Message_PrepareAccepted);
            // Pass empty vote
            primary->SendLastVote(&legislators);
            TestEngine::ReceiveResponse(Message_VoteAccepted);

            // Update expected status
            primary->GetStatus(&status);
        }
    }
    else if (!testingNodeInNewConfig)
        //if (newState == Legislator::PaxosInactive)
    {
        // Pick a primary
        UT_AssertIsTrue(legislators.size() > 0);
        FakeLegislator * primary = legislators[0];
        // Let the replica now that old config is defunct
        primary->SendDefunctConfiguration(status.m_configurationNumber-1);
    }
    // No messages left
    TestEngine::AssertEmptyQueues();

    // Wait for changes
    TestEngine::WaitForState(finalState, status);
}


//------------------------------------
// Helper functions
//------------------------------------

void PrintChange(const char* issuer, TestEngine::State initialState, RSLNodeCollection &oldNodes,
                 Legislator::State finalState, RSLNodeCollection &newNodes)
{
    char buffer[256];
    char oldNodesStr[256] = "";
    for (size_t i = 0; i < oldNodes.Count(); i++)
    {
        if (i > 0) strcat_s(oldNodesStr, ARRAYSIZE(oldNodesStr), ",");
        sprintf_s(buffer, ARRAYSIZE(buffer), "%s", oldNodes[i].m_memberIdString);
        strcat_s(oldNodesStr, ARRAYSIZE(oldNodesStr), buffer);
    }

    char newNodesStr[256] = "";
    for (size_t i = 0; i < newNodes.Count(); i++)
    {
        if (i > 0) strcat_s(newNodesStr, 256, ",");
        sprintf_s(buffer, ARRAYSIZE(buffer), "%s", newNodes[i].m_memberIdString);
        strcat_s(newNodesStr, ARRAYSIZE(newNodesStr), buffer);
    }

    sprintf_s(buffer, ARRAYSIZE(buffer), " %s-[%s]/%s\t->\t[%s]/%s", issuer,
        oldNodesStr, TestEngineStateToStr[initialState],
        newNodesStr, LegislatorStateToStr[finalState]);
    //RSLInfo(buffer);
    printf("%s\n", buffer);
}

bool IsTestingNodeInConfig(RSLNodeCollection &nodes)
{
    return TestEngine::ContainsNode(nodes, TestEngine::m_testingNode.m_memberIdString);
}

RSLNodeCollection BuildArray(RSLNode * node, ...)
{
    RSLNodeCollection list;
    va_list ptr;
    va_start(ptr, node);
    while (node != NULL) {
        list.Append(*node);
        node = va_arg(ptr, RSLNode *);
    }
    va_end(ptr);
    return list;
}

RSLNodeCollection GenerateArray(UInt32 bitMask, RSLNode * allNodes[])
{
    RSLNodeCollection list;
    for (UInt32 i=0; (bitMask > 0); i++, bitMask>>=1)
    {
        if ((bitMask & 1))
        {
            RSLNode * node = allNodes[i];
            list.Append(*node);
        }
    }
    return list;
}

//------------------------------------
// Test cases
//------------------------------------

void TestAddNewReplicas()
{
    // a) [A+,B]->[A,B,C]
    ReplicaIssuesConfigChange_AllFinalStates(BuildArray(A, B, NULL),    BuildArray(A, B, C, NULL));
    ReplicaIssuesConfigChange_AllFinalStates(BuildArray(A, B, C, NULL), BuildArray(A, B, C, D, NULL));

    // b) [A,B+]->[A,B,C]
    ReplicaFollowsConfigChange_AllStates(BuildArray(A, B, NULL),    BuildArray(A, B, C, NULL));
    ReplicaFollowsConfigChange_AllStates(BuildArray(A, B, C, NULL), BuildArray(A, B, C, D, NULL));

    // c) [B+,C]->[A,B,C]
    ReplicaFollowsConfigChange_AllStates(BuildArray(B, C, NULL),    BuildArray(A, B, C, NULL));
    ReplicaFollowsConfigChange_AllStates(BuildArray(B, C, D, NULL), BuildArray(A, B, C, D, NULL));

    // d) [A+]  ->[A,B]
    ReplicaIssuesConfigChange_AllFinalStates(BuildArray(A, NULL), BuildArray(A, B, NULL));

    // e) [B+]  ->[A,B]
    ReplicaFollowsConfigChange_AllStates(BuildArray(B, NULL), BuildArray(A, B, NULL));
}

void TestRemoveReplica()
{
    // a) [A+,B,C]->[A+,B]
    ReplicaIssuesConfigChange_AllFinalStates(BuildArray(A, B, C, NULL),     BuildArray(A, B, NULL));
    ReplicaIssuesConfigChange_AllFinalStates(BuildArray(A, B, C, D, NULL),  BuildArray(A, B, C, NULL));

    // b) [A,B+,C]->[A,B]
    ReplicaFollowsConfigChange_AllStates(BuildArray(A, B, C, NULL),    BuildArray(A, B, NULL));
    ReplicaFollowsConfigChange_AllStates(BuildArray(A, B, C, D, NULL), BuildArray(A, B, C, NULL));

    // c) [A+,B,C]->[B,C]
    ReplicaIssuesConfigChange_AllFinalStates(BuildArray(A, B, C, NULL),     BuildArray(B, C, NULL));
    ReplicaIssuesConfigChange_AllFinalStates(BuildArray(A, B, C, D, NULL),  BuildArray(B, C, D, NULL));

    // d) [A,B+,C]->[B+,C]
    ReplicaFollowsConfigChange_AllStates(BuildArray(A, B, C, NULL),    BuildArray(B, C, NULL));
    ReplicaFollowsConfigChange_AllStates(BuildArray(A, B, C, D, NULL), BuildArray(B, C, D, NULL));
}

void TestAddAndRemoveReplicas()
{
    // a) [A+,B,C]->[A,B,E]
    ReplicaIssuesConfigChange_AllFinalStates(BuildArray(A, B, C, NULL),    BuildArray(A, B, E, NULL));
    ReplicaIssuesConfigChange_AllFinalStates(BuildArray(A, B, C, D, NULL), BuildArray(A, B, E, F, NULL));

    // b) [A+,B]  ->[A,D]
    ReplicaIssuesConfigChange_AllFinalStates(BuildArray(A, B, NULL),    BuildArray(A, D, NULL));
    ReplicaIssuesConfigChange_AllFinalStates(BuildArray(A, B, C, NULL), BuildArray(A, D, E, NULL));

    // a) [A+,B]->[D,E]
    ReplicaIssuesConfigChange_AllFinalStates(BuildArray(A, B, NULL),    BuildArray(D, E, NULL));
    ReplicaIssuesConfigChange_AllFinalStates(BuildArray(A, B, C, NULL), BuildArray(D, E, F, NULL));

    // b) [A,B+]->[C,D]
    ReplicaFollowsConfigChange_AllStates(BuildArray(A, B, NULL),    BuildArray(D, E, NULL));
    ReplicaFollowsConfigChange_AllStates(BuildArray(A, B, C, NULL), BuildArray(D, E, F, NULL));

    // c) [A+]->[B]
    ReplicaIssuesConfigChange_AllFinalStates(BuildArray(A, NULL), BuildArray(B, NULL));

    // d) [B+]->[A]
    ReplicaFollowsConfigChange_AllStates(BuildArray(B, NULL), BuildArray(A, NULL));
}

void TestNoChanges()
{
    // a) [A+,B,C]->[A+,B,C]
    ReplicaIssuesConfigChange_AllFinalStates(BuildArray(A, B, C, NULL),    BuildArray(A, B, C, NULL));
    ReplicaIssuesConfigChange_AllFinalStates(BuildArray(A, B, C, D, NULL), BuildArray(A, B, C, D, NULL));

    // b) [A,B+,C]->[A,B+,C]
    ReplicaFollowsConfigChange_AllStates(BuildArray(A, B, C, NULL),    BuildArray(A, B, C, NULL));
    ReplicaFollowsConfigChange_AllStates(BuildArray(A, B, C, D, NULL), BuildArray(A, B, C, D, NULL));

    // c) [A+]    ->[A+]
    ReplicaIssuesConfigChange_AllFinalStates(BuildArray(A, NULL), BuildArray(A, NULL));
}

void TestAllPossibleOperationsFor6Replicas()
{
    //// Same as TestAddNewReplica_asPrimary
    //ReplicaIssuesConfigChange(BuildArray(A, B, C, NULL), Legislator::StablePrimary, BuildArray(A, B, C, D, NULL));
    //// Same as TestAddNewReplica_asSecondary
    //ReplicaFollowsConfigChange(TestEngine::Secondary, BuildArray(A, B, C, NULL), Legislator::StableSecondary, BuildArray(A, B, C, D, NULL));

    //RSLNode * allNodes[] = {A, B, C, D, E, F};
    //UInt32 bitMask = (1<<(LengthOf(allNodes)))-1;
    //for (UInt32 oldSet=1; oldSet <= bitMask ; oldSet++)
    //{
    //    RSLNodeCollection oldNodes = GenerateArray(oldSet, allNodes);
    //    bool testtingNodeInOldConfig = IsTestingNodeInConfig(oldNodes);
    //    for (UInt32 newSet=1; newSet <= bitMask ; newSet++)
    //    {
    //        RSLNodeCollection newNodes = GenerateArray(newSet, allNodes);
    //        if (testtingNodeInOldConfig)
    //        {
    //            ReplicaIssuesConfigChange_AllFinalStates(oldNodes, newNodes);
    //        }
    //        if (testtingNodeInOldConfig || IsTestingNodeInConfig(newNodes))
    //        {
    //            ReplicaFollowsConfigChange_AllStates(oldNodes, newNodes);
    //        }
    //    }
    //}
}

void TestNewConfigOldBallot()
{
    StatusResponse status;

    RSLNode *X = A, *Y = C, *Z = D;
    RSLNodeCollection XYZ_nodes = BuildArray(X, Y, Z, NULL);
    RSLNodeCollection Z_nodes = BuildArray(Z, NULL);
    RSLNodeCollection XZ_nodes = BuildArray(X, Z, NULL);

    FakeLegislator yLeg(Y->m_memberIdString);
    FakeLegislator zLeg(Z->m_memberIdString);
    FakeLegislatorCollection Y_legs(&yLeg, NULL);

    // X is a secondary
    TestEngine::MoveToConfiguration(XYZ_nodes);
    TestEngine::MoveToSecondary();

    TestEngine::GetStatus(&status);
    UInt32 configNumber = status.m_configurationNumber;
    yLeg.Reset(status);
    zLeg.Reset(status);

    // Establish Z as primary and pass vote (D, B, Z)
    zLeg.SendPrepare(&Y_legs);
    TestEngine::ReceiveResponse(Message_PrepareAccepted);
    zLeg.SendLastVote(&Y_legs);
    TestEngine::ReceiveResponse(Message_VoteAccepted);

    // - Z proposes decree a reconfiguration decree (D, B, Z) and logs it.
    //   The new configuration has only Z as the member
    // - X and Y log and accept this decree. But they lose network connectivity with Z as soon they log it
    zLeg.SendChangeConfig(Z_nodes, NULL, &Y_legs);
    TestEngine::ReceiveResponse(Message_VoteAccepted);
    // - Z logs the reconfigurationdecision (D, B) and becomes the primary
    zLeg.SendReconfigurationDecision(NULL, false);
    // Z becomes primary
    zLeg.SendPrepare(NULL, false);
    zLeg.SendLastVote(NULL, false);
    // Pass couple decrees
    zLeg.SendVote(NULL, false);
    zLeg.SendVote(NULL, false);
    zLeg.SendVote(NULL, false);

    // - Meanwhile X and Y realize that primary is not up, Y prepares becomes primary and
    //   X, Y both log the reconfiguration decree (D, B+1, Y)
    // Prepare
    yLeg.SendPrepare(NULL, true);
    TestEngine::ReceiveResponse(Message_PrepareAccepted);
    // Vote (D, B, Z)
    yLeg.SendLastVote(NULL, true);
    TestEngine::ReceiveResponse(Message_VoteAccepted);
    // Wait for X
    yLeg.GetStatus(&status);
    TestEngine::WaitForState(Legislator::StableSecondary, status);

    // Pass decision
    yLeg.SendReconfigurationDecision(NULL, true);
    // - Y dies, so X has (D, B+1, Y) as the last message.

    // Drain X's join message (there is only Z in new config)
    TestEngine::ReceiveRequests(1, Message_Join);

    // Wait X to change to new configuration
    yLeg.GetStatus(&status);
    TestEngine::WaitForState(Legislator::PaxosInactive, status);

    // - After some time another configuration change happens to change the configuration back to (X, Z)
    zLeg.SendChangeConfig(XZ_nodes, NULL, NULL, false);
    // Z logs reconfiguration decision
    zLeg.SendReconfigurationDecision(NULL, false);

    // - X regains network connectivity with Z
    // - Z send join message to X
    zLeg.SendJoin();

    // - X tries to learn from Z. But, it gets the reconfiguration decision (D, B, Z) whose
    //   ballot is less than B+1. So, it ignores the message and never switches to the new paxos state
    // X receives a JoinMessage from Z and start learning from Z
    TestEngine::HandleLearnVotes(zLeg.m_log, false);

    // Wait for X to be with Z's status (if learns succeed)
    zLeg.GetStatus(&status);
    TestEngine::WaitForState(Legislator::Initializing, status);

    // After learning, X goes to Initializing state where it queries for status
    Message * query = TestEngine::ReceiveRequests(1, Message_StatusQuery);
    UInt64 queryDecree = query->m_decree;
    BallotNumber queryBallot = query->m_ballot;

    // Drain X's join message to Z
    TestEngine::ReceiveRequests(1, Message_Join);
    // No messages left
    TestEngine::AssertEmptyQueues();

    // Send query's reponse
    zLeg.SendStatusResponse(queryDecree, queryBallot, 0);

    // Wait for X
    zLeg.GetStatus(&status);
    TestEngine::WaitForState(Legislator::StableSecondary, status);

    // Z becomes primary
    zLeg.SendPrepare(NULL);
    TestEngine::ReceiveResponse(Message_PrepareAccepted);
    zLeg.SendLastVote(NULL);
    TestEngine::ReceiveResponse(Message_VoteAccepted);

    // Check results
    TestEngine::CheckConfiguration(XZ_nodes, configNumber+2);
}

void TestABC_BCD_CDE_ADE()
{
    StatusResponse status;

    RSLNodeCollection ABC_nodes = BuildArray(A, B, C, NULL);
    RSLNodeCollection BCD_nodes = BuildArray(B, C, D, NULL);
    RSLNodeCollection CDE_nodes = BuildArray(C, D, E, NULL);
    RSLNodeCollection ADE_nodes = BuildArray(A, D, E, NULL);

    FakeLegislator bLeg(B->m_memberIdString);
    FakeLegislator cLeg(C->m_memberIdString);
    FakeLegislator dLeg(D->m_memberIdString);
    FakeLegislator eLeg(E->m_memberIdString);

    FakeLegislatorCollection BC_legs(&bLeg, &cLeg, NULL);
    FakeLegislatorCollection BCD_legs(&bLeg, &cLeg, &dLeg, NULL);
    FakeLegislatorCollection CDE_legs(&cLeg, &dLeg, &eLeg, NULL);
    FakeLegislatorCollection DE_legs(&dLeg, &eLeg, NULL);

    TestEngine::MoveToConfiguration(ABC_nodes);

    //
    // ABC
    //
    TestEngine::MoveToPrimary(true);
    TestEngine::GetStatus(&status);
    UInt32 configNumber = status.m_configurationNumber;
    CallForAll(&BC_legs, Reset(status));
    TestEngine::ApplyConfigurationChangedResults(&status);

    // Place the change
    RSLResponseCode res = TestEngine::m_machine->SubmitChangeConfig(BCD_nodes);
    UT_AssertIsTrue(res == RSLSuccess);
    // Receive and accept reconfiguration vote
    Vote * vote = (Vote *) TestEngine::ReceiveRequests(Message_Vote);
    CallForAll(&BC_legs, AcceptVote(vote));
    CallForAll(&BC_legs, SendVoteAccepted());
    // Receive and accept reconfiguration decision
    Message * msg = TestEngine::ReceiveRequests(Message_ReconfigurationDecision);
    CallForAll(&BC_legs, AcceptReconfigurationDecision(msg));
    // Make D learn from B
    dLeg.CopyFrom(bLeg);
    // Wait for results
    bLeg.GetStatus(&status);
    TestEngine::WaitForState(Legislator::PaxosInactive, status);

    // Drain X's join message to B, C and D
    TestEngine::ReceiveRequests(3, Message_Join);
    // No messages left
    TestEngine::AssertEmptyQueues();
    // Send defunct notification
    bLeg.SendDefunctConfiguration(configNumber);

    // Check results
    TestEngine::CheckConfiguration(BCD_nodes, configNumber+1);

    //
    // BCD
    //
    // D become primary
    dLeg.SendPrepare(&BCD_legs, false);
    dLeg.SendLastVote(&BCD_legs, false);
    // Place config change
    dLeg.SendChangeConfig(CDE_nodes, NULL, &BCD_legs, false);
    dLeg.SendReconfigurationDecision(&BCD_legs, false);
    // Make E learn from C
    eLeg.CopyFrom(cLeg);

    //
    // CDE
    //
    // E become primary
    eLeg.SendPrepare(&CDE_legs, false);
    eLeg.SendLastVote(&CDE_legs, false);
    // Place config change
    eLeg.SendChangeConfig(ADE_nodes, NULL, &CDE_legs, false);
    eLeg.SendReconfigurationDecision(&CDE_legs, false);

    // Invite A
    eLeg.SendJoin();
    // Make A learn from E
    TestEngine::HandleLearnVotes(eLeg.m_log, false);

    // Wait for A to be with E's status (if learns succeed)
    eLeg.GetStatus(&status);
    TestEngine::WaitForState(Legislator::Initializing, status);

    // Receive A's status queries
    Message * query = TestEngine::ReceiveRequests(2, Message_StatusQuery);
    UInt64 queryDecree = query->m_decree;
    BallotNumber queryBallot = query->m_ballot;

    // Drain X's join message to D and E
    TestEngine::ReceiveRequests(2, Message_Join);
    // No messages left
    TestEngine::AssertEmptyQueues();

    // Send query's reponse
    CallForAll(&DE_legs, SendStatusResponse(queryDecree, queryBallot, 0));

    //
    // ADE
    //

    // Wait for X
    eLeg.GetStatus(&status);
    TestEngine::WaitForState(Legislator::StableSecondary, status);

    // D becomes primary
    dLeg.SendPrepare(NULL);
    TestEngine::ReceiveResponse(Message_PrepareAccepted);
    dLeg.SendLastVote(NULL);
    TestEngine::ReceiveResponse(Message_VoteAccepted);

    // Check results
    TestEngine::CheckConfiguration(ADE_nodes, configNumber+3);
}

void TestJoinMessages()
{
    TestEngine::MoveToPaxosInactive();
    TestJoinMessages(TestEngine::PaxosInactive);

    // Other states
    TestEngine::State STATES[] =
    {
        TestEngine::Primary,
        TestEngine::Secondary,
        TestEngine::SecondaryPrepared,
        TestEngine::Prepare,
        TestEngine::Initialize,
        TestEngine::InitializePrepared
    };
    for (int i=0; i < LengthOf(STATES); i++)
    {
        TestEngine::MoveToDefaultConfig();
        TestEngine::MoveToState(STATES[i]);
        TestJoinMessages(STATES[i]);
    }
}

void TestJoinMessages(TestEngine::State state)
{
    printf(" sending join messages (%s)...\n", TestEngineStateToStr[state]);

    StatusResponse status;
    FakeLegislatorCollection legislators;
    TestEngine::GetSyncedFakeLegislatorsMajority(legislators, &status);
    Legislator::State oldState = (Legislator::State) status.m_state;
    UInt32 highestDefunctConfig = TestEngine::GetLegislator()->GetHighestDefunctConfigurationNumber();
    UT_AssertIsTrue(legislators.size() > 0);
    FakeLegislator * fakeLeg = legislators[0];
    Message * msg = NULL;

    // Join.configNumber <= Replica.currentConfigNumber
    for (int configNumberVar = -1; configNumberVar <= 0; configNumberVar++)
    {
        for (int minDecreeLogVar = -1; minDecreeLogVar <= +1; minDecreeLogVar++)
        {
            for (int chkpointedDecreeVar = -1; chkpointedDecreeVar <= +1; chkpointedDecreeVar++)
            {
                // Send lower config number
                fakeLeg->SendJoin(
                    status.m_configurationNumber + configNumberVar,
                    status.m_decree + minDecreeLogVar,
                    status.m_decree + chkpointedDecreeVar);

                // Machine knows this configuration as defunct, handle DefunctConfig message then
                if (status.m_configurationNumber + configNumberVar <= highestDefunctConfig+1)
                {
                    msg = TestEngine::ReceiveResponse(Message_DefunctConfiguration);
                    UT_AssertIsTrue(msg->m_configurationNumber == highestDefunctConfig);
                }
                // Make sure no messages were sent
                TestEngine::AssertEmptyQueues();
                // Make sure that nothing has changed
                TestEngine::WaitForState(oldState, status);
            }
        }
    }

    // Join.configNumber > Replica.currentConfigNumber
    for (int minDecreeLogVar = -1; minDecreeLogVar <= +1; minDecreeLogVar++)
    {
        for (int chkpointedDecreeVar = -1; chkpointedDecreeVar <= +1; chkpointedDecreeVar++)
        {
            // Send lower config number
            fakeLeg->SendJoin(
                status.m_configurationNumber + 1,
                status.m_decree + minDecreeLogVar,
                status.m_decree + chkpointedDecreeVar);

            if (minDecreeLogVar <= 0) // Machine can learn from logs
            {
                FetchRequest *req = TestEngine::ReceiveFetchRequest(Message_FetchVotes);
                UT_AssertIsNotNull(req);
                delete req;
            } else if (chkpointedDecreeVar >= 0) // Machine wants to copy the checkpoint
            {
                FetchRequest *req = TestEngine::ReceiveFetchRequest(Message_FetchCheckpoint);
                UT_AssertIsNotNull(req);
                delete req;
            }

            // Make sure no messages were sent
            TestEngine::AssertEmptyQueues();
            // Make sure that nothing has changed
            TestEngine::WaitForState(oldState, status);
        }
    }
}

void TestDefunctMessages()
{
    SendDefunctMessages(Legislator::PaxosInactive);
    SendDefunctMessages(Legislator::StableSecondary);

    ReceiveDefunctMessages(Legislator::PaxosInactive);
    ReceiveDefunctMessages(Legislator::StablePrimary);
    ReceiveDefunctMessages(Legislator::Preparing);
    ReceiveDefunctMessages(Legislator::Initializing);
}

void SendDefunctMessages(Legislator::State state)
{
    switch (state){
        case Legislator::PaxosInactive:
            TestEngine::MoveToPaxosInactive();
            break;
        case Legislator::StableSecondary:
            TestEngine::MoveToDefaultConfig();
            TestEngine::MoveToState(state);
            break;
        default:
            UT_AssertFail("State not supported");
            break;
    }

    // Gather current state
    StatusResponse status;
    FakeLegislatorCollection legislators;
    TestEngine::GetSyncedFakeLegislatorsMajority(legislators, &status);
    UT_AssertIsTrue(status.m_state == (UInt32) state);
    UInt32 highestDefunct = TestEngine::GetLegislator()->GetHighestDefunctConfigurationNumber();
    UT_AssertIsTrue(legislators.size() > 0);

    // Replica.C == Replica.HDC+1
    UT_AssertIsTrue(status.m_configurationNumber == highestDefunct+1);

    // Send Defunct.configNumber < Replica.HDC
    legislators[0]->SendDefunctConfiguration(highestDefunct-1);
    // Nothing changes
    UT_AssertIsTrue(status.m_state == (UInt32) state);
    UT_AssertIsTrue(highestDefunct == TestEngine::GetLegislator()->GetHighestDefunctConfigurationNumber());

    // Send Defunct.configNumber == Replica.HDC
    legislators[0]->SendDefunctConfiguration(highestDefunct);
    // Nothing changes
    UT_AssertIsTrue(status.m_state == (UInt32) state);
    UT_AssertIsTrue(highestDefunct == TestEngine::GetLegislator()->GetHighestDefunctConfigurationNumber());

    // Send Defunct.configNumber > Replica.HDC
    legislators[0]->SendDefunctConfiguration(highestDefunct+1);
    if (state == Legislator::PaxosInactive)
    {
        WaitUntil(highestDefunct+1 == TestEngine::GetLegislator()->GetHighestDefunctConfigurationNumber());
        TestEngine::WaitForState(Legislator::PaxosInactive, status);
    }
    else
    {
        TestEngine::WaitForState(Legislator::Initializing, status);
        // Receive status queries
        Message * query = TestEngine::ReceiveRequests(Message_StatusQuery);
        UInt64 queryDecree = query->m_decree;
        BallotNumber queryBallot = query->m_ballot;
        // There are no join requests since highestDefunct > currentConfig
        // No messages left
        TestEngine::AssertEmptyQueues();
        // Make it go to StableSecondary
        CallForAll(&legislators, SendStatusResponse(queryDecree, queryBallot, 0));
        // Wait for replica
        TestEngine::WaitForState(Legislator::StableSecondary, status);
    }
}

void ReceiveDefunctMessages(Legislator::State state)
{
    switch (state)
    {
        case Legislator::PaxosInactive:
            TestEngine::MoveToPaxosInactive();
            break;
        case Legislator::StablePrimary:
        case Legislator::StableSecondary:
        case Legislator::Preparing:
        case Legislator::Initializing:
            TestEngine::MoveToDefaultConfig();
            TestEngine::MoveToState(state);
            break;
        default:
            UT_AssertFail("State not supported");
            break;
    }

    // Gather current state
    StatusResponse status;
    FakeLegislatorCollection legislators;
    TestEngine::GetSyncedFakeLegislatorsMajority(legislators, &status);
    UInt32 highestDefunct = TestEngine::GetLegislator()->GetHighestDefunctConfigurationNumber();
    UT_AssertIsTrue(legislators.size() > 0);
    FakeLegislator * fake = legislators[0];
    fake->m_configNumber = highestDefunct;
    Message * msg = NULL;

    // Replica.C == Replica.HDC+1
    UT_AssertIsTrue(status.m_configurationNumber == highestDefunct+1);

    // Vote related messages
    TestEngine::SendRequest(fake->NewVote());
    msg = TestEngine::ReceiveResponse(Message_DefunctConfiguration);
    UT_AssertIsTrue(highestDefunct == msg->m_configurationNumber);

    RSLNodeCollection nodes;
    TestEngine::SendRequest(fake->NewVote(nodes, NULL));
    msg = TestEngine::ReceiveResponse(Message_DefunctConfiguration);
    UT_AssertIsTrue(highestDefunct == msg->m_configurationNumber);

    TestEngine::SendRequest(fake->NewReconfigurationDecision());
    msg = TestEngine::ReceiveResponse(Message_DefunctConfiguration);
    UT_AssertIsTrue(highestDefunct == msg->m_configurationNumber);

    // Prepare related messages
    TestEngine::SendRequest(fake->NewPrepare());
    msg = TestEngine::ReceiveResponse(Message_DefunctConfiguration);
    UT_AssertIsTrue(highestDefunct == msg->m_configurationNumber);

    fake->AppendLog(fake->NewVote());
    TestEngine::SendRequest(fake->NewPrepareAccepted());
    msg = TestEngine::ReceiveResponse(Message_DefunctConfiguration);
    UT_AssertIsTrue(highestDefunct == msg->m_configurationNumber);

    TestEngine::SendRequest(fake->NewNotAccepted());
    msg = TestEngine::ReceiveResponse(Message_DefunctConfiguration);
    UT_AssertIsTrue(highestDefunct == msg->m_configurationNumber);

    // Status related messages
    TestEngine::SendRequest(fake->NewStatusQuery());
    // Status query bypass configuration checking
    StatusResponse * resp = (StatusResponse *) TestEngine::ReceiveResponse(Message_StatusResponse);
    UT_AssertIsTrue(status.m_decree == resp->m_queryDecree);
    UT_AssertIsTrue(status.m_ballot == resp->m_queryBallot);
    UT_AssertIsTrue(status.m_configurationNumber == resp->m_configurationNumber);

    TestEngine::SendRequest(fake->NewStatusResponse(status.m_decree, status.m_ballot, 0));
    msg = TestEngine::ReceiveResponse(Message_DefunctConfiguration);
    UT_AssertIsTrue(highestDefunct == msg->m_configurationNumber);

    // Defunct related messages
    TestEngine::SendRequest(fake->NewDefunctConfiguration(highestDefunct-1));
    TestEngine::AssertEmptyQueues();

    // Join related messages
    TestEngine::SendRequest(fake->NewJoin());
    msg = TestEngine::ReceiveResponse(Message_DefunctConfiguration);
    UT_AssertIsTrue(highestDefunct == msg->m_configurationNumber);

    TestEngine::SendRequest(fake->NewJoinRequest());
    msg = TestEngine::ReceiveResponse(Message_DefunctConfiguration);
    UT_AssertIsTrue(highestDefunct == msg->m_configurationNumber);

    TestEngine::AssertEmptyQueues();
}

void TestPrimaryChangeDuringReconfiguration()
{
    TestPrimaryChangeDuringReconfiguration(false, false);
    TestPrimaryChangeDuringReconfiguration(false, true);
    TestPrimaryChangeDuringReconfiguration(true, false);
}

void TestPrimaryChangeDuringReconfiguration(bool replicaStarts, bool replicaFinishes)
{
    StatusResponse status;
    RSLNodeCollection ABCD_nodes = BuildArray(A, B, C, D, NULL);
    RSLNodeCollection ABCEF_nodes = BuildArray(A, B, C, E, F, NULL);
    FakeLegislator bLeg(B->m_memberIdString);
    FakeLegislator cLeg(C->m_memberIdString);
    FakeLegislatorCollection legislators(&bLeg, &cLeg, NULL);

    // Set configuration to ABC
    TestEngine::MoveToConfiguration(ABCD_nodes);
    TestEngine::GetStatus(&status);
    UInt32 configNumber = status.m_configurationNumber;

    //
    // Send Reconfiguration Vote
    //
    if (replicaStarts)
    {
        TestEngine::MoveToPrimary(true);
        TestEngine::GetStatus(&status);
        CallForAll(&legislators, Reset(status));
        // In case replica does not finish configuration change
        if (replicaFinishes == false)
        {
            // There should be an abort
            TestEngine::m_machine->m_mustAbortConfigurationChange = true;
        }
        // Place the change
        RSLResponseCode res = TestEngine::m_machine->SubmitChangeConfig(ABCEF_nodes);
        UT_AssertIsTrue(res == RSLSuccess);
        // Receive and accept reconfiguration vote
        Vote * vote = (Vote *) TestEngine::ReceiveRequests(Message_Vote);
        CallForAll(&legislators, AcceptVote(vote));
        TestEngine::GetStatus(&status);
    }
    else
    {
        CallForAll(&legislators, Reset(status));
        // A fake replica issues configuration change
        bLeg.SendPrepare(&legislators);
        TestEngine::ReceiveResponse(Message_PrepareAccepted);
        bLeg.SendLastVote(&legislators);
        TestEngine::ReceiveResponse(Message_VoteAccepted);
        // Place the change
        bLeg.SendChangeConfig(ABCEF_nodes, NULL, &legislators);
        TestEngine::ReceiveResponse(Message_VoteAccepted);
        bLeg.GetStatus(&status);
    }
    TestEngine::AssertEmptyQueues();

    //
    // Send Reconfiguration Decision
    //
    if (replicaFinishes)
    {
        // Make replica start preparing
        TestEngine::GetLegislator()->SetNextElectionTime(0);
        // Replica will time out and start preparing
        Message * query = TestEngine::ReceiveRequests(Message_StatusQuery);
        CallForAll(&legislators, SendStatusResponse(query->m_decree, query->m_ballot, -1));
        // Make sure it is preparing
        status.m_maxBallot = BallotNumber(status.m_maxBallot.m_ballotId+1, TestEngine::TestingNodeMemberId());
        TestEngine::WaitForState(Legislator::Preparing, status);

        // Accept prepare
        Message * prep = TestEngine::ReceiveRequests(Message_Prepare);
        CallForAll(&legislators, AcceptPrepare(prep));
        CallForAll(&legislators, SendPrepareAccepted());
        // Receive last vote
        Vote * vote = (Vote *) TestEngine::ReceiveRequests(Message_Vote);
        CallForAll(&legislators, AcceptVote(vote));
        CallForAll(&legislators, SendVoteAccepted());
        UT_AssertIsTrue(vote->m_isReconfiguration);
        UT_AssertAreEqual(0, vote->m_numRequests);

        // Receive decision
        Message * dec = TestEngine::ReceiveRequests(Message_ReconfigurationDecision);
        CallForAll(&legislators, AcceptReconfigurationDecision(dec));
    }
    else
    {
        // A fake replica issues configuration change
        cLeg.SendPrepare(&legislators);
        TestEngine::ReceiveResponse(Message_PrepareAccepted);
        cLeg.SendLastVote(&legislators);
        TestEngine::ReceiveResponse(Message_VoteAccepted);
        // Send decision
        cLeg.SendReconfigurationDecision(&legislators);

        // Check for any abort notification pending
        UT_AssertIsTrue(TestEngine::m_machine->m_mustAbortConfigurationChange == false);
    }

    // Wait for replica
    cLeg.GetStatus(&status);
    TestEngine::WaitForState(Legislator::Initializing, status);
    // Update majority
    TestEngine::UpdateMajority(legislators, ABCEF_nodes);

    //
    // Perform Join
    //

    // Receive status queries
    Message * query = TestEngine::ReceiveRequests(Message_StatusQuery);
    UInt64 queryDecree = query->m_decree;
    BallotNumber queryBallot = query->m_ballot;
    // Receive join requests
    TestEngine::ReceiveRequests(Message_Join);
    // No messages left
    TestEngine::AssertEmptyQueues();
    // Make it go to StableSecondary
    CallForAll(&legislators, SendStatusResponse(queryDecree, queryBallot, 0));

    // Wait for replica
    TestEngine::WaitForState(Legislator::StableSecondary, status);

    // A fake replica becomes the primary and sends last (empty) vote
    cLeg.SendPrepare(&legislators);
    TestEngine::ReceiveResponse(Message_PrepareAccepted);
    cLeg.SendLastVote(&legislators);
    TestEngine::ReceiveResponse(Message_VoteAccepted);

    // Wait for replica
    cLeg.GetStatus(&status);
    TestEngine::WaitForState(Legislator::StableSecondary, status);

    // Check new config
    TestEngine::CheckConfiguration(ABCEF_nodes, configNumber+1);
}

void TestConfigChangesDuringRestore(vector<RSLNodeCollection> &configHistory, const char* testingReplicaMemberId,
                                    bool checkpoint, int numOfVotes)
{
    UT_AssertIsTrue(configHistory.size() >= 2);
    map<UInt64, RSLNodeCollection*> configsToFix;
    UInt16 newPort = TestEngine::GetGPort();
    UInt64 chkptDecree = _I64_MAX;

    //
    // Perform changes with real replica
    //
    for (size_t i=0; i < configHistory.size(); i++)
    {
        UT_AssertIsTrue(TestEngine::ContainsNode(configHistory[i], TestEngine::m_testingNode.m_memberIdString));

        FakeLegislatorCollection legislators;
        StatusResponse status;
        // MoveToConfiguration will leave replica as Primary
        TestEngine::MoveToConfiguration(configHistory[i]);
        TestEngine::GetSyncedFakeLegislatorsMajority(legislators, &status);

        // Keep history of changes
        UInt64 reconfigDecree = status.m_decree - 1;
        configsToFix[reconfigDecree] = &configHistory[i];
        // Fix ports
        for (size_t j=0; j < configHistory[i].Count(); j++)
        {
            RSLNode * node = &configHistory[i][j];
            node->m_rslPort = (MemberId::Compare(testingReplicaMemberId, node->m_memberIdString) == 0) ? newPort : newPort+10;
        }

        // First configuration change must checkpoint
        if (checkpoint && i == 0)
        {
            UT_AssertIsTrue(status.m_state == Legislator::StablePrimary);
            chkptDecree = status.m_decree + 1;
            TestEngine::m_machine->m_setSaveStateAtDecree = chkptDecree;
        }

        // Pass a vote to ensure that configuration has been estabilished
        for (int j=0; j < numOfVotes; j++)
        {
            UT_AssertIsTrue(status.m_state == Legislator::StablePrimary);
            status.m_decree++;
            TestEngine::m_machine->SubmitRequest(status.m_decree);
            Vote * vote = (Vote *) TestEngine::ReceiveRequests(Message_Vote);
            CallForAll(&legislators, AcceptVote(vote));
            CallForAll(&legislators, SendVoteAccepted());
            // Wait for vote to be processed
            TestEngine::WaitForState(Legislator::StablePrimary, status);
            TestEngine::GetStatus(&status);
        }
    }
    UT_AssertIsTrue(chkptDecree != _I64_MAX);
    // Record expected configuration info
    StatusResponse expectedStatus;
    TestEngine::GetStatus(&expectedStatus);

    //
    // Create and fix logs for restore test
    //
    RSLConfigParam cfgParam;
    RSLNodeCollection initialNodes = configHistory[0];
    TestEngine::GenerateConfigFile(initialNodes, 2, cfgParam);
    TestEngine::CompareNodeArrays(configHistory[0], initialNodes);
    RSLProtocolVersion version = (RSLProtocolVersion) TestEngine::m_machine->m_version;

    DynString cmd, dest(cfgParam.m_workingDir), src;

    dest.AppendF("\\%s\\", testingReplicaMemberId);
    // Clear destination
    cmd.AppendF("rmdir /s /Q %s 2>NUL", dest.Str());
    system(cmd);
    UT_AssertIsTrue(CreateDirectoryA(dest, NULL));

    // Calculate source
    TestEngine::m_cfg.GetWorkingDir(src);
    src.AppendF("\\%s\\", TestEngine::TestingNodeMemberId());
    // Copy to destination
    cmd.Clear();
    cmd.AppendF("copy /Y %s* %s 1>NUL", src.Str(), dest.Str());
    system(cmd);

    // Fix checkpoint
    DynString cpFile;
    cpFile.AppendF("%s\\%I64u.codex", dest.Str(), chkptDecree);
    if (version >= RSLProtocolVersion_3)
    {
        RSLMemberSet memberset(initialNodes, NULL, 0);
        UT_AssertIsTrue(RSLCheckpointUtility::ChangeReplicaSet(cpFile, &memberset));
    }
    // Fix logs
    char *logPrefix = "*.log";
    vector<UInt64> logs;
    LogAssert(Legislator::GetFileNumbers(dest, logPrefix, logs) == NO_ERROR);
    for (int i = (int)logs.size()-1; i >= 0; i--)
    {
        UT_AssertIsTrue(Legislator::ForDebuggingPurposesUpdateLogFile(dest, logs[i], configsToFix));
        if (logs[i] < chkptDecree)
        {
            break;
        }
    }

    //
    // Perform restore test
    //
    TestStateMachine *sm = new TestStateMachine(version);
    RSLNode testingNode;
    strncpy_s(testingNode.m_memberIdString, ARRAYSIZE(testingNode.m_memberIdString), testingReplicaMemberId, sizeof(testingNode.m_memberIdString));
    testingNode.m_rslPort = newPort;
    UT_AssertIsTrue(sm->Initialize(&cfgParam, configHistory.back(), testingNode, version, true));

    Sleep(500);

    //
    // Check final state
    //
    RSLNodeCollection actualNodeArray;
    UInt32 actualConfigNumber;
    UInt32 actualHighestDefunct;
    TestEngine::GetConfigurationInfo(sm, &actualNodeArray, &actualConfigNumber, &actualHighestDefunct);
    Message query(version, Message_StatusQuery, MemberId(testingReplicaMemberId), 0, actualConfigNumber, BallotNumber());
    TestEngine::SendMessage(&query, true, newPort);
    StatusResponse *resp = (StatusResponse *) TestEngine::ReceiveResponse(Message_StatusResponse, false);
    // Configuration
    UT_AssertIsTrue(expectedStatus.m_configurationNumber    == actualConfigNumber);
    UT_AssertIsTrue(expectedStatus.m_configurationNumber-1  == actualHighestDefunct);
    TestEngine::CompareNodeArrays(configHistory.back(), actualNodeArray);
    // Status
    Legislator::State expectedState = Legislator::PaxosInactive;
    if (TestEngine::ContainsNode(configHistory.back(), testingNode.m_memberIdString))
    {
        expectedState = (configHistory.back().Count() > 1) ?
            Legislator::Initializing :
            Legislator::StablePrimary;
    }
    UT_AssertIsTrue((UInt32)expectedState       == resp->m_state);
    UT_AssertIsTrue(expectedStatus.m_decree     == resp->m_decree);
    UT_AssertIsTrue(expectedStatus.m_ballot     == resp->m_ballot);
    UT_AssertIsTrue(resp->m_maxBallot           >= expectedStatus.m_maxBallot);
    UT_AssertIsTrue(expectedStatus.m_decree-1   == sm->GetCurrentSequenceNumber());
    sm->Pause();
}

void TestConfigChangesDuringRestore()
{
    vector<RSLNodeCollection> changes;

    printf("[Restore replica as part of new configuration]\n");
    changes.clear();
    changes.push_back(BuildArray(A, B, C, NULL));
    changes.push_back(BuildArray(A, B, C, D, NULL));
    TestConfigChangesDuringRestore(changes, D->m_memberIdString, true, 5);

    printf("[Restore replica NOT as part of new configuration]\n");
    changes.clear();
    changes.push_back(BuildArray(A, B, C, D, NULL));
    changes.push_back(BuildArray(A, B, C, NULL));
    TestConfigChangesDuringRestore(changes, D->m_memberIdString, true, 5);

    printf("[Replica as part of configuration momentarily]\n");
    changes.clear();
    changes.push_back(BuildArray(A, B, C, NULL));
    changes.push_back(BuildArray(A, B, C, D, NULL));
    changes.push_back(BuildArray(A, B, C, NULL));
    TestConfigChangesDuringRestore(changes, D->m_memberIdString, true, 5);

    printf("[Replica as part of configuration at the beginning and at the end]\n");
    changes.clear();
    changes.push_back(BuildArray(A, B, C, D, NULL));
    changes.push_back(BuildArray(A, B, C, NULL));
    changes.push_back(BuildArray(A, B, C, D, NULL));
    TestConfigChangesDuringRestore(changes, D->m_memberIdString, true, 5);

    printf("[Same set (no changes)]\n");
    changes.clear();
    changes.push_back(BuildArray(A, B, C, D, NULL));
    changes.push_back(BuildArray(A, B, C, D, NULL));
    TestConfigChangesDuringRestore(changes, D->m_memberIdString, true, 5);

    printf("[Never part of configuration]\n");
    changes.clear();
    changes.push_back(BuildArray(A, B, NULL));
    changes.push_back(BuildArray(A, C, NULL));
    changes.push_back(BuildArray(A, D, NULL));
    TestConfigChangesDuringRestore(changes, E->m_memberIdString, true, 5);
}

//----------------------------------------------------------------------------
// Bootstrap Tests
//----------------------------------------------------------------------------

void InitBootstrapTests()
{
    TestEngine::EnsureVersion(RSLProtocolVersion_4);
}

void VerifyMemberSetIsEmpty(TestStateMachine * sm)
{
    RSLNodeCollection actualNodeArray;
    TestEngine::GetConfigurationInfo(sm, &actualNodeArray, NULL, NULL);

    RSLNodeCollection emptySet;
    TestEngine::CompareNodeArrays(emptySet, actualNodeArray);
}

void GetStatus(StatusResponse * status, const char* sender, UInt16 port, UInt32 expectedConfigNumber)
{
    RSLProtocolVersion version = (RSLProtocolVersion) TestEngine::m_machine->m_version;
    Message query(version, Message_StatusQuery, MemberId(sender), 0, expectedConfigNumber, BallotNumber());
    TestEngine::SendMessage(&query, true, port);
    StatusResponse *resp = (StatusResponse *) TestEngine::ReceiveResponse(Message_StatusResponse, false);
    *status = *resp;
}

void WaitForState(const char* sender, UInt16 port, Legislator::State expectedState, UInt64 expectedDecree,
                 UInt32 expectedConfigNumber, BallotNumber expectedBallot)
{
    int count = 0;
    while (count++ < c_WaitIterations)
    {
        StatusResponse s;
        GetStatus(&s, sender, port, expectedConfigNumber);
        if (s.m_state == (UInt32) expectedState && s.m_ballot == expectedBallot &&
            s.m_decree == expectedDecree && s.m_configurationNumber == expectedConfigNumber)
        {
            return;
        }
        Sleep(c_SleepTime);
    }
    UT_AssertFail();
}

void WaitForCheckpoint(const char* sender, UInt16 port, UInt32 expectedConfigNumber, UInt64 checkpoint)
{
    int count = 0;
    while (count++ < c_WaitIterations)
    {
        StatusResponse s;
        GetStatus(&s, sender, port, expectedConfigNumber);
        if (s.m_checkpointedDecree == checkpoint)
        {
            return;
        }
        Sleep(c_SleepTime);
    }
    UT_AssertFail();
}

void TryBootstrapAgainByMessageAndAPI(TestStateMachine * sm, RSLNodeCollection & initialNodes,
                                      RSLResponseCode expectedResult)
{
    RSLProtocolVersion version = (RSLProtocolVersion) TestEngine::m_machine->m_version;
    vector<RSLNodeCollection> sets;
    RSLNodeCollection newNodes;

    // Missing one
    newNodes = initialNodes;
    newNodes.Remove(newNodes.Count() -1);
    sets.push_back(newNodes);

    // MemberId changed
    newNodes = initialNodes;
    UInt64 id = newNodes[newNodes.Count() - 1].GetMemberIdAsUInt64();
    newNodes[newNodes.Count() - 1].SetMemberIdAsUInt64(id + 1);
    sets.push_back(newNodes);

    // Extra one
    newNodes = initialNodes;
    newNodes.Append(initialNodes[2]);
    id = newNodes[newNodes.Count() - 1].GetMemberIdAsUInt64();
    newNodes[newNodes.Count() - 1].SetMemberIdAsUInt64(id + 1);
    sets.push_back(newNodes);

    // Missing replica itself
    newNodes = initialNodes;
    newNodes.Remove(0);
    sets.push_back(newNodes);

    // Sends messages
    for (size_t i=0; i < sets.size(); i++)
    {
        MemberSet memberSet(sets[i], NULL, 0);
        BootstrapMsg msg(version, MemberId("2"), memberSet);
        TestEngine::SendMessage(&msg, true, initialNodes[0].m_rslPort);
        Sleep(500);
        // change this to get status

        // Check state
        RSLNodeCollection actualNodeArray;
        TestEngine::GetConfigurationInfo(sm, &actualNodeArray, NULL, NULL);
        TestEngine::CompareNodeArrays(initialNodes, actualNodeArray);
    }

    // Use API
    for (size_t i=0; i < sets.size(); i++)
    {
        // Send Bootstrap Message
        RSLMemberSet memberSet(sets[i], NULL, 0);
        UT_AssertIsTrue(sm->Bootstrap(&memberSet, 0) == expectedResult);

        // Check state
        RSLNodeCollection actualNodeArray;
        TestEngine::GetConfigurationInfo(sm, &actualNodeArray, NULL, NULL);
        TestEngine::CompareNodeArrays(initialNodes, actualNodeArray);
    }
}

void PrepareAndPassVote(const char* sender, UInt16 port, vector<Message *> & messages)
{
    RSLProtocolVersion version = RSLProtocolVersion_4;
    MemberId senderMemberId(sender);

    StatusResponse stsResp(version, senderMemberId,
        1,   // decree
        1,   // configuration number
        BallotNumber());
    stsResp.m_queryDecree = stsResp.m_decree;
    stsResp.m_queryBallot = stsResp.m_ballot;
    TestEngine::SendMessage(&stsResp, true, port);

    PrepareMsg * prep = new PrepareMsg(version, senderMemberId,
        1,   // decree
        1,   // configuration number
        BallotNumber(1, senderMemberId),
        new PrimaryCookie()
    );
    messages.push_back(prep);
    TestEngine::SendMessage(prep, true, port);
    TestEngine::ReceiveResponse(Message_PrepareAccepted, false);

    Vote * vote = new Vote(version, senderMemberId,
        1,   // decree
        1,   // configuration number
        BallotNumber(1, senderMemberId),
        new PrimaryCookie()
    );
    messages.push_back(vote);
    TestEngine::SendMessage(vote, true, port);
    TestEngine::ReceiveResponse(Message_VoteAccepted, false);
}

void PrepareAndPassVote(const char* sender, UInt16 port)
{
    vector<Message *> messages;
    PrepareAndPassVote(sender, port, messages);
    for (size_t i=0; i < messages.size(); i++)
    {
        delete messages[i];
    }
}

void BootstrapByMessage()
{
    RSLProtocolVersion version = (RSLProtocolVersion) TestEngine::m_machine->m_version;
    int configNumber = (version < RSLProtocolVersion_4) ? 1 : 0;
    RSLConfigParam cfgParam;
    RSLNodeCollection initialNodes;
    TestEngine::GenerateConfigFile(3, 1, 3, cfgParam, initialNodes);

    // Initialize
    TestStateMachine *sm = new TestStateMachine(version);
    UT_AssertIsTrue(sm->Initialize(&cfgParam, initialNodes[0], version, true));

    // Check state
    WaitForState(initialNodes[1].m_memberIdString, initialNodes[0].m_rslPort,
        Legislator::PaxosInactive, /*decree*/0, configNumber, BallotNumber());
    VerifyMemberSetIsEmpty(sm);

    // Bootstrap by Message
    MemberSet memberSet(initialNodes, NULL, 0);
    BootstrapMsg msg(version, MemberId("2"), memberSet);
    TestEngine::SendMessage(&msg, true, initialNodes[0].m_rslPort);
    Sleep(500);
    configNumber = 1;

    // Check state
    WaitForState(initialNodes[1].m_memberIdString, initialNodes[0].m_rslPort,
        Legislator::Initializing, /*decree*/1, configNumber, BallotNumber());
    // Try bootstrap again
    TryBootstrapAgainByMessageAndAPI(sm, initialNodes, RSLBootstrapFailed);

    PrepareAndPassVote(initialNodes[1].m_memberIdString, initialNodes[0].m_rslPort);

    // Check state
    WaitForState(initialNodes[1].m_memberIdString, initialNodes[0].m_rslPort,
    Legislator::StableSecondary, /*decree*/1, configNumber, BallotNumber(1, MemberId(initialNodes[1].m_memberIdString)));
    // Try bootstrap again
    TryBootstrapAgainByMessageAndAPI(sm, initialNodes, RSLAlreadyBootstrapped);

    sm->Pause();
}

void BootstrapByAPI()
{
    RSLProtocolVersion version = (RSLProtocolVersion) TestEngine::m_machine->m_version;
    int configNumber = (version < RSLProtocolVersion_4) ? 1 : 0;
    RSLConfigParam cfgParam;
    RSLNodeCollection initialNodes;
    TestEngine::GenerateConfigFile(3, 1, 3, cfgParam, initialNodes);

    // Initialize
    TestStateMachine *sm = new TestStateMachine(version);
    UT_AssertIsTrue(sm->Initialize(&cfgParam, initialNodes[0], version, true));

    // Check state
    WaitForState(initialNodes[1].m_memberIdString, initialNodes[0].m_rslPort,
        Legislator::PaxosInactive, /*decree*/0, configNumber, BallotNumber(0, MemberId()));
    VerifyMemberSetIsEmpty(sm);

    // Bootstrap by Bootstrap() method
    RSLMemberSet memberSet(initialNodes, NULL, 0);
    volatile bool end = false;
    Bootstrap(sm, &memberSet, RSLSuccess, &end);
    Sleep(500);
    configNumber = 1;

    // Check state
    WaitForState(initialNodes[1].m_memberIdString, initialNodes[0].m_rslPort,
        Legislator::Initializing, /*decree*/1, configNumber, BallotNumber());
    // Try bootstrap again
    TryBootstrapAgainByMessageAndAPI(sm, initialNodes, RSLBootstrapFailed);

    PrepareAndPassVote(initialNodes[1].m_memberIdString, initialNodes[0].m_rslPort);

    // Wait for bootstrap complete
    WaitUntil(end);

    // Check state
    WaitForState(initialNodes[1].m_memberIdString, initialNodes[0].m_rslPort,
        Legislator::StableSecondary, /*decree*/1, configNumber, BallotNumber(1, MemberId(initialNodes[1].m_memberIdString)));
    // Try bootstrap again
    TryBootstrapAgainByMessageAndAPI(sm, initialNodes, RSLAlreadyBootstrapped);

    sm->Pause();
}

void Bootstrap(TestStateMachine * sm, RSLMemberSet * memberSet, UInt32 expectedReturnCode, volatile bool * endFlag)
{
    UInt32 * rtnCodePtr = new UInt32[1];
    rtnCodePtr[0] = expectedReturnCode;

    void ** params = new void*[4];
    params[0] = (void *) sm;
    params[1] = (void *) memberSet;
    params[2] = (void *) rtnCodePtr;
    params[3] = (void *) endFlag;

    HANDLE h = (HANDLE) _beginthreadex(NULL, 0, BootstrapThread, params, NULL, NULL);
    LogAssert(h != 0);
    CloseHandle(h);
}

unsigned int __stdcall BootstrapThread(void *arg)
{
    void ** params = (void **) arg;
    TestStateMachine * sm = (TestStateMachine *) params[0];
    RSLMemberSet * memberSet = (RSLMemberSet *) params[1];
    UInt32 * rtnCodePtr = (UInt32 *) params[2];
    volatile bool * endFlag = (volatile bool *) params[3];

    UT_AssertIsTrue((UInt32) sm->Bootstrap(memberSet, 15) == *rtnCodePtr);

    delete params;
    delete[] rtnCodePtr;
    *endFlag = true;
    return 0;
}

void BootstrapByCheckpoint()
{
    RSLProtocolVersion version = (RSLProtocolVersion) TestEngine::m_machine->m_version;

    RSLConfigParam cfgParam;
    RSLNodeCollection initialNodes;
    TestEngine::GenerateConfigFile(3, 1, 3, cfgParam, initialNodes);
    DynString src(cfgParam.m_workingDir);
    src.AppendF("\\%s\\", initialNodes[0].m_memberIdString);

    //
    // Create checkpoint
    //
    // Initialize
    TestStateMachine *sm1 = new TestStateMachine(version);
    UT_AssertIsTrue(sm1->Initialize(&cfgParam, initialNodes[0], version, true));
    // Bootstrap by Message
    MemberSet memberSet(initialNodes, NULL, 0);
    BootstrapMsg msg(version, MemberId("2"), memberSet);
    TestEngine::SendMessage(&msg, true, initialNodes[0].m_rslPort);
    Sleep(500);
    PrepareAndPassVote(initialNodes[1].m_memberIdString, initialNodes[0].m_rslPort);
    Sleep(500);
    sm1->Pause();

    //
    // Copy checkpoint
    //
    initialNodes.Clear();
    TestEngine::GenerateConfigFile(3, 1, 3, cfgParam, initialNodes);
    DynString cmd, dest(cfgParam.m_workingDir);
    dest.AppendF("\\%s\\", initialNodes[0].m_memberIdString);
    // Clear destination
    cmd.AppendF("rmdir /s /Q %s 2>NUL", dest.Str());
    system(cmd);
    UT_AssertIsTrue(CreateDirectoryA(dest, NULL));
    // Copy to destination
    cmd.Clear();
    cmd.AppendF("copy /Y %s* %s 1>NUL", src.Str(), dest.Str());
    system(cmd);

    // Fix checkpoint
    DynString cpFile;
    cpFile.AppendF("%s\\%I64u.codex", dest.Str(), 0);
    if (version >= RSLProtocolVersion_3)
    {
        RSLMemberSet memberset(initialNodes, NULL, 0);
        UT_AssertIsTrue(RSLCheckpointUtility::ChangeReplicaSet(cpFile, &memberset));
    }
    //
    // Run
    //
    TestStateMachine *sm2 = new TestStateMachine(version);
    UT_AssertIsTrue(sm2->Initialize(&cfgParam, initialNodes[0], version, true));

    // Check state
    WaitForState(initialNodes[1].m_memberIdString, initialNodes[0].m_rslPort,
        Legislator::Initializing, /*decree*/1, /*config*/1, BallotNumber(1, MemberId(initialNodes[1].m_memberIdString)));
    // Try bootstrap again
    TryBootstrapAgainByMessageAndAPI(sm2, initialNodes, RSLAlreadyBootstrapped);

    sm2->Pause();
}

DWORD GetFileSize(DynString & fileName)
{
    HANDLE hFile = CreateFileA(fileName.Str(),
        FILE_READ_DATA,
        FILE_SHARE_READ|FILE_SHARE_WRITE,
        NULL,
        OPEN_ALWAYS,
        NULL,
        NULL);
    UT_AssertIsTrue(hFile != INVALID_HANDLE_VALUE);

    DWORD size = GetFileSize(hFile, NULL);
    LogAssert(size != INVALID_FILE_SIZE);
    CloseHandle(hFile);
    return size;
}

void TestReplicaJoinClusterLater()
{
    RSLProtocolVersion version = (RSLProtocolVersion) TestEngine::m_machine->m_version;

    //
    // 1. Generate some data (create checkpoint)
    //
    RSLConfigParam cfgParam;
    RSLNodeCollection initialNodes;
    TestEngine::GenerateConfigFile(3, 1, 3, cfgParam, initialNodes);
    DynString src(cfgParam.m_workingDir);
    src.AppendF("\\%s\\", initialNodes[0].m_memberIdString);
    // Initialize
    TestStateMachine *sm1 = new TestStateMachine(version);
    UT_AssertIsTrue(sm1->Initialize(&cfgParam, initialNodes[0], version, true));
    // Bootstrap by Message
    MemberSet memberSet1(initialNodes, NULL, 0);
    BootstrapMsg bootStrapMsg(version, MemberId("2"), memberSet1);
    TestEngine::SendMessage(&bootStrapMsg, true, initialNodes[0].m_rslPort);
    Sleep(500);
    vector<Message *> messages;
    PrepareAndPassVote(initialNodes[1].m_memberIdString, initialNodes[0].m_rslPort, messages);
    UInt64 lastDecree = messages.back()->m_decree;
    // Check state
    WaitForState(initialNodes[1].m_memberIdString, initialNodes[0].m_rslPort,
        Legislator::StableSecondary, lastDecree, /*config*/1,
        BallotNumber(1, MemberId(initialNodes[1].m_memberIdString)));
    sm1->Pause();
    //
    // 2. Create new replica set
    //
    initialNodes.Clear();
    TestEngine::GenerateConfigFile(3, 1, 3, cfgParam, initialNodes);
    DynString cmd, dest(cfgParam.m_workingDir);
    dest.AppendF("\\%s\\", initialNodes[2].m_memberIdString);
    // Clear destination
    cmd.AppendF("rmdir /s /Q %s 2>NUL", dest.Str());
    system(cmd);
    UT_AssertIsTrue(CreateDirectoryA(dest, NULL));
    // Copy to destination
    cmd.Clear();
    cmd.AppendF("copy /Y %s* %s 1>NUL", src.Str(), dest.Str());
    system(cmd);
    // Fix checkpoint
    DynString cpFile;
    cpFile.AppendF("%s\\%I64u.codex", dest.Str(), 0);
    RSLMemberSet memberSet2(initialNodes, NULL, 0);
    UT_AssertIsTrue(RSLCheckpointUtility::ChangeReplicaSet(cpFile, &memberSet2));

    //
    // 3. Start a replica with no replica set
    //
    TestStateMachine *sm2 = new TestStateMachine(version);
    UT_AssertIsTrue(sm2->Initialize(&cfgParam, initialNodes[0], version, true));
    // Check state
    WaitForState(initialNodes[1].m_memberIdString, initialNodes[0].m_rslPort,
        Legislator::PaxosInactive, /*decree*/0, /*config*/0, BallotNumber());
    //
    // 4. Send a new vote to replica
    //
    lastDecree++;
    Vote * vote = VoteFactory::NewVote(version, MemberId(initialNodes[1].m_memberIdString),
        lastDecree, /*config*/1, BallotNumber(1, MemberId(initialNodes[1].m_memberIdString)));
    messages.push_back(vote);
    TestEngine::SendMessage(vote, true, initialNodes[0].m_rslPort);
    //
    // 5. Replica sends a join request
    //
    TestEngine::ReceiveResponse(Message_JoinRequest, false);
    //
    // 6. Reply with a join message
    //
    JoinMessage joinMsg(version, MemberId(initialNodes[1].m_memberIdString),
        lastDecree, /*config*/ 1);
    joinMsg.m_learnPort = TestEngine::m_nodeTemplate.m_rslLearnPort;
    joinMsg.m_minDecreeInLog = 0;
    joinMsg.m_checkpointedDecree = 0;
    joinMsg.m_checkpointSize = (UInt32) GetFileSize(cpFile);
    TestEngine::SendMessage(&joinMsg, true, initialNodes[0].m_rslPort);
    //
    // 7. Replica reply with a Teach replica
    //
    FetchRequest *fetchReq = TestEngine::ReceiveFetchRequest(Message_FetchCheckpoint);
    UT_AssertIsNotNull(fetchReq);
    delete fetchReq;

    // Clean up
    sm2->Pause();
    for (size_t i=0; i < messages.size(); i++)
    {
        delete messages[i];
    }
}


void GenerateCheckpoint(RSLProtocolVersion version, UInt64 * cpDecree, DynString & cpFile,
                        UInt64 stateSize)
{
    RSLConfigParam cfgParam;
    RSLNodeCollection initialNodes;
    TestEngine::GenerateConfigFile(3, 1, 3, cfgParam, initialNodes);
    MemberId sender = MemberId(initialNodes[1].m_memberIdString);
    UInt16 port = initialNodes[0].m_rslPort;

    TestStateMachine *sm = new TestStateMachine(version);
    UT_AssertIsTrue(sm->Initialize(&cfgParam, initialNodes, initialNodes[0], version, true));
    Sleep(500);

    UInt64 decree = 0;

    StatusResponse stsResp(version, sender,
        decree,
        1, // configuration number
        BallotNumber());
    stsResp.m_queryDecree = stsResp.m_decree;
    stsResp.m_queryBallot = stsResp.m_ballot;
    TestEngine::SendMessage(&stsResp, true, port);

    PrepareMsg prep(version, sender,
        decree,
        1, // configuration number
        BallotNumber(1, sender),
        new PrimaryCookie()
    );
    TestEngine::SendMessage(&prep, true, port);
    TestEngine::ReceiveResponse(Message_PrepareAccepted, false);

    *cpDecree = 3;
    cpFile.Clear();
    cpFile.AppendF("%s\\%s\\%I64u.codex",
        cfgParam.m_workingDir,
        initialNodes[0].m_memberIdString,
        *cpDecree);
    sm->m_setSaveStateAtDecree = *cpDecree;
    sm->m_setStateSize = stateSize;

    for (UInt64 i=decree; i <= (*cpDecree) + 1; i++)
    {
        Vote vote(version, sender,
            i,   // decree
            1,   // configuration number
            BallotNumber(1, sender),
            new PrimaryCookie()
        );
        for (int j=0; j < VoteFactory::NumReqs(i); j++)
        {
            vote.AddRequest(VoteFactory::ReqBuf(i), VoteFactory::ReqLen(i), NULL);
        }
        TestEngine::SendMessage(&vote, true, port);
        TestEngine::ReceiveResponse(Message_VoteAccepted, false);
    }
    WaitForCheckpoint(sender.GetValue(), port, 1, *cpDecree);
    UT_AssertIsTrue(sm->m_setStateSize == 0);

    sm->Pause();
}

void CopyAndFixCheckpointFile(DynString & origCpFile, UInt64 cpDecree, char * workingDir,
                              const char* memberId, RSLNodeCollection & initialNodes, DynString & cpFile)
{
    DynString cmd, dest(workingDir);
    dest.AppendF("\\%s\\", memberId);
    // Clear destination
    cmd.AppendF("rmdir /s /Q %s 2>NUL", dest.Str());
    system(cmd);
    UT_AssertIsTrue(CreateDirectoryA(dest, NULL));
    // Copy to destination
    cmd.Clear();
    cmd.AppendF("copy /Y %s %s 1>NUL", origCpFile.Str(), dest.Str());
    system(cmd);
    // Fix checkpoint
    cpFile.Clear();
    cpFile.AppendF("%s\\%I64u.codex", dest.Str(), cpDecree);

    CheckpointHeader header;
    UT_AssertIsTrue(header.UnMarshal(cpFile));

    if (header.m_version >= RSLProtocolVersion_3)
    {
        RSLMemberSet memberset(initialNodes, NULL, 0);
        UT_AssertIsTrue(RSLCheckpointUtility::ChangeReplicaSet(cpFile, &memberset));
    }
}

void BootstrapWithOldCheckpoint_OldAndNewApi(RSLProtocolVersion checkPointVersion,
                                             RSLProtocolVersion machineVersion, UInt64 cpDecree,
                                             DynString & origCpFile)
{
    printf("  Testing checkpoint version %d against machine version %d\n",
        (int) checkPointVersion, (int) machineVersion);

    RSLConfigParam cfgParam;
    RSLNodeCollection initialNodes;

    //
    // Initialize with old API
    //
    DynString cpFile;
    TestEngine::GenerateConfigFile(3, 1, 3, cfgParam, initialNodes);
    CopyAndFixCheckpointFile(origCpFile, cpDecree, cfgParam.m_workingDir,
        initialNodes[0].m_memberIdString, initialNodes, cpFile);

    TestStateMachine *sm1 = new TestStateMachine(machineVersion);
    UT_AssertIsTrue(sm1->Initialize(&cfgParam, initialNodes, initialNodes[0], machineVersion, true));
    // Check state
    WaitForState(initialNodes[1].m_memberIdString, initialNodes[0].m_rslPort,
        Legislator::Initializing, cpDecree+1, /*config*/1, BallotNumber(1, MemberId(initialNodes[1].m_memberIdString)));

    // Try bootstrap again
    TryBootstrapAgainByMessageAndAPI(sm1, initialNodes, RSLAlreadyBootstrapped);
    sm1->Pause();

    //
    // Initialize with new API
    //
    TestEngine::GenerateConfigFile(3, 1, 3, cfgParam, initialNodes);
    CopyAndFixCheckpointFile(origCpFile, cpDecree, cfgParam.m_workingDir,
        initialNodes[0].m_memberIdString, initialNodes, cpFile);

    TestStateMachine *sm2 = new TestStateMachine(machineVersion);
    bool initResult = sm2->Initialize(&cfgParam, initialNodes[0], machineVersion, true);
    if (checkPointVersion <= RSLProtocolVersion_2 || machineVersion <= RSLProtocolVersion_3)
    {
        UT_AssertIsFalse(initResult);
        return;
    }
    UT_AssertIsTrue(initResult);

    // Check state
    WaitForState(initialNodes[1].m_memberIdString, initialNodes[0].m_rslPort,
        Legislator::Initializing, cpDecree+1, /*config*/1, BallotNumber(1, MemberId(initialNodes[1].m_memberIdString)));

    // Try bootstrap again
    TryBootstrapAgainByMessageAndAPI(sm2, initialNodes, RSLAlreadyBootstrapped);

    sm2->Pause();
}

void BootstrapWithOldCheckpoints()
{
    UInt32 latestVersion = (UInt32) RSLProtocolVersion_4;
    for (UInt32 cpVer = (UInt32) RSLProtocolVersion_1; cpVer <= latestVersion; cpVer++)
    {
        UInt64 cpDecree;
        DynString origCpFile;
        GenerateCheckpoint((RSLProtocolVersion) cpVer, &cpDecree, origCpFile);

        for (UInt32 smVer = cpVer; smVer <= latestVersion; smVer++)
        {
            BootstrapWithOldCheckpoint_OldAndNewApi(
                (RSLProtocolVersion) cpVer,
                (RSLProtocolVersion) smVer,
                cpDecree, origCpFile);
        }
    }
}

void InsertBogusDataRelativeToEnd(DynString & origCpFile)
{
    HANDLE hFile = CreateFileA(origCpFile.Str(),
        FILE_WRITE_DATA | FILE_WRITE_ATTRIBUTES,
        FILE_SHARE_READ|FILE_SHARE_WRITE,
        NULL,
        OPEN_ALWAYS,
        NULL,
        NULL);
    UT_AssertIsTrue(hFile != INVALID_HANDLE_VALUE);

    DDWORD pos;
    DWORD cbWritten;
    char * buffer = "BOGUS!";

    long long offset = -10 - (int) sizeof(UInt64);
    pos.ddw = (DWORD64) offset;
    UT_AssertIsTrue(SetFilePointer(hFile, pos.dw.low, (PLONG) &pos.dw.high, FILE_END));
    UT_AssertIsTrue(WriteFile(hFile, buffer, sizeof(buffer), &cbWritten, NULL));
    UT_AssertIsTrue(cbWritten == sizeof(buffer));
    UT_AssertIsTrue(CloseHandle(hFile));
}

void ResizeFile(DynString & file, int adjustment)
{
    HANDLE hFileSrc = CreateFileA(file.Str(),
        FILE_READ_DATA,
        FILE_SHARE_READ|FILE_SHARE_WRITE,
        NULL,
        OPEN_ALWAYS,
        NULL,
        NULL);
    UT_AssertIsTrue(hFileSrc != INVALID_HANDLE_VALUE);

    DynString fileTmp(file);
    fileTmp.Append(".tmp");

    HANDLE hFileDst = CreateFileA(fileTmp.Str(),
        FILE_WRITE_DATA | FILE_WRITE_ATTRIBUTES,
        FILE_SHARE_READ|FILE_SHARE_WRITE,
        NULL,
        OPEN_ALWAYS,
        NULL,
        NULL);
    UT_AssertIsTrue(hFileDst != INVALID_HANDLE_VALUE);

    DWORD size = GetFileSize(hFileSrc, NULL);
    LogAssert(size != INVALID_FILE_SIZE);
    size += adjustment;
    char buffer[1024];
    DWORD cbRead, cbWritten;
    while (size > 0)
    {
        DWORD toRead = size;
        if (toRead > sizeof(buffer))
        {
            toRead = sizeof(buffer);
        }
        ReadFile(hFileSrc, buffer, (DWORD) toRead, &cbRead, NULL);
        if (cbRead == 0)
        {
            cbRead = size;
            if (cbRead > sizeof(buffer))
            {
                cbRead = sizeof(buffer);
            }
        }
        UT_AssertIsTrue(WriteFile(hFileDst, buffer, cbRead, &cbWritten, NULL));
        UT_AssertIsTrue(cbWritten == cbRead);
        size -= cbWritten;
    }
    UT_AssertIsTrue(CloseHandle(hFileSrc));
    UT_AssertIsTrue(CloseHandle(hFileDst));

    UT_AssertIsTrue(MoveFileExA(fileTmp.Str(), file.Str(),
        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH));
}

// failureType
// 0 - No failure
// 1 - Bogus data
// 2 - Extra bytes
// 3 - Missing bytes
void StartReplicaWithCheckpoint(RSLProtocolVersion machineVersion, UInt64 cpDecree,
                                DynString & origCpFile, int failureType, UInt64 stateSize)
{
    RSLConfigParam cfgParam;
    RSLNodeCollection initialNodes;

    DynString cpFile;
    TestEngine::GenerateConfigFile(3, 1, 3, cfgParam, initialNodes);
    CopyAndFixCheckpointFile(origCpFile, cpDecree, cfgParam.m_workingDir,
        initialNodes[0].m_memberIdString, initialNodes, cpFile);

    switch (failureType)
    {
    case 0:
        break;
    case 1:
        InsertBogusDataRelativeToEnd(cpFile);
        break;
    case 2:
        ResizeFile(cpFile, 100);
        break;
    case 3:
        ResizeFile(cpFile, -100);
        break;
    }

    TestStateMachine *sm2 = new TestStateMachine(machineVersion);
    sm2->m_setStateSize = stateSize;
    UT_AssertIsTrue(sm2->Initialize(&cfgParam, initialNodes[0], machineVersion, true));

    // Check state
    UT_AssertIsTrue(((failureType==0) && sm2->m_setStateSize == 0) ||
        ((failureType!=0) && sm2->m_setStateSize != 0));

    UInt64 expectedDecree = (failureType!=0) ? 0 : (cpDecree+1);
    Legislator::State expectedState = (failureType!=0) ?
        Legislator::PaxosInactive :
        Legislator::Initializing;
    int configNumber = (failureType!=0) ? 0 : 1;
    BallotNumber expectedBallot = (failureType!=0) ?
        BallotNumber(0, MemberId()) :
        BallotNumber(1, MemberId(initialNodes[1].m_memberIdString));

    WaitForState(initialNodes[1].m_memberIdString, initialNodes[0].m_rslPort,
        expectedState, expectedDecree, configNumber, expectedBallot);

    sm2->Pause();
}

void TestCheckpointChecksum()
{
    RSLProtocolVersion version = RSLProtocolVersion_4;

    UInt64 cpDecree;
    DynString origCpFile;
    int checksumTokenSize = sizeof(UInt64);
    unsigned long long checkpointBlockSize = RSLibImpl::s_ChecksumBlockSize;
    unsigned long long size;

    // 1 Mega
    size = 1024 * 1024;
    GenerateCheckpoint(version, &cpDecree, origCpFile, size);
    StartReplicaWithCheckpoint(version, cpDecree, origCpFile, 2, size); // Grow file
    StartReplicaWithCheckpoint(version, cpDecree, origCpFile, 3, size); // Shrink file

    // 1/2 block + checksum token
    size = checkpointBlockSize/2;
    GenerateCheckpoint(version, &cpDecree, origCpFile, size);
    StartReplicaWithCheckpoint(version, cpDecree, origCpFile, 0, size);
    StartReplicaWithCheckpoint(version, cpDecree, origCpFile, 1, size);

    // Exactly 1 block
    size = checkpointBlockSize - checksumTokenSize;
    GenerateCheckpoint(version, &cpDecree, origCpFile, size);
    StartReplicaWithCheckpoint(version, cpDecree, origCpFile, 0, size);
    StartReplicaWithCheckpoint(version, cpDecree, origCpFile, 1, size);

    // 1 + 1/2 block + checksum token
    size = checkpointBlockSize + checkpointBlockSize/2;
    GenerateCheckpoint(version, &cpDecree, origCpFile, size);
    StartReplicaWithCheckpoint(version, cpDecree, origCpFile, 0, size);
    StartReplicaWithCheckpoint(version, cpDecree, origCpFile, 1, size);
}


#define CHECKSUM_SIZE sizeof(unsigned long long)

void TestPatternForCheckpoint(unsigned int blockSize, unsigned int chunkSize, unsigned long long dataSize)
{
    char * file= ".\\tempTestFile.chk";
    char * buffer = new char[chunkSize];
    for (unsigned int i=0; i < chunkSize; i++)
    {
        buffer[i] = 'a' + (char)(i%26);
    }

    RSLNode node;
    node.SetMemberIdAsUInt64(rand());
    //
    // Write data out
    //
    CheckpointHeader header1;
    header1.m_version = RSLProtocolVersion_4;
    header1.m_memberId = MemberId(node.m_memberIdString);
    header1.m_lastExecutedDecree = (UInt64) rand();
    header1.m_checksumBlockSize = blockSize;
    RSLNodeCollection emptyArray;
    header1.m_stateConfiguration = new ConfigurationInfo(1, 1, new MemberSet(emptyArray, NULL, 0));
    header1.m_nextVote = VoteFactory::NewVote(header1.m_version, MemberId(),
        header1.m_lastExecutedDecree, 1, BallotNumber());
    header1.m_nextVote->CalculateChecksum();

    node.SetMemberIdAsUInt64(rand());
    header1.m_maxBallot = BallotNumber((UInt32) rand(), MemberId(node.m_memberIdString));
    header1.m_stateSaved = (rand() & 1) ? true : false;

    RSLCheckpointStreamWriter writer;
    UT_AssertIsTrue(writer.Init(file, &header1) == NO_ERROR);
    unsigned long long dataToWrite = dataSize;
    while (dataToWrite > 0)
    {
        unsigned int bytesWritten = chunkSize;
        if (bytesWritten > dataToWrite)
        {
            bytesWritten = (unsigned int) dataToWrite;
        }
        UT_AssertIsTrue(writer.Write((void*)buffer, bytesWritten) == NO_ERROR);
        dataToWrite -= bytesWritten;
    }
    // Write checkpoint header
    header1.SetBytesIssued(&writer);
    UT_AssertIsTrue(writer.Close() == NO_ERROR);
    header1.Marshal(file);

    //
    // Read data in
    //
    CheckpointHeader header2;
    {
        auto_ptr<APSEQREAD> seqRead(new APSEQREAD());
        UT_AssertIsTrue(seqRead->DoInit(file, 2, s_AvgMessageLen, false) == NO_ERROR);
        DiskStreamReader streamReader(seqRead.get());
        UT_AssertIsTrue(header2.UnMarshal(&streamReader));
    }

    RSLCheckpointStreamReader reader;
    reader.Init(file, &header2);
    UT_AssertIsTrue(header1.m_version             == header2.m_version);
    UT_AssertIsTrue(header1.m_memberId            == header2.m_memberId);
    UT_AssertIsTrue(header1.m_lastExecutedDecree  == header2.m_lastExecutedDecree);
    UT_AssertIsTrue(header1.m_stateConfiguration->GetConfigurationNumber() ==
        header2.m_stateConfiguration->GetConfigurationNumber());
    UT_AssertIsTrue(header1.m_stateConfiguration->GetInitialDecree() ==
        header2.m_stateConfiguration->GetInitialDecree());
    UT_AssertIsTrue(header1.m_stateConfiguration->GetNumMembers() ==
        header2.m_stateConfiguration->GetNumMembers());
    UT_AssertIsTrue(header1.m_checksumBlockSize   == header2.m_checksumBlockSize);
    UT_AssertIsTrue(header1.m_nextVote->m_checksum == header2.m_nextVote->m_checksum);
    UT_AssertIsTrue(header1.m_maxBallot           == header2.m_maxBallot);
    UT_AssertIsTrue(header1.m_stateSaved          == header2.m_stateSaved);
    UT_AssertIsTrue(header1.m_size                == header2.m_size);
    unsigned long long dataToRead = dataSize;
    while (dataToRead > 0)
    {
        unsigned int bytesToRead = chunkSize;
        if (bytesToRead > dataToRead)
        {
            bytesToRead = (unsigned int) dataToRead;
        }
        for (unsigned int offset=0; offset < bytesToRead;)
        {
            char * internalBuffer = NULL;
            unsigned long bytesActuallyRead = 0;

            DWORD ec = reader.GetDataPointer(
                (void**)&internalBuffer,
                bytesToRead-offset,
                &bytesActuallyRead);
            UT_AssertIsTrue(ec == NO_ERROR);

            UT_AssertIsTrue(memcmp(buffer+offset, internalBuffer, bytesActuallyRead) == 0);
            UT_AssertIsTrue(dataToRead >= bytesActuallyRead);
            dataToRead -= bytesActuallyRead;
            offset += bytesActuallyRead;
        }
    }

    DeleteFileA(file);
}

void TestPatternForCheckpoint(unsigned int blockSize, unsigned int chunkSize)
{
    for (unsigned int i=0; i <= 3; i++)
    {
        unsigned int pad = i * (blockSize - CHECKSUM_SIZE);
        // dataSize < blockSize
        TestPatternForCheckpoint(blockSize, chunkSize, pad + (blockSize / 2));

        // dataSize = blockSize - checksumSize => remains 0
        TestPatternForCheckpoint(blockSize, chunkSize, pad + blockSize - CHECKSUM_SIZE);
        // dataSize = blockSize - (checksumSize/2) => remains checksumSize/2
        TestPatternForCheckpoint(blockSize, chunkSize, pad + blockSize - (CHECKSUM_SIZE/2));
        // dataSize = blockSize => remains checksumSize
        TestPatternForCheckpoint(blockSize, chunkSize, pad + blockSize);
    }
}

void TestDifferentWritePatternsForCheckpoint()
{
    // Chunk sizes
    unsigned int CHUNK_SIZES[] =
    {
        s_PageSize / 2 - 17,
        (s_PageSize-CHECKSUM_SIZE) / 2,
        s_PageSize / 2,
        s_PageSize
    };

    // Multiple
    for (int i=0; i < LengthOf(CHUNK_SIZES); i++)
    {
        TestPatternForCheckpoint(s_PageSize,   CHUNK_SIZES[i]);
        TestPatternForCheckpoint(2*s_PageSize, CHUNK_SIZES[i]);
        TestPatternForCheckpoint(3*s_PageSize, CHUNK_SIZES[i]);
    }
}

void TestTimeBasedCheckpoint()
{
    RSLProtocolVersion version = (RSLProtocolVersion) TestEngine::m_machine->m_version;
    char * cpPrefix = "*.codex";

    RSLConfigParam cfgParam;
    RSLNodeCollection initialNodes;
    TestEngine::GenerateConfigFile(3, 1, 3, cfgParam, initialNodes);
    DynString cpPath(cfgParam.m_workingDir);
    cpPath.AppendF("\\%s\\", initialNodes[0].m_memberIdString);
    // Checkpoint every 3 seconds
    cfgParam.m_maxCheckpointIntervalSec = 2;
    cfgParam.m_maxCheckpoints = 16;
    const char * senderId = initialNodes[1].m_memberIdString;
    UInt16 port = initialNodes[0].m_rslPort;

    // Initialize
    TestStateMachine *sm1 = new TestStateMachine(version);
    UT_AssertIsTrue(sm1->Initialize(&cfgParam, initialNodes[0], version, true));
    // Bootstrap by Message
    MemberSet memberSet1(initialNodes, NULL, 0);
    BootstrapMsg bootStrapMsg(version, MemberId(senderId), memberSet1);
    TestEngine::SendMessage(&bootStrapMsg, true, port);

    // Check state
    WaitForState(senderId, port, Legislator::Initializing,
        /*decree*/1, /*config*/1, BallotNumber());

    // Tell replica it is current
    StatusResponse stsResp(version, MemberId(senderId), /*decree*/1, /*config*/1, BallotNumber());
    stsResp.m_queryDecree = stsResp.m_decree;
    stsResp.m_queryBallot = stsResp.m_ballot;
    TestEngine::SendMessage(&stsResp, true, port);
    // Elect test replica
    PrepareMsg * prep = new PrepareMsg(version, MemberId(senderId),
        /*decree*/1, /*config*/1, BallotNumber(1, MemberId(senderId)), new PrimaryCookie());
    TestEngine::SendMessage(prep, true, port);
    TestEngine::ReceiveResponse(Message_PrepareAccepted, false);

    UInt64 lastDecree = 1;

    // Pass 20 votes, no delay => 1 checkpoint (bootstrap)
    for (int i=0; i < 20; i++, lastDecree++)
    {
        sm1->m_setSaveStateAtDecree = lastDecree-1;

        Vote * vote = new Vote(version, MemberId(senderId), lastDecree, /*config*/1,
            BallotNumber(1, MemberId(senderId)),new PrimaryCookie());
        TestEngine::SendMessage(vote, true, port);
        TestEngine::ReceiveResponse(Message_VoteAccepted, false);
        delete vote;
    }

    // Check state
    WaitForState(senderId, port, Legislator::StableSecondary,
        lastDecree-1, /*config*/1, BallotNumber(1, MemberId(senderId)));

    // Verify we have 1 checkpoint only
    vector<UInt64> cpBefore;
    LogAssert(Legislator::GetFileNumbers(cpPath, cpPrefix, cpBefore) == NO_ERROR);
    UT_AssertIsTrue(cpBefore.size() == 1);

    // Pass +10 votes, 1 vote per second
    for (int i=0; i < 10; i++, lastDecree++)
    {
        sm1->m_setSaveStateAtDecree = lastDecree-1;

        Vote * vote = new Vote(version, MemberId(senderId), lastDecree, /*config*/1,
            BallotNumber(1, MemberId(senderId)),new PrimaryCookie());
        TestEngine::SendMessage(vote, true, port);
        TestEngine::ReceiveResponse(Message_VoteAccepted, false);
        delete vote;
        Sleep(1000);
    }

    // Check state
    WaitForState(senderId, port, Legislator::StableSecondary,
        lastDecree-1, /*config*/1, BallotNumber(1, MemberId(senderId)));

    // Verify we have 1 checkpoint only
    vector<UInt64> cpAfter;
    LogAssert(Legislator::GetFileNumbers(cpPath, cpPrefix, cpAfter) == NO_ERROR);
    UT_AssertIsTrue(cpAfter.size() >= 4);

    sm1->Pause();
}
