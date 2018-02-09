/*
.........1.........2.........3.........4.........5.........6.........7.........
RSLStateMachineWrapper.cpp
    This is a very simple wrapper for the RSL state machine, it is going to be
    used by the ManagedRSLLib managed class to interact with the RSL.
*/

#include "ManagedRSLib.h"
#include "rsl.h"

using namespace ManagedRSLib;

bool RSLStateMachineWrapper::LoadState(RSLCheckpointStreamReader* reader)
{
    ManagedRSLCheckpointStream^ gc_pReader = (reader == NULL) ? nullptr : gcnew ManagedRSLCheckpointStream(reader);
    return m_pManagedSM->LoadState(gc_pReader);
}

void RSLStateMachineWrapper::ExecuteReplicatedRequest(void* request, size_t len, void* cookie, bool *saveState)
{
    m_pManagedSM->UnmarshalExecuteRequest(false, request, (unsigned int)len, cookie, saveState);
}

void RSLStateMachineWrapper::ExecuteFastReadRequest(void* request, size_t len, void* cookie) 
{
    m_pManagedSM->UnmarshalExecuteRequest(true, request, (unsigned int) len, cookie, NULL);
}

void RSLStateMachineWrapper::AbortRequest(RSLResponseCode status, void* cookie)
{
    m_pManagedSM->UnmarshalAbortRequest(status, cookie);
}

void RSLStateMachineWrapper::SaveState(RSLCheckpointStreamWriter* writer)
{
    ManagedRSLCheckpointStream^ gc_pWriter = gcnew ManagedRSLCheckpointStream(writer);
    m_pManagedSM->SaveState(gc_pWriter);
}

bool RSLStateMachineWrapper::TrySaveState(RSLCheckpointStreamWriter* writer)
{
    ManagedRSLCheckpointStream^ gc_pWriter = gcnew ManagedRSLCheckpointStream(writer);
    return m_pManagedSM->TrySaveState(gc_pWriter);
}

void RSLStateMachineWrapper::NotifyStatus(bool isPrimary)
{
    m_pManagedSM->NotifyStatus(isPrimary);
}

void RSLStateMachineWrapper::NotifyConfigurationChanged(void* cookie)
{
    m_pManagedSM->UnmarshallNotifyConfigurationChanged(cookie);
}

void RSLStateMachineWrapper::AbortChangeConfiguration(RSLResponseCode status, void* cookie)
{
    m_pManagedSM->UnmarshallAbortChangeConfiguration(status, cookie);
}

void RSLStateMachineWrapper::NotifyPrimaryRecovered()
{
    return m_pManagedSM->NotifyPrimaryRecovered();
}

bool RSLStateMachineWrapper::CanBecomePrimary(RSLPrimaryCookie *cookie)
{
    array<System::Byte>^ data = nullptr;
    bool ret = m_pManagedSM->CanBecomePrimary(data);
    pin_ptr<Byte> pBuffer = nullptr;
    if (ret == true && data != nullptr)
    {
        pBuffer = &data[0];
        cookie->Set(pBuffer, data->Length);
    }
    return ret;
}

bool RSLStateMachineWrapper::AcceptMessageFromReplica(RSLNode *node, void * data, unsigned int len)
{
    return m_pManagedSM->UnmarshalAcceptMessageFromReplica(node, data, len);
}

void RSLStateMachineWrapper::StateSaved(unsigned long long seqNo, const char* fileName)
{
    m_pManagedSM->UnmarshalStateSaved(seqNo, fileName);
}

void RSLStateMachineWrapper::StateCopied(unsigned long long seqNo, const char* fileName, void *cookie)
{
    m_pManagedSM->UnmarshalStateCopied(seqNo, fileName, cookie);
}

void RSLStateMachineWrapper::ShutDown(RSLResponseCode status)
{
    m_pManagedSM->ShutDown((RSLResponse) status);
}

RSLStateMachineWrapper::~RSLStateMachineWrapper()
{
}
