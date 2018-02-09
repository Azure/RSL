#include "legislatortest.h"
#include "unittest.h"
#include "libfuncs.h"
#include "ConfigurationTests.h"
#include <time.h>
#include "strsafe.h"

#include "rslutil.h"

#define AddTest(_x) suite1.AddTestCase(#_x, _x)

LegislatorTest* g_test = NULL;

// Initializes g_test. When this test finishes, the replica
// is in "secondary" state with decree 1 accepted. We need
// to pass a decree so that the next tests can issue
// m_status.m_decree - 1.
static void TestInitialize()
{
    g_test->Init();
    g_test->MoveToSecondary();
    // pass a decree.
    UInt64 decree = g_test->m_status.m_decree+1;
    BallotNumber b = g_test->m_status.m_ballot;
    g_test->SendRequest(Message_Vote, decree, b);
    g_test->ReceiveResponse(Message_VoteAccepted, decree, b);
    g_test->AssertEmptyQueues();
}

// Tests all possible state transitions.
static void TestTransitions()
{
    for (UInt32 i = LegislatorTest::Primary; i < LegislatorTest::State_Count; i++)
    {
        for (UInt32 j = LegislatorTest::Primary; j < LegislatorTest::State_Count; j++)
        {
            g_test->MoveToState(i);
            g_test->MoveToState(j);
        }
    }
    g_test->AssertEmptyQueues();

}

// Test random state transitions.
static void TestRandomTransitions()
{
    g_test->MoveToSecondary();
    g_test->MoveToInitializing();
    g_test->MoveToPreparing();
    g_test->MoveToPrimary();
    g_test->MoveToPreparing();
    g_test->MoveToSecondary();
    g_test->MoveToSecondary();
    g_test->MoveToPreparing();
    g_test->MoveToInitializing();
    g_test->MoveToPrimary();
    g_test->MoveToPrimary();
    g_test->MoveToInitializing();
    g_test->MoveToSecondary();
    g_test->MoveToPreparing();
    g_test->MoveToPreparing();
    g_test->MoveToPrimary();
    g_test->MoveToSecondary();
    g_test->MoveToPrimary();
    g_test->MoveToInitializing();
    
    g_test->AssertEmptyQueues();
}

// Tests sending invalid messages. Replica should ignore all of
// these messages and should not crash.
static void TestInvalidMessages()
{
    g_test->TestInvalidMessages();
    g_test->AssertEmptyQueues();
}

static void TestNotAcceptedMsg()
{
    g_test->TestNotAcceptedMsg(LegislatorTest::Primary);
    g_test->TestNotAcceptedMsg(LegislatorTest::Secondary);
    g_test->TestNotAcceptedMsg(LegislatorTest::Prepare);
    g_test->TestNotAcceptedMsg(LegislatorTest::SecondaryPrepared);
    g_test->TestNotAcceptedMsgInInitialize(LegislatorTest::Initialize);
    g_test->TestNotAcceptedMsgInInitialize(LegislatorTest::InitializePrepared);
    g_test->AssertEmptyQueues();
    
}

static void TestVoteMsg()
{
    g_test->TestVoteMsg(LegislatorTest::Primary);
    g_test->TestVoteMsg(LegislatorTest::Secondary);
    g_test->TestVoteMsgInPrepared(LegislatorTest::Prepare);
    g_test->TestVoteMsgInPrepared(LegislatorTest::SecondaryPrepared);
    g_test->TestVoteMsgInInitialize(LegislatorTest::Initialize);
    g_test->TestVoteMsgInInitialize(LegislatorTest::InitializePrepared);
    g_test->AssertEmptyQueues();
}

static void TestVoteAcceptedMsg()
{
    g_test->TestVoteAcceptedMsg(LegislatorTest::Secondary);
    g_test->TestVoteAcceptedMsg(LegislatorTest::SecondaryPrepared);
    g_test->TestVoteAcceptedMsg(LegislatorTest::Prepare);
    g_test->TestVoteAcceptedMsg(LegislatorTest::Initialize);
    g_test->TestVoteAcceptedMsg(LegislatorTest::InitializePrepared);
    g_test->TestVoteAcceptedMsgInPrimary();
    g_test->AssertEmptyQueues();
}

static void TestPrepareMsg()
{
    g_test->TestPrepareMsg(LegislatorTest::Primary);
    g_test->TestPrepareMsg(LegislatorTest::Secondary);
    g_test->TestPrepareMsg(LegislatorTest::Prepare);
    g_test->TestPrepareMsg(LegislatorTest::SecondaryPrepared);
    g_test->TestPrepareMsgInInitialize(LegislatorTest::Initialize);
    g_test->TestPrepareMsgInInitialize(LegislatorTest::InitializePrepared);
    g_test->AssertEmptyQueues();
}

