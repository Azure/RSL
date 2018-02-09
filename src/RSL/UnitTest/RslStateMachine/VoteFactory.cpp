#include "VoteFactory.h"

// Every vote has a default number of requests (based on the decree number).
// (see NumReqs()). The length of the requests and the content are
// also based on the decree. This helps us test that the call backs
// in ExecuteRequest() and vote proposed by the primary are correct.
// For this reason, every Vote message MUST be contructed by the
// newVote() method. Test cases can overide the default # of requests
// by specifying the number of requests and length in NewVote().
// Those overrides are stored in the b_votes table.

map<UInt64, VoteFactory::VoteInfo> VoteFactory::g_votes;
map<void*, bool> VoteFactory::g_fastReads;
CRITSEC VoteFactory::g_votesLock;
char* VoteFactory::g_buffer = NULL;

void
VoteFactory::Init()
{
    if (g_buffer == NULL)
    {
        g_buffer = (char *) malloc(c_BufSize);
        for (int i = 0; i < c_BufSize; i++)
        {
            g_buffer[i] = 'a' + (char) (i % 26);
        }
    }
}

int
VoteFactory::NumReqs(UInt64 decree)
{
    AutoCriticalSection lock(&g_votesLock);
    map<UInt64, VoteInfo>::iterator iter = g_votes.find(decree);
    if (iter != g_votes.end())
    {
        return iter->second.first;
    }
    else
    {
        //  use a prime number (17) to get a better distrubtion.
        return (int) (decree*17 % 30) + 1;
    }
}

int
VoteFactory::ReqLen(UInt64 decree)
{
    AutoCriticalSection lock(&g_votesLock);
    map<UInt64, VoteInfo>::iterator iter = g_votes.find(decree);
    if (iter != g_votes.end())
    {
        return iter->second.second;
    }
    else
    {
        return (int) (decree*13 % 10) + 1;
    }
}

char*
VoteFactory::ReqBuf(UInt64 decree)
{
    return  g_buffer + (int) (decree % 26);
}

// constructs and returns a new vote.
Vote*
VoteFactory::NewVote(RSLProtocolVersion version, MemberId memberId, UInt64 decree,
                    UInt32 configurationNumber, BallotNumber ballot, int numReqs,
                    int len, void *cookie)
{
    PrimaryCookie primaryCookie;
    Vote *vote = new Vote(version, memberId, decree, configurationNumber, ballot, &primaryCookie);
    for (int i = 0; i < numReqs; i++)
    {
        vote->AddRequest(ReqBuf(decree), len, cookie);
    }
    AutoCriticalSection lock(&g_votesLock);
    map<UInt64, VoteInfo>::iterator iter = g_votes.find(decree);
    // make sure this is the first time we're constructing this vote.
    // Otherwise we'll end up with conflicting # of requests for this vote. 
    UT_AssertIsTrue(iter == g_votes.end());
    g_votes.insert(make_pair(decree, VoteInfo(numReqs, len)));
    return vote;
}

Vote*
VoteFactory::NewEmptyVote(RSLProtocolVersion version, UInt64 decree, UInt32 configurationNumber)
{
    // Add empty vote to map (for validation later)
    {
        AutoCriticalSection lock(&g_votesLock);
        map<UInt64, VoteInfo>::iterator iter = g_votes.find(decree);
        if (iter == g_votes.end())
        {
            g_votes.insert(make_pair(decree, VoteInfo(0, 0)));
        }
    }
    PrimaryCookie primaryCookie;
    Vote *vote = new Vote(version, MemberId(), decree, configurationNumber, BallotNumber(), &primaryCookie);
    return vote;
}

// Returns a vote with default # of requests.
Vote*
VoteFactory::NewVote(RSLProtocolVersion version, MemberId memberId, UInt64 decree,
                    UInt32 configurationNumber, BallotNumber ballot)
{
    PrimaryCookie primaryCookie;
    return NewVote(version, memberId, decree, configurationNumber, ballot, &primaryCookie);
}

Vote*
VoteFactory::NewVote(RSLProtocolVersion version, MemberId memberId, UInt64 decree,
                    UInt32 configurationNumber, BallotNumber ballot, PrimaryCookie *primaryCookie)
{
    Vote *vote = new Vote(version, memberId, decree, configurationNumber, ballot, primaryCookie);
    for (int i = 0; i < NumReqs(decree); i++)
    {
        vote->AddRequest(ReqBuf(decree), ReqLen(decree), NULL);
    }
    return vote;
}

void
VoteFactory::InsertFastRead(void *cookie, bool success)
{
    AutoCriticalSection lock(&g_votesLock);
    map<void*, bool>::iterator iter = g_fastReads.find(cookie);
    UT_AssertIsTrue(iter == g_fastReads.end());
    g_fastReads.insert(make_pair(cookie, success));
}

UInt32
VoteFactory::NumFastReads()
{
    AutoCriticalSection lock(&g_votesLock);
    return (UInt32) g_fastReads.size();
}

void
VoteFactory::DeleteFastRead(void *cookie, bool success)
{
    AutoCriticalSection lock(&g_votesLock);
    map<void*, bool>::iterator iter = g_fastReads.find(cookie);
    UT_AssertIsTrue(iter != g_fastReads.end());
    bool exp = iter->second;
    UT_AssertAreEqual(success, exp);
    g_fastReads.erase(iter);
}

void
VoteFactory::VerifyVote(Vote *vote)
{
    MarshalData marshal;
    vote->Marshal(&marshal);
    // vote must be correctly checksumed.
    UT_AssertIsTrue(vote->VerifyChecksum((char *) marshal.GetMarshaled(), marshal.GetMarshaledLength()));

    if (vote->m_isReconfiguration)
    {
        UT_AssertAreEqual(0, vote->m_numRequests);
    } 
    else
    {
        // vote must have the same # of requests, request length as we expect.
        UInt64 decree = vote->m_decree;
        UInt32 expectedNumRequests = NumReqs(decree);
        UT_AssertAreEqual(expectedNumRequests, vote->m_numRequests);
        RequestCtx *ctx;
        for (ctx = vote->m_requests.head; ctx; ctx = ctx->link.next)
        {
            UInt32 expectedReqLen = ReqLen(decree);
            UT_AssertAreEqual(expectedReqLen, ctx->m_bufLen);
            UT_AssertIsTrue(memcmp(ctx->m_requestBuf, ReqBuf(decree), ctx->m_bufLen) == 0);
        }
    }
}

