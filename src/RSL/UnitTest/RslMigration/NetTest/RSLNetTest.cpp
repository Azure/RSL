#define _WINSOCKAPI_
#include <rsl.h>
#include <iostream>
#include <string>
#include <strsafe.h>
#include "libfuncs.h"
#include "marshal.h"

using namespace std;
using namespace RSLib;
using namespace RSLibImpl;

#define SLEEP_TIME 500
int g_port = 20000;
BYTE* g_buf;
int g_maxConfigurationReported = 0;
char * RSLResponseCodeStr[] = { "RSLSuccess", "RSLFastReadStale", "RSLNotPrimary", "RSLShuttingDown" };

bool ReadConfigurationFromFile(int *configurationNumberPtr, int *numMembersPtr, int **memberIdsPtr)
{
    FILE *fp = fopen("members.txt", "r");
    if (fp == NULL) { 
        printf("WARNING: Could not open members.txt for reading.\n");
        return false;
    }

    char line[100];
    if (fgets(line, 100, fp) == NULL) {
        printf("WARNING: Could not read configuration number in members.txt.\n");
        fclose(fp);
        return false;
    }
    *configurationNumberPtr = atoi(line);

    if (fgets(line, 100, fp) == NULL) {
        printf("WARNING: Could not read number of members in members.txt.\n");
        fclose(fp);
        return false;
    }
    *numMembersPtr = atoi(line);

    *memberIdsPtr = new int[*numMembersPtr];
    int memberIdsRead = 0;
    while (memberIdsRead < *numMembersPtr && fgets(line, 100, fp) != NULL) {
        (*memberIdsPtr)[memberIdsRead] = atoi(line);
        ++memberIdsRead;
    }
    if (memberIdsRead < *numMembersPtr) {
        printf("WARNING: Only found %d out of %d member IDs in members.txt.\n",
               memberIdsRead,
               *numMembersPtr);
        delete [] *memberIdsPtr;
        *memberIdsPtr = NULL;
        fclose(fp);
        return false;
    }

    fclose(fp);
    return true;
}

bool ReportCurrentConfiguration(int currentConfigurationNumber)
{
    FILE *fp = fopen("CurrentConfig.txt", "w");
    if (fp == NULL) {
        printf("WARNING: Could not open CurrentConfig.txt for writing.\n");
        return false;
    }
    fprintf(fp, "%d\n", currentConfigurationNumber);
    fclose(fp);
    return true;
}

static void PropulateNode(RSLNode &node, UInt64 memberId)
{
    LogAssert(SUCCEEDED(StringCchCopyA(node.m_hostName, sizeof(node.m_hostName), "127.0.0.1")));
    node.m_ip = inet_addr(node.m_hostName);
    node.SetMemberIdAsUInt64(memberId);
    node.m_rslPort = (UInt16) (g_port + memberId * 1000);
}

class TestRSLStateMachineProcessor : public RSLStateMachine
{
public:
    
    void Run(RSLConfigParam &cfgParam, RSLNode &selfNode)
    {
        m_score = 0;
        m_outstanding = 0;
        m_size = 10*1024*1024;
        m_state = new char[m_size];
        m_clientEnd = false;
        m_isPrimary = false;

        LogAssert(Initialize(&cfgParam, selfNode, RSLProtocolVersion_4, false));
    }

    void MainLoop()
    {
        for (int timer = 0; !m_clientEnd; timer++) {
            if (timer >= 1000 / SLEEP_TIME) {
                timer = 0;
                if (m_isPrimary) {
                    ChangeConfigurationIfNeeded();
                }
            }
            if (m_isPrimary && m_outstanding < 1) {
                int inc = 1;
                SendRequest(&inc, sizeof(inc), this, _UI64_MAX);
            }
            Sleep(SLEEP_TIME);
        }
    }

