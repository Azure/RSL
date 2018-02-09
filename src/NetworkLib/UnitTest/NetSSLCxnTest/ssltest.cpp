#include <conio.h>
#include "unittest.h"
#include <time.h>
#include "strsafe.h"
#include "Suite.h"
#include "NetPacketSvc.h"
#include "NetPacket.h"
#include <memory>
#include "SSLImpl.h"
#include <map>
#include "basic_types.h"
#include "ScopeGuard.h"
#include <functional>

typedef std::function<void()> AsyncFunction;

using namespace RSLibImpl;
#define AddTest(_x) suite1.AddTestCase(#_x, _x)

#define DEFAULT_DATA_SIZE_TO_SEND 300
#define MAX_DATA_SIZE_TO_SEND (4 * 1024 * 1024)
#define MAX_DATA_SIZE_LIMIT_FOR_SERVICE (MAX_DATA_SIZE_TO_SEND + 4 * 1024)
#define SERVER_ADDRESS 0x0100007F

#define TEST_WAIT_TIMEOUT 60000
#define PACKET_TIMEOUT 10000
#define PACKET_TIMEOUT_MANY 20000
#define MAX_PENDING_PACKETS 100
#define DEFAULT_LISTENING_PORT 2116


void DisplayStringAndInt(LPCTSTR str, int val)
{
    char buff[100];
    OutputDebugString(str); OutputDebugString(" : ");

    _itoa_s(val, buff, ARRAYSIZE(buff), 10);
    OutputDebugString(buff); OutputDebugString("\r\n");
}

class AsyncWorkCleanup
{
    LPVOID m_handle;
    int m_timeout;
public:
    AsyncWorkCleanup(HANDLE handle, DWORD timeout)
    {
        m_handle = handle;
        m_timeout = timeout;
        UT_AssertIsNotNull(m_handle);
    }

    ~AsyncWorkCleanup()
    {
        UT_AssertIsTrue(WaitForSingleObject(m_handle, m_timeout) == WAIT_OBJECT_0);
        CloseHandle(m_handle);
    }
};

DWORD WINAPI AsyncWorkThread(LPVOID lpParam)
{
    AsyncFunction *pFunction = (AsyncFunction*) lpParam;

    (*pFunction)();
    return 0;
}

AsyncWorkCleanup RunAsyncWorkItem(AsyncFunction &functionToExecute, DWORD timeout)
{
    return AsyncWorkCleanup(CreateThread(NULL, 0, AsyncWorkThread, (LPVOID)&functionToExecute, 0, NULL), timeout);
}

#define CONCATENATE_DIRECT(s1, s2) s1##s2
#define CONCATENATE(s1, s2) CONCATENATE_DIRECT(s1, s2)
#define ANONYMOUS_VARIABLE(str) CONCATENATE(str, __LINE__)
#define INVOKE_ASYNC_WORK_ITEM(func, timeout) AsyncWorkCleanup ANONYMOUS_VARIABLE(__waitComplete) = RunAsyncWorkItem(func, timeout)

#define DEFINE_ASYNC_WORK_ITEM(workItem) AsyncFunction ANONYMOUS_VARIABLE(__asyncWI) = [](){ workItem; };
#define DEFINE_ASYNC_WORK_ITEM_WITH_CAPTURE(workItem) AsyncFunction ANONYMOUS_VARIABLE(__asyncWI) = [=](){ workItem; };

//#define RUN_ASYNC_WORK_ITEM(workItem)   DEFINE_ASYNC_WORK_ITEM(workItem);  INVOKE_ASYNC_WORK_ITEM(ANONYMOUS_VARIABLE(__asyncWI));
#define RUN_ASYNC_WORK_ITEM_WITH_CAPTURE_TIMEOUT(workItem, timeout)  DEFINE_ASYNC_WORK_ITEM_WITH_CAPTURE(workItem); INVOKE_ASYNC_WORK_ITEM(ANONYMOUS_VARIABLE(__asyncWI), timeout);
#define RUN_ASYNC_WORK_ITEM(workitem) RUN_ASYNC_WORK_ITEM_WITH_CAPTURE_TIMEOUT(workitem, TEST_WAIT_TIMEOUT)
#define RUN_ASYNC_WORK_ITEM_LONG(workitem) RUN_ASYNC_WORK_ITEM_WITH_CAPTURE_TIMEOUT(workitem, INFINITE)

