#include <rsl.h>
#include <ctime>
#include <iostream>
#include <sstream>
#include <string>
#include <strsafe.h>
#include "streamio.h"

#include "basic_types.h"

#include "Wincrypt.h"

using namespace SslPlumbing;

int GetName(const char *name, in_addr * add)
{
    struct hostent *he;
    struct in_addr **addr_list;

    if ((he = gethostbyname(name)) == NULL)
    {
        // get the host info
        return 2;
    }

    // print information about this host:
    addr_list = (struct in_addr **)he->h_addr_list;

    if (addr_list[0] != NULL)
    {
        CopyMemory(add, addr_list[0], sizeof(in_addr));
        return 0;
    }
    return 1;
}

void GetAddress(const char *str, in_addr *addr)
{
    long *p = (long*)addr;
    *p = inet_addr(str);
}

SOCKET ConnectClient(const char* serverAddress, u_short aPortNumber)
{
    SOCKET sockfd = socket(PF_INET, SOCK_STREAM, 0);
    if (sockfd == SOCKET_ERROR)
    {
        perror("ERROR creating socket");
        //Log and Error
        return NULL;
    }

    struct sockaddr_in saiServerAddress;
    memset((char *)&saiServerAddress, 0, sizeof(saiServerAddress));

    in_addr serv_addr;

    GetName(serverAddress, &serv_addr);

    saiServerAddress.sin_family = AF_INET;
    saiServerAddress.sin_port = htons(aPortNumber);
    saiServerAddress.sin_addr = serv_addr;

    int ok = connect(sockfd, (struct sockaddr *) &saiServerAddress, sizeof(saiServerAddress));
    if (ok != 0)
    {
        perror("ERROR connecting to socket");
        closesocket(sockfd);
        return NULL;
    }

    printf("Socket connect succeeded\n");

    return sockfd;
}

int Client(const char * serverAddress, u_short aPortNumber, const char * certThumbprint, void(*clientFunction)(SChannelSocket* sck))
{
    SOCKET sockfd = ConnectClient(serverAddress, aPortNumber);

    if(sockfd == NULL)
    {
        return 1;
    }

    SSLAuth::SetSSLThumbprints("MY", certThumbprint, certThumbprint, true, true);
    std::unique_ptr<SChannelSocket> sslsock(new SChannelSocket(sockfd, NULL, false));

    int ret = sslsock->Connect();
    if (ret == NO_ERROR)
    {
        printf("SSL auth completed\n");
        clientFunction(sslsock.get());

        sslsock->Close();
    }
    else
    {
        printf("SSL auth failed: %d\n", ret);
        return 1;
    }

    return 0;
}

SOCKET ListenForAndAcceptClient(const char* bindAddr, u_short aPortNumber)
{
    SOCKET sockfd = socket(PF_INET, SOCK_STREAM, 0);

    if (sockfd == SOCKET_ERROR)
    {
        int  err = WSAGetLastError();
        printf("error: %d\n", err);
        perror("ERROR opening socket");
        return NULL;
    }

    sockaddr_in saiServerAddress;
    memset((char *)&saiServerAddress, 0, sizeof(saiServerAddress));

    in_addr serv_addr;
    GetAddress(bindAddr, &serv_addr);

    saiServerAddress.sin_family = AF_INET;
    saiServerAddress.sin_port = htons(aPortNumber);
    saiServerAddress.sin_addr.s_addr = INADDR_ANY;// serv_addr.s_addr;

    if (bind(sockfd, (const sockaddr*)&saiServerAddress, sizeof(saiServerAddress)) < 0)
    {
        int  err = WSAGetLastError();
        printf("error: %d\n", err);
        perror("ERROR binding socket");
        closesocket(sockfd);
        return NULL;
    }
    listen(sockfd, 10);

    sockaddr_in cli_addr;
    int clilen = sizeof(sockaddr_in);
    SOCKET newsockfd = accept(sockfd, (sockaddr*)&cli_addr, &clilen);

    if (newsockfd == SOCKET_ERROR)
    {
        int  err = WSAGetLastError();
        printf("error: %d\n", err);
        perror("ERROR accepting socket");
        closesocket(sockfd);
        return NULL;
    }

    printf("TCP client accept successful\n");

    return newsockfd;
}

