/*
.........1.........2.........3.........4.........5.........6.........7.........
ManagedRSLib.cpp
    This is a managed class that will be used to build RSL based appications
    written in managed code, it provides access to all the RSL features and
    it encapsulates all the marshaling that is needed to interact with the
    unmanaged RSLib.
*/

#include "ManagedRSLib.h"
#include "rsl.h"
#include "rslutil.h"

using namespace System;
using namespace System::Runtime::InteropServices;
using namespace ManagedRSLib;
using namespace RSLibImpl;

ManagedRSLNode::ManagedRSLNode(const RSLNode * node)
{
    RSLNode *copy = new RSLNode();
    *copy = *node;
    m_node = copy;
}

System::String^
ManagedRSLNode::MemberId::get()
{
    return gcnew System::String(m_node->m_memberIdString);
}

void
ManagedRSLNode::MemberId::set(System::String^ value)
{
    IntPtr pStr = Marshal::StringToHGlobalAnsi(value);
    HRESULT hresult = StringCbCopyA(m_node->m_memberIdString, sizeof(m_node->m_memberIdString), (PSTR)pStr.ToPointer());
    Marshal::FreeHGlobal(pStr);
    if (!SUCCEEDED(hresult))
    {
        String^ msg = String::Format("length of value must be less {0}", sizeof(m_node->m_memberIdString));
        throw gcnew ArgumentException(msg);
    }
}

ManagedRSLMemberSet::ManagedRSLMemberSet(array<ManagedRSLNode^>^ gc_members,
                                         array<System::Byte>^ gc_cookie,
                                         int offset,
                                         int length)
{
    if (gc_members == nullptr)
    {
        throw gcnew System::ArgumentNullException("members");
    }
    if (gc_cookie != nullptr)
    {
        if (offset >= gc_cookie->Length)
        {
            throw gcnew System::ArgumentException("offset must be less than gc_cookie length");
        }
        if (offset + length > gc_cookie->Length)
        {
            throw gcnew System::ArgumentException("offset+ length must be less than gc_cookie length");
        }
    }

    RSLNodeCollection nodeArray;
    for (int i = 0; i < gc_members->Length; i++)
    {
        nodeArray.Append(*gc_members[i]->m_node);
    }
    pin_ptr<Byte> pBuffer = nullptr;
    if (gc_cookie != nullptr)
    {
        pBuffer = &gc_cookie[offset];
    }
    m_rslMemberSet = new RSLMemberSet(nodeArray, pBuffer, length);
}

ManagedRSLNode^
ManagedRSLMemberSet::GetMemberInfo(System::String^ gc_memberId)
{
    RSLNodeCollection nodes;
    m_rslMemberSet->GetMemberCollection(/*out*/nodes);

    size_t count = m_rslMemberSet->GetNumMembers();
    IntPtr pStr = Marshal::StringToHGlobalAnsi(gc_memberId);
    for (size_t i = 0; i < count; i++)
    {
        const RSLNode& node = nodes[i];
        if (strncmp(node.m_memberIdString, (PSTR)pStr.ToPointer(), sizeof(node.m_memberIdString)) == 0)
        {
            Marshal::FreeHGlobal(pStr);
            return gcnew ManagedRSLNode(&node);
        }
    }
    Marshal::FreeHGlobal(pStr);

    return nullptr;
}


array<ManagedRSLNode^>^
ManagedRSLMemberSet::MemberArray::get()
{
    RSLNodeCollection nodeArray;
    m_rslMemberSet->GetMemberCollection(/*out*/nodeArray);

    array<ManagedRSLNode^>^ gc_array = gcnew array<ManagedRSLNode^>((int)nodeArray.Count());
    for (int i=0; i < gc_array->Length; i++)
    {
        gc_array[i] = gcnew ManagedRSLNode(&(nodeArray)[i]);
    }
    return gc_array;
}

array<System::Byte>^
ManagedRSLMemberSet::ConfigurationCookie::get()
{
    unsigned int length;
    void * cookie = m_rslMemberSet->GetConfigurationCookie(&length);
    array<System::Byte>^ gc_buffer = nullptr;
    if (cookie != NULL)
    {
        gc_buffer = gcnew array<System::Byte>(length);
        Marshal::Copy((System::IntPtr) cookie, gc_buffer, 0, length);
    }
    return gc_buffer;
}

class RSLCheckpointCreatorWrapper : public RSLib::IRSLCheckpointCreator
{
public:
    RSLCheckpointCreatorWrapper(IManagedRSLCheckpointCreator^ gc_checkpointCreator)
    {
        m_checkpointCreator = gc_checkpointCreator;
    }

    void SaveState(RSLCheckpointStreamWriter* writer)
    {
        ManagedRSLCheckpointStream^ stream = gcnew ManagedRSLCheckpointStream(writer);
        m_checkpointCreator->SaveState(stream);
    }

private:
    gcroot<IManagedRSLCheckpointCreator^> m_checkpointCreator;
};

