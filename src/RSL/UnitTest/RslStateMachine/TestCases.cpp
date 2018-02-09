
#pragma warning(disable:4512)
#include "legislatortest.h"
#include "unittest.h"
#include "apdiskio.h"
#include "rslutil.h"
#include <map>
#include <strsafe.h>

using namespace std;

void
LegislatorTest::TestInvalidMessages()
{
    MoveToSecondary();

    StatusResponse initState;
    GetStatus(&initState);

    UT_AssertAreEqual(Legislator::StableSecondary, initState.m_state);

    // Send garbage data to client and server ports
    const UInt32 length = 1000;
    char buf[length];
    for (UInt32 i = 0; i < length; ++i)
    {
        buf[i] = (char)(i % 26) + 'a';
    }
    SendBuffer(buf, length, true);
    SendBuffer(buf, length, false);

    // Send message with incorrect version
    Message *msg = new Message(m_version, Message_StatusQuery, m_memberId, 0, m_self.m_configNumber, BallotNumber());
    msg->m_version = (RSLProtocolVersion)100;
    SendMessage(msg, true);
    SendMessage(msg, false);
    delete msg;

    // Send an invalid messageId
    msg = new Message(m_version, 1000, m_memberId, 0, m_self.m_configNumber, BallotNumber());
    SendMessage(msg, true);
    SendMessage(msg, false);
    delete msg;

    // Send message with incorrect magic
    msg = NewVote(m_version, m_memberId, initState.m_decree + 2, initState.m_ballot);
    msg->m_magic = 1;
    SendMessage(msg, true);
    SendMessage(msg, false);
    delete msg;

    UInt16 messages[] = { Message_Vote, Message_VoteAccepted,
        Message_Prepare, Message_PrepareAccepted,
        Message_StatusQuery, Message_StatusResponse,
        Message_FetchVotes, Message_FetchCheckpoint,
        Message_NotAccepted };

    // send only part of each of the above message.
    for (int i = 0; i < sizeof(messages) / sizeof(messages[0]); i++)
    {
        msg = GenerateMessage(messages[i], initState.m_decree + 1, initState.m_ballot);
        MarshalData *marshal = new MarshalData();
        msg->Marshal(marshal);
        SendBuffer(marshal->GetMarshaled(), marshal->GetMarshaledLength() - 1, true);
        SendBuffer(marshal->GetMarshaled(), marshal->GetMarshaledLength() - 1, false);
        delete marshal;
        delete msg;
    }

    // Send prepare accepted without a vote
    msg = new Message(m_version, Message_PrepareAccepted, m_memberId, 0, m_self.m_configNumber, BallotNumber());
    SendMessage(msg, true);
    SendMessage(msg, false);
    delete msg;

    // Send a valid vote from an unknown member. Vote should be ignored
    msg = NewVote(m_version, MemberId("500"), initState.m_decree + 1, initState.m_ballot);
    SendMessage(msg, true);
    delete msg;

    // send the following votes to the server port. Since these are
    // supposed to be responses, or go to fetch port, replica
    // should ignore these messages
    UInt16 respMsg[] = { Message_VoteAccepted, Message_PrepareAccepted, Message_NotAccepted,
        Message_StatusResponse, Message_FetchVotes, Message_FetchCheckpoint };

    for (int i = 0; i < sizeof(respMsg) / sizeof(respMsg[0]); i++)
    {
        msg = GenerateMessage(respMsg[i], initState.m_decree, initState.m_ballot);
        SendRequest(msg);
        delete msg;
    }

    // send the following votes to the client port. Since these are
    // supposed to be requests, or go to fetch port, replica
    // should ignore these messages
    UInt16 reqMsg[] = { Message_Vote, Message_Prepare, Message_StatusQuery,
        Message_FetchVotes, Message_FetchCheckpoint };

    for (int i = 0; i < sizeof(reqMsg) / sizeof(reqMsg[0]); i++)
    {
        msg = GenerateMessage(reqMsg[i], initState.m_decree, initState.m_ballot);
        SendResponse(msg);
        delete msg;
    }

    // send a message with legislator's memberid
    msg = new Message(m_version, Message_StatusQuery, m_legislatorMemberId, 0, m_self.m_configNumber, BallotNumber());
    SendRequest(msg);
    delete msg;
    ReceiveResponse(Message_StatusResponse, m_status.m_decree, m_status.m_ballot);

    // wait for 200 ms to stabalize everything
    Sleep(200);

    // These replica's state should not have changed. 
    StatusResponse finalState;
    GetStatus(&finalState);
    UT_AssertAreEqual(Legislator::StableSecondary, finalState.m_state);
    UT_AssertAreEqual(initState.m_decree, finalState.m_decree);
    UT_AssertIsTrue(initState.m_ballot == finalState.m_ballot);
    UT_AssertIsTrue(initState.m_maxBallot == finalState.m_maxBallot);
}

unsigned int largeRand(unsigned long long maxuint)
{
	unsigned int num = 0;

	unsigned long long max = 1;

	do
	{
		num = num * RAND_MAX + rand();
		max = max * RAND_MAX;
	} while (max < maxuint);

	return num % maxuint;
}

void
LegislatorTest::TestNotAcceptedMsg(UInt32 state)
{
    MoveToState(state);

    // same ballot with different decree combinations
    SendResponse(Message_NotAccepted, m_status.m_decree - 1, m_status.m_ballot);
    SendResponse(Message_NotAccepted, m_status.m_decree, m_status.m_ballot);

    SendResponse(Message_NotAccepted, m_status.m_decree + 1, m_status.m_ballot);
    // replica should move to Initializing
    ReceiveRequest(Message_StatusQuery, m_status.m_decree, m_status.m_ballot);
    MoveToState(state);

    // lower ballot with different valid decree combinations.
    // Legislator should ignore this message
    SendResponse(Message_NotAccepted, m_status.m_decree - 1, LowerBallot(m_status.m_ballot));
    SendResponse(Message_NotAccepted, m_status.m_decree, LowerBallot(m_status.m_ballot));
    SendResponse(Message_NotAccepted, m_status.m_decree + 1, LowerBallot(m_status.m_ballot));

    // higher ballot with different valid decree combinations.
    SendResponse(Message_NotAccepted, m_status.m_decree - 1, HighBallot(m_status.m_ballot));
    SendResponse(Message_NotAccepted, m_status.m_decree, HighBallot(m_status.m_ballot));
    if (HighBallot(m_status.m_ballot) > m_status.m_maxBallot)
    {
        // replica should remain stable with a higher ballot
        WaitForState(Legislator::StableSecondary, m_status.m_ballot, m_status.m_decree, HighBallot(m_status.m_ballot));
        MoveToState(state);
    }
    // else replica should ignore this message

    // Higher decree with higher ballot
    SendResponse(Message_NotAccepted, m_status.m_decree + 1, HighBallot(m_status.m_ballot));
    // replica should move to Initializing
    ReceiveRequest(Message_StatusQuery, m_status.m_decree, m_status.m_ballot);
    MoveToState(state);
    // 
    if (m_status.m_maxBallot > m_status.m_ballot)
    {
        // legislator in state Preparing or SecondaryPrepared

        SendResponse(Message_NotAccepted, m_status.m_decree - 1, m_status.m_maxBallot);

        SendResponse(Message_NotAccepted, m_status.m_decree - 1, LowerBallot(m_status.m_maxBallot));

        SendResponse(Message_NotAccepted, m_status.m_decree - 1, HighBallot(m_status.m_maxBallot));

        SendResponse(Message_NotAccepted, m_status.m_decree, m_status.m_maxBallot);

        SendResponse(Message_NotAccepted, m_status.m_decree, LowerBallot(m_status.m_maxBallot));

        SendResponse(Message_NotAccepted, m_status.m_decree, HighBallot(m_status.m_maxBallot));
        // replica should remain stable with a higher ballot
        WaitForState(Legislator::StableSecondary, m_status.m_ballot, m_status.m_decree, HighBallot(m_status.m_maxBallot));
        MoveToState(state);

        SendResponse(Message_NotAccepted, m_status.m_decree + 1, m_status.m_maxBallot);
        ReceiveRequest(Message_StatusQuery, m_status.m_decree, m_status.m_ballot);
        MoveToState(state);

        // send a higher decree with a ballot lower than maxballot but higher than ballot
        // should move to initializing
        SendResponse(Message_NotAccepted, m_status.m_decree + 1, LowerBallot(m_status.m_maxBallot));
        ReceiveRequest(Message_StatusQuery, m_status.m_decree, m_status.m_ballot);
        MoveToState(state);

        // higher decree with higher ballot
        SendResponse(Message_NotAccepted, m_status.m_decree + 1, HighBallot(m_status.m_maxBallot));
        ReceiveRequest(Message_StatusQuery, m_status.m_decree, m_status.m_ballot);
        MoveToState(state);
    }
}

void
LegislatorTest::TestNotAcceptedMsgInInitialize(UInt32 state)
{
    // send all possible combinations of NotAcceptedMsg.
    MoveToState(state);
    BallotNumber ballot(m_status.m_maxBallot.m_ballotId, m_memberId);
    MsgCombinations(Message_NotAccepted, m_status.m_decree, ballot);

    if (m_status.m_maxBallot > m_status.m_ballot)
    {
        ballot.m_ballotId = m_status.m_ballot.m_ballotId;
        MsgCombinations(Message_NotAccepted, m_status.m_decree, ballot);
    }
}

void
LegislatorTest::TestVoteMsg(UInt32 state)
{
    MoveToState(state);

    // this test case is only for non-prepared states.
    UT_AssertIsTrue(m_status.m_ballot == m_status.m_maxBallot);

    // Vote message should always have the proposer's memberId in the ballot.
    // Propose votes with m_status.m_ballot if it has my memberId.
    if (m_status.m_ballot.m_memberId == m_memberId)
    {
        // send all possible combinations of decree.
        SendRequest(Message_Vote, m_status.m_decree - 1, m_status.m_ballot);
        //        ReceiveResponse(Message_NotAccepted, m_status.m_decree, m_status.m_ballot);

        SendRequest(Message_Vote, m_status.m_decree, m_status.m_ballot);
        ReceiveResponse(Message_VoteAccepted, m_status.m_decree, m_status.m_ballot);

        SendRequest(Message_Vote, m_status.m_decree + 1, m_status.m_ballot);
        ReceiveResponse(Message_VoteAccepted, m_status.m_decree + 1, m_status.m_ballot);
        // since this changes the decree number and possibly changes state
        // call MoveToState() to move back to the initial state.
        MoveToState(state);
    }

    SendRequest(Message_Vote, m_status.m_decree - 1, LowerBallot(m_status.m_ballot));
    ReceiveResponse(Message_NotAccepted, m_status.m_decree, m_status.m_ballot);

    SendRequest(Message_Vote, m_status.m_decree, LowerBallot(m_status.m_ballot));
    ReceiveResponse(Message_NotAccepted, m_status.m_decree, m_status.m_ballot);

    SendRequest(Message_Vote, m_status.m_decree + 1, LowerBallot(m_status.m_ballot));
    ReceiveResponse(Message_NotAccepted, m_status.m_decree, m_status.m_ballot);

    SendRequest(Message_Vote, m_status.m_decree - 1, HighBallot(m_status.m_ballot));
    //    ReceiveResponse(Message_NotAccepted, m_status.m_decree, m_status.m_ballot);

    SendRequest(Message_Vote, m_status.m_decree, HighBallot(m_status.m_ballot));
    ReceiveResponse(Message_VoteAccepted, m_status.m_decree, HighBallot(m_status.m_ballot));
    MoveToState(state);

    SendRequest(Message_Vote, m_status.m_decree + 1, HighBallot(m_status.m_ballot));
    ReceiveRequest(Message_StatusQuery, m_status.m_decree, m_status.m_ballot);
    MoveToState(state);
}