void CleanupWorkItem(LPVOID handle)
{
    UT_AssertIsTrue(WaitForSingleObject(handle, TEST_WAIT_TIMEOUT) == WAIT_OBJECT_0);
    
}

class PacketSvcCallbackHandler : public SendHandler, public ReceiveHandler, public  ConnectHandler
{
    HANDLE m_connectEvent;
    HANDLE m_recvPacketEvent;
    HANDLE m_sentPacketEvent;

    HANDLE m_readyForConnectEvent;
    HANDLE m_readyForRecvPacketEvent;
    HANDLE m_readyForSentPacketEvent;

    
    Packet *m_pSentPacket;
    TxRxStatus m_sentStatus;

    Packet *m_pReceivedPacket;

    UInt32 m_connectedIP;
    UInt16 m_connectedPort;
    ConnectState m_connectState;

    bool m_timeoutHappened;
  
public:
    PacketSvcCallbackHandler()
    {
        m_connectEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
        UT_AssertIsNotNull(m_connectEvent);
        
        m_recvPacketEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
        UT_AssertIsNotNull(m_recvPacketEvent);
        
        m_sentPacketEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
        UT_AssertIsNotNull(m_sentPacketEvent);

        m_readyForConnectEvent = CreateEvent(NULL, FALSE, TRUE, NULL);
        UT_AssertIsNotNull(m_readyForConnectEvent);

        m_readyForRecvPacketEvent = CreateEvent(NULL, FALSE, TRUE, NULL);
        UT_AssertIsNotNull(m_readyForRecvPacketEvent);

        m_readyForSentPacketEvent = CreateEvent(NULL, FALSE, TRUE, NULL);
        UT_AssertIsNotNull(m_readyForSentPacketEvent);

        m_pSentPacket = m_pReceivedPacket = NULL;

        m_timeoutHappened = false;
    }

    ~PacketSvcCallbackHandler()
    {
        CloseHandle(m_connectEvent);
        CloseHandle(m_recvPacketEvent);
        CloseHandle(m_sentPacketEvent);
    }
  
    virtual void ProcessSend(Packet *pPacket, TxRxStatus status)
    {
        UT_AssertIsTrue(WaitForSingleObject(m_readyForSentPacketEvent, TEST_WAIT_TIMEOUT) == WAIT_OBJECT_0);

        UT_AssertIsNull(m_pSentPacket);
        m_pSentPacket = pPacket;
        m_sentStatus = status;

        SetEvent(m_sentPacketEvent);
    }

    void GetSendStatus(Packet **ppPacket, TxRxStatus *pStatus)
    {
        UT_AssertIsTrue(WaitForSingleObject(m_sentPacketEvent, TEST_WAIT_TIMEOUT) == WAIT_OBJECT_0);
        *ppPacket = m_pSentPacket;
        *pStatus = m_sentStatus;

        m_pSentPacket = NULL;

        SetEvent(m_readyForSentPacketEvent);
    }

    virtual void ProcessReceive(Packet *pPacket)
    {
        UT_AssertIsTrue(WaitForSingleObject(m_readyForRecvPacketEvent, TEST_WAIT_TIMEOUT) == WAIT_OBJECT_0);

        UT_AssertIsNull(m_pReceivedPacket);

        m_pReceivedPacket = pPacket;

        SetEvent(m_recvPacketEvent);
    }

    void GetReceivedPacket(Packet **ppPacket)
    {
        UT_AssertIsTrue(WaitForSingleObject(m_recvPacketEvent, TEST_WAIT_TIMEOUT) == WAIT_OBJECT_0);

        *ppPacket = m_pReceivedPacket;

        m_pReceivedPacket = NULL;

        SetEvent(m_readyForRecvPacketEvent);
    }

    virtual void ProcessConnect(UInt32 ip, UInt16 port, ConnectState state)
    {
        UT_AssertIsTrue(WaitForSingleObject(m_readyForConnectEvent, TEST_WAIT_TIMEOUT) == WAIT_OBJECT_0);

        m_connectedIP = ip;
        m_connectedPort = port;
        m_connectState = state;

        SetEvent(m_connectEvent);
    }

    void GetConnectResult(UInt32 *pIp, UInt16 *pPort, ConnectState *pState)
    {
        UT_AssertIsTrue(WaitForSingleObject(m_connectEvent, TEST_WAIT_TIMEOUT) == WAIT_OBJECT_0);

        *pIp = m_connectedIP;
        *pPort = m_connectedPort;
        *pState = m_connectState;

        SetEvent(m_readyForConnectEvent);
    }