void
ManagedRSLCheckpointUtility::SaveCheckpoint(String^ gc_directoryName, ManagedRSLProtocolVersion version,
            unsigned long long lastExecutedSequenceNumber, unsigned int configurationNumber,
            ManagedRSLMemberSet^ gc_memberSet, IManagedRSLCheckpointCreator^ gc_checkpointCreator)
{
    if (gc_directoryName == nullptr)
    {
        throw gcnew System::ArgumentNullException("gc_directoryName");
    }
    if (gc_memberSet == nullptr)
    {
        throw gcnew System::ArgumentNullException("gc_memberSet");
    }
    if (gc_checkpointCreator == nullptr)
    {
        throw gcnew System::ArgumentNullException("gc_checkpointCreator");
    }

    RSLCheckpointCreatorWrapper wrapper(gc_checkpointCreator);

    IntPtr pStr = Marshal::StringToHGlobalAnsi(gc_directoryName);

    RSLib::RSLCheckpointUtility::SaveCheckpoint(
        (PSTR) pStr.ToPointer(), (RSLib::RSLProtocolVersion) version,
        lastExecutedSequenceNumber, configurationNumber,
        gc_memberSet->m_rslMemberSet,
        &wrapper);

    GC::KeepAlive(gc_memberSet);

    Marshal::FreeHGlobal(pStr);

}

String^
ManagedRSLCheckpointUtility::GetLatestCheckpoint(String^ rslDirectory)
{
    if (String::IsNullOrEmpty(rslDirectory))
    {
        throw gcnew System::ArgumentNullException("rslDirectory");
    }

    char file[_MAX_PATH+1];

    IntPtr pStr = Marshal::StringToHGlobalAnsi(rslDirectory);

    DWORD ret = RSLib::RSLCheckpointUtility::GetLatestCheckpoint(
        (PSTR) pStr.ToPointer(),
        file,
        sizeof(file));

    Marshal::FreeHGlobal(pStr);

    if (ret != NO_ERROR)
    {
        throw gcnew Exception(String::Format("Win32 error code: {0}", ret));
    }

    return gcnew String(file);
}


bool
ManagedRSLCheckpointUtility::ChangeReplicaSet(
    String^ checkpointName,
    ManagedRSLMemberSet^ memberSet)
{
    IntPtr pStr = Marshal::StringToHGlobalAnsi(checkpointName);

    bool ret = RSLib::RSLCheckpointUtility::ChangeReplicaSet(
        (PSTR) pStr.ToPointer(),
        memberSet->m_rslMemberSet);

    GC::KeepAlive(memberSet);

    Marshal::FreeHGlobal(pStr);
    return ret;
}

bool
ManagedRSLCheckpointUtility::MigrateToVersion3(
    String^ checkpointName,
    ManagedRSLMemberSet^ memberSet)
{
    IntPtr pStr = Marshal::StringToHGlobalAnsi(checkpointName);

    bool ret = RSLib::RSLCheckpointUtility::MigrateToVersion3(
        (PSTR) pStr.ToPointer(),
        memberSet->m_rslMemberSet);

    GC::KeepAlive(memberSet);

    Marshal::FreeHGlobal(pStr);
    return ret;
}

bool
ManagedRSLCheckpointUtility::MigrateToVersion4(
    String^ checkpointName,
    ManagedRSLMemberSet^ memberSet)
{
    IntPtr pStr = Marshal::StringToHGlobalAnsi(checkpointName);

    bool ret = RSLib::RSLCheckpointUtility::MigrateToVersion4(
        (PSTR) pStr.ToPointer(),
        memberSet->m_rslMemberSet);

    GC::KeepAlive(memberSet);

    Marshal::FreeHGlobal(pStr);
    return ret;
}

bool
ManagedRSLCheckpointUtility::GetReplicaSet(
    String^ checkpointName,
    ManagedRSLMemberSet^ memberSet)
{
    IntPtr pStr = Marshal::StringToHGlobalAnsi(checkpointName);

    bool ret = RSLib::RSLCheckpointUtility::GetReplicaSet(
        (PSTR) pStr.ToPointer(),
        memberSet->m_rslMemberSet);

    GC::KeepAlive(memberSet);

    Marshal::FreeHGlobal(pStr);
    return ret;
}

ManagedRSLCheckpointUtility::ManagedRSLCheckpointUtility()
{
}

ManagedRSLStateMachine::ManagedRSLStateMachine()
{
    m_oMRSLMachine = NULL;
    m_lastAcceptedNode = gcnew ManagedRSLNode();
    m_lastAcceptedBuffer = gcnew array<System::Byte>(0);
}

ManagedRSLStateMachine::!ManagedRSLStateMachine()
{
    if (m_oMRSLMachine != NULL)
    {
        delete m_oMRSLMachine;
    }
    m_oMRSLMachine = NULL;
}

ManagedRSLStateMachine::~ManagedRSLStateMachine()
{
    if (m_oMRSLMachine != NULL)
    {
        delete m_oMRSLMachine;
    }
    m_oMRSLMachine = NULL;
}

CRITSEC s_currentAppDomainCritSec;

static int s_currentAppDomain = 0;

// This routine is the one installed by the ManagedRSL into the logging system.
// it will get called whenever there is a notification (including assert violation).
// This routine will upcall the managed code if a managed routine is provided to managedRSL,
// or return right away if not.
void NotificationsCallbackRoutine(int level, int logId, const char* title, const char *message)
{
    // first we need to get the callback as stored by the managed app.
    ManagedRSLStateMachine::NotificationsCallbackDelegate^ callback = ManagedRSLStateMachine::NotificationsCallback::get();

    // pin the pointer, to avoid GC to move it while we are here
    pin_ptr<ManagedRSLStateMachine::NotificationsCallbackDelegate^> cb = &callback;

    // if the pointer points to nowhere, there was nothing on the callback, and we can get out of here.
    if (*cb != nullptr)
    {
        // if there is no message to log, use nullptr
        String^ strMessage = nullptr;

        if (message!=NULL)
        {
            // otherwise, construct a string with the char* received here
            strMessage = gcnew String(message);
        }

        // if there is no message to log, use nullptr
        String^ strTitle = nullptr;

        if (title!=NULL)
        {
            // otherwise, construct a string with the char* received here
            strTitle = gcnew String(title);
        }

        (*cb)((NotificationLevel)level, (NotificationLogID)logId, strTitle, strMessage);
    }
}