void
LegislatorTest::TestVoteMsgInPrepared(UInt32 state)
{
    // send vote message
    MoveToState(state);

    // this test case is only for prepared states. In prepared state
    // the difference between m_status.m_maxBallot and m_status.m_ballot
    // is >= 10.
    UT_AssertIsTrue(m_status.m_maxBallot > m_status.m_ballot);

#if 0
    // Send all possible combinations of vote message with ballotId < maxBallotId.
    // All of these should be rejected.
    BallotNumber ballot(m_status.m_ballot.m_ballotId, m_memberId);
    MsgCombinations(Message_Vote, m_status.m_decree, ballot,
        Message_NotAccepted, m_status.m_decree, m_status.m_maxBallot);
#endif

    if (m_status.m_maxBallot.m_memberId == m_memberId)
    {
        SendRequest(Message_Vote, m_status.m_decree - 1, m_status.m_maxBallot);
        //        ReceiveResponse(Message_NotAccepted, m_status.m_decree, m_status.m_maxBallot);

        SendRequest(Message_Vote, m_status.m_decree, m_status.m_maxBallot);
        ReceiveResponse(Message_VoteAccepted, m_status.m_decree, m_status.m_maxBallot);
        MoveToState(state);

        SendRequest(Message_Vote, m_status.m_decree + 1, m_status.m_maxBallot);
        // should move to initializing
        ReceiveRequest(Message_StatusQuery, m_status.m_decree, m_status.m_ballot);
        WaitForState(Legislator::Initializing, m_status.m_ballot, m_status.m_decree, m_status.m_maxBallot);
        MoveToState(state);
    }

    SendRequest(Message_Vote, m_status.m_decree - 1, LowerBallot(m_status.m_maxBallot));
    ReceiveResponse(Message_NotAccepted, m_status.m_decree, m_status.m_maxBallot);

    SendRequest(Message_Vote, m_status.m_decree, LowerBallot(m_status.m_maxBallot));
    ReceiveResponse(Message_NotAccepted, m_status.m_decree, m_status.m_maxBallot);

    SendRequest(Message_Vote, m_status.m_decree + 1, LowerBallot(m_status.m_maxBallot));
    ReceiveResponse(Message_NotAccepted, m_status.m_decree + 1, m_status.m_maxBallot);

    GetStatus(&m_status);
    SendRequest(Message_Vote, m_status.m_decree - 1, HighBallot(m_status.m_maxBallot));
    //    ReceiveResponse(Message_NotAccepted, m_status.m_decree, m_status.m_maxBallot);

    SendRequest(Message_Vote, m_status.m_decree, HighBallot(m_status.m_maxBallot));
    ReceiveResponse(Message_VoteAccepted, m_status.m_decree, HighBallot(m_status.m_maxBallot));
    MoveToState(state);

    SendRequest(Message_Vote, m_status.m_decree + 1, HighBallot(m_status.m_maxBallot));
    ReceiveRequest(Message_StatusQuery, m_status.m_decree, m_status.m_ballot);

    WaitForState(Legislator::Initializing, m_status.m_ballot, m_status.m_decree, m_status.m_maxBallot);
    MoveToState(state);
}

void
LegislatorTest::TestVoteMsgInInitialize(UInt32 state)
{
    StatusResponse serialize;

    MoveToState(state);

    // send votes in initializing state
    UT_AssertIsTrue(m_status.m_state == Legislator::Initializing);

    // send a single vote. This should be cached in the m_voteCache.
    SendRequest(Message_Vote, m_status.m_decree - 1, m_status.m_ballot);

    // Before we send the response, we must make sure that the legislator
    // got the request. The only way to ensure that is to send a status
    // request and get a response back. Otherwise, even if we get
    // a notification from netlib it does not mean that the legislator
    // already got the request
    GetStatus(&serialize);

    SendStatusResponse(m_status.m_decree + 1, m_status.m_ballot, m_status.m_decree, m_status.m_ballot, -1);
    // when sending the votes, wait for the legislator to close the connection
    HandleLearnVotes(m_status.m_decree, m_status.m_ballot, m_status.m_decree + 1, m_status.m_ballot, true);

    // legislator would get the first vote, look in its voteCache and realize
    // that it doesn't need to learn any more votes.
    // It should go back to stable with intial decrees and ballot
    if (m_status.m_maxBallot > m_status.m_ballot)
    {
        ReceiveResponse(Message_NotAccepted, m_status.m_decree, m_status.m_maxBallot);
    }
    WaitForState(Legislator::StableSecondary, m_status.m_ballot, m_status.m_decree, m_status.m_maxBallot);
    MoveToState(state);

    // send multiple votes. All of them should be cached in m_voteCache.
    SendRequest(Message_Vote, m_status.m_decree + 1, m_status.m_ballot);
    SendRequest(Message_Vote, m_status.m_decree + 2, m_status.m_ballot);
    SendRequest(Message_Vote, m_status.m_decree + 2, m_status.m_ballot);
    SendRequest(Message_Vote, m_status.m_decree + 3, m_status.m_ballot);
    SendRequest(Message_Vote, m_status.m_decree + 3, HighBallot(m_status.m_ballot));

    // serialize all previous requests.
    GetStatus(&serialize);
    UInt16 msgId;
    // move the replica to learning and send only enough votes to fill the gaps
    SendStatusResponse(m_status.m_decree + 1, m_status.m_ballot, m_status.m_decree, m_status.m_ballot, -1);
    HandleLearnVotes(m_status.m_decree, m_status.m_ballot, m_status.m_decree + 1, m_status.m_ballot, true);

    BallotNumber maxBallot = max(m_status.m_maxBallot, HighBallot(m_status.m_ballot));
    if (m_status.m_maxBallot > HighBallot(m_status.m_ballot))
    {
        msgId = Message_NotAccepted;
    }
    else
    {
        msgId = Message_VoteAccepted;
    }
    ReceiveResponse(msgId, m_status.m_decree + 3, maxBallot);

    WaitForState(Legislator::StableSecondary, HighBallot(m_status.m_ballot), m_status.m_decree + 3, maxBallot);
    MoveToState(state);

    BallotNumber ballot = HighBallot(m_status.m_ballot);
    UInt64 decree = m_status.m_decree + 1;
    SendRequest(Message_Vote, decree - 1, ballot);
    SendRequest(Message_Vote, decree, ballot);
    SendRequest(Message_Vote, decree, LowerBallot(ballot));
    SendRequest(Message_Vote, decree - 1, ballot);
    SendRequest(Message_Vote, decree - 1, LowerBallot(ballot));
    SendRequest(Message_Vote, decree + 1, ballot);

    // serialize all previous requests.
    GetStatus(&serialize);
    SendStatusResponse(m_status.m_decree + 1, m_status.m_ballot, m_status.m_decree, m_status.m_ballot, -1);
    HandleLearnVotes(m_status.m_decree, m_status.m_ballot, m_status.m_decree, m_status.m_ballot, true);

    maxBallot = max(m_status.m_maxBallot, ballot);
    if (m_status.m_maxBallot > ballot)
    {
        msgId = Message_NotAccepted;
    }
    else
    {
        msgId = Message_VoteAccepted;
    }
    ReceiveResponse(msgId, decree + 1, maxBallot);

    WaitForState(Legislator::StableSecondary, ballot, decree + 1, maxBallot);
    MoveToState(state);

    // send some decrees and then send some more decrees with a hole in middle
    SendRequest(Message_Vote, m_status.m_decree + 1, m_status.m_ballot);
    SendRequest(Message_Vote, m_status.m_decree + 2, m_status.m_ballot);
    // this should cause    the legislator to switch to learning
    SendRequest(Message_Vote, m_status.m_decree + 4, m_status.m_ballot);
    SendRequest(Message_Vote, m_status.m_decree + 5, m_status.m_ballot);

    // serialize all previous requests.
    GetStatus(&serialize);
    SendStatusResponse(m_status.m_decree + 1, m_status.m_ballot, m_status.m_decree, m_status.m_ballot, -1);
    HandleLearnVotes(m_status.m_decree, m_status.m_ballot, m_status.m_decree + 2, m_status.m_ballot);

    ReceiveRequest(Message_StatusQuery, m_status.m_decree + 2, m_status.m_ballot);
    SendRequest(Message_Vote, m_status.m_decree + 5, m_status.m_ballot);
    // serialize all previous requests.
    GetStatus(&serialize);

    SendStatusResponse(m_status.m_decree + 5, m_status.m_ballot, m_status.m_decree + 2, m_status.m_ballot, -1);
    HandleLearnVotes(m_status.m_decree + 2, m_status.m_ballot, m_status.m_decree + 5, m_status.m_ballot, true);

    if (m_status.m_maxBallot > m_status.m_ballot)
    {
        msgId = Message_NotAccepted;
    }
    else
    {
        msgId = Message_VoteAccepted;
    }
    ReceiveResponse(msgId, m_status.m_decree + 5, m_status.m_maxBallot);

    WaitForState(Legislator::StableSecondary, m_status.m_ballot, m_status.m_decree + 5, m_status.m_maxBallot);
    MoveToState(state);

}

void
LegislatorTest::TestVoteAcceptedMsg(UInt32 state)
{
    MoveToState(state);

    if (m_status.m_ballot < m_status.m_maxBallot)
    {
        BallotNumber ballot(m_status.m_ballot.m_ballotId, m_memberId);
        MsgCombinations(Message_VoteAccepted, m_status.m_decree, ballot);
    }

    BallotNumber ballot(m_status.m_maxBallot.m_ballotId, m_memberId);
    MsgCombinations(Message_VoteAccepted, m_status.m_decree, ballot);

    ballot.m_memberId = m_legislatorMemberId;
    MsgCombinations(Message_VoteAccepted, m_status.m_decree, ballot);
}

