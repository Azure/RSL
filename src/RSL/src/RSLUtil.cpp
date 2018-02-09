#include <vector>
#include "rslutil.h"
#include "rsldebug.h"
#include "legislator.h"
#include <strsafe.h>

using namespace std;
using namespace RSLib;
using namespace RSLibImpl;

RSLCheckpointUtility::RSLCheckpointUtility()
{
}

void
RSLCheckpointUtility::SaveCheckpoint(char * directoryName, RSLProtocolVersion version,
                                     unsigned long long lastExecutedSequenceNumber,
                                     unsigned int configurationNumber, RSLMemberSet* rslMemberSet,
                                     IRSLCheckpointCreator * checkpointCreator)
{
    LogAssert(directoryName);
    LogAssert(lastExecutedSequenceNumber > 0);
    if (version > RSLProtocolVersion_2)
    {
        LogAssert(rslMemberSet); 
        LogAssert(rslMemberSet->m_memberSet->Verify(version));
    }
    LogAssert(checkpointCreator);

    DynString file(directoryName);
    CheckpointHeader::GetCheckpointFileName(file, lastExecutedSequenceNumber);

    MemberId memberId;
    if (version > RSLProtocolVersion_2)
    {
        memberId = MemberId(rslMemberSet->GetMemberInfo(0)->m_memberIdString);
    }

    // Though version 2 does not require a memberSet, current RSL implementation
    // may need a memberSet object, thus creating an instance anyways
    MemberSet * memberSet = new MemberSet();
    if (rslMemberSet != NULL)
    {
        memberSet->Copy(rslMemberSet->m_memberSet);
    }

    BallotNumber maxBallot(1, memberId);

    // Fill checkpoint header
    CheckpointHeader header;
    header.m_version = version;
    header.m_memberId = memberId;
    header.m_lastExecutedDecree = lastExecutedSequenceNumber;
    // Configuration contains a initial decree which, in
    // this case, should be the lastExecutedSequenceNumber
    header.m_stateConfiguration = new ConfigurationInfo(
        configurationNumber,
        lastExecutedSequenceNumber, // initialDecree
        memberSet);
    // Place an empty vote with lastExecutedSequenceNumber + 1
    header.m_nextVote = new Vote(version, memberId,
        lastExecutedSequenceNumber + 1,
        configurationNumber,
        maxBallot,
        new PrimaryCookie());
    header.m_nextVote->CalculateChecksum();
    // This is resetting maxBallot (which is OK since
    // all replicas will have the same state)
    header.m_maxBallot = maxBallot;
    header.m_stateSaved = true;
    if (header.m_version >= RSLProtocolVersion_4)
    {
        header.m_checksumBlockSize = s_ChecksumBlockSize;
    }

    // Create checkpoint
    RSLCheckpointStreamWriter writer;
    LogAssert(writer.Init(file, &header) == NO_ERROR);
    checkpointCreator->SaveState(&writer);
                    
    // Update checkpoint header
    header.SetBytesIssued(&writer);
    LogAssert(writer.Close() == NO_ERROR);
    header.Marshal(file);
}

DWORD
RSLCheckpointUtility::GetLatestCheckpoint(
    const char* directoryName,
    char* buf,
    size_t size)
{
    if (directoryName == NULL || buf == NULL)
    {
        return ERROR_INVALID_PARAMETER;
    }

    // find the highest numbered checkpoint
    char *cpPrefix = "*.codex";
    vector<UInt64> checkpoints;
    DynString file(directoryName);

    if (file.Length() == 0)
    {
        return ERROR_INVALID_PARAMETER;
    }

    if (file[file.Length()-1] != '\\')
    {
        file.Append('\\');
    }

    DWORD ec = Legislator::GetFileNumbers(file, cpPrefix, checkpoints);
    if (ec != NO_ERROR)
    {
        return ec;
    }

    if (checkpoints.size() == 0)
    {
        return ERROR_FILE_NOT_FOUND;
    }

    CheckpointHeader::GetCheckpointFileName(file, checkpoints[checkpoints.size()-1]);

    if (FAILED(StringCbCopyA(buf, size, file.Str())))
    {
        return ERROR_INSUFFICIENT_BUFFER;
    }

    return NO_ERROR;
}


bool
RSLCheckpointUtility::ChangeReplicaSet(
    const char* checkpointName,
    const RSLMemberSet *rslMemberSet)
{
    CheckpointHeader header;
    if (!header.UnMarshal(checkpointName))
    {
        return false;
    }
    
    return Legislator::ForDebuggingPurposesUpdateCheckpointFile(
        header.m_version, 
        checkpointName, 
        rslMemberSet->m_memberSet);
}

bool 
RSLCheckpointUtility::MigrateToVersion3(const char* checkpointName, 
                                        const RSLMemberSet *rslMemberSet)
{
    return Legislator::ForDebuggingPurposesUpdateCheckpointFile(
        RSLProtocolVersion_3, 
        checkpointName, 
        rslMemberSet->m_memberSet);
}

bool 
RSLCheckpointUtility::MigrateToVersion4(const char* checkpointName, 
                                          const RSLMemberSet *rslMemberSet)
{
    return Legislator::ForDebuggingPurposesUpdateCheckpointFile(
        RSLProtocolVersion_4, 
        checkpointName, 
        rslMemberSet->m_memberSet);
}

bool
RSLCheckpointUtility::GetReplicaSet(
    const char* checkpointName,
    RSLMemberSet *rslMemberSet)
{
    CheckpointHeader header;
    if (!header.UnMarshal(checkpointName))
    {
        return false;
    }
    
    if (header.m_version < RSLProtocolVersion_3)
    {
        RSLError("Can not get replica set for checkpoint version less than RSLProtocolVersion_3");
        return false;
    }

    rslMemberSet->m_memberSet->Copy(header.m_stateConfiguration->GetMemberSet());

    return true;
}