void
ManagedRSLStateMachine::Init(System::String^ logPath)
{
    Init(logPath, true);
}

void
ManagedRSLStateMachine::Init(System::String^ logPath, bool minidumpEnabled)
{
    Init(logPath, nullptr, nullptr, false, true, true, minidumpEnabled);
}

void
ManagedRSLStateMachine::Init(System::String^ logPath, System::String^ thumbPrintA, System::String^ thumbPrintB, bool listenOnAllIPs, bool validateCAChain, bool checkCertificateRevocation)
{
    Init(logPath, thumbPrintA, thumbPrintB, listenOnAllIPs, validateCAChain, checkCertificateRevocation, true);
}

void
ManagedRSLStateMachine::Init(System::String^ logPath, System::String^ thumbPrintA, System::String^ thumbPrintB, bool listenOnAllIPs, bool validateCAChain, bool checkCertificateRevocation, bool minidumpEnabled)
{
    Init(logPath, thumbPrintA, thumbPrintB, nullptr, nullptr, nullptr, nullptr, listenOnAllIPs, true, validateCAChain, checkCertificateRevocation, minidumpEnabled);
}

void
ManagedRSLStateMachine::ReplaceThumbprints(System::String^ thumbPrintA, System::String^ thumbPrintB, bool validateCAChain, bool checkCertificateRevocation)
{
    // Set up thumbprints if needed
    char* thA = NULL;
    char* thB = NULL;
    IntPtr thAPtr;
    IntPtr thBPtr;

    if (!System::String::IsNullOrEmpty(thumbPrintA))
    {
        thAPtr = Marshal::StringToHGlobalAnsi(thumbPrintA);
        thA = (PSTR)thAPtr.ToPointer();
    }
    if (!System::String::IsNullOrEmpty(thumbPrintB))
    {
        thBPtr = Marshal::StringToHGlobalAnsi(thumbPrintB);
        thB = (PSTR)thBPtr.ToPointer();
    }

    bool ok = SetThumbprintsForSsl(thA, thB, validateCAChain, checkCertificateRevocation);

    if (thA != NULL)
    {
        Marshal::FreeHGlobal(thAPtr);
    }
    if (thB != NULL)
    {
        Marshal::FreeHGlobal(thBPtr);
    }

    if (!ok)
    {
        throw gcnew System::InvalidOperationException("Couldn't set up thumbprints");
    }
}

void ManagedRSLStateMachine::ReplaceSubjects(
    System::String^ subjectA, 
    System::String^ thumbPrintsParentA, 
    System::String^ subjectB, 
    System::String^ thumbPrintsParentB,
    bool considerIdentitiesWhitelist)
{
    char* subA = NULL;
    char* subB = NULL;
    char* thumbsparA = NULL;
    char* thumbsparB = NULL;
    IntPtr subAPtr;
    IntPtr subBPtr;
    IntPtr thumbsparAPtr;
    IntPtr thumbsparBPtr;

    if (subjectA != nullptr)
    {
        subAPtr = Marshal::StringToHGlobalAnsi(subjectA);
        subA = (PSTR)subAPtr.ToPointer();
    }

    if (subjectB != nullptr)
    {
        subBPtr = Marshal::StringToHGlobalAnsi(subjectB);
        subB = (PSTR)subBPtr.ToPointer();
    }

    if (thumbPrintsParentA != nullptr)
    {
        thumbsparAPtr = Marshal::StringToHGlobalAnsi(thumbPrintsParentA);
        thumbsparA = (PSTR)thumbsparAPtr.ToPointer();
    }

    if (thumbPrintsParentB != nullptr)
    {
        thumbsparBPtr = Marshal::StringToHGlobalAnsi(thumbPrintsParentB);
        thumbsparB = (PSTR)thumbsparBPtr.ToPointer();
    }

    bool ok = true;

    if (subA != NULL || subB != NULL)
    {
        ok = SetSubjectNamesForSsl(subA, thumbsparA, subB, thumbsparB, considerIdentitiesWhitelist);
    }

    if (subA != NULL)
    {
        Marshal::FreeHGlobal(subAPtr);
    }

    if (subB != NULL)
    {
        Marshal::FreeHGlobal(subBPtr);
    }

    if (thumbsparA != NULL)
    {
        Marshal::FreeHGlobal(thumbsparAPtr);
    }

    if (thumbsparB != NULL)
    {
        Marshal::FreeHGlobal(thumbsparBPtr);
    }

    if (!ok)
    {
        throw gcnew System::InvalidOperationException("Couldn't set up subjects");
    }
}