    UInt32 GetRemoteIP() { return m_connectedIP; }
    UInt16 GetRemotePort() { return m_connectedPort; } 

    void WaitForConnectionFrom(UInt32 ip, UInt16 port)
    {
        UInt32 connectedIP;
        UInt16 connectedPort;
        ConnectState connectState = DisConnected;

        while(connectState != Connected)
            {
                GetConnectResult(&connectedIP, &connectedPort, &connectState);
            }

        if( ip != INADDR_ANY )
            {
                UT_AssertIsTrue(ip == connectedIP);
            }

        if( port != 0)
            {
                UT_AssertIsTrue(port == connectedPort);
            }
    }

    void WaitForPacketSent()
    {
        Packet *pPacket;
        TxRxStatus status;
        GetSendStatus( &pPacket, &status);

        UT_AssertIsTrue(status == TxSuccess);
    }

    bool& TimeoutHappened()
    {
        return m_timeoutHappened;
    }

    void WaitForPacketSentOrTimeout()
    {
        Packet *pPacket;
        TxRxStatus status;
        GetSendStatus(&pPacket, &status);

        if(status == TxTimedOut)
            {
                m_timeoutHappened = true;
                //printf("Test Timedout\r\n");
            }
        
        UT_AssertIsTrue(status == TxSuccess || status == TxTimedOut);
    }
};

NetPacketSvc *g_pPacketSvc;
NetPacketSvc *g_pPacketClient;
PacketFactory *g_pPacketFactory;

PacketSvcCallbackHandler *g_pSvcCallbackHandler;
PacketSvcCallbackHandler *g_pClientCallbackHandler;

void TestSSLInitialize()
{
    UT_AssertIsTrue(Netlib::Initialize());
    
    g_pPacketSvc = new NetPacketSvc(MAX_DATA_SIZE_LIMIT_FOR_SERVICE);
    UT_AssertIsNotNull(g_pPacketSvc);

    g_pPacketFactory = new PacketFactory( MAX_DATA_SIZE_LIMIT_FOR_SERVICE, MAX_DATA_SIZE_LIMIT_FOR_SERVICE);
    UT_AssertIsNotNull(g_pPacketFactory);

    g_pSvcCallbackHandler = new PacketSvcCallbackHandler();
    UT_AssertIsNotNull(g_pSvcCallbackHandler);
  
    g_pPacketSvc->StartAsServer( DEFAULT_LISTENING_PORT, g_pSvcCallbackHandler, g_pSvcCallbackHandler, g_pSvcCallbackHandler, g_pPacketFactory, INADDR_ANY);
  
    g_pPacketClient = new NetPacketSvc(MAX_DATA_SIZE_LIMIT_FOR_SERVICE);
    UT_AssertIsNotNull(g_pPacketClient);

    g_pClientCallbackHandler = new PacketSvcCallbackHandler();
    g_pPacketClient->StartAsClient(g_pClientCallbackHandler, g_pClientCallbackHandler, g_pClientCallbackHandler, g_pPacketFactory, INADDR_ANY);
}

void TestSSLCleanup()
{
    g_pPacketClient->Stop();
    g_pPacketSvc->Stop();

    delete g_pPacketSvc; g_pPacketSvc = NULL;
    delete g_pPacketClient; g_pPacketClient = NULL;
    delete g_pPacketFactory; g_pPacketFactory = NULL;
    delete g_pSvcCallbackHandler; g_pSvcCallbackHandler = NULL;
    delete g_pClientCallbackHandler; g_pClientCallbackHandler = NULL;
}

struct TestPacketHeader
{
    UInt64 MagicNumber;
    UInt32 DataLength;
    UInt32 PacketSequenceNumber;
};

struct TestPacket
{
    TestPacketHeader Header;
    BYTE Data[1];
};

#define PACKET_SIZE_FOR_DATA(len) (sizeof(TestPacketHeader) + (len))
#define DATA_SIZE_FOR_PACKET(len) ((len) - sizeof(TestPacketHeader))