int Listener(const char* bindAddr, u_short aPortNumber, const char* certThumbprint, void(*clientFunction)(SChannelSocket* sck))
{
    SOCKET newsockfd = ListenForAndAcceptClient(bindAddr, aPortNumber);
    
    if(newsockfd == NULL)
    {
        return 1;
    }

    SSLAuth::SetSSLThumbprints("MY", certThumbprint, certThumbprint, true, true);
    std::unique_ptr<SChannelSocket> sslsock(new SChannelSocket(newsockfd, "contoso", true));

    int ret = sslsock->Accept();
    if (ret == NO_ERROR)
    {
        printf("SSL auth succeeded\n");
        clientFunction(sslsock.get());

        sslsock->Close();
    }
    else
    {
        printf("SSL auth failed: %d\n", ret);
        return 1;
    }

    return 0;
}

struct SocketTestParam
{
    const char* bindAddr;
    const char* sPortNumber;
    u_short aPortNumber;
};

DWORD __stdcall ListenAndAcceptClientThreadMain(LPVOID param)
{
    SocketTestParam* p = (SocketTestParam*)param;

    ListenForAndAcceptClient(p->bindAddr, p->aPortNumber);

    return 0;
}

DWORD __stdcall ConnectClientThreadMain(LPVOID param)
{
    SocketTestParam* p = (SocketTestParam*)param;

    ConnectClient(p->bindAddr, p->aPortNumber);

    return 0;
}

DWORD __stdcall StreamSocketClientThreadMain(LPVOID param)
{
    SocketTestParam* p = (SocketTestParam*)param;

    StreamSocket* clientSocket = StreamSocket::CreateStreamSocket();
    clientSocket->Connect(p->bindAddr, p->sPortNumber, 5000, 5000);

    return 0;
}

int StreamSocketClientTimeoutTest(const char* bindAddr, const char* portNumber, const char* certThumbprint)
{
    SocketTestParam SocketTestParam;
    SocketTestParam.bindAddr = bindAddr;
    SocketTestParam.sPortNumber = portNumber;
    SocketTestParam.aPortNumber = (u_short)atoi(portNumber);

    SSLAuth::SetSSLThumbprints("MY", certThumbprint, certThumbprint, true, true);

    // spawn misbehaving server which accepts TCP client but doesn't complete auth
    HANDLE threadHandle = CreateThread(NULL, 0, ListenAndAcceptClientThreadMain, (LPVOID)&SocketTestParam, 0, NULL);
    if (threadHandle == NULL)
    {
        printf("ListenAndAcceptClient thread create failed %d\n", GetLastError());
        return 1;
    }

    StreamSocket* streamSocket = StreamSocket::CreateStreamSocket();

    std::clock_t connectStart = std::clock();

    printf("Testing that SSL auth timeouts from client if server does not respond\n");

    int ret = streamSocket->Connect(bindAddr, portNumber, 5000, 5000);

    double connectTime = (std::clock() - connectStart) / (double)CLOCKS_PER_SEC;

    printf("Connect result: %d time: %f seconds", ret, connectTime);

    WaitForSingleObject(threadHandle, INFINITE);

    if(ret != WSAETIMEDOUT)
    {
        perror("Did not receive timeout error as expected");
        return 1;
    }

    return 0;
}

int StreamSocketServerAcceptTimeoutTest(const char* bindAddr, const char* portNumber, const char* certThumbprint)
{
    SocketTestParam socketTestParam;
    socketTestParam.bindAddr = bindAddr;
    socketTestParam.aPortNumber = (u_short)atoi(portNumber);
    socketTestParam.sPortNumber = portNumber;

    SSLAuth::SetSSLThumbprints("MY", certThumbprint, certThumbprint, true, true);

    StreamSocket* listenSocket = StreamSocket::CreateStreamSocket();
    int ret = listenSocket->BindAndListen(socketTestParam.aPortNumber, 1024, 120, 1);

    if (ret != NO_ERROR)
    {
        printf("Unable to bind socket for listening: %d\n", ret);
        return 1;
    }

    // spawn misbehaving client which doesn't complete auth
    HANDLE threadHandle = CreateThread(NULL, 0, ConnectClientThreadMain, (LPVOID)&socketTestParam, 0, NULL);
    if (threadHandle == NULL)
    {
        printf("ConnectClient thread create failed %d\n", GetLastError());
        return 1;
    }

    StreamSocket* clientSocket = StreamSocket::CreateStreamSocket();
    std::clock_t connectStart = std::clock();

    ret = listenSocket->Accept(clientSocket, 5000, 5000);

    double acceptTime = (std::clock() - connectStart) / (double)CLOCKS_PER_SEC;

    printf("Accept result: %d time: %f seconds", ret, acceptTime);

    if(ret != WSAETIMEDOUT)
    {
        perror("Did not receive timeout error as expected");
        return 1;
    }

    WaitForSingleObject(threadHandle, INFINITE);
    
    // make sure we can accept legitimate clients now still
    threadHandle = CreateThread(NULL, 0, StreamSocketClientThreadMain, (LPVOID)&socketTestParam, 0, NULL);
    if (threadHandle == NULL)
    {
        printf("StreamSocketClient thread create failed %d\n", GetLastError());
        return 1;
    }

    ret = listenSocket->Accept(clientSocket, 5000, 5000);
    if(ret != NO_ERROR)
    {
        perror("Unable to accept legitimate client");
        printf("Accept of legimate client failed. %d\n", ret);
        return 1;
    }

    printf("Successfully accepted new client\n");

    WaitForSingleObject(threadHandle, INFINITE);

    return 0;
}