void ManagedRSLStateMachine::ReplaceThumbprintsAndSubjects(
    System::String^ thumbPrintA,
    System::String^ thumbPrintB,
    System::String^ subjectA,
    System::String^ thumbPrintsParentA,
    System::String^ subjectB,
    System::String^ thumbPrintsParentB,
    bool considerIdentitiesWhitelist,
    bool validateCAChain,
    bool checkCertificateRevocation)
{
    ReplaceThumbprints(thumbPrintA, thumbPrintB, validateCAChain, checkCertificateRevocation);
    ReplaceSubjects(subjectA, thumbPrintsParentA, subjectB, thumbPrintsParentB, considerIdentitiesWhitelist);
}

void
ManagedRSLStateMachine::Init(
    System::String^ logPath,
    System::String^ thumbPrintA,
    System::String^ thumbPrintB,
    System::String^ subjectA,
    System::String^ thumbPrintsParentA,
    System::String^ subjectB,
    System::String^ thumbPrintsParentB,
    bool considerIdentitiesWhitelist,
    bool listenOnAllIPs,
    bool validateCAChain,
    bool checkCertificateRevocation)
{
    Init(logPath, thumbPrintA, thumbPrintB, subjectA, thumbPrintsParentA, subjectB, thumbPrintsParentB, considerIdentitiesWhitelist, listenOnAllIPs, validateCAChain, checkCertificateRevocation, true);
}

void
ManagedRSLStateMachine::Init(
    System::String^ logPath,
    System::String^ thumbPrintA,
    System::String^ thumbPrintB,
    System::String^ subjectA,
    System::String^ thumbPrintsParentA,
    System::String^ subjectB,
    System::String^ thumbPrintsParentB,
    bool considerIdentitiesWhitelist,
    bool listenOnAllIPs,
    bool validateCAChain,
    bool checkCertificateRevocation,
    bool minidumpEnabled)
{
    bool res = false;

    ReplaceThumbprintsAndSubjects(thumbPrintA, thumbPrintB, subjectA, thumbPrintsParentA, subjectB, thumbPrintsParentB, considerIdentitiesWhitelist, validateCAChain, checkCertificateRevocation);

    {
        AutoCriticalSection lock(&s_currentAppDomainCritSec);
        if (s_currentAppDomain != 0)
        {
            throw gcnew System::InvalidOperationException("Another AppDomain has initialized RSL already");
        }

        s_currentAppDomain = AppDomain::CurrentDomain::get()->Id;
    }

    if (listenOnAllIPs)
    {
        EnableListenOnAllIPs();
    }

    SetMinidumpEnabled(minidumpEnabled);

    if (System::String::IsNullOrEmpty(logPath))
    {
        res = RSLInit(NULL,
                     false,
                     &NotificationsCallbackRoutine);
    }
    else
    {
        IntPtr logPathPtr  = Marshal::StringToHGlobalAnsi(logPath);
        res = RSLInit((PSTR)logPathPtr.ToPointer(),
                      false,
                      &NotificationsCallbackRoutine);
        Marshal::FreeHGlobal(logPathPtr);
    }
    if (res == false)
    {
        throw gcnew System::Exception("Failed to initialize RSLib");
    }
}

ref class ArgClass
{
    private:
    unsigned int (*Routine)(void*);
    void* Param;

    public:
    ArgClass(unsigned int (*startMethod)(void*), void *arg)
    {
        this->Routine = startMethod;
        this->Param = arg;
    }

    void Run()
    {
        this->Routine(this->Param);
    }
};

delegate void SpawnThreadDelegate(unsigned int (*startMethod)(void*), ThreadPriority priority, void *arg);

// this routine is registered with the native runtime to create managed threads.
// it will create a managed thread for the given routine, and will create that thread with the required priority.
static void _stdcall SpawnThreadRoutine(unsigned int (*startMethod)(void*), ThreadPriority priority, void *arg)
{
    ArgClass^ myarg = gcnew ArgClass(startMethod, arg);

    pin_ptr<ArgClass^> parg =  &myarg;

    System::Threading::ThreadStart ^ts = gcnew System::Threading::ThreadStart(*parg, &ArgClass::Run);

    System::Threading::Thread^ thr = gcnew System::Threading::Thread(ts);

    thr->IsBackground = true;

    switch (priority)
    {
    case Priority_Normal:
        thr->Priority = System::Threading::ThreadPriority::Normal;
        break;
    case Priority_BelowNormal:
        thr->Priority = System::Threading::ThreadPriority::BelowNormal;
        break;
    case Priority_AboveNormal:
        thr->Priority = System::Threading::ThreadPriority::AboveNormal;
        break;
    case Priority_Highest:
        thr->Priority = System::Threading::ThreadPriority::Highest;
        break;
    case Priority_Lowest:
        thr->Priority = System::Threading::ThreadPriority::Lowest;
        break;
    }

    thr->Start();

    GC::KeepAlive(myarg);
}