Packet* GenerateRandomPacket(int len = DEFAULT_DATA_SIZE_TO_SEND, int seqNo = 0)
{
    std::unique_ptr<Packet> pNetPacket(g_pPacketFactory->CreatePacket());

    pNetPacket->m_MemoryManager.ResizeBuffer(PACKET_SIZE_FOR_DATA(len));
    
    TestPacket *pPacket = (TestPacket*)pNetPacket->m_MemoryManager.GetBuffer();
    pPacket->Header.MagicNumber = 0xFACEF00D;
    pPacket->Header.DataLength = len;
    pPacket->Header.PacketSequenceNumber = seqNo ? seqNo : rand();
    
    for(int i = 0; i < len; i++)
        {
            pPacket->Data[i] = (BYTE)rand();
        }

    pNetPacket->m_MemoryManager.SetValidLength(PACKET_SIZE_FOR_DATA(len));
    return pNetPacket.release();
}

void VerifyPacketsMatch(Packet *pPkt1, Packet *pPkt2)
{
    UT_AssertIsTrue(pPkt1->m_MemoryManager.GetValidLength() == pPkt2->m_MemoryManager.GetValidLength());

    TestPacket *pPacket2 = (TestPacket*) pPkt2->m_MemoryManager.GetBuffer();
    UT_AssertIsNotNull(pPacket2);

    TestPacket *pPacket = (TestPacket*)pPkt1->m_MemoryManager.GetBuffer();
    UT_AssertIsNotNull(pPacket);

    int length = pPkt2->m_MemoryManager.GetValidLength();

    UT_AssertIsTrue(pPacket->Header.DataLength == DATA_SIZE_FOR_PACKET(length));
    
    //printf("Test Comparing packets of size %d for data equality\n", length);
    UT_AssertIsTrue(memcmp((LPVOID)pPacket2, (LPVOID)pPacket, length) == 0);
}

void TestConnectAndSendPacketFromClientToServer()
{
    std::unique_ptr<Packet> pNetPacket(GenerateRandomPacket(DEFAULT_DATA_SIZE_TO_SEND));
    // send DEFAULT_DATA_SIZE_TO_SEND bytes of data
    
    pNetPacket->SetServerAddr(SERVER_ADDRESS, DEFAULT_LISTENING_PORT);

    g_pPacketClient->Send(pNetPacket.get(), PACKET_TIMEOUT);


    {
        // scope to complete these items
        RUN_ASYNC_WORK_ITEM(g_pClientCallbackHandler->WaitForConnectionFrom(INADDR_ANY, DEFAULT_LISTENING_PORT));
        RUN_ASYNC_WORK_ITEM(g_pSvcCallbackHandler->WaitForConnectionFrom(INADDR_ANY, 0));
        RUN_ASYNC_WORK_ITEM(g_pClientCallbackHandler->WaitForPacketSent());
    }

    Packet *pRecvPacket = NULL;
    g_pSvcCallbackHandler->GetReceivedPacket(&pRecvPacket);
    UT_AssertIsNotNull(pRecvPacket);
    std::unique_ptr<Packet> pNetRecvPacket(pRecvPacket);

    VerifyPacketsMatch(pNetPacket.get(), pNetRecvPacket.get());
}


void SendPacketFromClientToServerWithSize(int len, DWORD timeOut = PACKET_TIMEOUT, bool timeOutAllowed = false)
{
    std::unique_ptr<Packet> pNetPacket(GenerateRandomPacket(len));
    // send DEFAULT_DATA_SIZE_TO_SEND bytes of data
    
    pNetPacket->SetServerAddr(SERVER_ADDRESS, DEFAULT_LISTENING_PORT);

    g_pPacketClient->Send(pNetPacket.get(), timeOut);

    if(timeOutAllowed)
        {
            g_pClientCallbackHandler->WaitForPacketSentOrTimeout();
        }
    else
        {
            g_pClientCallbackHandler->WaitForPacketSent();
        }

    Packet *pRecvPacket = NULL;
    g_pSvcCallbackHandler->GetReceivedPacket(&pRecvPacket);
    UT_AssertIsNotNull(pRecvPacket);
    std::unique_ptr<Packet> pNetRecvPacket(pRecvPacket);

    VerifyPacketsMatch(pNetPacket.get(), pNetRecvPacket.get());
}