void WriteSomething(SChannelSocket *sck, char *str)
{
    sck->Write(str, (int)strlen(str) + 1);
    printf("Writing %s done.\n", str);
}

void ReadSomething(SChannelSocket *sck)
{
    char buffer[1024];
    int read = sck->Read(buffer, 1024);
    if (read != SOCKET_ERROR)
    {
        buffer[read] = 0;
        printf("Buffer read: %d. %s\n", read, (char*)buffer);
    }
    else
    {
        printf("Unable to read\n");
    }
}

struct ThreadParam
{
    char * str;
    SChannelSocket *sck;
};

ThreadParam p1 = { "Client" };
ThreadParam p2 = { "Server" };

DWORD __stdcall SendThreadMain(LPVOID param)
{
    ThreadParam * p = (ThreadParam*)param;
    do
    {
        {
            std::unique_ptr<int[]> dataBuffer(new int[1024 * 1024 + 1]);
            for (int i = 0; i < 1024 * 1024 + 1; i++)
            {
                dataBuffer[i] = -1 - i;
            }

            if (p->sck->Write((char*)dataBuffer.get(), (sizeof(int) * 1024 * 1024) - 1) == SOCKET_ERROR)
            {
                break;
            }
            printf("Completed send 4M-1\n");

            if (p->sck->Write((char*)dataBuffer.get(), (sizeof(int) * 1024 * 1024) ) == SOCKET_ERROR)
            {
                break;
            }
            printf("Completed send 4M\n");

            if (p->sck->Write((char*)dataBuffer.get(), (sizeof(int) * 1024 * 1024) + 1) == SOCKET_ERROR)
            {
                break;
            }
            printf("Completed send 4M+1\n");
        }


        int len = (int)strlen(p->str) + 1;

        int count = 0;
        while (p->sck->Write(p->str, len) != SOCKET_ERROR)
        {
            count++;
            if (count % 100000 == 0)
            {
                printf("Completed sends %d\n", count);
            }
        }
    } while (false);

    printf("send thread exiting\n");
    return 0;
}

int ReadExactly(SChannelSocket *sck, char *buff, int len)
{
    int dataRead = 0;
    while (dataRead < len)
    {
        int status = sck->Read(buff + dataRead, len - dataRead);
        if (status == SOCKET_ERROR)
        {
            return status;
        }
        dataRead += status;
    }

    return dataRead;
}

DWORD __stdcall RecvThreadMain(LPVOID param)
{
    ThreadParam * p = (ThreadParam*)param;

    do
    {
        {
            std::unique_ptr<int[]> dataBuffer(new int[1024 * 1024 + 1]);
            for (int i = 0; i < 1024 * 1024 + 1; i++)
            {
                dataBuffer[i] = -1 - i;
            }

            std::unique_ptr<int[]> dataBuffer2(new int[1024 * 1024 + 1]);
            if (ReadExactly(p->sck, (char*)dataBuffer2.get(), (sizeof(int) * 1024 * 1024) - 1) == SOCKET_ERROR)
            {
                break;
            }
            if (memcmp((LPVOID)dataBuffer.get(), (LPVOID)dataBuffer2.get(), (sizeof(int) * 1024 * 1024) - 1) != 0)
            {
                printf("Validation failed\n");
                break;
            }
            printf("Completed recv 4M - 1\n");

            if (ReadExactly(p->sck, (char*)dataBuffer2.get(), (sizeof(int) * 1024 * 1024)) == SOCKET_ERROR)
            {
                break;
            }
            if (memcmp((LPVOID)dataBuffer.get(), (LPVOID)dataBuffer2.get(), (sizeof(int) * 1024 * 1024)) != 0)
            {
                printf("Validation failed\n");
                break;
            }
            printf("Completed recv 4M\n");

            if (ReadExactly(p->sck, (char*)dataBuffer2.get(), (sizeof(int) * 1024 * 1024) + 1) == SOCKET_ERROR)
            {
                break;
            }
            if (memcmp((LPVOID)dataBuffer.get(), (LPVOID)dataBuffer2.get(), (sizeof(int) * 1024 * 1024) + 1) != 0)
            {
                printf("Validation failed\n");
                break;
            }
            printf("Completed recv 4M+1\n");
        }

        int len = (int)strlen(p->str) + 1;

        int count = 0;
        int dataSize;
        char buffer[1024];
        while ((dataSize = p->sck->Read(buffer, 1024)) != SOCKET_ERROR)
        {
            if (len != dataSize || memcmp(buffer, p->str, len) != 0)
            {
                printf("Mismatch\n");
                while (1)
                    Sleep(10);
            }
            memset(buffer, 0, dataSize);

            count++;
            if (count % 100000 == 0)
            {
                printf("Completed recv %d\n", count);
            }
        }
    }while (false);

    printf("recv thread exiting\n");

    return 0;
}