void
LegislatorTest::TestVoteAcceptedMsgInPrimary()
{
    // Move to secondary and pass a new decree and wait until
    // the previous decree gets executed.
    // The reason for first doing this instead of moving to
    // primary directly is because we want to be in a state
    // where the last decree has been accepted but has not been executed - as
    // soon as the replica becomes the primary and reproposes
    // the decree, the decree should be accepted and then executed immediately.
    MoveToSecondary();
    Ptr<Vote> msg = NewVote(m_version, m_memberId, m_status.m_decree + 1, m_status.m_ballot);

    Message *resp = SendAndReceive(msg, Message_VoteAccepted);
    UT_AssertAreEqual(msg->m_decree, resp->m_decree);
    UT_AssertIsTrue(msg->m_ballot == resp->m_ballot);

    WaitUntil(m_machine->GetCurrentSequenceNumber() == m_status.m_decree);

    MoveToPrimary();

    UInt64 expSeq = m_machine->GetCurrentSequenceNumber();

    // send all possible message combinations.
    for (int i = -1; i <= 1; i++)
    {
        for (int j = -1; j <= 1; j++)
        {
            UInt64 decree = m_status.m_decree + i;
            BallotNumber ballot = m_status.m_maxBallot;
            ballot.m_ballotId += j;
            if (decree < m_status.m_decree && ballot > m_status.m_ballot ||
                decree > m_status.m_decree && ballot < m_status.m_ballot)
            {
                continue;
            }
            UT_AssertAreEqual(expSeq, m_machine->GetCurrentSequenceNumber());
            SendResponse(Message_VoteAccepted, decree, ballot);
            if (decree == m_status.m_decree && ballot == m_status.m_maxBallot)
            {
                // As sson as we accept the current outstanding decree, it
                // should be executed.
                expSeq = m_status.m_decree;
                WaitUntil(expSeq == m_machine->GetCurrentSequenceNumber());
            }
        }
    }
    WaitForState(m_status.m_state, m_status.m_ballot, m_status.m_decree, m_status.m_maxBallot);

    m_machine->SubmitRequest(m_status.m_decree + 1, 5, 200);

    ReceiveRequest(Message_Vote, m_status.m_decree + 1, m_status.m_ballot);

    // submit another request while it is waiting for a response from this
    m_machine->SubmitRequest(m_status.m_decree + 2, 5, 200);

    // accept status.m_decree+1
    SendResponse(Message_VoteAccepted, m_status.m_decree + 1, m_status.m_ballot);
    WaitUntil(m_status.m_decree + 1 == m_machine->GetCurrentSequenceNumber());

    ReceiveRequest(Message_Vote, m_status.m_decree + 2, m_status.m_ballot);
    m_machine->SubmitRequest(m_status.m_decree + 3, 5, 200);
    // send not accepted - should move to stable secondary and abort requests
    SendResponse(Message_NotAccepted, m_status.m_decree + 2, HighBallot(m_status.m_ballot));
    WaitUntil(m_machine->m_isPrimary == false);
    // should not have executed decrees m_status+2 and m_status+3.
    UT_AssertAreEqual(m_status.m_decree + 1, m_machine->GetCurrentSequenceNumber());
}

void
LegislatorTest::TestPrepareMsg(UInt32 state)
{
    MoveToState(state);

    // PrepareMsg must have m_ballotId.m_memberId == proposer's memberId. 
    if (m_status.m_ballot < m_status.m_maxBallot)
    {
        // All these messages should be rejected.
        BallotNumber ballot(m_status.m_ballot.m_ballotId, m_memberId);
        MsgCombinations(Message_Prepare, m_status.m_decree, ballot,
            Message_NotAccepted, m_status.m_decree, m_status.m_maxBallot);
    }
    else
    {
        // lower decree
        BallotNumber ballot(m_status.m_ballot.m_ballotId, m_memberId);
        SendRequest(Message_Prepare, m_status.m_decree - 1, ballot);
        if (ballot < m_status.m_maxBallot)
        {
            ReceiveResponse(Message_NotAccepted, m_status.m_decree, m_status.m_maxBallot);
        }

        SendRequest(Message_Prepare, m_status.m_decree - 1, LowerBallot(ballot));
        ReceiveResponse(Message_NotAccepted, m_status.m_decree, m_status.m_maxBallot);

        SendRequest(Message_Prepare, m_status.m_decree - 1, HighBallot(ballot));
        //        ReceiveResponse(Message_NotAccepted, m_status.m_decree, m_status.m_maxBallot);

    }

    PrepareAccepted *msg;
    if (m_status.m_maxBallot.m_memberId == m_memberId)
    {
        SendRequest(Message_Prepare, m_status.m_decree, m_status.m_maxBallot);
        msg = (PrepareAccepted *)ReceiveResponse(Message_PrepareAccepted, m_status.m_decree, m_status.m_maxBallot);
        UT_AssertAreEqual(msg->m_vote->m_decree, m_status.m_decree);
        UT_AssertIsTrue(msg->m_vote->m_ballot == m_status.m_ballot);
        WaitForState(Legislator::StableSecondary, m_status.m_ballot, m_status.m_decree, m_status.m_maxBallot);
        MoveToState(state);

        SendRequest(Message_Prepare, m_status.m_decree + 1, m_status.m_maxBallot);
        msg = (PrepareAccepted *)ReceiveResponse(Message_PrepareAccepted, m_status.m_decree + 1, m_status.m_maxBallot);
        UT_AssertAreEqual(msg->m_vote->m_decree, m_status.m_decree);
        UT_AssertIsTrue(msg->m_vote->m_ballot == m_status.m_ballot);
        // should move to initializing
        ReceiveRequest(Message_StatusQuery, m_status.m_decree, m_status.m_ballot);
        WaitForState(Legislator::Initializing, m_status.m_ballot, m_status.m_decree, m_status.m_maxBallot);
        MoveToState(state);
    }

    SendRequest(Message_Prepare, m_status.m_decree - 1, LowerBallot(m_status.m_maxBallot));
    ReceiveResponse(Message_NotAccepted, m_status.m_decree, m_status.m_maxBallot);

    SendRequest(Message_Prepare, m_status.m_decree, LowerBallot(m_status.m_maxBallot));
    ReceiveResponse(Message_NotAccepted, m_status.m_decree, m_status.m_maxBallot);

    SendRequest(Message_Prepare, m_status.m_decree + 1, LowerBallot(m_status.m_maxBallot));
    ReceiveResponse(Message_NotAccepted, m_status.m_decree, m_status.m_maxBallot);

    SendRequest(Message_Prepare, m_status.m_decree - 1, HighBallot(m_status.m_maxBallot));
    //    ReceiveResponse(Message_NotAccepted, m_status.m_decree, m_status.m_maxBallot);

    SendRequest(Message_Prepare, m_status.m_decree, HighBallot(m_status.m_maxBallot));
    msg = (PrepareAccepted *)ReceiveResponse(Message_PrepareAccepted, m_status.m_decree, HighBallot(m_status.m_maxBallot));
    UT_AssertAreEqual(msg->m_vote->m_decree, m_status.m_decree);
    UT_AssertIsTrue(msg->m_vote->m_ballot == m_status.m_ballot);
    WaitForState(Legislator::StableSecondary, m_status.m_ballot, m_status.m_decree, HighBallot(m_status.m_maxBallot));
    MoveToState(state);

    SendRequest(Message_Prepare, m_status.m_decree + 1, HighBallot(m_status.m_maxBallot));
    msg = (PrepareAccepted *)ReceiveResponse(Message_PrepareAccepted, m_status.m_decree + 1, HighBallot(m_status.m_maxBallot));
    UT_AssertAreEqual(msg->m_vote->m_decree, m_status.m_decree);
    UT_AssertIsTrue(msg->m_vote->m_ballot == m_status.m_ballot);
    ReceiveRequest(Message_StatusQuery, m_status.m_decree, m_status.m_ballot);
    WaitForState(Legislator::Initializing, m_status.m_ballot, m_status.m_decree, HighBallot(m_status.m_maxBallot));
    MoveToState(state);
}

void
LegislatorTest::TestPrepareMsgInInitialize(UInt32 state)
{
    MoveToState(state);
    // prepare
    if (m_status.m_ballot < m_status.m_maxBallot)
    {
        BallotNumber ballot(m_status.m_ballot.m_ballotId, m_memberId);
        MsgCombinations(Message_Prepare, m_status.m_decree, ballot,
            Message_NotAccepted, m_status.m_decree, m_status.m_maxBallot);
    }
    else
    {
        BallotNumber ballot(m_status.m_ballot.m_ballotId, m_memberId);
        SendRequest(Message_Prepare, m_status.m_decree - 1, ballot);
        //        ReceiveResponse(Message_NotAccepted, m_status.m_decree, m_status.m_maxBallot);

        SendRequest(Message_Prepare, m_status.m_decree - 1, LowerBallot(ballot));
        ReceiveResponse(Message_NotAccepted, m_status.m_decree, m_status.m_maxBallot);

        SendRequest(Message_Prepare, m_status.m_decree - 1, HighBallot(ballot));
        //        ReceiveResponse(Message_NotAccepted, m_status.m_decree, m_status.m_maxBallot);
    }

    if (m_status.m_maxBallot.m_memberId == m_memberId)
    {
        SendRequest(Message_Prepare, m_status.m_decree, m_status.m_maxBallot);
        SendRequest(Message_Prepare, m_status.m_decree + 1, m_status.m_maxBallot);
    }

    SendRequest(Message_Prepare, m_status.m_decree - 1, LowerBallot(m_status.m_maxBallot));
    ReceiveResponse(Message_NotAccepted, m_status.m_decree, m_status.m_maxBallot);

    SendRequest(Message_Prepare, m_status.m_decree, LowerBallot(m_status.m_maxBallot));
    ReceiveResponse(Message_NotAccepted, m_status.m_decree, m_status.m_maxBallot);

    SendRequest(Message_Prepare, m_status.m_decree + 1, LowerBallot(m_status.m_maxBallot));
    ReceiveResponse(Message_NotAccepted, m_status.m_decree, m_status.m_maxBallot);

    SendRequest(Message_Prepare, m_status.m_decree - 1, HighBallot(m_status.m_maxBallot));
    //    ReceiveResponse(Message_NotAccepted, m_status.m_decree, m_status.m_maxBallot);

    SendRequest(Message_Prepare, m_status.m_decree, HighBallot(m_status.m_maxBallot));

    SendRequest(Message_Prepare, m_status.m_decree + 1, HighBallot(m_status.m_maxBallot));
}

