#pragma once
#include "legislator.h"
#include "unittest.h"
#include "Util.h"
#include "VoteFactory.h"

using namespace RSLibImpl;
using namespace std;

class StateCopiedCookie
{
public:

    CRITSEC m_lock;
    UInt64 m_seqNo;
    DynString m_fileName;

    // don't access this directly. Use CallbackDone()
    bool m_stateCopiedCallback;

    StateCopiedCookie()
    {
        Clear();
    }

    void Clear()
    {
        m_seqNo = 0;
        m_fileName.Clear();
        m_stateCopiedCallback = false;
    }

    bool CallbackDone()
    {
        AutoCriticalSection lock(&m_lock);
        return m_stateCopiedCallback;
    }
};

class TestStateMachine : public RSLStateMachine
{
public:
    TestStateMachine(RSLProtocolVersion version);

    //------------------------------------------------------------------------
    // RSLStateMachine methods
    //------------------------------------------------------------------------

    bool LoadState(RSLCheckpointStreamReader* reader);
    void ExecuteReplicatedRequest(void* request, size_t len, void* cookie, bool *saveState);
    void ExecuteFastReadRequest(void* request, size_t len, void* cookie);
    void AbortRequest(RSLResponseCode status, void* cookie);
    void NotifyConfigurationChanged(void *cookie);
    void AbortChangeConfiguration(RSLResponseCode status, void* cookie);
    void SaveState(RSLCheckpointStreamWriter* writer); 
    void NotifyStatus(bool isPrimary);
    void ShutDown(RSLResponseCode status);
    bool CanBecomePrimary(RSLPrimaryCookie *cookie);
    bool AcceptMessageFromReplica(RSLNode *node, void *data, UInt32 len);

    void StateCopied(UInt64 seqNo, const char* fileName, void *cookie);

    //------------------------------------------------------------------------
    // Test methods
    //------------------------------------------------------------------------

    void SubmitRequest(UInt64 decree, int numReqs, int len);
    void SubmitRequest(UInt64 decree);
    void SetLoadState(bool *loadStateRet, UInt32 count);
    void SetPrimaryCookie(void *data, UInt32 len);
    RSLResponseCode SubmitFastReadRequest(UInt64 decree, UInt32 timeout, bool success);
    RSLResponseCode SubmitChangeConfig(RSLNodeCollection &nodes);

    //------------------------------------------------------------------------
    // Internal data
    //------------------------------------------------------------------------
private:
    volatile bool m_started;
    volatile long m_numOutstanding;
    UInt64 m_lastSequenceNumber;
    UInt32 m_lastConfigurationNumber;
    int m_remaining;
    vector<bool> m_loadStateRetArray;
public:
    RSLProtocolVersion m_version;
    volatile bool m_isPrimary;
    volatile int m_saveStateCalled;
    volatile int m_executeRequestWait;
    volatile bool m_canBecomePrimary;
    volatile bool m_canBecomePrimaryCalled;
    volatile bool m_acceptMsgFromReplica;
    PrimaryCookie *m_primaryCookie;
    UInt64 m_setSaveStateAtDecree;
    volatile bool m_mustAbortConfigurationChange;
    volatile long long m_setStateSize;
};
