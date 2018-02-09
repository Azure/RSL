#include <rsl.h>
#include <iostream>
#include <sstream>
#include <string>
#include <strsafe.h>
#include "sslplumbing.h"

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

int Client(const char * serverAddress, u_short aPortNumber, void(*clientFunction)(SChannelSocket* sck))
{
    SOCKET sockfd = socket(PF_INET, SOCK_STREAM, 0);
    if (sockfd == SOCKET_ERROR)
    {
        perror("ERROR creating socket");
        //Log and Error
        return 1;
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
        return 1;
    }

    SSLAuth::SetSSLThumbprints("MY", "ff4b8dc4dd64c15293ec810975fc91ebd00dc06f", "f0ba8e6e05042b7b0cc8875257a1d8119213353c", true, true);
    std::unique_ptr<SChannelSocket> sslsock(new SChannelSocket(sockfd, NULL, false));

    if (sslsock->Connect())
    {
        printf("Connected\n");
        clientFunction(sslsock.get());

        sslsock->Close();
    }

    return 0;
}

int Listener(const char * bindAddr, u_short aPortNumber, void(*clientFunction)(SChannelSocket* sck))
{
    SOCKET sockfd = socket(PF_INET, SOCK_STREAM, 0);

    if (sockfd == SOCKET_ERROR)
    {
        int  err = WSAGetLastError();
        printf("error: %d\n", err);
        perror("ERROR opening socket");
        return 1;
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
        return 1;
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
        return 1;
    }

    SSLAuth::SetSSLThumbprints("MY", "f0ba8e6e05042b7b0cc8875257a1d8119213353c", "ff4b8dc4dd64c15293ec810975fc91ebd00dc06f", true, true);
    std::unique_ptr<SChannelSocket> sslsock(new SChannelSocket(newsockfd, "contoso", true));

    if (sslsock->Accept())
    {
        printf("Accepted\n");
        clientFunction(sslsock.get());

        sslsock->Close();
    }
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
        return 0;
    }
    if (strcmp(argv[1], "c") == 0)
    {
        if (argc < 3)
        {
            return 0;
        }
        Client(argv[2], (u_short)atoi(argv[3]), ClientOperation);
    }
    else if (strcmp(argv[1], "s") == 0)
    {
        if (argc < 2)
        {
            return 0;
        }

        Listener("127.0.0.1", (u_short)atoi(argv[2]), ServerOperation);
    }
    return 0;
}