void
LegislatorTest::TestPrepareAcceptedMsg(UInt32 state)
{
    MoveToState(state);

    if (m_status.m_maxBallot > m_status.m_ballot)
    {
        BallotNumber ballot(m_status.m_ballot.m_ballotId, m_memberId);
        MsgCombinations(Message_PrepareAccepted, m_status.m_decree, ballot);
    }

    BallotNumber ballot(m_status.m_maxBallot.m_ballotId, m_memberId);
    MsgCombinations(Message_PrepareAccepted, m_status.m_decree, ballot);

    if (state != LegislatorTest::Prepare)
    {
        ballot.m_memberId = m_legislatorMemberId;
        MsgCombinations(Message_PrepareAccepted, m_status.m_decree, ballot);
    }
    else
    {
        StatusResponse initStatus;
        GetStatus(&initStatus);

        for (int i = -1; i <= 1; i++)
        {
            for (int j = -1; j <= 1; j++)
            {
                UInt64 decree = m_status.m_decree + i;
                BallotNumber ballot2(m_status.m_maxBallot.m_ballotId + j, m_status.m_memberId);

                if (decree == m_status.m_decree && ballot2 == m_status.m_maxBallot)
                {
                    continue;
                }
                SendResponse(Message_PrepareAccepted, decree, ballot2);
            }
        }
        WaitForState(initStatus.m_state, initStatus.m_ballot, initStatus.m_decree, initStatus.m_maxBallot);

        PrepareAccepted *msg;
        msg = (PrepareAccepted *)GenerateMessage(Message_PrepareAccepted, m_status.m_decree, m_status.m_maxBallot);
        msg->m_vote = NewVote(m_version, m_memberId, m_status.m_decree, m_status.m_ballot);
        SendResponse(msg);
        delete msg;
        ReceiveRequest(Message_Vote, m_status.m_decree, m_status.m_maxBallot);
        WaitForState(Legislator::StablePrimary, m_status.m_maxBallot, m_status.m_decree, m_status.m_maxBallot);
        MoveToState(state);

        msg = (PrepareAccepted *)GenerateMessage(Message_PrepareAccepted, m_status.m_decree, m_status.m_maxBallot);
        msg->m_vote = NewVote(m_version, m_memberId, m_status.m_decree - 1, m_status.m_ballot);
        SendResponse(msg);
        delete msg;
        ReceiveRequest(Message_Vote, m_status.m_decree, m_status.m_maxBallot);
        WaitForState(Legislator::StablePrimary, m_status.m_maxBallot, m_status.m_decree, m_status.m_maxBallot);
        MoveToState(state);

        msg = (PrepareAccepted *)GenerateMessage(Message_PrepareAccepted, m_status.m_decree, m_status.m_maxBallot);
        msg->m_vote = NewVote(m_version, m_memberId, m_status.m_decree + 1, m_status.m_ballot);
        SendResponse(msg);
        delete msg;

        msg = (PrepareAccepted *)GenerateMessage(Message_PrepareAccepted, m_status.m_decree, m_status.m_maxBallot);
        msg->m_vote = NewVote(m_version, m_memberId, m_status.m_decree, HighBallot(m_status.m_ballot));
        SendResponse(msg);
        delete msg;
        ReceiveRequest(Message_Vote, m_status.m_decree, m_status.m_maxBallot);
        WaitForState(Legislator::StablePrimary, m_status.m_maxBallot, m_status.m_decree, m_status.m_maxBallot);
        MoveToState(state);

        msg = (PrepareAccepted *)GenerateMessage(Message_PrepareAccepted, m_status.m_decree, m_status.m_maxBallot);
        msg->m_vote = NewVote(m_version, m_memberId, m_status.m_decree + 1, HighBallot(m_status.m_ballot));
        SendResponse(msg);
        delete msg;

        msg = (PrepareAccepted *)GenerateMessage(Message_PrepareAccepted, m_status.m_decree, m_status.m_maxBallot);
        msg->m_vote = NewVote(m_version, m_memberId, m_status.m_decree, LowerBallot(m_status.m_ballot));
        SendResponse(msg);
        delete msg;
        ReceiveRequest(Message_Vote, m_status.m_decree, m_status.m_maxBallot);
        WaitForState(Legislator::StablePrimary, m_status.m_maxBallot, m_status.m_decree, m_status.m_maxBallot);
        MoveToState(state);

        msg = (PrepareAccepted *)GenerateMessage(Message_PrepareAccepted, m_status.m_decree, m_status.m_maxBallot);
        msg->m_vote = NewVote(m_version, m_memberId, m_status.m_decree - 1, LowerBallot(m_status.m_ballot));
        SendResponse(msg);
        delete msg;
        ReceiveRequest(Message_Vote, m_status.m_decree, m_status.m_maxBallot);
        WaitForState(Legislator::StablePrimary, m_status.m_maxBallot, m_status.m_decree, m_status.m_maxBallot);
        MoveToState(state);

    }
}

void
LegislatorTest::TestStatusResponseMsg(UInt32 state)
{
    MoveToState(state);

    // send a status response with different query decree
    SendStatusResponse(m_status.m_decree, m_status.m_ballot, m_status.m_decree + 1, m_status.m_ballot, -1);
    SendStatusResponse(m_status.m_decree, m_status.m_ballot, m_status.m_decree - 1, m_status.m_ballot, -1);

    if (state == LegislatorTest::Primary)
    {
        // In primary state, legislator ignores all status response message
        // unless it is trying to copy a checkpoint.
        return;
    }
    // send response with decree+1 and ballot >= maxAccepterBallot
    SendStatusResponse(m_status.m_decree + 1, HighBallot(m_status.m_ballot), m_status.m_decree, m_status.m_ballot, -1);

    HandleLearnVotes(m_status.m_decree, m_status.m_ballot, m_status.m_decree + 1, HighBallot(m_status.m_ballot));
    ReceiveRequest(Message_StatusQuery, m_status.m_decree + 1, HighBallot(m_status.m_ballot));

    MoveToState(state);

    // send response with decree+1 and ballot >= maxAccepterBallot
    SendStatusResponse(m_status.m_decree + 1, HighBallot(m_status.m_ballot), m_status.m_decree, m_status.m_ballot, -1);
    // wait for a fetch request
    FetchRequest *req = ReceiveFetchRequest(Message_FetchVotes);
    UT_AssertIsNotNull(req);
    Message *msg = req->m_message;
    UT_AssertAreEqual(m_status.m_decree, msg->m_decree);

    // send a single vote. This should be cached in the m_voteCache.
    SendRequest(Message_Vote, m_status.m_decree - 1, LowerBallot(m_status.m_ballot));
    StatusResponse serialize;
    GetStatus(&serialize);

    vector< pair<UInt64, BallotNumber> > v;
    v.push_back(make_pair(m_status.m_decree, m_status.m_ballot));

    HandleLearnVotes(req, v, true);
    delete req;
    req = NULL;

    ReceiveResponse(Message_NotAccepted, m_status.m_decree, m_status.m_maxBallot);

#if 0    
    // legislator would get the first vote, look in its voteCache and realize
    // that it doesn't need to learn any more votes.
    if (m_status.m_maxBallot.m_memberId == m_legislatorMemberId)
    {
        ReceiveRequest(Message_Prepare, m_status.m_decree, m_status.m_maxBallot);
        WaitForState(Legislator::Preparing, m_status.m_ballot, m_status.m_decree, m_status.m_maxBallot);
    }
    else
#endif
    {
        WaitForState(Legislator::StableSecondary, m_status.m_ballot, m_status.m_decree, m_status.m_maxBallot);
    }
    MoveToState(state);

}
void
LegislatorTest::TestStatusResponseMsgInInitialize(UInt32 state)
{
    MoveToState(state);

    // send a status response with different query decree
    SendStatusResponse(m_status.m_decree, m_status.m_ballot, m_status.m_decree + 1, m_status.m_ballot, -1);
    SendStatusResponse(m_status.m_decree, m_status.m_ballot, m_status.m_decree - 1, m_status.m_ballot, -1);

    // send a response with a lower decree, same ballot
    SendStatusResponse(m_status.m_decree - 1, m_status.m_ballot, m_status.m_decree, m_status.m_ballot, -1);
    BallotNumber newBallot(MaxSentBallotId() + 1, m_legislatorMemberId);
    ReceiveRequest(Message_Prepare, m_status.m_decree, newBallot);
    WaitForState(Legislator::Preparing, m_status.m_ballot, m_status.m_decree, newBallot);
    MoveToState(state);

    SendStatusResponse(m_status.m_decree, LowerBallot(m_status.m_ballot), m_status.m_decree, m_status.m_ballot, -1);
    BallotNumber newBallot1(MaxSentBallotId() + 1, m_legislatorMemberId);
    ReceiveRequest(Message_Prepare, m_status.m_decree, newBallot1);
    WaitForState(Legislator::Preparing, m_status.m_ballot, m_status.m_decree, newBallot1);
    MoveToState(state);

    // lower decree with higher ballot. Should move to preparing
    SendStatusResponse(m_status.m_decree - 1, HighBallot(m_status.m_ballot), m_status.m_decree, m_status.m_ballot, -1);
    BallotNumber newBallot2(MaxSentBallotId() + 1, m_legislatorMemberId);
    ReceiveRequest(Message_Prepare, m_status.m_decree, newBallot2);
    WaitForState(Legislator::Preparing, m_status.m_ballot, m_status.m_decree, newBallot2);
    MoveToState(state);

    // higher decree with lower ballot. Should move to preparing
    SendStatusResponse(m_status.m_decree + 1, LowerBallot(m_status.m_ballot), m_status.m_decree, m_status.m_ballot, -1);
    BallotNumber newBallot3(MaxSentBallotId() + 1, m_legislatorMemberId);
    ReceiveRequest(Message_Prepare, m_status.m_decree, newBallot3);
    WaitForState(Legislator::Preparing, m_status.m_ballot, m_status.m_decree, newBallot3);
    MoveToState(state);

    if (m_status.m_maxBallot > m_status.m_ballot)
    {
        SendStatusResponse(m_status.m_decree, LowerBallot(m_status.m_maxBallot), m_status.m_decree, m_status.m_ballot, -1);
        HandleLearnVotes(m_status.m_decree, m_status.m_ballot, m_status.m_decree, m_status.m_maxBallot);
        ReceiveRequest(Message_StatusQuery, m_status.m_decree, m_status.m_maxBallot);
        MoveToState(state);
    }

    StatusResponse *msg;

    SendStatusResponse(m_status.m_decree + 1, HighBallot(m_status.m_ballot), m_status.m_decree, m_status.m_ballot, -1);
    vector< pair<UInt64, BallotNumber> > v;
    v.push_back(make_pair(m_status.m_decree - 1, m_status.m_ballot));
    v.push_back(make_pair(m_status.m_decree, m_status.m_ballot));
    v.push_back(make_pair(m_status.m_decree + 1, m_status.m_ballot));

    HandleLearnVotes(m_status.m_decree, m_status.m_ballot, v);
    ReceiveRequest(Message_StatusQuery, m_status.m_decree, m_status.m_ballot);

    // Send votes that have a gap in between.
    SendStatusResponse(m_status.m_decree + 1, HighBallot(m_status.m_ballot), m_status.m_decree, m_status.m_ballot, -1);
    v.clear();
    v.push_back(make_pair(m_status.m_decree, m_status.m_ballot));
    v.push_back(make_pair(m_status.m_decree + 1, HighBallot(m_status.m_ballot)));

    HandleLearnVotes(m_status.m_decree, m_status.m_ballot, v);
    ReceiveRequest(Message_StatusQuery, m_status.m_decree, m_status.m_ballot);

    SendStatusResponse(m_status.m_decree + 1, HighBallot(m_status.m_ballot), m_status.m_decree, m_status.m_ballot, -1);
    v.clear();
    v.push_back(make_pair(m_status.m_decree, LowerBallot(m_status.m_ballot)));
    v.push_back(make_pair(m_status.m_decree + 1, LowerBallot(m_status.m_ballot)));

    HandleLearnVotes(m_status.m_decree, m_status.m_ballot, v);
    // Since the ballot is lower than replica's maxAcceptedVote->ballot,
    // should not log any of the votes and should move back to initializing
    ReceiveRequest(Message_StatusQuery, m_status.m_decree, m_status.m_ballot);

    SendStatusResponse(m_status.m_decree + 1, HighBallot(m_status.m_ballot), m_status.m_decree    // Since the ballot is lower than replica's maxAcceptedVote->ballot,
        // it should not log any of the votes and should move back to initializing
        , m_status.m_ballot, -1);

    v.clear();
    v.push_back(make_pair(m_status.m_decree, LowerBallot(m_status.m_ballot)));
    v.push_back(make_pair(m_status.m_decree + 1, LowerBallot(m_status.m_ballot)));
    v.push_back(make_pair(m_status.m_decree + 1, HighBallot(m_status.m_ballot)));

    HandleLearnVotes(m_status.m_decree, m_status.m_ballot, v);
    ReceiveRequest(Message_StatusQuery, m_status.m_decree + 1, HighBallot(m_status.m_ballot));
    MoveToState(state);

    // send response message with message in the log. Should not try to copy checkpoint
    msg = (StatusResponse *)GenerateMessage(Message_StatusResponse, m_status.m_decree + 10, m_status.m_ballot);

    msg->m_queryDecree = m_status.m_decree;
    msg->m_queryBallot = m_status.m_ballot;

    msg->m_minDecreeInLog = m_status.m_decree;
    SendResponse(msg);
    delete msg;

    HandleLearnVotes(m_status.m_decree, m_status.m_ballot, m_status.m_decree + 10, m_status.m_ballot);

    ReceiveRequest(Message_StatusQuery, m_status.m_decree + 10, m_status.m_ballot);
    MoveToState(state);

    msg = (StatusResponse *)GenerateMessage(Message_StatusResponse, m_status.m_decree + 10, m_status.m_ballot);

    msg->m_queryDecree = m_status.m_decree;
    msg->m_queryBallot = m_status.m_ballot;

    msg->m_minDecreeInLog = m_status.m_decree + 1;
    SendResponse(msg);

    // the replica should wait for more messages
    msg->m_checkpointedDecree = m_status.m_decree;
    msg->m_checkpointSize = 100 * 1024;
    SendResponse(msg);
    delete msg;

    FetchRequest *req = ReceiveFetchRequest(Message_FetchCheckpoint);
    Message *fMsg = req->m_message;
    UT_AssertAreEqual(m_status.m_decree, fMsg->m_decree);
    UT_AssertIsTrue(fMsg->m_ballot == BallotNumber());
    delete req;

    // failed to copy checkpoint
    ReceiveRequest(Message_StatusQuery, m_status.m_decree, m_status.m_ballot);
    MoveToState(state);

    StatusResponse serialize;
    SendRequest(Message_Vote, m_status.m_decree + 3, m_status.m_ballot);
    // Before we send the response, we must make sure that the legislator
    // got the request. The only way to ensure that is to send a status
    // request and get a response back.
    GetStatus(&serialize);

    msg = (StatusResponse *)GenerateMessage(Message_StatusResponse, m_status.m_decree + 10, m_status.m_ballot);

    msg->m_queryDecree = m_status.m_decree;
    msg->m_queryBallot = m_status.m_ballot;

    msg->m_minDecreeInLog = m_status.m_decree + 1;
    msg->m_checkpointedDecree = m_status.m_decree + 1;
    msg->m_checkpointSize = 100 * 1024;
    SendResponse(msg);
    delete msg;

    req = ReceiveFetchRequest(Message_FetchCheckpoint);
    fMsg = req->m_message;
    delete req;

    // failed to copy checkpoint
    ReceiveRequest(Message_StatusQuery, m_status.m_decree, m_status.m_ballot);
    SendStatusResponse(m_status.m_decree + 3, m_status.m_ballot, m_status.m_decree, m_status.m_ballot, -1);
    HandleLearnVotes(m_status.m_decree, m_status.m_ballot, m_status.m_decree + 3, m_status.m_ballot, true);
    if (m_status.m_ballot == m_status.m_maxBallot)
    {
        ReceiveResponse(Message_VoteAccepted, m_status.m_decree + 3, m_status.m_maxBallot);
    }
    else
    {
        ReceiveResponse(Message_NotAccepted, m_status.m_decree + 3, m_status.m_maxBallot);
    }
    WaitForState(Legislator::StableSecondary, m_status.m_ballot, m_status.m_decree + 3, m_status.m_maxBallot);
    MoveToState(state);

}