void SendPacketFromServerToClientWithSize(int len, DWORD timeOut = PACKET_TIMEOUT, bool timeOutAllowed = false)
{
    std::unique_ptr<Packet> pNetPacket(GenerateRandomPacket(len));
    // send DEFAULT_DATA_SIZE_TO_SEND bytes of data
    
    pNetPacket->SetClientAddr(g_pSvcCallbackHandler->GetRemoteIP(), g_pSvcCallbackHandler->GetRemotePort());

    g_pPacketSvc->Send(pNetPacket.get(), timeOut);

    if(timeOutAllowed)
        {
            g_pSvcCallbackHandler->WaitForPacketSentOrTimeout();
        }
    else
        {
            g_pSvcCallbackHandler->WaitForPacketSent();
        }

    Packet *pRecvPacket = NULL;
    g_pClientCallbackHandler->GetReceivedPacket(&pRecvPacket);
    UT_AssertIsNotNull(pRecvPacket);
    std::unique_ptr<Packet> pNetRecvPacket(pRecvPacket);

    VerifyPacketsMatch(pNetPacket.get(), pNetRecvPacket.get());
}

void TestSendPacketFromServerToClient()
{
    SendPacketFromServerToClientWithSize(DEFAULT_DATA_SIZE_TO_SEND);
}

void TestSendLargePacketFromServerToClient()
{
    SendPacketFromServerToClientWithSize(MAX_DATA_SIZE_TO_SEND);
}

void TestSendPacketFromClientToServer()
{
    SendPacketFromClientToServerWithSize(DEFAULT_DATA_SIZE_TO_SEND);
}

void TestSendLargePacketFromClientToServer()
{
    SendPacketFromClientToServerWithSize(MAX_DATA_SIZE_TO_SEND);
}

void TestSendPacketAndTimeout()
{
    g_pClientCallbackHandler->TimeoutHappened() = false;
    g_pSvcCallbackHandler->TimeoutHappened() = false;
    
    for(int i = 0; i < 10; i++)
        {
            // large packet can't be sent in a ms, so timeout will happen.
            // Even though the timeout happens, the packet transmission would have started and will finish
            // so we will receive a packet on the other end just like if timeout never happened.
            SendPacketFromClientToServerWithSize(MAX_DATA_SIZE_TO_SEND, 1, true); 
            SendPacketFromServerToClientWithSize(MAX_DATA_SIZE_TO_SEND, 1, true); 
        }

    // in case if this does not trigger timeout, increase the packet size
    UT_AssertIsTrue(g_pClientCallbackHandler->TimeoutHappened());
    UT_AssertIsTrue(g_pSvcCallbackHandler->TimeoutHappened());
}

template<class Key,class Value>
class SynchronousMap
{
private:
    CRITSEC m_cs;
    std::map<Key, Value> m_map;

public:
    Value RemoveValue(Key key)
    {
        AutoCriticalSection _autoCs(&m_cs);
        std::map<Key, Value>::iterator it = m_map.find(key);
        UT_AssertIsTrue( it != m_map.end());

        return it->second;
    }

    void SetValue(Key key, Value value)
    {
        AutoCriticalSection _autoCs(&m_cs);
        m_map[key] = value;
    }
};

int g_TotalPacketsToSend = 10000;

void ReceivePacketsAndValidate(SynchronousMap<int, Packet*> *pPacketMap, PacketSvcCallbackHandler *pCallbackHandler)
{

    for(int i = 0; i < g_TotalPacketsToSend; i++)
        {
            Packet *pPacket;
            pCallbackHandler->GetReceivedPacket(&pPacket);
            UT_AssertIsNotNull(pPacket);
            std::unique_ptr<Packet> pNetPacket(pPacket);

            TestPacket *pDataRecvd = (TestPacket*) pNetPacket->m_MemoryManager.GetBuffer();

            Packet *pOriginalPktSent = pPacketMap->RemoveValue(pDataRecvd->Header.PacketSequenceNumber);
            UT_AssertIsNotNull(pOriginalPktSent);

            VerifyPacketsMatch(pOriginalPktSent, pNetPacket.get());

            if( i % 1000 )
                {
                    DisplayStringAndInt("Test total packets validated", i);
                }
        }

    //printf("Test total packets validated %d\n", g_TotalPacketsToSend);
}

void HandleSentEventsForManySends(PacketSvcCallbackHandler *pCallbackHandler, HANDLE semaphore)
{
    for(int i =0; i < g_TotalPacketsToSend; i++)
        {
            Packet *pPacket = NULL;
            TxRxStatus status;
            pCallbackHandler->GetSendStatus(&pPacket, &status);
            UT_AssertIsTrue( status == TxSuccess);
            UT_AssertIsNotNull(pPacket);

            ReleaseSemaphore(semaphore, 1, (LPLONG)NULL);
        }
}

