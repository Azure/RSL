#pragma once
#include "legislator.h"
#include <map>
#include "unittest.h"

using namespace RSLibImpl;
using namespace std;

class VoteFactory
{
private:
    typedef pair<int, int> VoteInfo;
    static map<UInt64, VoteInfo> g_votes;
    static map<void*, bool> g_fastReads;
    static CRITSEC g_votesLock;

public:
    static char* g_buffer;
    static const int c_BufSize = 10*1024*1024;
    static const UInt32 s_MaxVoteLen = 3*1000*1000;
    static const UInt32 s_MaxCacheLength = 50*1024*1024;

    static void Init();

    static int NumReqs(UInt64 decree);
    static int ReqLen(UInt64 decree);
    static char* ReqBuf(UInt64 decree);

    static Vote* NewVote(RSLProtocolVersion version, MemberId memberId, UInt64 decree,
                         UInt32 configurationNumber, BallotNumber ballot, int numReqs,
                         int len, void *cookie);

    static Vote* NewVote(RSLProtocolVersion version, MemberId memberId, UInt64 decree,
                         UInt32 configurationNumber, BallotNumber ballot);

    static Vote* NewVote(RSLProtocolVersion version, MemberId memberId, UInt64 decree,
                         UInt32 configurationNumber, BallotNumber ballot, PrimaryCookie *primaryCookie);
    static Vote* NewEmptyVote(RSLProtocolVersion version, UInt64 decree, UInt32 configurationNumber);

    static void VerifyVote(Vote *vote);

    static void InsertFastRead(void *cookie, bool success);
    static void DeleteFastRead(void *cookie, bool success);
    static UInt32 NumFastReads();
};