    void ChangeConfigurationIfNeeded()
    {
        RSLMemberSet currentMemberSet;
        UInt32 configurationNumber;
        LogAssert(GetConfiguration(&currentMemberSet, &configurationNumber));
        int currentConfigurationNumber = 0;
        UInt32 length;
        void* cookie = currentMemberSet.GetConfigurationCookie(&length);
        
        if (length >= sizeof(int)) {
            currentConfigurationNumber = * (int *) cookie;
        }

        int desiredConfigurationNumber, numMembersInDesiredConfiguration, *memberIdsOfDesiredConfiguration;
        if (!ReadConfigurationFromFile(&desiredConfigurationNumber,
                                       &numMembersInDesiredConfiguration,
                                       &memberIdsOfDesiredConfiguration)) {
            return;
        }

        if (currentConfigurationNumber >= desiredConfigurationNumber) {
            if (currentConfigurationNumber > g_maxConfigurationReported) {
                if (ReportCurrentConfiguration(currentConfigurationNumber)) {
                    g_maxConfigurationReported = currentConfigurationNumber;
                }
            }
            return;
        }

        printf("New configuration found:\n");
        RSLNodeCollection membersOfNewConfiguration;
        for (int whichMember = 0; whichMember < numMembersInDesiredConfiguration; ++whichMember)
        {
            RSLNode node;
            PropulateNode(node, memberIdsOfDesiredConfiguration[whichMember]);
            printf("%I64u", node.GetMemberIdAsUInt64());
            membersOfNewConfiguration.Append(node);
        }
        printf("\n");
        RSLMemberSet *memberSet = new RSLMemberSet(membersOfNewConfiguration,
                                                   &desiredConfigurationNumber,
                                                   sizeof(int));
        ChangeConfiguration(memberSet, this);

        delete [] memberIdsOfDesiredConfiguration;
    }

    void SendRequest(void * data, size_t avail, void * cookie, UInt64 maxSeenSeqNo)
    {
        RSLResponseCode ec;
        if (maxSeenSeqNo == _UI64_MAX) {
            ec = ReplicateRequest(data, avail, cookie);
        } else {
            ec = FastReadRequest(maxSeenSeqNo, data, avail, cookie);
        }
        if (ec != RSLSuccess) {
            printf("ReplicateRequest() failed: %s\n", RSLResponseCodeStr[(int) ec]);
        }
        InterlockedIncrement(&m_outstanding);
    }

    //
    // RSL State machine implementation
    //

    bool LoadState(RSLCheckpointStreamReader* reader)
    {
        if (reader == NULL)
        {
            m_score = 0;
            return true;
        }
        UInt64 totalRead = 0;
        DWORD read;
        while (m_size > totalRead)
        {
            int ec = reader->GetData(m_state+totalRead, (DWORD) (m_size-totalRead), &read);
            LogAssert(ec == NO_ERROR);
            totalRead += read;
        }
        memcpy(&m_score, m_state, sizeof(m_score));
        return true;
    }

    void ExecuteReplicatedRequest(void* request, size_t len, void* cookie, bool * /*saveState*/)
    {
        return ExecuteRequest(false, request, len, cookie);
    }

    void ExecuteFastReadRequest(void* request, size_t len, void* cookie)
    {
        return ExecuteRequest(true, request, len, cookie);
    }
    
    void ExecuteRequest(bool isFastRead, void* request, size_t len, void* cookie)
    {
        if (isFastRead == false)
        {
            MarshalData marshal(request, (UInt32) len, false);
            UInt32 inc;
            LogAssert(marshal.ReadUInt32(&inc));
            m_score += inc;
            printf("score: %u\n", m_score);
        }
        if (this == cookie) {
            LogAssert(m_outstanding > 0);
            InterlockedDecrement(&m_outstanding);
        }
    }

    void AbortRequest(RSLResponseCode /*status*/, void* cookie)
    {
        printf("Request aborted (outstanding=%u)\n", m_outstanding);
        if (this == cookie) {
            LogAssert(m_outstanding > 0);
            InterlockedDecrement(&m_outstanding);
        }
    }
    
    void SaveState(RSLCheckpointStreamWriter* writer)
    {
        memcpy(m_state, &m_score, sizeof(m_score));
        int ec = writer->Write(m_state, m_size);
        LogAssert(ec == NO_ERROR);
    }
    
    void NotifyStatus(bool isPrimary)
    {
        printf("current primary status is: %s\n", isPrimary ? "true" : "false");
        m_isPrimary = isPrimary;
    }

    virtual void NotifyConfigurationChanged(void * /*cookie*/)
    {
        printf("Configuration changed!\n");
    }

