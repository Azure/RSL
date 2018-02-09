// sslplumbing.cpp : Defines the entry point for the console application.
//
#pragma warning(disable:4512)
#include "StreamIO.h"
#include <ws2tcpip.h>
#include <string>
using namespace std;
using namespace RSLibImpl;

typedef unsigned char byte;

namespace SslPlumbing
{
    SChannelSocket::SChannelSocket(SOCKET socket, LPCSTR remoteEndpointName, bool isServer)
        : SSLAuth(), m_bufferedSocket(socket)
    {
        m_isServer = isServer;

        if (NULL != remoteEndpointName)
        {
            m_remoteEndpointName = remoteEndpointName;
        }
    }

    SChannelSocket::~SChannelSocket()
    {
        Close();
    }

    HRESULT SChannelSocket::AuthenticateChannel(bool authServer)
    {
        SEC_CHAR* validateUsingName = NULL;

        if (authServer && m_remoteEndpointName.length() != 0)
        {
            validateUsingName = (SEC_CHAR*)m_remoteEndpointName.c_str(); // Let the system validate the endpoint name
        }

        SECURITY_STATUS secStatus = SEC_I_CONTINUE_NEEDED;

        int originalOutBufferLength = 10240;
        BYTE outputbuffer[10240];

        while (secStatus == SEC_I_CONTINUE_NEEDED || secStatus == SEC_E_INCOMPLETE_MESSAGE || secStatus == SEC_I_INCOMPLETE_CREDENTIALS)
        {
            if (!authServer || m_ctxtHandleInited)
            {
                if (m_bufferedSocket.GetReadBufferDataLength() == 0 || SEC_E_INCOMPLETE_MESSAGE == secStatus)
                {
                    // No data or data is insufficient. Read more
                    HRESULT hr = m_bufferedSocket.Recv();
                    if (FAILED(hr))
                    {
                        return hr;
                    }
                }
            }

            int bufferLength;
            LPVOID inData = m_bufferedSocket.PeekReadBufferData(&bufferLength);
            OutputType outType;

            int outBufferLength = originalOutBufferLength;
            secStatus = Authenticate(authServer, validateUsingName, &bufferLength, inData, &outBufferLength, outputbuffer, &outType);

            if (secStatus == HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER))
            {
                //printf("%p resize needed: %s\n", this, amIServer ? "srv" : "clt");
                return secStatus;
            }

            if (SEC_E_INCOMPLETE_MESSAGE != secStatus)
            {
                m_bufferedSocket.SkipBytes(bufferLength);
            }

            if (secStatus == SEC_E_OK || secStatus == SEC_I_CONTINUE_NEEDED)
            {
                if (outBufferLength != 0)
                {
                    // SSPI created token to be sent to client
                    HRESULT hr = m_bufferedSocket.Send(outputbuffer, outBufferLength);
                    if (FAILED(hr))
                    {
                        return hr;
                    }
                }
            }
            else if (secStatus == SEC_E_INCOMPLETE_MESSAGE)
            {
            }
            else if (secStatus == SEC_E_INCOMPLETE_CREDENTIALS)
            {
            }
            else if (FAILED(secStatus))
            {
                return secStatus;
            }
        }