bool ManagedRSLStateMachine::Initialize(ManagedRSLConfigParam^ cfg, array<ManagedRSLNode^>^ nodes,
                                        ManagedRSLNode^ selfNode, ManagedRSLProtocolVersion version,
                                        bool serializeFastReadsWithReplicates)
{
    if (cfg == nullptr)
    {
        throw gcnew System::ArgumentNullException("cfg");
    }
    if (nodes == nullptr)
    {
        throw gcnew System::ArgumentNullException("nodes");
    }
    if (selfNode == nullptr)
    {
        throw gcnew System::ArgumentNullException("selfNode");
    }

    //Create the unmanaged state machine engine
    m_oMRSLMachine = new RSLStateMachineWrapper(this);

    RSLNodeCollection rslNodes;
    for (int i=0; i < nodes->Length; i++)
    {
        rslNodes.Append(*nodes[i]->m_node);
    }

    // according to msdn, by wrapping the routine in a delegate, and keeping it alive, we ensure the object
    // will be always available, and the FunctionPointer will stay valid.
    SpawnThreadDelegate^ managed = gcnew SpawnThreadDelegate(&SpawnThreadRoutine);
    pin_ptr<SpawnThreadDelegate^> pinManaged=&managed;

    bool result = m_oMRSLMachine->Initialize(
        cfg->m_configParam,
        rslNodes,
        *(selfNode->m_node),
        (RSLib::RSLProtocolVersion) version,
        serializeFastReadsWithReplicates,
        //&SpawnThreadRoutine,
        static_cast<SpawnRoutine>(Marshal::GetFunctionPointerForDelegate(*pinManaged).ToPointer()),
        true);

    GC::KeepAlive(*pinManaged);

    GC::KeepAlive(cfg);
    GC::KeepAlive(selfNode);

    return result;
}

bool ManagedRSLStateMachine::Initialize(ManagedRSLConfigParam^ cfg, ManagedRSLNode^ selfNode,
                                        ManagedRSLProtocolVersion version, bool serializeFastReadsWithReplicates)
{
    if (cfg == nullptr)
    {
        throw gcnew System::ArgumentNullException("cfg");
    }
    if (selfNode == nullptr)
    {
        throw gcnew System::ArgumentNullException("selfNode");
    }

    //Create the unmanaged state machine engine
    m_oMRSLMachine = new RSLStateMachineWrapper(this);

    // according to msdn, by wrapping the routine in a delegate, and keeping it alive, we ensure the object
    // will be always available, and the FunctionPointer will stay valid.
    SpawnThreadDelegate^ managed = gcnew SpawnThreadDelegate(&SpawnThreadRoutine);
    pin_ptr<SpawnThreadDelegate^> pinManaged=&managed;

    bool result = m_oMRSLMachine->Initialize(
        cfg->m_configParam,
        *(selfNode->m_node),
        (RSLib::RSLProtocolVersion) version,
        serializeFastReadsWithReplicates,
        //&SpawnThreadRoutine,
        static_cast<SpawnRoutine>(Marshal::GetFunctionPointerForDelegate(*pinManaged).ToPointer()),
        true);

    GC::KeepAlive(*pinManaged);

    GC::KeepAlive(cfg);
    GC::KeepAlive(selfNode);

    return result;
}

void
ManagedRSLStateMachine::Unload()
{
    RSLStateMachine::Unload();
    RSLib::RSLUnload();

    {
        AutoCriticalSection lock(&s_currentAppDomainCritSec);
        s_currentAppDomain = 0;
    }
}
void
ManagedRSLStateMachine::ChangeElectionDelay(unsigned int delayInSecs)
{
    if (m_oMRSLMachine == NULL)
    {
        throw gcnew System::ApplicationException("Initialize must be called first");
    }

    m_oMRSLMachine->ChangeElectionDelay( delayInSecs);
}

RSLResponse
ManagedRSLStateMachine::Bootstrap(ManagedRSLMemberSet^ gc_memberSet, int timeout)
{
    if (gc_memberSet == nullptr)
    {
        throw gcnew System::ArgumentNullException("gc_memberSet");
    }
    if (m_oMRSLMachine == NULL)
    {
        throw gcnew System::ApplicationException("Initialize must be called first");
    }
    RSLResponse resp = (RSLResponse) m_oMRSLMachine->Bootstrap(gc_memberSet->m_rslMemberSet, (unsigned int) timeout);

    GC::KeepAlive(gc_memberSet);
    return resp;
}

bool ManagedRSLStateMachine::Replay(
    System::String^ directory,
    System::UInt64 maxSeqNo)
{
    bool ret;

    if (System::String::IsNullOrEmpty(directory))
    {
        throw gcnew System::ArgumentException("directory");
    }

    //Unamanged string pointer
    IntPtr pStr = Marshal::StringToHGlobalAnsi(directory);
    //Create the unmanaged state machine engine
    m_oMRSLMachine = new RSLStateMachineWrapper(this);
    ret = m_oMRSLMachine->Replay((PSTR) pStr.ToPointer(), maxSeqNo);

    Marshal::FreeHGlobal(pStr);

    return ret;
}

//This method will create an unmanaged copy of the request
//object to replicate and call the RSL ReplicateRequest method
RSLResponse ManagedRSLStateMachine::ReplicateRequest(
    array<System::Byte>^ gc_request,
    System::Object^ gc_cookie)
{
    return CreateRequest(0, gc_request, 0, gc_request->Length, gc_cookie, FALSE, FALSE, 0);
}

RSLResponse ManagedRSLStateMachine::ReplicateRequest(
    array<System::Byte>^ gc_request,
    System::Object^ gc_cookie,
    bool isLastRequest)
{
    return CreateRequest(0, gc_request, 0, gc_request->Length, gc_cookie, FALSE, isLastRequest, 0);
}

RSLResponse ManagedRSLStateMachine::ReplicateRequest(
    array<System::Byte>^ gc_request,
    int offset,
    int count,
    System::Object^ gc_cookie)
{
    return CreateRequest(0, gc_request, offset, count, gc_cookie, FALSE, FALSE, 0);
}

