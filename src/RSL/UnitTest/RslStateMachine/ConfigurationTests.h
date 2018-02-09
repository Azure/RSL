#pragma once
#include "TestEngine.h"

void InitConfigurationTests();

void TestAddNewReplica_asPrimary();
void TestAddNewReplica_asSecondary();

void ReplicaIssuesConfigChange_AllFinalStates(RSLNodeCollection oldNodes, RSLNodeCollection newNodes);
void ReplicaIssuesConfigChange(RSLNodeCollection &oldNodes, Legislator::State finalState, RSLNodeCollection &newNodes);
void ReplicaIssuesConfigChangeImpl(FakeLegislatorCollection &legislators, RSLNodeCollection &newNodes);

void ReplicaFollowsConfigChange_AllStates(RSLNodeCollection oldNodes, RSLNodeCollection newNodes);
void ReplicaFollowsConfigChange(TestEngine::State initialState, RSLNodeCollection &oldNodes, Legislator::State finalState, RSLNodeCollection &newNodes);
void ReplicaFollowsConfigChangeImpl(bool testingNodeInOldConfig, FakeLegislatorCollection &legislators, RSLNodeCollection &newNodes);

void JoinNewConfig(bool testingNodeInOldConfig, Legislator::State finalState, FakeLegislatorCollection &legislators, RSLNodeCollection &newNodes);

void PrintChange(const char* issuer, TestEngine::State initialState, RSLNodeCollection &oldNodes, Legislator::State finalState, RSLNodeCollection &newNodes);
bool IsTestingNodeInConfig(RSLNodeCollection &nodes);
RSLNodeCollection BuildArray(RSLNode * node, ...);

void TestAddNewReplicas();
void TestRemoveReplica();
void TestAddAndRemoveReplicas();
void TestNoChanges();

void TestAllPossibleOperationsFor6Replicas();

void TestNewConfigOldBallot();
void TestABC_BCD_CDE_ADE();

void TestJoinMessages();
void TestJoinMessages(TestEngine::State state);

void TestDefunctMessages();
void SendDefunctMessages(Legislator::State state);
void ReceiveDefunctMessages(Legislator::State state);

void TestPrimaryChangeDuringReconfiguration();
void TestPrimaryChangeDuringReconfiguration(bool replicaStarts, bool replicaFinishes);

void TestConfigChangesDuringRestore(vector<RSLNodeCollection> &configHistory, const char* testingReplicaMemberId,
                                    bool checkpoint, int numOfVotes);
void TestConfigChangesDuringRestore();

void InitBootstrapTests();
void VerifyMemberSetIsEmpty(TestStateMachine * sm);
void GetStatus(StatusResponse * status, const char* sender, UInt16 port, UInt32 expectedConfigNumber);
void WaitForState(const char* sender, UInt16 port, Legislator::State expectedState, UInt64 expectedDecree, UInt32 expectedConfigNumber, BallotNumber expectedBallot);
void WaitForCheckpoint(const char* sender, UInt16 port, UInt32 expectedConfigNumber, UInt64 checkpoint);
void TryBootstrapAgainByMessageAndAPI(TestStateMachine * sm, RSLNodeCollection & initialNodes, RSLResponseCode expectedResult);

void BootstrapByMessage();
void BootstrapByAPI();
void Bootstrap(TestStateMachine * sm, RSLMemberSet * memberSet, UInt32 expectedReturnCode, volatile bool * endFlag);
unsigned int __stdcall BootstrapThread(void *arg);
void BootstrapByCheckpoint();
DWORD GetFileSize(DynString & fileName);
void TestReplicaJoinClusterLater();

void GenerateCheckpoint(RSLProtocolVersion version, UInt64 * cpDecree, DynString & cpFile, UInt64 stateSize=0);
void CopyAndFixCheckpointFile(DynString & origCpFile, UInt64 cpDecree, char * workingDir, const char* memberId, RSLNodeCollection & initialNodes, DynString & cpFile);
void BootstrapWithOldCheckpoint_OldAndNewApi(RSLProtocolVersion checkPointVersion, RSLProtocolVersion machineVersion, UInt64 cpDecree, DynString & origCpFile);
void BootstrapWithOldCheckpoints();

void InsertBogusDataRelativeToEnd(DynString & origCpFile);
void ResizeFile(DynString & file, int adjustment);
void StartReplicaWithCheckpoint(RSLProtocolVersion machineVersion, UInt64 cpDecree, DynString & origCpFile, bool mustFail, UInt64 stateSize);
void TestCheckpointChecksum();

void TestPatternForCheckpoint(unsigned int blockSize, unsigned int chunkSize, unsigned long long dataSize);
void TestPatternForCheckpoint(unsigned int blockSize, unsigned int chunkSize);
void TestDifferentWritePatternsForCheckpoint();
void TestTimeBasedCheckpoint();