void
LegislatorTest::TestExecutionQueueOnDisk()
{
    MoveToSecondary();
    Ptr<Vote> vote;
    StatusResponse status;

    m_machine->m_saveStateCalled = 0;

    // Pass votes while the execution thread is blocked. The execution queue should
    // eventually fill up and it should start writing the votes to disk. When we
    // un block the execution thread, it should read the decrees from disk and
    // play them
    GetStatus(&status);

    UInt64 decree = status.m_decree + 1;
    for (UInt32 totalLen = 0; totalLen + s_MaxVoteLen < s_MaxCacheLength; decree++)
    {
        // have a random length between 0 and 1 MB.
        UInt32 len = largeRand(1024 * 1024);
        UInt32 numRequests = (len / 4096) + 1;
        vote = NewVote(m_version, m_memberId, decree, status.m_ballot, numRequests, 4096, NULL);
        SendAndReceive(vote, Message_VoteAccepted);

        // add an extra 512 bytes for the header
        totalLen += len + 512;
    }

    WaitUntil(m_machine->m_saveStateCalled == 1);

    m_machine->m_executeRequestWait = 1;

    // unblock the execution thread from checkpointing
    UInt64 seqNo = m_machine->GetCurrentSequenceNumber();
    m_machine->m_saveStateCalled = 2;
    WaitForCheckpoint(seqNo);

    for (UInt32 totalLen = 0; totalLen + s_MaxVoteLen < s_MaxCacheLength; decree++)
    {
        // have a random length between 0 and 1 MB.
        UInt32 len = largeRand(1024 * 1024);
        UInt32 numRequests = (len / 4096) + 1;
        vote = NewVote(m_version, m_memberId, decree, status.m_ballot, numRequests, 4096, NULL);
        SendAndReceive(vote, Message_VoteAccepted);

        // add an extra 512 bytes for the header
        totalLen += len + 512;
    }
    m_machine->m_executeRequestWait = 0;

    WaitUntil(m_machine->m_saveStateCalled == 1);
    // unblock the execution thread
    m_machine->m_saveStateCalled = 2;
    WaitForCheckpoint(seqNo);

    // wait for all the votes to get executed
    WaitUntil(decree == m_machine->GetCurrentSequenceNumber() + 2);

    // send this as the last vote. These votes should create a new checkpoint 
    vote = NewVote(m_version, m_memberId, decree, status.m_ballot);
    SendAndReceive(vote, Message_VoteAccepted);

    WaitUntil(m_machine->m_saveStateCalled == 1);
    seqNo = m_machine->GetCurrentSequenceNumber();
    UT_AssertAreEqual(decree, seqNo + 1);
    // unblock the execution thread
    m_machine->m_saveStateCalled = 2;
    WaitForCheckpoint(seqNo);
}

void
LegislatorTest::TestCanBecomePrimary()
{
    // move the machine to initializing with status.m_ballot == status.m_maxBallot
    MoveToPrimary();
    StatusResponse status;
    GetStatus(&status);

    SendResponse(Message_NotAccepted, status.m_decree + 1, status.m_ballot);
    // state machine can not become the primary now
    m_machine->m_canBecomePrimary = false;

    ReceiveRequest(Message_StatusQuery, status.m_decree, status.m_ballot);
    SendStatusResponse(status.m_decree, LowerBallot(status.m_ballot),
        status.m_decree, status.m_ballot, -1);

    WaitForState(Legislator::StableSecondary, status.m_ballot, status.m_decree, status.m_maxBallot);

    MoveToInitializing();

    // try to move the state machine to preparing. The state machine
    // should not move to preparing, instead it should move
    // to stable secondary
    GetStatus(&status);
    SendStatusResponse(status.m_decree, status.m_ballot, status.m_decree, status.m_ballot, -1);
    WaitForState(Legislator::StableSecondary, status.m_ballot, status.m_decree, status.m_maxBallot);

    m_machine->m_canBecomePrimary = true;
}

void
LegislatorTest::TestReadNextMessage()
{
    //move the legislator to initializing
    MoveToInitializing();

    StandardMarshalMemoryManager memory(s_AvgMessageLen);
    char *buf = (char *)memory.GetBuffer();
    Message *msg;

    SendStatusResponse(m_status.m_decree + 1, m_status.m_ballot, m_status.m_decree, m_status.m_ballot, 0);
    FetchRequest *req = ReceiveFetchRequest(Message_FetchVotes);

    // send only part of the request and close the connection
    for (UInt32 i = 0; i < 1000; ++i)
    {
        buf[i] = (char)(i % 26) + 'a';
    }
    req->m_sock->Write(buf, 1000);
    delete req;

    ReceiveRequest(Message_StatusQuery, m_status.m_decree, m_status.m_ballot);
    SendStatusResponse(m_status.m_decree + 1, m_status.m_ballot, m_status.m_decree, m_status.m_ballot, 0);
    req = ReceiveFetchRequest(Message_FetchVotes);

    // 3. Send an invalid messageId
    msg = new Message(m_version, 1000, m_memberId, m_status.m_decree, m_self.m_configNumber, m_status.m_ballot);
    msg->MarshalBuf(buf, s_AvgMessageLen);
    msg->CalculateChecksum(buf, msg->GetMarshalLen());
    req->m_sock->Write(buf, 512);
    delete msg;
    delete req;

    // send a prepare message which is not a multiple of 512 bytes
    ReceiveRequest(Message_StatusQuery, m_status.m_decree, m_status.m_ballot);
    SendStatusResponse(m_status.m_decree + 1, m_status.m_ballot, m_status.m_decree, m_status.m_ballot, 0);
    req = ReceiveFetchRequest(Message_FetchVotes);

    msg = new PrepareMsg(m_version, m_memberId, m_status.m_decree, m_self.m_configNumber, HighBallot(m_status.m_ballot), m_self.m_primaryCookie);
    msg->MarshalBuf(buf, s_AvgMessageLen);
    msg->CalculateChecksum(buf, msg->GetMarshalLen());
    req->m_sock->Write(buf, msg->GetMarshalLen());
    delete msg;
    delete req;

    // send a partial vote
    // send a prepare message which is not a multiple of 512 bytes
    ReceiveRequest(Message_StatusQuery, m_status.m_decree, m_status.m_ballot);
    SendStatusResponse(m_status.m_decree + 1, m_status.m_ballot, m_status.m_decree, m_status.m_ballot, 0);
    req = ReceiveFetchRequest(Message_FetchVotes);

    Ptr<Vote> vote = new Vote(m_version, m_memberId, m_status.m_decree + 1, m_self.m_configNumber, m_status.m_ballot, m_self.m_primaryCookie);
    for (int i = 0; i < 5; i++)
    {
        vote->AddRequest(ReqBuf(m_status.m_decree + 1), 900, NULL);
    }
    vote->CalculateChecksum();
    vote->MarshalBuf(buf, s_AvgMessageLen);
    req->m_sock->Write(buf, 1000);
    delete req;

    // don't checksum the buffer
    ReceiveRequest(Message_StatusQuery, m_status.m_decree, m_status.m_ballot);
    SendStatusResponse(m_status.m_decree + 1, m_status.m_ballot, m_status.m_decree, m_status.m_ballot, 0);
    req = ReceiveFetchRequest(Message_FetchVotes);

    vote->MarshalBuf(buf, s_AvgMessageLen);
    req->m_sock->Write(buf, RoundUpToPage(vote->GetMarshalLen()));
    delete req;

    ReceiveRequest(Message_StatusQuery, m_status.m_decree, m_status.m_ballot);
}