RSLResponse ManagedRSLStateMachine::FastReadRequest(
    System::UInt64 maxSeenSeqNo,
    array<System::Byte>^ gc_request,
    System::Object^ gc_cookie)
{
    return CreateRequest(maxSeenSeqNo, gc_request, 0, gc_request->Length, gc_cookie, TRUE, FALSE, 0);
}

RSLResponse ManagedRSLStateMachine::FastReadRequest(
    System::UInt64 maxSeenSeqNo,
    array<System::Byte>^ gc_request,
    int offset,
    int count,
    System::Object^ gc_cookie,
    System::UInt32 timeout)
{
    return CreateRequest(maxSeenSeqNo, gc_request, offset, count, gc_cookie, TRUE, FALSE, timeout);
}

//This method will create an unmanaged copy of the request object to replicate and
//call the RSL FastReadRequest method
RSLResponse ManagedRSLStateMachine::FastReadRequest(
    System::UInt64 maxSeenSeqNo,
    array<System::Byte>^ gc_request,
    int offset,
    int count,
    System::Object^ gc_cookie)
{
    return CreateRequest(maxSeenSeqNo, gc_request, offset, count, gc_cookie, TRUE, FALSE, 0);
}

void
ManagedRSLStateMachine::AttemptPromotion()
{
    m_oMRSLMachine->AttemptPromotion();
}

void
ManagedRSLStateMachine::RelinquishPrimaryStatus()
{
    m_oMRSLMachine->RelinquishPrimaryStatus();
}

RSLResponse ManagedRSLStateMachine::CreateRequest(
    System::UInt64 maxSeenSeqNo,
    array<System::Byte>^ gc_request,
    int offset,
    int count,
    System::Object^ gc_cookie,
    bool isFast,
    bool isLastRequest,
    System::UInt32 timeout)
{
    if(gc_request == nullptr)
    {
        throw gcnew System::ArgumentNullException("request");
    }
    if (offset < 0)
    {
        throw gcnew System::ArgumentException("offset");
    }

    if (count < 0)
    {
        throw gcnew System::ArgumentException("count");
    }
    if (offset + count > gc_request->Length)
    {
        throw gcnew System::ArgumentException("offset + count >= request.Length");
    }

    gcroot<System::Object^> *cookie = NULL;
    System::IntPtr requestPtr;
    RSLResponse ret = RSLResponse::RSLSuccess;
    pin_ptr<Byte> pBuffer = nullptr;

    if(gc_cookie != nullptr)
    {
         cookie = new gcroot<System::Object^>(gc_cookie);
    }

    //Now, since there is no method to pin an array, we
    //will pin the first element, this, by implication,
    //pins the whole array
    pBuffer = &gc_request[0];

    //Replicate the request using the RSL engine
    if(isFast == true)
    {
        ret = (RSLResponse) m_oMRSLMachine->FastReadRequest(maxSeenSeqNo,
                                                            pBuffer+offset,
                                                            (unsigned int) count,
                                                            cookie,
                                                            timeout);
    }
    else if (!isLastRequest)
    {
        ret = (RSLResponse) m_oMRSLMachine->ReplicateRequest(pBuffer+offset,
                                                             (unsigned int) count,
                                                             cookie);
    }
    else
    {
        ret = (RSLResponse)m_oMRSLMachine->ReplicateRequestExclusiveVote(pBuffer + offset,
                                                                         (unsigned int)count,
                                                                         cookie,
                                                                         true);
    }

    if (cookie != NULL && ret != (RSLResponse) RSLSuccess)
    {
        delete cookie;
    }
    return ret;
}

RSLResponse
ManagedRSLStateMachine::ChangeConfiguration(
    ManagedRSLMemberSet^ gc_memberSet,
    System::Object^ gc_cookie)
{
    if (gc_memberSet == nullptr)
    {
        throw gcnew System::ArgumentNullException("memberset");
    }
    if (m_oMRSLMachine == NULL)
    {
        throw gcnew System::ApplicationException("Initialize must be called first");
    }

    gcroot<System::Object^> * cookie = NULL;
    if(gc_cookie != nullptr)
    {
         cookie = new gcroot<System::Object^>(gc_cookie);
    }
    RSLResponseCode res = m_oMRSLMachine->ChangeConfiguration(gc_memberSet->m_rslMemberSet, cookie);
    if (cookie != NULL && res != RSLSuccess)
    {
        delete cookie;
    }
    GC::KeepAlive(gc_memberSet);
    return (RSLResponse) res;
}

bool
ManagedRSLStateMachine::GetConfiguration(
    ManagedRSLMemberSet^ gc_memberSet,
    unsigned int % gc_configNumber)
{
    unsigned int configNumber;
    bool ret = m_oMRSLMachine->GetConfiguration(gc_memberSet->m_rslMemberSet, &configNumber);
    gc_configNumber = configNumber;
    GC::KeepAlive(gc_memberSet);
    return ret;
}

//Unmarshal the request object before calling ExecuteRequest
void ManagedRSLStateMachine::UnmarshalExecuteRequest(
    bool isFastRead,
    void* request,
    unsigned int len,
    void* cookie,
    bool* saveState)
{
    //Unmarshal the managed object from the unmanaged buffer
    gcroot<System::Object^> *theCookie = (gcroot<System::Object^>*) cookie;
    System::IntPtr buffer = (System::IntPtr) request;

    System::Object^ gc_cookie = (theCookie != NULL) ? *theCookie : nullptr;
    //Create a buffer big enough for the request
    array<System::Byte>^ gc_buffer = gcnew array<System::Byte>(len);

    Marshal::Copy(buffer,gc_buffer,0, len);

    if (isFastRead)
    {
        ExecuteFastReadRequest(gc_buffer, gc_cookie);
    }
    else
    {
        System::Boolean ss = false;
        ExecuteReplicatedRequest(gc_buffer, gc_cookie, ss);
        *saveState = ss;
    }
    //we need to deletethe reference to the cookie to
    //instruct the gc that it can collect the memory if needed.
    if(theCookie != NULL)
    {
        delete theCookie;
    }
}

