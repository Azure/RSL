#pragma once

#include "NetCxn.h"

namespace RSLibImpl
{

#define SSL_MAX_ENCRYPT_DATA_SIZE (16 * 1024)

    // DO NOT MODIFY: following macros directly unless code issue.
    // Encryption header size 21
    // Encryption trailor size 64
#define SSL_ENCRYPT_HEADER_SIZE 21
#define SSL_ENCRYPT_TRAILER_SIZE_MAX 64
#define SSL_ENCRYPT_BUFFER_SIZE (SSL_MAX_ENCRYPT_DATA_SIZE + SSL_ENCRYPT_HEADER_SIZE + SSL_ENCRYPT_TRAILER_SIZE_MAX)


    class NetSslCxn : public NetCxn
    {
    public:
        static NetCxn* CreateNetCxn(NetPacketSvc *svc, UInt32 ip, UInt16 port);
        
        virtual void Start(NetVConnection *vc);
        virtual TxRxStatus    EnqueuePacket(NetPacketCtx *ctx);

        virtual int CloseConnection(bool abort = false);

    protected:
    NetSslCxn(NetPacketSvc *svc, UInt32 ip, UInt16 port) : NetCxn(svc, ip, port), m_readSocketBuffer(SSL_ENCRYPT_BUFFER_SIZE), m_decryptedData(SSL_MAX_ENCRYPT_DATA_SIZE)
        {
            m_bIsSendingAuthData = false;
            m_serializedPktLength = m_chunkSent = 0;
            m_pSerializedPktBuffer = NULL;

#ifdef _DEBUG                
            m_bytesRecv = m_bytesSent = m_pktIssued = m_pktSent = 0; m_pktRecv = 0;
#endif
        }

        virtual void ReadReadyInternal();
        virtual int         WriteReadyInternal();

        HRESULT ProcessEncryptedBuffer(bool allowEmptyInput = false);

        void SendBufferInternal(Buffer *buffer);

        bool ProcessQueuedPacketsForWrite();

        virtual int CloseCleanup();

        TxRxStatus EnqueueAuthData(NetPoolBuffer *buffer);

        DWORD EncryptData(Buffer *pDataToSend, int startIndex, int dataLength, NetPoolBuffer **ppEncryptedData);

        bool IsSspiAuthCompleted() 
        {
            return m_pSslAuth.get() != NULL && m_pSslAuth->AuthCompleted();
        }

        
        std::unique_ptr<ISSLAuth> m_pSslAuth;

        NetPoolBuffer m_readSocketBuffer;
        NetPoolBuffer m_decryptedData;

        bool m_bIsSendingAuthData;
        Queue<NetPoolBuffer>     m_SendAuthDataq;
        Queue<NetPoolBuffer>     m_DeleteSendAuthDataq;

        int m_chunkSent;
        int m_serializedPktLength;
        Buffer *m_pSerializedPktBuffer; // If a packet is cancelled we can't still stop sending the packet and switch to another packet. In that case, client and server will be out of sync on protocol. We keep the data here to send later.

#ifdef _DEBUG
        int m_pktSent;
        int m_pktRecv;
        int m_pktIssued;
        int m_bytesSent;
        int m_bytesRecv;

        void DisplayDebugInfo(LPCTSTR str)
        {
            char buff[100];
            OutputDebugString (str); OutputDebugString("  : Conn : ");

            _i64toa_s((INT64)this,  buff, ARRAYSIZE(buff), 16);
            OutputDebugString(buff); OutputDebugString("  : Issued : ");

            _itoa_s(m_pktIssued, buff, ARRAYSIZE(buff), 10);
            OutputDebugString(buff); OutputDebugString("  : Sent : ");

            _itoa_s(m_pktSent, buff, ARRAYSIZE(buff), 10);
            OutputDebugString(buff); OutputDebugString("  : Recv : ");

            _itoa_s(m_pktRecv, buff, ARRAYSIZE(buff), 10);
            OutputDebugString(buff); OutputDebugString("\r\n");    
        }

        void DisplayStrAndInt(LPCTSTR str, int val)
        {
            char buff[100];
            OutputDebugString (str); OutputDebugString("  : Conn : ");

            _i64toa_s((INT64)this,  buff, ARRAYSIZE(buff), 16);
            OutputDebugString(buff); OutputDebugString("  : Val : ");

            _itoa_s(val, buff, ARRAYSIZE(buff), 10);
            OutputDebugString(buff); OutputDebugString("\r\n");
        }

        void DisplayOutputFromProcessData(int err, int cbInData, int cbOutData, int outType)
        {
            char buff[100];
            OutputDebugString("ProcessData output  : Conn : ");

            _i64toa_s((INT64)this,  buff, ARRAYSIZE(buff), 16);
            OutputDebugString(buff); OutputDebugString("  : err : ");


            _itoa_s(err, buff, ARRAYSIZE(buff), 10);
            OutputDebugString(buff); OutputDebugString("  : cbInData : ");

            _itoa_s(cbInData, buff, ARRAYSIZE(buff), 10);
            OutputDebugString(buff); OutputDebugString("  : cbOutData : ");

            _itoa_s(cbOutData, buff, ARRAYSIZE(buff), 10);
            OutputDebugString(buff); OutputDebugString("  : outType : ");

            _itoa_s(outType, buff, ARRAYSIZE(buff), 10);
            OutputDebugString(buff); OutputDebugString("\r\n");
        }

#endif
    };
}