void
LegislatorTest::TestFastRead(UInt32 /*state*/)
{
    MoveToState(LegislatorTest::Primary);
    // accept this vote
    SendResponse(Message_VoteAccepted, m_status.m_decree, m_status.m_ballot);
    WaitUntil(m_machine->GetCurrentSequenceNumber() == m_status.m_decree);

    UInt64 decree = m_machine->GetCurrentSequenceNumber();
    // submit a higher decree with 0 timeout. It should fail
    // immediately
    RSLResponseCode ret = m_machine->SubmitFastReadRequest(decree + 1, 0, false);
    UT_AssertAreEqual(ret, RSLFastReadStale);

    ret = m_machine->SubmitFastReadRequest(decree + 10, 0, false);
    UT_AssertAreEqual(ret, RSLFastReadStale);

    UT_AssertIsTrue(m_status.m_decree >= 10);
    // put random requests between decree-10, decree. All should pass
    for (int i = 0; i < 100; i++)
    {
        UInt64 d = m_status.m_decree - (rand() % 10);
        ret = m_machine->SubmitFastReadRequest(d, 0, true);
        UT_AssertAreEqual(ret, RSLSuccess);
    }
    WaitUntil(NumFastReads() == 0);

    // put random requests between decree-10, decree. All should pass
    for (int i = 0; i < 100; i++)
    {
        UInt64 d = m_status.m_decree - (rand() % 10);
        bool success = true;
        if (i == 30)
        {
            d = m_status.m_decree + 2;
        }
        else if (i == 50)
        {
            d = m_status.m_decree + 3;
            success = false;
        }
        else if (i == 90)
        {
            d = m_status.m_decree + 1;
        }
        UInt32 timeout = 10 * 1000 - (rand() % 1000);
        ret = m_machine->SubmitFastReadRequest(d, timeout, success);
        UT_AssertAreEqual(ret, RSLSuccess);
    }
    // all decrees except decree+1 and decree+2 and decree+3 should get executed
    WaitUntil(NumFastReads() == 3);

    m_machine->SubmitRequest(m_status.m_decree + 1);
    ReceiveRequest(Message_Vote, m_status.m_decree + 1, m_status.m_ballot);
    SendResponse(Message_VoteAccepted, m_status.m_decree + 1, m_status.m_ballot);
    WaitUntil(NumFastReads() == 2);

    m_machine->SubmitRequest(m_status.m_decree + 2);
    ReceiveRequest(Message_Vote, m_status.m_decree + 2, m_status.m_ballot);
    SendResponse(Message_VoteAccepted, m_status.m_decree + 2, m_status.m_ballot);
    WaitUntil(NumFastReads() == 0);
}

void
LegislatorTest::TestAcceptMessageFromReplica()
{
    MoveToSecondary();
    m_machine->m_acceptMsgFromReplica = false;
    m_machine->SetPrimaryCookie(VoteFactory::g_buffer, 1024);

    Message *msg = new PrepareMsg(
        RSLProtocolVersion_2,
        m_memberId,
        m_status.m_decree,
        m_self.m_configNumber,
        HighBallot(m_status.m_maxBallot),
        m_machine->m_primaryCookie
        );

    SendRequest(msg);
    delete msg;

    // This should get logged, but we shouldn't get a response
    WaitForState(Legislator::StableSecondary, m_status.m_ballot, m_status.m_decree, HighBallot(m_status.m_maxBallot));

    BallotNumber b(HighBallot(m_status.m_ballot));

    msg = NewVote(RSLProtocolVersion_2, m_memberId, m_status.m_decree, b, m_machine->m_primaryCookie);
    SendRequest(msg);
    delete msg;

    msg = NewVote(RSLProtocolVersion_2, m_memberId, m_status.m_decree + 1, b, m_machine->m_primaryCookie);
    SendRequest(msg);
    delete msg;

    WaitForState(Legislator::StableSecondary, b, m_status.m_decree + 1, HighBallot(m_status.m_maxBallot));

    m_machine->m_acceptMsgFromReplica = true;

    msg = NewVote(RSLProtocolVersion_2, m_memberId, m_status.m_decree + 2, b, m_machine->m_primaryCookie);
    SendRequest(msg);
    delete msg;
    ReceiveResponse(Message_VoteAccepted, m_status.m_decree + 2, b);
    m_machine->m_acceptMsgFromReplica = true;
    m_machine->SetPrimaryCookie(NULL, 0);

}

void
LegislatorTest::TestRestoreFromCheckpoint()
{
    StatusResponse status;
    GetStatus(&status);

    if (status.m_checkpointedDecree == 0)
    {
        return;
    }

    RSLConfigParam cfgParam;

    RSLNodeCollection replicas;
    GenerateConfigFile(2, cfgParam, replicas);

    DynString cpFile(cfgParam.m_workingDir);
    cpFile.AppendF("\\%s\\%I64u.codex", replicas[0].m_memberIdString, status.m_checkpointedDecree);

    Message fetchReq(m_version, Message_FetchCheckpoint, m_memberId, status.m_checkpointedDecree, m_self.m_configNumber, BallotNumber());
    UInt32 size = Fetch(&fetchReq, cpFile);
    UT_AssertAreEqual(status.m_checkpointSize, size);

    if (m_version >= RSLProtocolVersion_3)
    {
        RSLMemberSet memberset(replicas, NULL, 0);
        UT_AssertIsTrue(RSLCheckpointUtility::ChangeReplicaSet(cpFile, &memberset));
    }

    TestStateMachine *sm = new TestStateMachine(m_version);
    UT_AssertIsTrue(sm->Initialize(&cfgParam, replicas, replicas[0], m_version, true));

    Message query(m_version, Message_StatusQuery, MemberId(replicas[1].m_memberIdString), 0, m_self.m_configNumber, BallotNumber());
    SendMessage(&query, true, replicas[0].m_rslPort);

#if 0
    // wait for 10 seconds
    int i = 0;
    for (; i < c_WaitIterations; i++)
    {
        m_lock.Enter();
        Message *msg = m_respRecvQ.dequeue();
        m_lock.Leave();

        if (msg)
        {
            UT_AssertAreEqual(Message_StatusResponse, msg->m_msgId);
            StatusResponse *resp = (StatusResponse *)msg;
            UT_AssertAreEqual(status.m_checkpointedDecree + 1, resp->m_decree);
            UT_AssertAreEqual(status.m_checkpointedDecree, sm->GetCurrentSequenceNumber());
            delete msg;
            sm->Pause();
            return;
        }
        Sleep(c_SleepTime);
    }
    UT_AssertFail();

#endif

    Message *msg = ReceiveResponse(Message_StatusResponse, false);
    StatusResponse *resp = (StatusResponse *)msg;
    UT_AssertAreEqual(status.m_checkpointedDecree + 1, resp->m_decree);
    UT_AssertAreEqual(status.m_checkpointedDecree, sm->GetCurrentSequenceNumber());
    sm->Pause();
}

