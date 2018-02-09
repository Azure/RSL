#include "TestStateMachine.h"

TestStateMachine::TestStateMachine(RSLProtocolVersion version) :
    m_started(false), m_numOutstanding(0), m_lastSequenceNumber(0), m_lastConfigurationNumber(0),
    m_remaining(0), m_version(version), m_isPrimary(false), m_saveStateCalled(0),
    m_executeRequestWait(0), m_canBecomePrimary(true), m_canBecomePrimaryCalled(false),
    m_acceptMsgFromReplica(true), m_setSaveStateAtDecree(0), 
    m_mustAbortConfigurationChange(false), m_setStateSize(0)
    
{
    m_primaryCookie = new PrimaryCookie();
}

//----------------------------------------------------------------------------
// RSLStateMachine methods
//----------------------------------------------------------------------------

bool
TestStateMachine::LoadState(RSLCheckpointStreamReader* reader)
{
    if (m_loadStateRetArray.size() && !m_loadStateRetArray[0])
    {
        m_loadStateRetArray.erase(m_loadStateRetArray.begin());
        return false;
    }
    
    UT_AssertIsFalse(m_started);
    if (reader != NULL)
    {
        if (m_setStateSize > 0)
        {
            UT_AssertIsTrue((unsigned long long)m_setStateSize == reader->Size());
            while (m_setStateSize > 0)
            {
                char buffer[1024];
                unsigned long long bytesToRead = m_setStateSize;
                if (bytesToRead > sizeof(buffer))
                {
                    bytesToRead = sizeof(buffer);
                }
                unsigned long bytesRead;
                DWORD ec = reader->GetData(buffer, (unsigned int) bytesToRead, &bytesRead);
                if (ec != NO_ERROR)
                {
                    return false;
                }
                m_setStateSize -= bytesRead;
            }
            return true;
        }

        DWORD read;
        // Last sequence number
        UInt64 decree;
        UInt32 configNumber;

        UT_AssertIsTrue(sizeof(decree)+sizeof(configNumber) == reader->Size());
        UT_AssertAreEqual(NO_ERROR, reader->GetData(&decree, sizeof(decree), &read));
        UT_AssertAreEqual(sizeof(decree), read);
        m_lastSequenceNumber = decree;
        // Last configuration number
        UT_AssertAreEqual(NO_ERROR, reader->GetData(&configNumber, sizeof(configNumber), &read));
        UT_AssertAreEqual(sizeof(configNumber), read);
        m_lastConfigurationNumber = configNumber;
    }
    
    m_started = true;
    if (m_loadStateRetArray.size() > 0)
    {
        m_loadStateRetArray.erase(m_loadStateRetArray.begin());
    }
    return true;
}

void
TestStateMachine::ExecuteFastReadRequest(void*  /*request*/, size_t len, void* cookie)
{
    UInt64 decree = GetCurrentSequenceNumber();
    UT_AssertIsNotNull(cookie);
    UInt64 d = *(UInt64 *) cookie;
    UT_AssertIsTrue(d <= decree);
    UT_AssertAreEqual(len, sizeof(UInt64));
    VoteFactory::DeleteFastRead(cookie, true);
    delete cookie;
    return;
}