//Unmarshal the cookie and call back the managed RSL state machine
void ManagedRSLStateMachine::UnmarshalAbortRequest(
    RSLResponseCode status,
    void* cookie)
{
    gcroot<System::Object^> *theCookie = (gcroot<System::Object^>*) cookie;

    if(theCookie != NULL)
    {
        AbortRequest((RSLResponse) status, *theCookie);
        delete theCookie;
    }
    else
    {
        AbortRequest((RSLResponse) status, nullptr);
    }
}

void ManagedRSLStateMachine::UnmarshallNotifyConfigurationChanged(void* cookie)
{
    gcroot<System::Object^> *theCookie = (gcroot<System::Object^>*) cookie;
    System::Object^ gc_cookie = (theCookie != NULL) ? *theCookie : nullptr;

    NotifyConfigurationChanged(gc_cookie);

    if(theCookie != NULL)
    {
        delete theCookie;
    }
}

void ManagedRSLStateMachine::UnmarshallAbortChangeConfiguration(RSLResponseCode status, void* cookie)
{
    gcroot<System::Object^> *theCookie = (gcroot<System::Object^>*) cookie;
    System::Object^ gc_cookie = (theCookie != NULL) ? *theCookie : nullptr;

    AbortChangeConfiguration((RSLResponse) status, gc_cookie);

    if(theCookie != NULL)
    {
        delete theCookie;
    }
}

bool ManagedRSLStateMachine::UnmarshalAcceptMessageFromReplica(RSLNode *node, void * data, unsigned int len)
{
    *m_lastAcceptedNode->m_node = *node;

    if (len > 0)
    {
        if (m_lastAcceptedBuffer == nullptr || (int) len != m_lastAcceptedBuffer->Length)
        {
            //Create a buffer big enough for the request
            m_lastAcceptedBuffer = gcnew array<System::Byte>(len);
        }

        Marshal::Copy((IntPtr) data, m_lastAcceptedBuffer, 0, (int) len);
    }
    else
    {
        m_lastAcceptedBuffer = nullptr;
    }

    return AcceptMessageFromReplica(m_lastAcceptedNode, m_lastAcceptedBuffer);
}

void ManagedRSLStateMachine::UnmarshalStateSaved(
    unsigned long long seqNo,
    const char* fileName)
{
    String^ gc_file = gcnew String(fileName);
    StateSaved(seqNo, gc_file);
}

void ManagedRSLStateMachine::UnmarshalStateCopied(
    unsigned long long seqNo,
    const char* fileName,
    void *cookie)
{
    gcroot<System::Object^> *theCookie = (gcroot<System::Object^>*) cookie;
    System::Object^ gc_cookie = (theCookie != NULL) ? *theCookie : nullptr;
    String^ gc_file = gcnew String(fileName);

    StateCopied(seqNo, gc_file, gc_cookie);

    if(theCookie != NULL)
    {
        delete theCookie;
    }
}

//This method will return the current sequence number given by the RSL state machine
System::UInt64 ManagedRSLStateMachine::GetCurrentSequenceNumber()
{
    if(m_oMRSLMachine != NULL)
    {
        return m_oMRSLMachine->GetCurrentSequenceNumber();
    }
    throw gcnew System::ApplicationException("Initialize must be called first");
}

//This method will return the highest passed sequence number given by the RSL state machine
System::UInt64 ManagedRSLStateMachine::GetHighestPassedSequenceNumber()
{
    if(m_oMRSLMachine != NULL)
    {
        return m_oMRSLMachine->GetHighestPassedSequenceNumber();
    }
    throw gcnew System::ApplicationException("Initialize must be called first");
}


array<ManagedReplicaHealth^>^
ManagedRSLStateMachine::MemberSetHealth::get()
{
    unsigned short result = 0;
    int numEntries;

    unsigned int configNumber;
    RSLMemberSet memberset;

    bool ret = m_oMRSLMachine->GetConfiguration(&memberset, &configNumber);

    if (!ret)
    {
        String^ msg = "legislator failed to complete GetConfiguration";
        throw gcnew InvalidDataException(msg);
    }

    numEntries = (int)memberset.GetNumMembers();

    if (numEntries == 0)
    {
        return gcnew array<ManagedReplicaHealth^>(0);
    }

    ReplicaHealth *replicaHealth = NULL;
    do
    {
        if (replicaHealth != NULL)
        {
            delete[] replicaHealth;
        }

        replicaHealth = new ReplicaHealth[numEntries];
        result = m_oMRSLMachine->GetReplicasInformation((unsigned short)numEntries, replicaHealth);

        if (result > 0)
        {
            numEntries = result;
        }
    } while (result>0);

    array<ManagedReplicaHealth^>^ gc_array = gcnew array<ManagedReplicaHealth^>((int) numEntries);

    for (int i=0; i < gc_array->Length; i++)
    {
        gc_array[i] = gcnew ManagedReplicaHealth(&(replicaHealth[i]));
    }

    delete[] replicaHealth;

    return gc_array;
}