static void TestPrepareAcceptedMsg()
{
    g_test->TestPrepareAcceptedMsg(LegislatorTest::Primary);
    g_test->TestPrepareAcceptedMsg(LegislatorTest::Secondary);
    g_test->TestPrepareAcceptedMsg(LegislatorTest::SecondaryPrepared);
    g_test->TestPrepareAcceptedMsg(LegislatorTest::Prepare);
    g_test->TestPrepareAcceptedMsg(LegislatorTest::Initialize);
    g_test->TestPrepareAcceptedMsg(LegislatorTest::InitializePrepared);
    g_test->AssertEmptyQueues();
}

static void TestStatusResponseMsg()
{
    g_test->TestStatusResponseMsg(LegislatorTest::Primary);
    g_test->TestStatusResponseMsg(LegislatorTest::Secondary);
    g_test->TestStatusResponseMsg(LegislatorTest::SecondaryPrepared);
    g_test->TestStatusResponseMsg(LegislatorTest::Prepare);
    g_test->TestStatusResponseMsgInInitialize(LegislatorTest::Initialize);
    g_test->TestStatusResponseMsgInInitialize(LegislatorTest::InitializePrepared);
    g_test->AssertEmptyQueues();
}

static void TestExecutionQueueOnDisk()
{
    g_test->TestExecutionQueueOnDisk();
}

static void TestCanBecomePrimary()
{
    g_test->TestCanBecomePrimary();
}

static void TestAcceptMessageFromReplica()
{
    g_test->TestAcceptMessageFromReplica();
}

static void TestReadNextMessage()
{
    g_test->TestReadNextMessage();
    g_test->AssertEmptyQueues();
}

static void TestFastRead()
{
    g_test->TestFastRead(LegislatorTest::Primary);
    g_test->AssertEmptyQueues();
}

static void TestCheckpointing()
{
    // TestCheckpointingInPrimary creates an invalid checkpoint (checkpoint
    // does not have application data). We need to call Testcheckpointing()
    // after Testcheckpointinprimary() so that a valid checkpoint is
    // finally created. Checkpoint created by this test is required by
    // TestRestore
    g_test->TestCheckpointingInPrimary();
    g_test->TestCheckpointing();
    g_test->AssertEmptyQueues();
}

static void TestRestore()
{
    // Always move the legislator to secondary first. 
    g_test->MoveToSecondary();

    // propse a new decree.  
    UInt64 decree = g_test->m_status.m_decree+1;
    BallotNumber b = g_test->m_status.m_ballot;
    g_test->SendRequest(Message_Vote, decree, b);
    g_test->ReceiveResponse(Message_VoteAccepted, decree, b);
    
    // Now Propose the same decree with a higher ballot number.
    // This will cause the legislator to log th same decree again.
    b = g_test->HighBallot(b);
    g_test->SendRequest(Message_Vote, decree, b);
    g_test->ReceiveResponse(Message_VoteAccepted, decree, b);

    // When the replica restores from this log, the last decree
    // should never be executed. There was a bug where if
    // the last decree in the log was logged twice (with different
    // ballot numbers), then that decree was being executed.

    g_test->TestRestore();
    // Test restore with a corrupt log file.
    g_test->TestRestore(TestCorruption_Zero);
    g_test->TestRestore(TestCorruption_Incomplete);
    g_test->TestRestoreFromLoadState();
    g_test->AssertEmptyQueues();
}

static void TestSaveCheckpointAtRestore()
{
    g_test->TestSaveCheckpointAtRestore();
}

static void TestCopyCheckpoint()
{
    g_test->TestCopyCheckpoint();
}

static void TestReplay()
{
    g_test->TestReplay();
}
        
static void TestSingleReplica()
{
    g_test->TestSingleReplica();
}

class TestCheckpointCreator : public RSLib::IRSLCheckpointCreator
{
public:
    void SaveState(RSLCheckpointStreamWriter* writer)
    {
        char message[] = "Hello World!";
        writer->Write((void*)message, sizeof(message));
    }
};