void
LegislatorTest::TestRestore(TestCorruptionEnum testCorruption)
{
    // start another legislator with the log file from this replicator. Both
    // should end up with the same decree
    // remove the legislator directory

    TestRestoreFromCheckpoint();

    StatusResponse status;
    GetStatus(&status);

    RSLConfigParam cfgParam;
    RSLNodeCollection replicaCfgs;

    GenerateConfigFile(2, cfgParam, replicaCfgs);

    DynString dest(cfgParam.m_workingDir);
    dest.AppendF("\\%s\\", replicaCfgs[0].m_memberIdString);

    // learn the checkpoint from the replica
    if (status.m_checkpointedDecree != 0)
    {
        DynString file("foo");

        // learn the log from the replica
        Message req(m_version, Message_FetchCheckpoint, m_memberId, status.m_checkpointedDecree + 1, m_self.m_configNumber, BallotNumber());
        UInt32 size = Fetch(&req, file);
        UT_AssertAreEqual(0, size);

        req.m_decree = status.m_checkpointedDecree - 1;
        size = Fetch(&req, file);
        UT_AssertAreEqual(0, size);

        Message fetchReq(m_version, Message_FetchCheckpoint, m_memberId, status.m_checkpointedDecree, m_self.m_configNumber, BallotNumber());
        file = dest;
        file.AppendF("%I64u.codex", fetchReq.m_decree);
        size = Fetch(&fetchReq, file);
        UT_AssertAreEqual(status.m_checkpointSize, size);

        if (m_version >= RSLProtocolVersion_3)
        {
            RSLMemberSet memberset(replicaCfgs, NULL, 0);
            UT_AssertIsTrue(RSLCheckpointUtility::ChangeReplicaSet(file, &memberset));
        }
    }

    // learn the log from the replica
    Message req(m_version, Message_FetchVotes, m_memberId, status.m_minDecreeInLog - 1, m_self.m_configNumber, BallotNumber());
    DynString file("foo");
    UInt32 size = Fetch(&req, file);
    UT_AssertAreEqual(0, size);

    req.m_decree = status.m_decree + 1;
    size = Fetch(&req, file);
    UT_AssertAreEqual(0, size);

    req.m_decree = status.m_minDecreeInLog;
    req.m_ballot = HighBallot(status.m_ballot);
    file = dest;
    file.AppendF("%I64u.log", req.m_decree);
    size = Fetch(&req, file);
    UT_AssertIsTrue(size > 0);

    DeleteFileA(file);

    req.m_decree = status.m_minDecreeInLog;
    req.m_ballot = BallotNumber();
    file = dest;
    file.AppendF("%I64u.log", req.m_decree);
    size = Fetch(&req, file);
    UT_AssertIsTrue(size > 0);

    if (testCorruption == TestCorruption_Zero)
    {
        // extend the file. It should get filled with 0
        HANDLE handle = CreateFileA(file, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, NULL, OPEN_ALWAYS, 0, NULL);
        if (handle == INVALID_HANDLE_VALUE)
        {
            UT_AssertFail("CreateFileA failed %s: (%d)", file.Str(), GetLastError());
        }

        if (SetFilePointer(handle, 512 * 10, NULL, FILE_END) == INVALID_SET_FILE_POINTER)
        {
            UT_AssertFail("SetFilePointer failed %s: (%d)", file.Str(), GetLastError());
        }
        SetEndOfFile(handle);
        CloseHandle(handle);

        // create a bogus checkpoint file
        DynString corruptCPFile;
        corruptCPFile.AppendF("%I64u.codex", status.m_checkpointedDecree + 10);
        // extend the file. It should get filled with 0
        handle = CreateFileA(corruptCPFile, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, NULL, OPEN_ALWAYS, 0, NULL);
        if (handle == INVALID_HANDLE_VALUE)
        {
            UT_AssertFail("CreateFileA failed %s: (%d)", corruptCPFile.Str(), GetLastError());
        }

        if (SetFilePointer(handle, 512 * 10, NULL, FILE_END) == INVALID_SET_FILE_POINTER)
        {
            UT_AssertFail("SetFilePointer failed %s: (%d)", corruptCPFile.Str(), GetLastError());
        }
        SetEndOfFile(handle);
        CloseHandle(handle);
    }
    else if (testCorruption == TestCorruption_Incomplete)
    {
        // extend the file. write an incomplete log
        HANDLE handle = CreateFileA(file, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, NULL, OPEN_ALWAYS, 0, NULL);
        if (handle == INVALID_HANDLE_VALUE)
        {
            UT_AssertFail("CreateFileA failed %s: (%d)", file.Str(), GetLastError());
        }
        StandardMarshalMemoryManager memory(s_AvgMessageLen);
        char *buf = (char *)memory.GetBuffer();
        Ptr<Vote> vote = NewVote(m_version, m_memberId, m_status.m_decree + 1, m_status.m_ballot, 1, 1 * 1024, NULL);
        vote->MarshalBuf(buf, s_AvgMessageLen);
        vote->CalculateChecksum();
        DWORD written;
        if (SetFilePointer(handle, 0, NULL, FILE_END) == INVALID_SET_FILE_POINTER ||
            WriteFile(handle, buf, 512, &written, NULL) == FALSE ||
            written != 512)
        {
            UT_AssertFail("SetFilePointer/WriteFile failed %s: (%d)", file.Str(), GetLastError());
        }
        CloseHandle(handle);
    }

    bool loadStateRet[2];
    loadStateRet[0] = false;
    loadStateRet[1] = false;
    TestStateMachine *sm = new TestStateMachine(m_version);
    sm->SetLoadState(loadStateRet, 2);
    UT_AssertIsFalse(sm->Initialize(&cfgParam, replicaCfgs[0], m_version, true));
    delete sm;

    sm = new TestStateMachine(m_version);
    UT_AssertIsTrue(sm->Initialize(&cfgParam, replicaCfgs, replicaCfgs[0], m_version, true));

    Message query(m_version, Message_StatusQuery, MemberId(replicaCfgs[1].m_memberIdString), 0, m_self.m_configNumber, BallotNumber());
    SendMessage(&query, true, replicaCfgs[0].m_rslPort);
    StatusResponse *resp = (StatusResponse *)ReceiveResponse(Message_StatusResponse, false);
    UT_AssertAreEqual(status.m_decree, resp->m_decree);
    UT_AssertIsTrue(resp->m_ballot == status.m_ballot);
    UT_AssertIsTrue(resp->m_maxBallot == status.m_maxBallot);
    UT_AssertAreEqual(status.m_decree - 1, sm->GetCurrentSequenceNumber());
    sm->Pause();
}

void
LegislatorTest::TestRestoreFromLoadState()
{
    // we should have all the logs. MaxLogs is set to 100 in the configuration file
    // copy the log.
    StatusResponse status;
    GetStatus(&status);
    UT_AssertIsTrue(status.m_checkpointedDecree != 0);

    RSLConfigParam cfgParam;
    RSLNodeCollection replicaCfgs;

    GenerateConfigFile(2, cfgParam, replicaCfgs);

    DynString cmd, dest(cfgParam.m_workingDir), src;
    dest.AppendF("\\%s\\", replicaCfgs[0].m_memberIdString);
    DynString srcWorkingDir;
    TestEngine::m_cfg.GetWorkingDir(srcWorkingDir);
    src.AppendF("%s\\%s", srcWorkingDir.Str(), m_legislatorMemberId.GetValue());

    cmd.AppendF("rmdir /s /Q %s 2>NUL", dest.Str());
    system(cmd);
    UT_AssertIsTrue(CreateDirectoryA(dest, NULL));

    cmd.Clear();
    cmd.AppendF("copy /Y %s\\* %s 1>NUL", src.Str(), dest.Str());
    system(cmd);

    bool loadStateRet[100];
    memset(loadStateRet, 0, sizeof(loadStateRet));

    TestStateMachine *sm = new TestStateMachine(m_version);
    sm->SetLoadState(loadStateRet, 100);
    UT_AssertIsTrue(sm->Initialize(&cfgParam, replicaCfgs, replicaCfgs[0], m_version, true));

    Message query(m_version, Message_StatusQuery, MemberId(replicaCfgs[1].m_memberIdString), 0, m_self.m_configNumber, BallotNumber());
    SendMessage(&query, true, replicaCfgs[0].m_rslPort);
    StatusResponse *resp = (StatusResponse *)ReceiveResponse(Message_StatusResponse, false);
    UT_AssertAreEqual(status.m_decree, resp->m_decree);
    UT_AssertIsTrue(resp->m_ballot == status.m_ballot);
    UT_AssertIsTrue(resp->m_maxBallot == status.m_maxBallot);
    UT_AssertAreEqual(status.m_decree - 1, sm->GetCurrentSequenceNumber());
    sm->Pause();
}

void
LegislatorTest::TestSaveCheckpointAtRestore()
{
    MoveToSecondary();
    StatusResponse status;
    GetStatus(&status);
    UT_AssertIsTrue(status.m_checkpointedDecree != 0);

    UT_AssertIsTrue(status.m_decree > status.m_checkpointedDecree);
    if (status.m_decree - status.m_checkpointedDecree < 2)
    {
        Ptr<Vote> vote = NewVote(m_version, m_memberId, m_status.m_decree + 1, m_status.m_ballot, 1, 1, NULL);
        SendAndReceive(vote, Message_VoteAccepted);
        GetStatus(&status);
    }

    RSLConfigParam cfgParam;
    RSLNodeCollection replicaCfgs;

    GenerateConfigFile(2, cfgParam, replicaCfgs);

    DynString dest(cfgParam.m_workingDir);
    dest.AppendF("\\%s\\", replicaCfgs[0].m_memberIdString);

    // learn the checkpoint from the replica
    Message fetchReq(m_version, Message_FetchCheckpoint, m_memberId, status.m_checkpointedDecree, m_self.m_configNumber, BallotNumber());
    DynString file(dest);
    file.AppendF("%I64u.codex", fetchReq.m_decree);
    UInt64 size = Fetch(&fetchReq, file);
    UT_AssertAreEqual(status.m_checkpointSize, size);

    // learn the log from the replica
    Message req(m_version, Message_FetchVotes, m_memberId, status.m_minDecreeInLog, m_self.m_configNumber, BallotNumber());
    file.Set(dest);
    file.AppendF("%I64u.log", req.m_decree);
    size = Fetch(&req, file);
    UT_AssertIsTrue(size > 0);

    TestStateMachine *sm = new TestStateMachine(m_version);
    sm->m_setSaveStateAtDecree = m_status.m_decree - 1;
    UT_AssertIsTrue(sm->Initialize(&cfgParam, replicaCfgs, replicaCfgs[0], m_version, true));

    Message query(m_version, Message_StatusQuery, MemberId(replicaCfgs[1].m_memberIdString), 0, m_self.m_configNumber, BallotNumber());
    SendMessage(&query, true, replicaCfgs[0].m_rslPort);
    StatusResponse *resp = (StatusResponse *)ReceiveResponse(Message_StatusResponse, false);
    UT_AssertAreEqual(status.m_decree, resp->m_decree);
    UT_AssertIsTrue(resp->m_ballot == status.m_ballot);
    UT_AssertIsTrue(resp->m_maxBallot == status.m_maxBallot);
    UT_AssertAreEqual(status.m_decree - 1, sm->GetCurrentSequenceNumber());
    UT_AssertAreEqual(status.m_decree - 1, resp->m_checkpointedDecree);
    sm->Pause();
}

void
LegislatorTest::TestCheckpointing()
{
    MoveToSecondary();
    Ptr<Vote> vote;
    Message *msg;

    // send a big vote this should fill up the log
    UInt32 numRequests = (s_MaxVoteLen / (32 * 1024));
    vote = NewVote(m_version, m_memberId, m_status.m_decree + 1, m_status.m_ballot, numRequests, 32 * 1024, NULL);
    SendAndReceive(vote, Message_VoteAccepted);

    m_machine->m_saveStateCalled = 0;
    // The previous vote will have to be executed before the checkpoint is created.
    vote = NewVote(m_version, m_memberId, m_status.m_decree + 2, m_status.m_ballot);
    SendAndReceive(vote, Message_VoteAccepted);

    WaitUntil(m_machine->m_saveStateCalled == 1);

    UT_AssertAreEqual(m_status.m_decree + 1, m_machine->GetCurrentSequenceNumber());

    // execute another decree while the state is being saved
    vote = NewVote(m_version, m_memberId, m_status.m_decree + 3, m_status.m_ballot);
    SendAndReceive(vote, Message_VoteAccepted);

    // try to move it the preparing, it should not become the primary
    SendRequest(Message_Vote, m_status.m_decree + 10, m_status.m_ballot);
    msg = ReceiveRequest(Message_StatusQuery, m_status.m_decree + 3, m_status.m_ballot);
    SendStatusResponse(msg->m_decree, msg->m_ballot, msg->m_decree, msg->m_ballot, -1);

    // release the checkpoint thread.
    m_machine->m_saveStateCalled = 2;
    WaitUntil(m_machine->m_saveStateCalled == 0);
    // Make sure that RSL finished renaming the file and is completely done
    // with checkpoint. Otherwise, if RSL is still in the middle of
    // checkpointing, it will not move to preparing.
    WaitForCheckpoint(m_status.m_decree + 1);

    GetStatus(&m_status);

    vote = NewVote(m_version, m_memberId, m_status.m_decree + 1, m_status.m_ballot, 1, s_MaxVoteLen, NULL);
    SendAndReceive(vote, Message_VoteAccepted);

    // try to move it the preparing
    SendRequest(Message_Vote, m_status.m_decree + 10, m_status.m_ballot);
    msg = ReceiveRequest(Message_StatusQuery, m_status.m_decree + 1, m_status.m_ballot);
    SendStatusResponse(msg->m_decree, msg->m_ballot, msg->m_decree, msg->m_ballot, -1);

    BallotNumber prepareBallot(MaxSentBallotId() + 1, m_legislatorMemberId);
    msg = ReceiveRequest(Message_Prepare, m_status.m_decree + 1, prepareBallot);
    WaitForState(Legislator::Preparing, m_status.m_ballot, m_status.m_decree + 1, msg->m_ballot);

    // now pass a vote. Since the replica is in preparing, it should be create a new checkpoint
    BallotNumber newBallot = HighBallot(msg->m_ballot);
    vote = NewVote(m_version, m_memberId, m_status.m_decree + 1, newBallot);
    SendAndReceive(vote, Message_VoteAccepted);

    // The previous vote will have to be executed before the checkpoint is created.
    vote = NewVote(m_version, m_memberId, m_status.m_decree + 2, newBallot);
    SendAndReceive(vote, Message_VoteAccepted);

    WaitUntil(m_machine->m_saveStateCalled == 1);
    UT_AssertAreEqual(m_status.m_decree + 1, m_machine->GetCurrentSequenceNumber());

    TestRestore();

    // release the checkpoint thread.
    m_machine->m_saveStateCalled = 2;
    WaitUntil(m_machine->m_saveStateCalled == 0);
    WaitForCheckpoint(m_status.m_decree + 1);
}