void
TestStateMachine::ExecuteReplicatedRequest(void* request, size_t len, void* cookie, bool *saveState)
{
    // This is current state (it might change between requests)
    UInt32 configNumber;
    RSLMemberSet rslMemberSet;
    GetConfiguration(&rslMemberSet, &configNumber);
    UT_AssertIsTrue(m_lastConfigurationNumber <= configNumber);
    m_lastConfigurationNumber = configNumber;

    // This is current executing vote (does not change between requests)
    UInt64 decree = GetCurrentSequenceNumber();
    if (decree != m_lastSequenceNumber)
    {
        // this is the start of the next vote.
        if (m_executeRequestWait == 1)
        {
            m_executeRequestWait = 2;
            WaitUntil(m_executeRequestWait == 0);
        }
        // igatis
        //UT_AssertAreEqual(m_lastSequenceNumber+1, decree);
        UT_AssertIsTrue(m_lastSequenceNumber < decree);
        UT_AssertAreEqual(0, m_remaining);
        m_lastSequenceNumber = decree;
        m_remaining = VoteFactory::NumReqs(decree);
    }
    UT_AssertAreEqual(VoteFactory::ReqLen(decree), len);
    UT_AssertIsTrue(memcmp(request, VoteFactory::ReqBuf(decree), len) == 0);
    m_remaining--;
    if (cookie != NULL)
    {
        UInt64 myDecree = *(UInt64 *) cookie;
        UT_AssertAreEqual(myDecree, decree);
        if (m_remaining == 0)
        {
            delete cookie;
            InterlockedDecrement(&m_numOutstanding);
        }
    }
    UT_AssertIsNotNull(saveState);
    if (m_setSaveStateAtDecree == decree)
    {
        *saveState = true;
    }
}

void
TestStateMachine::NotifyConfigurationChanged(void * /*cookie*/)
{
    if (m_mustAbortConfigurationChange)
    {
        m_mustAbortConfigurationChange = false;
        UT_AssertFail();
    }
}

void
TestStateMachine::AbortChangeConfiguration(RSLResponseCode /*status*/, void* /*cookie*/)
{
    if (m_mustAbortConfigurationChange == false)
    {
        UT_AssertFail();
    }
    else
    {
        m_mustAbortConfigurationChange = false;
    }
}

void
TestStateMachine::AbortRequest(RSLResponseCode status, void* cookie)
{
    UT_AssertIsNotNull(cookie);
    UInt64 decree = *(UInt64 *) cookie;
    if (status == RSLFastReadStale)
    {
        VoteFactory::DeleteFastRead(cookie, false);
        delete cookie;
        return;
    }
        
    // this is the start of a new decree
    if (m_remaining == 0)
    {
        m_remaining = VoteFactory::NumReqs(decree);
    }
    m_remaining--;
    if (m_remaining == 0)
    {
        delete cookie;
        InterlockedDecrement(&m_numOutstanding);
    }
}

void
TestStateMachine::SaveState(RSLCheckpointStreamWriter* writer)
{
    UT_AssertAreEqual(0, m_saveStateCalled);
    if (m_setSaveStateAtDecree != GetCurrentSequenceNumber())
    {
        m_saveStateCalled = 1;
        WaitUntil(m_saveStateCalled == 2);
    }
    UInt64 decree = GetCurrentSequenceNumber();
    UT_AssertAreEqual(NO_ERROR, writer->Write(&decree, sizeof(decree)));

    UInt32 configNumber;
    RSLMemberSet rslMemberSet;
    GetConfiguration(&rslMemberSet, &configNumber);
    UT_AssertAreEqual(NO_ERROR, writer->Write(&configNumber, sizeof(configNumber)));

    if (m_setStateSize > 0)
    {
        m_setStateSize -= sizeof(decree) + sizeof(configNumber);
        UT_AssertIsTrue(m_setStateSize > 0);

        while (m_setStateSize > 0)
        {
            unsigned long long bytesToWrite = m_setStateSize;
            if (bytesToWrite > VoteFactory::ReqLen(decree))
            {
                bytesToWrite = VoteFactory::ReqLen(decree);
            }
            UT_AssertAreEqual(NO_ERROR,
                writer->Write(VoteFactory::ReqBuf(decree),(unsigned int) bytesToWrite));
            m_setStateSize -= bytesToWrite;
        }
    }

    m_saveStateCalled = 0;
}
    
void
TestStateMachine::NotifyStatus(bool isPrimary)
{
        UT_AssertIsTrue(m_canBecomePrimaryCalled);
        m_isPrimary = isPrimary;
        UT_AssertIsTrue(m_numOutstanding == 0);
}

void
TestStateMachine::ShutDown(RSLResponseCode  /*status*/)
{
    UT_AssertFail();
}