static void TestRSLUtilCreateCheckpoint()
{
    char * path = ".\\";
    RSLNodeCollection emptyNodeArray;
    RSLMemberSet emptyMemberSet(emptyNodeArray, NULL, 0);

    RSLNodeCollection nodeArray;
    RSLNode node;
    node.m_rslPort = 20000;
    StringCbCopyA(node.m_memberIdString, sizeof(node.m_memberIdString), "1");
    nodeArray.Append(node);
    RSLMemberSet memberSet(nodeArray, NULL, 0);

    StringCbCopyA(nodeArray[0].m_memberIdString, sizeof(node.m_memberIdString), "test string");
    RSLMemberSet memberSet2(nodeArray, NULL, 0);

    TestCheckpointCreator checkpointCreator;
    unsigned long long lastExecutedDecree = 0;

    // Version 2
    RSLCheckpointUtility::SaveCheckpoint(path, RSLProtocolVersion_2,
            ++lastExecutedDecree,
            1, // configurationNumber,
            NULL,
            &checkpointCreator);
    RSLCheckpointUtility::SaveCheckpoint(path, RSLProtocolVersion_2,
            ++lastExecutedDecree,
            1, // configurationNumber,
            &emptyMemberSet,
            &checkpointCreator);
    RSLCheckpointUtility::SaveCheckpoint(path, RSLProtocolVersion_2,
            ++lastExecutedDecree,
            1, // configurationNumber,
            &memberSet,
            &checkpointCreator);


    DynString file(path);
    CheckpointHeader::GetCheckpointFileName(file, lastExecutedDecree);

    // this should fail - its invalid to set memberset for RSLProtocolVersion_2
    UT_AssertIsFalse(RSLCheckpointUtility::ChangeReplicaSet(
        file,
        &memberSet));

    // this should fail - its invalid to set memberid to non integer for RSLProtocolVersion_3
    UT_AssertIsFalse(RSLCheckpointUtility::MigrateToVersion3(
        file,
        &memberSet2));

    UT_AssertIsTrue(RSLCheckpointUtility::MigrateToVersion3(
        file,
        &memberSet));

    CheckpointHeader header;
    UT_AssertIsTrue(header.UnMarshal(file));
    UT_AssertAreEqual(RSLProtocolVersion_3, header.m_version);
    UT_AssertAreEqual(lastExecutedDecree, header.m_lastExecutedDecree);

    // Version 3 and up
    RSLCheckpointUtility::SaveCheckpoint(path, RSLProtocolVersion_3,
            ++lastExecutedDecree,
            1, // configurationNumber,
            &memberSet,
            &checkpointCreator);
    RSLCheckpointUtility::SaveCheckpoint(path, RSLProtocolVersion_4,
            ++lastExecutedDecree,
            1, // configurationNumber,
            &memberSet,
            &checkpointCreator);

    // change the memberId to non integer
    StringCbCopyA(nodeArray[0].m_memberIdString, sizeof(node.m_memberIdString), "string test");\
    file.Set(path);
    CheckpointHeader::GetCheckpointFileName(file, lastExecutedDecree);

    UT_AssertIsTrue(RSLCheckpointUtility::ChangeReplicaSet(
        file,
        &memberSet2));
}

int __cdecl main(int argc, char **argv)
{
    (void) argc;
    (void) argv;

    srand((unsigned int ) time(NULL));

    UT_AssertIsTrue(RSLib::RSLInit(".\\debuglogs\\rsl", true));

    TestEngine::Init(
        RSLProtocolVersion_1,
        3,  // Number of replicas
        2,  // memberId of the real replica
        5); // Read threads
    g_test = new LegislatorTest();
    
    CUnitTestSuite suite1("RSL Test");

    // TestInitialize must be the first test. This initializes
    // g_test object.
    AddTest(TestInitialize);
    AddTest(TestTransitions);
    AddTest(TestRandomTransitions);

    AddTest(TestInvalidMessages);
    AddTest(TestNotAcceptedMsg);
    AddTest(TestVoteMsg);
    AddTest(TestVoteAcceptedMsg);
    AddTest(TestPrepareMsg);
    AddTest(TestPrepareAcceptedMsg);
    AddTest(TestStatusResponseMsg);
    AddTest(TestExecutionQueueOnDisk);
    AddTest(TestCanBecomePrimary);
    AddTest(TestAcceptMessageFromReplica);
    AddTest(TestReadNextMessage);
    AddTest(TestFastRead);
    AddTest(TestCheckpointing);
    //TestLog should be one of the last tests, so
    //that we test restore with a big log.
    AddTest(TestRestore);
    AddTest(TestSaveCheckpointAtRestore);
    AddTest(TestCopyCheckpoint);

    AddTest(TestReplay);
    AddTest(TestSingleReplica);

    // Configuration tests
    AddTest(InitConfigurationTests);
    AddTest(TestAddNewReplica_asPrimary);
    AddTest(TestAddNewReplica_asSecondary);
    AddTest(TestAddNewReplicas);
    AddTest(TestRemoveReplica);
    AddTest(TestAddAndRemoveReplicas);
    AddTest(TestNoChanges);
    AddTest(TestNewConfigOldBallot);
    AddTest(TestABC_BCD_CDE_ADE);
    AddTest(TestJoinMessages);
    AddTest(TestDefunctMessages);
    AddTest(TestPrimaryChangeDuringReconfiguration);
    AddTest(TestConfigChangesDuringRestore);

    // Bootstrap tests
    AddTest(InitBootstrapTests);
    AddTest(BootstrapByMessage);
    AddTest(BootstrapByAPI);
    AddTest(BootstrapByCheckpoint);
    AddTest(TestReplicaJoinClusterLater);
    AddTest(BootstrapWithOldCheckpoints);

    // Checkpoint tests
    AddTest(TestCheckpointChecksum);
    AddTest(TestDifferentWritePatternsForCheckpoint);
    AddTest(TestRSLUtilCreateCheckpoint);
    AddTest(TestTimeBasedCheckpoint);

    suite1.RunAllSuites();
}