        return secStatus;
    }

    HRESULT SChannelSocket::RunSSPILoop(int *, LPVOID , int *, LPVOID , OutputType *)
    {
        return E_FAIL;
    }

    HRESULT SChannelSocket::ConnectInternal()
    {
        if (!SSLAuth::InitializeSChannel())
        {
            return E_FAIL;
        }

        HRESULT hr = AuthenticateChannel(!m_isServer);

        if (SUCCEEDED(hr))
        {
            m_maxDataSizeEncryptable = sizeof(m_secStreamSizes) - m_secStreamSizes.cbHeader - m_secStreamSizes.cbTrailer;
            if ((ULONG)m_maxDataSizeEncryptable > m_secStreamSizes.cbMaximumMessage)
            {
                m_maxDataSizeEncryptable = (int)m_secStreamSizes.cbMaximumMessage;
            }
        }

        return hr;
    }

    DWORD SChannelSocket::Connect() 
    {
        while (!AuthCompleted())
        {
            HRESULT hr = ConnectInternal();
            if (FAILED(hr))
            {
                return HRESULT_CODE(hr);
            }
        }

        return NO_ERROR;
    }

    DWORD SChannelSocket::Accept()
    {
        while (!AuthCompleted())
        {
            HRESULT hr = ConnectInternal();
            if (FAILED(hr))
            {
                return HRESULT_CODE(hr);
            }
        }

        return NO_ERROR;
    }

    HRESULT SChannelSocket::WriteChunk(const char *buffer, int Len)
    {
        SecBufferDesc   inputBufferDesc;
        SecBuffer       inputBuffers[4];
        SECURITY_STATUS secStatus;

        //
        // Initialize security buffer structs
        //

        inputBufferDesc.ulVersion = SECBUFFER_VERSION;
        inputBufferDesc.cBuffers = 4;
        inputBufferDesc.pBuffers = inputBuffers;

        inputBuffers[0].BufferType = SECBUFFER_EMPTY;
        inputBuffers[1].BufferType = SECBUFFER_EMPTY;
        inputBuffers[2].BufferType = SECBUFFER_EMPTY;
        inputBuffers[3].BufferType = SECBUFFER_EMPTY;

        // Put the message in the right place in the buffer
        errno_t error = memcpy_s(m_encryptDataBuffer + m_secStreamSizes.cbHeader, sizeof(m_encryptDataBuffer) - m_secStreamSizes.cbHeader - m_secStreamSizes.cbTrailer, (LPVOID)buffer, Len);
        if (0 != error)
        {
            return HRESULT_FROM_WIN32(error);
        }

        // Fillup the input buffer's header, data and trailor
        inputBuffers[0].pvBuffer = m_encryptDataBuffer;
        inputBuffers[0].cbBuffer = m_secStreamSizes.cbHeader;
        inputBuffers[0].BufferType = SECBUFFER_STREAM_HEADER;

        inputBuffers[1].pvBuffer = m_encryptDataBuffer + m_secStreamSizes.cbHeader;
        inputBuffers[1].cbBuffer = Len;
        inputBuffers[1].BufferType = SECBUFFER_DATA;

        inputBuffers[2].pvBuffer = m_encryptDataBuffer + m_secStreamSizes.cbHeader + Len;
        inputBuffers[2].cbBuffer = m_secStreamSizes.cbTrailer;
        inputBuffers[2].BufferType = SECBUFFER_STREAM_TRAILER;

        inputBuffers[3].BufferType = SECBUFFER_EMPTY;
        inputBuffers[3].cbBuffer = 0;

        secStatus = s_pSSPIfnTbl->EncryptMessage(&m_ctxtHandle, 0, &inputBufferDesc, 0);

        if (secStatus != SEC_E_OK)
        {
            return HRESULT_FROM_WIN32(secStatus);
        }

        HRESULT hr = m_bufferedSocket.Send(m_encryptDataBuffer, inputBuffers[0].cbBuffer + inputBuffers[1].cbBuffer + inputBuffers[2].cbBuffer);

        if (FAILED(hr))
        {
            return hr;
        }

        return S_OK;
    }

    int SChannelSocket::Write(const char *buffer, int bLen)
    {
        if (!IsValid())
        {
            WSASetLastError(ERROR_INVALID_HANDLE);
            return SOCKET_ERROR;
        }

        AutoCriticalSection autoCs(&m_csSend);

        int dataWritten = 0;

        while (dataWritten < bLen)
        {
            int sizeToWrite = min(bLen - dataWritten, m_maxDataSizeEncryptable);
            
            HRESULT hr = WriteChunk(buffer + dataWritten, sizeToWrite);

            if (FAILED(hr))
            {
                WSASetLastError(HRESULT_CODE(hr));
                return SOCKET_ERROR;
            }

            dataWritten += sizeToWrite;
        }

        return dataWritten;
    }

    HRESULT SChannelSocket::RecvAndDecrypt()
    {
        SecBufferDesc   inputBufferDesc;
        SecBuffer       inputBuffers[4];
        SECURITY_STATUS secStatus;

        inputBufferDesc.ulVersion = SECBUFFER_VERSION;
        inputBufferDesc.cBuffers = 4;
        inputBufferDesc.pBuffers = inputBuffers;

        int bufferLength = 0;

        secStatus = SEC_E_OK;
        do
        {
            if (SEC_E_INCOMPLETE_MESSAGE == secStatus || m_bufferedSocket.GetReadBufferDataLength() == 0)
            {
                // don't have data or data is not enough
                HRESULT hr = m_bufferedSocket.Recv();
                if (FAILED(hr))
                {
                    return hr;
                }
            }

            // init input buffer
            inputBuffers[0].pvBuffer = m_bufferedSocket.PeekReadBufferData(&bufferLength);
            inputBuffers[0].cbBuffer = bufferLength;
            inputBuffers[0].BufferType = SECBUFFER_DATA;

            inputBuffers[1].BufferType = SECBUFFER_EMPTY;
            inputBuffers[1].cbBuffer = 0;
            inputBuffers[1].pvBuffer = NULL;

            inputBuffers[2].BufferType = SECBUFFER_EMPTY;
            inputBuffers[2].cbBuffer = 0;
            inputBuffers[2].pvBuffer = NULL;

            inputBuffers[3].BufferType = SECBUFFER_EMPTY;
            inputBuffers[3].cbBuffer = 0;
            inputBuffers[3].pvBuffer = NULL;

            secStatus = s_pSSPIfnTbl->DecryptMessage(&m_ctxtHandle, &inputBufferDesc, 0, NULL);
        } while (secStatus == SEC_E_INCOMPLETE_MESSAGE);

        // Get extra data and adjust input
        PSecBuffer pExtraDataBuffer = NULL;

        for (int i = 1; i < 4; i++)
        {
            if (inputBuffers[i].BufferType == SECBUFFER_EXTRA)
            {
                pExtraDataBuffer = &inputBuffers[i];
                break;
            }
        }

        int dataProcessed = bufferLength;
        if (NULL != pExtraDataBuffer)
        {
            dataProcessed -= pExtraDataBuffer->cbBuffer;
        }
        m_bufferedSocket.SkipBytes(dataProcessed);

        if (secStatus != SEC_E_OK)
        {
            return HRESULT_FROM_WIN32(secStatus);
        }

        // Find the decrypted data
        PSecBuffer pDataBuffer = NULL;

        for (int i = 1; i < 4; i++)
        {
            if (inputBuffers[i].BufferType == SECBUFFER_DATA)
            {
                pDataBuffer = &inputBuffers[i];
                break;
            }
        }

        if (!pDataBuffer)
        {
            return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
        }

        // Move the decrypted data to the buffer
        return m_bufferedDecryptedMessage.AddData((LPBYTE)pDataBuffer->pvBuffer, pDataBuffer->cbBuffer);
    }

    DWORD SChannelSocket::Close()
    {
        AutoCriticalSection autoCs(&m_csSend);
        return SSLAuth::Close();
    }

    int SChannelSocket::Read(char *buf, int maxLen)
    {
        if (!IsValid())
        {
            WSASetLastError(ERROR_INVALID_HANDLE);
            return SOCKET_ERROR;
        }

        AutoCriticalSection autoCs(&m_csRecv);

        while (true)
        {
            LPBYTE pbData = m_bufferedDecryptedMessage.GetData(&maxLen);
            if (NULL != pbData)
            {
                memcpy(buf, pbData, maxLen);

                return maxLen;
            }

            HRESULT hr = RecvAndDecrypt();
            if (FAILED(hr))
            {
                WSASetLastError(HRESULT_CODE(hr));

                if(HRESULT_FROM_WIN32(ERROR_HANDLE_EOF) == hr)
                {
                    return 0;
                }
                else
                {
                    return SOCKET_ERROR;
                }
            }
        }
    }
}