bool
TestStateMachine::CanBecomePrimary(RSLPrimaryCookie *cookie)
{
    m_canBecomePrimaryCalled = true;
    cookie->Set(m_primaryCookie->m_data, m_primaryCookie->m_len);
    return m_canBecomePrimary;
}

bool
TestStateMachine::AcceptMessageFromReplica(RSLNode * /*node*/, void *data, UInt32 len)
{
    UT_AssertAreEqual(len, m_primaryCookie->m_len);
    if (len)
    {
        UT_AssertIsTrue(memcmp(data, m_primaryCookie->m_data, len) == 0);
    }
    return m_acceptMsgFromReplica;
}

void 
TestStateMachine::StateCopied(UInt64 seqNo, const char* fileName, void *cookie)
{
    if (cookie != NULL)
    {
        StateCopiedCookie *stateCopied = (StateCopiedCookie *) cookie;

        AutoCriticalSection lock(&stateCopied->m_lock);
        stateCopied->m_seqNo = seqNo;
        stateCopied->m_fileName = fileName;
        stateCopied->m_stateCopiedCallback = true;
    }
}

//------------------------------------------------------------------------
// Test methods
//------------------------------------------------------------------------

void
TestStateMachine::SubmitRequest(UInt64 decree, int numReqs, int len)
{
    // We have to pause the replica before submitting a decree
    // with more than 1 request. Otherwise the replica might pick up just a single
    // request and propose it.
    Pause();
    UT_AssertIsTrue(m_isPrimary);
    InterlockedIncrement(&m_numOutstanding);
    UInt64 *cookie = new UInt64(decree);
    Ptr<Vote> vote = VoteFactory::NewVote(m_version, MemberId(), decree,
        m_lastConfigurationNumber, BallotNumber(), numReqs, len, cookie);
    for (int i = 0; i < numReqs; i++)
    {
        RSLResponseCode resp = ReplicateRequest(VoteFactory::ReqBuf(decree), len, cookie);
        UT_AssertAreEqual(resp, RSLSuccess);
    }
    Resume();
                
}

RSLResponseCode
TestStateMachine::SubmitFastReadRequest(UInt64 decree, UInt32 timeout, bool success)
{
    UInt64 *cookie = new UInt64(decree);
    VoteFactory::InsertFastRead(cookie, success);
    RSLResponseCode s = FastReadRequest(decree, cookie, sizeof(UInt64), cookie, timeout);
    if (s != RSLSuccess)
    {
        UT_AssertAreEqual(s, RSLFastReadStale);
        VoteFactory::DeleteFastRead(cookie, false);
        delete cookie;
    }
    return s;
}

RSLResponseCode
TestStateMachine::SubmitChangeConfig(RSLNodeCollection &nodes)
{
    RSLMemberSet newMemberSet(nodes, NULL, 0);
    return ChangeConfiguration(&newMemberSet, NULL);
}

void
TestStateMachine::SubmitRequest(UInt64 decree)
{
    Pause();
    UT_AssertIsTrue(m_isPrimary);
    InterlockedIncrement(&m_numOutstanding);
    UInt64 *cookie = new UInt64(decree);
    Ptr<Vote> vote = VoteFactory::NewVote(m_version, MemberId(), decree, m_lastConfigurationNumber, BallotNumber());
    for (int i = 0; i < VoteFactory::NumReqs(decree); i++)
    {
        RSLResponseCode resp = ReplicateRequest(VoteFactory::ReqBuf(decree), VoteFactory::ReqLen(decree), cookie);
        UT_AssertAreEqual(resp, RSLSuccess);
    }
    Resume();
}

void
TestStateMachine::SetLoadState(bool *loadRet, UInt32 count)
{
    for (UInt32 i = 0; i < count; i++)
    {
        m_loadStateRetArray.push_back(loadRet[i]);
    }
}

void
TestStateMachine::SetPrimaryCookie(void *data, UInt32 len)
{
    delete m_primaryCookie;
    m_primaryCookie = new PrimaryCookie(data, len, true);
}