void SendManyRandomPackets(SynchronousMap<int, Packet*> *pPacketMap, PacketSvcCallbackHandler *pCallbackHandler, NetPacketSvc *pSvcToSend, bool serverToClient)
{
    HANDLE semaphore = CreateSemaphore(NULL, MAX_PENDING_PACKETS, MAX_PENDING_PACKETS, NULL);
    UT_AssertIsNotNull(semaphore);
    ON_BLOCK_EXIT(CloseHandle, semaphore);

    RUN_ASYNC_WORK_ITEM_LONG(HandleSentEventsForManySends(pCallbackHandler, semaphore)); // completes before function returns

    for(int i =0; i < g_TotalPacketsToSend; i++)
        {
            UT_AssertIsTrue(WaitForSingleObject(semaphore, TEST_WAIT_TIMEOUT) == WAIT_OBJECT_0);
            
            int size = DEFAULT_DATA_SIZE_TO_SEND + rand() % (32*1024);
            Packet *pPacket = GenerateRandomPacket(size, i+1);
            UT_AssertIsNotNull(pPacket);
            pPacketMap->SetValue(i+1, pPacket);

            if(serverToClient)
                {
                    pPacket->SetClientAddr(pCallbackHandler->GetRemoteIP(), pCallbackHandler->GetRemotePort());
                }
            else
                {
                    pPacket->SetServerAddr(pCallbackHandler->GetRemoteIP(), pCallbackHandler->GetRemotePort());
                }

            pSvcToSend->Send(pPacket, PACKET_TIMEOUT);
        }
}


void SendManyPacketsFromServerToClient()
{
    SynchronousMap<int, Packet*> packetMap;
    SynchronousMap<int, Packet*> *pPacketMap = &packetMap;

    // run in parallel and complete before function returns
    RUN_ASYNC_WORK_ITEM_LONG(SendManyRandomPackets(pPacketMap, g_pSvcCallbackHandler, g_pPacketSvc, true));
    RUN_ASYNC_WORK_ITEM_LONG(ReceivePacketsAndValidate(pPacketMap, g_pClientCallbackHandler));
}

void SendManyPacketsFromClientToServer()
{
    SynchronousMap<int, Packet*> packetMap;
    SynchronousMap<int, Packet*> *pPacketMap = &packetMap;

    // run in parallel and complete before function returns
    RUN_ASYNC_WORK_ITEM_LONG(SendManyRandomPackets(pPacketMap, g_pClientCallbackHandler, g_pPacketClient, false));
    RUN_ASYNC_WORK_ITEM_LONG(ReceivePacketsAndValidate(pPacketMap, g_pSvcCallbackHandler));
}

void TestManyPacketsToAndFro()
{
    // run in parallel and complete before function returns
    RUN_ASYNC_WORK_ITEM_LONG(SendManyPacketsFromServerToClient());
    RUN_ASYNC_WORK_ITEM_LONG(SendManyPacketsFromClientToServer());
}

int __cdecl main(int argc, char **argv)
{
    if(argc > 1)
        {
            g_TotalPacketsToSend = atoi(argv[1]);
        }
    
    UT_AssertIsTrue(SSLAuth::SetSSLThumbprints("MY", "1b32891adb56d3f7115e7e031cc41e1793252015", NULL, true, true) == ERROR_SUCCESS);

    srand((unsigned int ) time(NULL));

    //UT_AssertIsTrue(RSLib::RSLInit(".\\debuglogs\\rsl", true));

    CUnitTestSuite suite1("RSL Test");

    // TestInitialize must be the first test. This initializes
    // g_test object.
    AddTest(TestSSLInitialize);

    AddTest(TestConnectAndSendPacketFromClientToServer); // This has to be the first. Otherwise connect events wont be handled by other callers which may interfere with other functions

    // Begin : Add tests here
    AddTest(TestSendPacketFromServerToClient);
    AddTest(TestSendPacketFromClientToServer);
    AddTest(TestSendLargePacketFromServerToClient);
    AddTest(TestSendLargePacketFromClientToServer);
    AddTest(TestSendPacketAndTimeout);
    AddTest(TestManyPacketsToAndFro);
    // End : Add tests here
    
    AddTest(TestSSLCleanup);
    suite1.RunAllSuites();
    
    //printf("Press any key to exit...");
    //_getch();
}