void ClientOperation(SChannelSocket *sck)
{
    p1.sck = sck;
    p2.sck = sck;
    
    HANDLE threadHandles[2];
    threadHandles[0] = CreateThread(NULL, 0, SendThreadMain, (LPVOID)&p1, 0, NULL);
    if (NULL == threadHandles[0])
    {
        printf("Send thread create failed %d\n", GetLastError());
    }

    threadHandles[1] = CreateThread(NULL, 0, RecvThreadMain, (LPVOID)&p2, 0, NULL);
    if (NULL == threadHandles[1])
    {
        printf("Recv thread create failed %d\n", GetLastError());
    }

    WaitForMultipleObjects(2, threadHandles, TRUE, INFINITE);
}

void ServerOperation(SChannelSocket *sck)
{
    p1.sck = sck;
    p2.sck = sck;

    HANDLE threadHandles[2];
    threadHandles[0] = CreateThread(NULL, 0, SendThreadMain, (LPVOID)&p2, 0, NULL);
    if (NULL == threadHandles[0])
    {
        printf("Send thread create failed %d\n", GetLastError());
    }

    threadHandles[1] = CreateThread(NULL, 0, RecvThreadMain, (LPVOID)&p1, 0, NULL);
    if (NULL == threadHandles[1])
    {
        printf("Recv thread create failed %d\n", GetLastError());
    }

    WaitForMultipleObjects(2, threadHandles, TRUE, INFINITE);
}

void PrintUsage()
{
    printf("Client Usage: rslssltest c ServerAddress ServerPort CertThumbprint\n");
    printf("Server Usage: rslssltest s ServerPort CertThumbprint\n");
    printf("Server Usage: rslssltest clienttimeouttest ServerPort CertThumbprint\n");
    printf("Server Usage: rslssltest servertimeouttest ServerPort CertThumbprint\n");
}

int main(int argc, char* argv[])
{
    WORD wVersionRequested;
    WSADATA wsaData;
    int err = 0;

    memset((LPVOID)&wsaData, 0, sizeof(wsaData));

    /* Use the MAKEWORD(lowbyte, highbyte) macro declared in Windef.h */
    wVersionRequested = MAKEWORD(2, 2);

    err = WSAStartup(wVersionRequested, &wsaData);
    if (err != 0)
    {
        printf("WSAStartup failed with error: %d\n", err);
    }


    if (argc < 2)
    {
        PrintUsage();
        return 1;
    }
    if (strcmp(argv[1], "c") == 0)
    {
        if (argc < 5)
        {
            PrintUsage();
            return 1;
        }

        return Client(argv[2], (u_short)atoi(argv[3]), argv[4], ClientOperation);
    }
    else if (strcmp(argv[1], "s") == 0)
    {
        if (argc < 4)
        {
            PrintUsage();
            return 1;
        }

        return Listener("127.0.0.1", (u_short)atoi(argv[2]), argv[3], ServerOperation);
    }
    else if (strcmp(argv[1], "clienttimeouttest") == 0)
    {
        if (argc < 4)
        {
            PrintUsage();
            return 1;
        }

        return StreamSocketClientTimeoutTest("127.0.0.1", argv[2], argv[3]);
    }
    else if (strcmp(argv[1], "servertimeouttest") == 0)
    {
        if (argc < 4)
        {
            PrintUsage();
            return 1;
        }

        return StreamSocketServerAcceptTimeoutTest("127.0.0.1", argv[2], argv[3]);
    }
    else
    {
        PrintUsage();
        return 1;
    }
}