void
LegislatorTest::TestCheckpointingInPrimary()
{
    MoveToPrimary();

    TestRestore();

    // accept this vote
    SendResponse(Message_VoteAccepted, m_status.m_decree, m_status.m_ballot);

    WaitUntil(m_machine->m_isPrimary == true);

    m_machine->SubmitRequest(m_status.m_decree + 1, 10, s_MaxVoteLen / 10);
    ReceiveRequest(Message_Vote);
    SendResponse(Message_VoteAccepted, m_status.m_decree + 1, m_status.m_ballot);

    // while this is waiting for a vote accepted, submit new requests
    m_machine->SubmitRequest(m_status.m_decree + 2, 1, 99);
    ReceiveRequest(Message_Vote);
    SendResponse(Message_VoteAccepted, m_status.m_decree + 2, m_status.m_ballot);

    ReceiveRequest(Message_StatusQuery);

    // while it is waiting for checkpoint, submit more requests
    m_machine->SubmitRequest(m_status.m_decree + 3, 1, 99);
    ReceiveRequest(Message_Vote);
    SendResponse(Message_VoteAccepted, m_status.m_decree + 3, m_status.m_ballot);

    m_machine->SubmitRequest(m_status.m_decree + 4, 1, 99);
    ReceiveRequest(Message_Vote);
    SendResponse(Message_VoteAccepted, m_status.m_decree + 4, m_status.m_ballot);

    m_machine->SubmitRequest(m_status.m_decree + 5, 1, 99);
    Vote* vote = (Vote *)ReceiveRequest(Message_Vote);
    Ptr<Vote> voteCopy = new Vote((RSLProtocolVersion)vote->m_version, vote, &vote->m_primaryCookie);
    voteCopy->CalculateChecksum();
    StatusResponse *msg;
    msg = (StatusResponse *)GenerateMessage(Message_StatusResponse, vote->m_decree, vote->m_ballot);

    CheckpointHeader header;
    header.m_version = m_version;
    header.m_stateConfiguration = new ConfigurationInfo(1, 1, new MemberSet());
    header.m_nextVote = voteCopy;
    header.m_maxBallot = vote->m_ballot;

    msg->m_checkpointedDecree = vote->m_decree - 1;
    msg->m_checkpointSize = RoundUpToPage(header.GetMarshalLen()) + sizeof(UInt64);

    SendResponse(msg);

    FetchRequest *fReq = ReceiveFetchRequest(Message_FetchCheckpoint);
    UT_AssertAreEqual(msg->m_checkpointedDecree, fReq->m_message->m_decree);

    MarshalData marshal;
    header.Marshal(&marshal);

    UT_AssertAreEqual(NO_ERROR, fReq->m_sock->Write(marshal.GetMarshaled(), marshal.GetMarshaledLength()));
    UT_AssertAreEqual(NO_ERROR, fReq->m_sock->Write(&msg->m_checkpointedDecree, sizeof(UInt64)));
    delete fReq;

    DynString srcWorkingDir;
    TestEngine::m_cfg.GetWorkingDir(srcWorkingDir);

    DynString checkpointName;
    checkpointName.AppendF("%s\\%s\\%d.codex", srcWorkingDir.Str(), m_legislatorMemberId.GetValue(), msg->m_checkpointedDecree);

    WaitUntil((GetFileAttributesA(checkpointName) != INVALID_FILE_ATTRIBUTES));

    delete msg;

}

void
LegislatorTest::TestCopyCheckpoint()
{
    FetchRequest *fRequest = NULL;
    StatusResponse *msg = NULL;
    DWORD ec = NO_ERROR;
    DWORD bytesRead = 0;
    void *buf;
    StateCopiedCookie *cookie = new StateCopiedCookie();

    UT_AssertAreEqual(RSLSuccess, m_machine->CopyStateFromReplica(cookie));

    // simulate no response from replica
    fRequest = ReceiveFetchRequest(Message_StatusQuery);
    delete fRequest;
    fRequest = NULL;

    // simulate no recent checkpoint from replica
    fRequest = ReceiveFetchRequest(Message_StatusQuery);

    msg = (StatusResponse *)GenerateMessage(Message_StatusResponse, 0, BallotNumber());
    {
        MarshalData marshal;
        msg->Marshal(&marshal);

        ec = fRequest->m_sock->Write(marshal.GetMarshaled(), marshal.GetMarshaledLength());
        UT_AssertAreEqual(NO_ERROR, ec);
    }
    delete msg;
    delete fRequest;

    // wait for statemachine to get notified
    WaitUntil(cookie->CallbackDone());
    UT_AssertAreEqual(0, cookie->m_seqNo);
    UT_AssertAreEqual(0, cookie->m_fileName.Length());

    cookie->Clear();

    GetStatus(&m_status);

    DynString srcWorkingDir;
    TestEngine::m_cfg.GetWorkingDir(srcWorkingDir);
    DynString checkpointName;
    checkpointName.AppendF("%s\\%s\\%d.codex", srcWorkingDir.Str(), m_legislatorMemberId.GetValue(), m_status.m_checkpointedDecree);

    HANDLE hFile = CreateFileA(
        checkpointName,
        GENERIC_READ,
        FILE_SHARE_READ,
        NULL,
        OPEN_EXISTING,
        0,
        NULL);

    UT_AssertIsTrue(hFile != INVALID_HANDLE_VALUE);

    DWORD size = GetFileSize(hFile, NULL);
    CloseHandle(hFile);

    UT_AssertAreEqual(RSLSuccess, m_machine->CopyStateFromReplica(cookie));
    fRequest = ReceiveFetchRequest(Message_StatusQuery);

    msg = (StatusResponse *)GenerateMessage(Message_StatusResponse, m_status.m_decree, m_status.m_ballot);
    msg->m_checkpointedDecree = m_status.m_checkpointedDecree + 1;
    msg->m_checkpointSize = size;
    {
        MarshalData marshal;
        msg->Marshal(&marshal);

        ec = fRequest->m_sock->Write(marshal.GetMarshaled(), marshal.GetMarshaledLength());
        UT_AssertAreEqual(NO_ERROR, ec);
    }
    delete fRequest;

    fRequest = ReceiveFetchRequest(Message_FetchCheckpoint);

    UT_AssertAreEqual(msg->m_checkpointedDecree, fRequest->m_message->m_decree);

    auto_ptr<APSEQREAD> reader(new APSEQREAD());
    ec = reader->DoInit(checkpointName, APSEQREAD::c_maxReadsDefault, APSEQREAD::c_readBufSize, true);
    UT_AssertAreEqual(NO_ERROR, ec);

    for (Int64 toRead = size; toRead > 0; toRead -= bytesRead)
    {
        ec = reader->GetDataPointer(&buf, APSEQREAD::c_readBufSize, &bytesRead);
        UT_AssertAreEqual(NO_ERROR, ec);
        ec = fRequest->m_sock->Write(buf, bytesRead);
        UT_AssertAreEqual(NO_ERROR, ec);
    }

    delete fRequest;

    // wait for statemachine to get notified
    WaitUntil(cookie->CallbackDone());
    UT_AssertAreEqual(msg->m_checkpointedDecree, cookie->m_seqNo);

    GetStatus(&m_status);
    UT_AssertAreEqual(msg->m_checkpointedDecree, m_status.m_checkpointedDecree);

    UT_AssertIsTrue(DeleteFileA(cookie->m_fileName));

    delete msg;
    delete cookie;
}

void
LegislatorTest::TestReplay()
{
    GetStatus(&m_status);

    DynString srcDir, destDir, cmd;
    DynString srcWorkingDir;
    TestEngine::m_cfg.GetWorkingDir(srcWorkingDir);

    srcDir.AppendF("%s\\%s", srcWorkingDir.Str(), m_legislatorMemberId.GetValue());
    destDir.Append(".\\rsltest\\replay1");

    cmd.AppendF("rmdir /s /Q %s 2>NUL", destDir.Str());
    system(cmd);
    UT_AssertIsTrue(CreateDirectoryA(destDir, NULL));

    cmd.Clear();
    cmd.AppendF("copy /Y %s\\* %s 1>NUL", srcDir.Str(), destDir.Str());
    system(cmd);

    TestStateMachine *sm = new TestStateMachine(m_version);
    UT_AssertIsTrue(sm->Replay(destDir, m_status.m_checkpointedDecree));
    UT_AssertAreEqual(m_status.m_checkpointedDecree, sm->GetCurrentSequenceNumber());

    GetStatus(&m_status);

    destDir.Clear();
    destDir.Append(".\\rsltest\\replay2");

    cmd.Clear();
    cmd.AppendF("rmdir /s /Q %s 2>NUL", destDir.Str());
    system(cmd);
    UT_AssertIsTrue(CreateDirectoryA(destDir, NULL));

    cmd.Clear();
    cmd.AppendF("copy /Y %s\\* %s 1>NUL", srcDir.Str(), destDir.Str());
    system(cmd);

    sm = new TestStateMachine(m_version);
    UT_AssertIsTrue(sm->Replay(destDir, m_status.m_checkpointedDecree - 1));
    UT_AssertAreEqual(m_status.m_checkpointedDecree - 1, sm->GetCurrentSequenceNumber());
}

void
LegislatorTest::TestSingleReplica()
{
    RSLConfigParam cfgParam;
    RSLNodeCollection replicas;
    GenerateConfigFile(1, cfgParam, replicas);

    TestStateMachine *sm = new TestStateMachine(m_version);
    UT_AssertIsTrue(sm->Initialize(&cfgParam, replicas, replicas[0], m_version, true));

    WaitUntil(sm->m_isPrimary == true);
    // submit 10 requests
    UInt64 decree = sm->GetCurrentSequenceNumber() + 1;
    for (UInt32 i = 0; i < 10; i++, decree++)
    {
        sm->SubmitRequest(decree);
        WaitUntil(decree == sm->GetCurrentSequenceNumber());
    }

    sm->Pause();
}