ManagedReplicaHealth^
ManagedRSLStateMachine::GetReplicaHealth(System::String^ memberId)
{
    IntPtr pStr = Marshal::StringToHGlobalAnsi(memberId);
    ReplicaHealth replicaHealth;

    HRESULT hresult = StringCbCopyA(replicaHealth.m_memberId, sizeof(replicaHealth.m_memberId), (PSTR) pStr.ToPointer());
    Marshal::FreeHGlobal(pStr);
    if (!SUCCEEDED(hresult))
    {
        String^ msg = String::Format("length of value must be less {0}", sizeof(replicaHealth.m_memberId));
        throw gcnew ArgumentException(msg);
    }

    bool success = m_oMRSLMachine->GetReplicaInformation(&replicaHealth);

    if (!success)
    {
        String^ msg = "Legislator couldn't calculate the Health";
        throw gcnew InvalidDataException(msg);
    }

    ManagedReplicaHealth^ gc_result = gcnew ManagedReplicaHealth(&replicaHealth);

    return gc_result;
}

//This method will pause processing on the RSL state machine
void ManagedRSLStateMachine::Pause()
{
    if(m_oMRSLMachine != NULL)
    {
        return m_oMRSLMachine->Pause();
    }
    throw gcnew System::ApplicationException("Initialize must be called first");
}

//This method will unload this SM
void ManagedRSLStateMachine::UnloadThisOne()
{
    if(m_oMRSLMachine != NULL)
    {
        return m_oMRSLMachine->UnloadThisOne();
    }

    throw gcnew System::ApplicationException("Initialize must be called first");
}


//This method will resume processing on the RSL state machine
void ManagedRSLStateMachine::Resume()
{
    if(m_oMRSLMachine != NULL)
    {
        return m_oMRSLMachine->Resume();
    }
    throw gcnew System::ApplicationException("Initialize must be called first");
}

//This method will return the index of the primary replica
void ManagedRSLStateMachine::GetCurrentPrimary(ManagedRSLNode ^node)
{
    if(m_oMRSLMachine != NULL)
    {
        m_oMRSLMachine->GetCurrentPrimary(node->m_node);
        GC::KeepAlive(node);
        return;
    }
    throw gcnew System::ApplicationException("Initialize must be called first");
}

void ManagedRSLStateMachine::SetVotePayload(unsigned long long payload)
{
    if (m_oMRSLMachine != NULL)
    {
        return m_oMRSLMachine->SetVotePayload(payload);
    }
    throw gcnew System::ApplicationException("Initialize must be called first");
}

void ManagedRSLStateMachine::AllowSaveState(bool yes)
{
    if (m_oMRSLMachine != NULL)
    {
        return m_oMRSLMachine->AllowSaveState(yes);
    }
    throw gcnew System::ApplicationException("Initialize must be called first");
}

bool ManagedRSLStateMachine::IsAnyRequestPendingExecution()
{
    if (m_oMRSLMachine != NULL)
    {
        return m_oMRSLMachine->IsAnyRequestPendingExecution();
    }
    throw gcnew System::ApplicationException("Initialize must be called first");
}

RSLResponse ManagedRSLStateMachine::CopyStateFromReplica(System::Object^ gc_cookie)
{
    gcroot<System::Object^> * cookie = NULL;
    if(gc_cookie != nullptr)
    {
         cookie = new gcroot<System::Object^>(gc_cookie);
    }
    RSLResponseCode res = m_oMRSLMachine->CopyStateFromReplica(cookie);
    if (cookie != NULL && res != RSLSuccess)
    {
        delete cookie;
    }
    return (RSLResponse) res;
}

ManagedRSLStats^ ManagedRSLStateMachine::GetStatisticsSnapshot()
{
    RSLStats stats;
    stats.m_cbSizeOfThisStruct = sizeof(RSLStats);
    m_oMRSLMachine->GetStatisticsSnapshot(&stats);

    ManagedRSLStats^ result = gcnew ManagedRSLStats();
    result->captureTimeInMicroseconds = stats.m_cCaptureTimeInMicroseconds;
    result->decreesExecuted = stats.m_cDecreesExecuted;
    result->logReads = stats.m_cLogReads;
    result->logReadBytes = stats.m_cLogReadBytes;
    result->logReadMicroseconds = stats.m_cLogReadMicroseconds;
    result->logReadMaxMicroseconds = stats.m_cLogReadMaxMicroseconds;
    result->logWrites = stats.m_cLogWrites;
    result->logWriteBytes = stats.m_cLogWriteBytes;
    result->logWriteMicroseconds = stats.m_cLogWriteMicroseconds;
    result->logWriteMaxMicroseconds = stats.m_cLogWriteMaxMicroseconds;
    result->votingTimeMicroseconds = stats.m_cVotingTimeMicroseconds;
    result->votingTimeMaxMicroseconds = stats.m_cVotingTimeMaxMicroseconds;

    return result;
}

System::UInt64 ManagedRSLUtils::CalculateChecksum(array<System::Byte>^ data)
{
    unsigned char* blob = new unsigned char[data->Length];

    for (int i = 0; i < data->Length; ++i)
    {
        blob[i] = data[i];
    }

    System::UInt64 res = RSLUtils::CalculateChecksum(blob, data->Length);

    delete[] blob;

    return res;
}