    virtual void AbortChangeConfiguration(RSLResponseCode /*status*/, void* /*cookie*/)
    {
        printf("Change configuration aborted!\n");
        LogAssert(!"Change configuration aborted");
    }
        
    void ShutDown(RSLResponseCode /*status*/)
    {
        printf("Shutting down\n");
        m_clientEnd = true;
    }

    UInt32 m_score;
    volatile long m_outstanding;
    UInt32 m_size;
    char *m_state;
    bool m_clientEnd;
    bool m_isPrimary;
};

void PrintUsage(char * errMsg)
{
    if (errMsg != NULL)
    {
        printf("Error: %s\n", errMsg);
    }
    printf("Usage: \n");
    printf("RSLNetTest <memberId>\n");
    printf("\n");
}

void Exit(int code)
{
#ifdef _DEBUG
    LogAssert(FALSE);
#endif
    ::ExitProcess(code);
}

int __cdecl main(int argc, char **argv)
{
    if (argc != 2)
    {
        PrintUsage(NULL);
        Exit(1);
    }
    int id = atoi(argv[1]);
    char path[256];
    _snprintf_s(path, sizeof(path), ".\\debuglogs\\%d", id);
    LogAssert(RSLib::RSLInit(path));
    Logger::AddRule("*", "A", "popup");

    RSLConfigParam cfgParam;
    // Initialize the default config param
    cfgParam.m_newLeaderGracePeriodSec=15;
    cfgParam.m_heartBeatIntervalSec=2;
    cfgParam.m_electionDelaySec=10;
    cfgParam.m_maxElectionRandomizeSec=1;
    cfgParam.m_initializeRetryIntervalSec=1;
    cfgParam.m_prepareRetryIntervalSec=1;
    cfgParam.m_voteRetryIntervalSec=3;
    cfgParam.m_cPQueryRetryIntervalSec=5;
    cfgParam.m_maxCheckpointIntervalSec=0;
    cfgParam.m_joinMessagesIntervalSec=1;
    cfgParam.m_maxLogLenMB=1;
    cfgParam.m_sendTimeoutSec=5;
    cfgParam.m_receiveTimeoutSec=5;
    cfgParam.m_maxCacheLengthMB=50;
    cfgParam.m_maxVotesInLog=10000;
    cfgParam.m_maxOutstandingPerReplica=10;
    cfgParam.m_maxCheckpoints=4;
    cfgParam.m_maxLogs=10;
    cfgParam.m_logLenRandomize=20;
    cfgParam.m_electionRandomize=10;
    cfgParam.m_fastReadSeqThreshold=5;
    cfgParam.m_inMemoryExecutionQueueSizeMB=10;
    cfgParam.m_numReaderThreads = 3;
    LogAssert(SUCCEEDED(StringCchCopyA(cfgParam.m_workingDir, sizeof(cfgParam.m_workingDir), ".\\data")));

    int configNumber = 0;
    int numReplicas = 0;
    int *memberIds = NULL;
    LogAssert(ReadConfigurationFromFile(&configNumber, &numReplicas, &memberIds));

    // Populate member set
    if (id <= 0)
    {
        PrintUsage("invalid member id!");
        Exit(1);
    }

    UInt32 bufSize = 100*1024;
    g_buf = (BYTE *) malloc(bufSize);
    for (UInt32 i = 0; i < bufSize; i++)
    {
        g_buf[i] = 'a' + (char) (i % 26);
    }
    LogAssert(g_buf);

    RSLNodeCollection nodes;
    for (int i=0; i < numReplicas; i++)
    {
        RSLNode node;
        PropulateNode(node, memberIds[i]);
        nodes.Append(node);
    }
    RSLNode selfNode;
    PropulateNode(selfNode, id);

    // Start replica
    printf("Starting member #%I64u\n", selfNode.GetMemberIdAsUInt64());
    TestRSLStateMachineProcessor *sm = new TestRSLStateMachineProcessor();
    sm->Run(cfgParam, selfNode);
    if (id == 1)
    {
      RSLMemberSet memberSet(nodes, NULL, 0);
      LogAssert(sm->Bootstrap(&memberSet, 15) != RSLBootstrapFailed);
    }
    sm->MainLoop();

    free(g_buf);
}

