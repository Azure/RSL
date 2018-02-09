#pragma once
#include <windows.h>
#define SECURITY_WIN32
#include <security.h>
#include <Schannel.h>
#include <memory>
#include "ScopeGuard.h"
#include <string>

#define SSLDebug(title, ...)   Log (LogID_Netlib, LogLevel_Debug, title, __VA_ARGS__)
#define SSLInfo(title, ...)    Log (LogID_Netlib, LogLevel_Info, title, __VA_ARGS__)
#define SSLWarning(title, ...) Log (LogID_Netlib, LogLevel_Warning, title, __VA_ARGS__)
#define SSLError(title, ...)   Log (LogID_Netlib, LogLevel_Error, title, __VA_ARGS__)
#define SSLAlert(title, ...)   Log (LogID_Netlib, LogLevel_Alert, title, __VA_ARGS__)


namespace RSLibImpl
{

    class ISSLAuth
    {
    public:
        enum OutputType { AuthData, AuthDataEnd, DecryptedData };
        virtual DWORD ProcessData(int *cbInDataLen, LPVOID inData, int *cbOutDataLen, LPVOID outData, OutputType *outType) = 0;
        virtual DWORD EncryptData(int cbInDataLen, LPVOID inData, int *cbOutDataLen, LPVOID outData) = 0;
        virtual bool  AuthCompleted() = 0;
        virtual bool  IsCertCAValidationPassed() = 0;
        virtual ~ISSLAuth() {};
    };


#define SHA1_HASH_SIZE 20
#define PBYTE BYTE*

    class SSLAuth : public ISSLAuth
    {
    protected:
        bool m_bAuthCompleted;
        bool m_bCertCAValidationPassed; /* Remove it after CA validation fixed */

        CtxtHandle m_ctxtHandle;
        bool m_ctxtHandleInited;
        bool m_ctxtHandleValid;
        bool m_connectionClosed;

        SecPkgContext_StreamSizes m_secStreamSizes;

        virtual HRESULT RunSSPILoop(int *cbInDataLen, LPVOID inData, int *cbOutDataLen, LPVOID outData, OutputType *outType) = 0;
        HRESULT DecryptData(int *cbInDataLen, LPVOID inData, int *cbOutDataLen, LPVOID outData, OutputType *outType);
        HRESULT ValidateCertificateAndGetContextAttributes(bool authServer);

        HRESULT Authenticate(bool authServer, SEC_CHAR* validateUsingName, int *cbInDataLen, LPVOID inData, int *cbOutDataLen, LPVOID outData, OutputType *outType);

        // static
        enum SChannelInitializedState { NotInitialized, Initializing, Initialized };
        static volatile LONG s_sChannelInitialized;
        static PSecurityFunctionTable s_pSSPIfnTbl;
        static CredHandle s_scClientCredHandle;
        static CredHandle s_scServerCredHandle;
        static bool s_scClientCredHandleValid;
        static bool s_scServerCredHandleValid;
        static std::string s_store;
        static bool s_bSslEnabled;
        static BYTE s_pThumbPrintA[SHA1_HASH_SIZE];
        static BYTE s_pThumbPrintB[SHA1_HASH_SIZE];
        static bool s_bThumbPrintAValid;
        static bool s_bThumbPrintBValid;
        static std::string s_pThumbPrintsParentA;
        static std::string s_pThumbPrintsParentB;
        static std::string s_pSubjectA;
        static std::string s_pSubjectB;
        static bool s_bSubjectAValid;
        static bool s_bSubjectBValid;
        static bool s_bValidateCAChain;
        static bool s_bCheckCertificateRevocation;
        static bool s_considerIdentitiesWhitelist;

        static BOOL InitializeSChannel();
        static BYTE HexToDec(char ch);
        static CHAR QuadToChar(BYTE ch);
        static DWORD ConvertStringThumbprintToByteArray(LPCSTR thumbprint, BYTE *pThumbBytes);
        static std::string HexToString(BYTE pThumbBytes[SHA1_HASH_SIZE]);
        static HRESULT ConvertStringThumbprintsToArrayOfByteArray(std::string thumbprintSequence, PBYTE **arrayOfThumbprints, int *num);

        static DWORD GetCredential(LPCSTR store);
        static HRESULT GetCertificateThumbprint(PCCERT_CONTEXT pCertContext, BYTE pbSha1Hash[SHA1_HASH_SIZE]);
        static HRESULT GetCertificateSubject(PCCERT_CONTEXT pCertContext, bool authServer, std::string &subject, PCCERT_CONTEXT &pParentCertContext);
        static PCCERT_CONTEXT GetCertificate(LPCSTR store, BYTE *pThumbPrint);
        static DWORD CreateCertificateCredential(PCCERT_CONTEXT pCertCtx);
        static HRESULT IsCertificateTrusted(PCCERT_CONTEXT pCertContext, bool authServer);
        static HRESULT IsCertificateThumbprintAcceptable(PCCERT_CONTEXT pCertContext, LPVOID pThumbprints[], int numThumbprints, std::string* debugMsg);
        static HRESULT IsCertificateSubjectAcceptable(PCCERT_CONTEXT pCertContext, bool authServer, std::string* debugMsg);

    public:
        SSLAuth();
        ~SSLAuth();
        bool IsValid() { return m_ctxtHandleValid && !m_connectionClosed; }

        virtual DWORD Close();
        virtual DWORD ProcessData(int *cbInDataLen, LPVOID inData, int *cbOutDataLen, LPVOID outData, OutputType *outType);
        virtual DWORD EncryptData(int cbInDataLen, LPVOID inData, int *cbOutDataLen, LPVOID outData);
        virtual bool AuthCompleted();
        virtual bool IsCertCAValidationPassed();

        //static
        static bool HasAnyThumbprint();
        static bool IsSSLEnabled();
        static DWORD SetSSLThumbprints(LPCSTR store, LPCSTR thumbPrintA, LPCSTR thumbPrintB, bool validateCAChain, bool checkCertificateRevocation);
        static DWORD SetSSLSubjectNames(LPCSTR subjectA, LPCSTR thumbPrintsParentA, LPCSTR subjectB, LPCSTR thumbPrintsParentB, bool considerIdentitiesWhitelist);
    };

    // Used by TLS clients to authenticate the server
    class SSLAuthServer : public SSLAuth
    {
    protected:
        std::string m_remoteEndpointName;
        virtual HRESULT RunSSPILoop(int *cbInDataLen, LPVOID inData, int *cbOutDataLen, LPVOID outData, OutputType *outType);

    public:
        SSLAuthServer(LPCTSTR authEndPointName = NULL);
    };

    // Used by TLS servers to authenticate the client
    class SSLAuthClient : public SSLAuth
    {
    protected:
        virtual HRESULT RunSSPILoop(int *cbInDataLen, LPVOID inData, int *cbOutDataLen, LPVOID outData, OutputType *outType);
    };


#ifdef _DEBUG
    const int ClientHello = 59;
    const int ServerHello = 63;
    const int ClientAuth  = 73;
    const int ServerAuth = 85;

    class FakeSSLClientAuth : public ISSLAuth
    {
        enum SslClientState { NotInit, ClientHelloReceived, ClientAuthCompleted };
        SslClientState m_clientState;

    public:
    FakeSSLClientAuth() : m_clientState(NotInit) {}
        ~FakeSSLClientAuth() {}


        DWORD ProcessData(int *cbInDataLen, LPVOID inData, int *cbOutDataLen, LPVOID outData, OutputType *outType)
        {
            //        int outDataOrigSize = *cbOutDataLen;
            *cbOutDataLen = 0;
            *outType = AuthData;

            switch(m_clientState)
                {
                case NotInit:
                    // Need client hello
                    if(*cbInDataLen < sizeof(int))
                        {
                            return ERROR_INSUFFICIENT_BUFFER;
                        }

                    if(*((int*)inData) == ClientHello)
                        {
                            m_clientState = ClientHelloReceived;
                            *((int*)outData) = ServerHello;
                            *cbOutDataLen = sizeof(int);

                            *cbInDataLen = sizeof(int);
                            return ERROR_SUCCESS;
                        }
                    else
                        {
                            return ERROR_INVALID_PARAMETER;
                        }

                    break;

                case ClientHelloReceived:
                    if(*cbInDataLen < sizeof(int))
                        {
                            return ERROR_INSUFFICIENT_BUFFER;
                        }

                    if(*((int*)inData) == ClientAuth)
                        {
                            m_clientState = ClientAuthCompleted;
                            *((int*)outData) = ServerAuth;
                            *cbOutDataLen = sizeof(int);

                            *cbInDataLen = sizeof(int);
                            return ERROR_SUCCESS;
                        }
                    else
                        {
                            return ERROR_INVALID_PARAMETER;
                        }
                    break;

                case ClientAuthCompleted:
                    {
                        if(*cbInDataLen < sizeof(int))
                            {
                                return ERROR_INSUFFICIENT_BUFFER;
                            }

                        int encryptedDataSize = *((int*)inData);
                        if(encryptedDataSize + (int)sizeof(int) > *cbInDataLen)
                            {
                                return ERROR_INSUFFICIENT_BUFFER;
                            }

                        memcpy(outData, (LPVOID)((LPBYTE)inData + sizeof(int)), encryptedDataSize);
                        *cbOutDataLen = encryptedDataSize;

                        *cbInDataLen = encryptedDataSize + sizeof(int);

                        *outType = DecryptedData;

                        return ERROR_SUCCESS;
                    }

                default:
                    return ERROR_INVALID_PARAMETER; // Should not call here
                    break;
                }
        }

        virtual DWORD EncryptData(int cbInDataLen, LPVOID inData, int *cbOutDataLen, LPVOID outData)
        {
            switch(m_clientState)
                {
                case ClientAuthCompleted:
                    {
                        *((int*)outData) = cbInDataLen;
                        memcpy( (LPVOID)((LPBYTE)outData + sizeof(int)), inData, cbInDataLen);
                        *cbOutDataLen = cbInDataLen + sizeof(int);
                        return ERROR_SUCCESS;
                    }

                default:
                    return ERROR_INVALID_PARAMETER; // Should not call here
                    break;
                }
        }


        virtual bool  AuthCompleted()
        {
            return m_clientState == ClientAuthCompleted;
        }
    };

    class FakeSSLServerAuth : public ISSLAuth
    {
        enum SslServerState { NotInit, ClientHelloSent, ServerHelloReceived, ServerAuthCompleted };
        SslServerState m_serverState;

    public:
    FakeSSLServerAuth() : m_serverState(NotInit) {}
        ~FakeSSLServerAuth() {}

        DWORD ProcessData(int *cbInDataLen, LPVOID inData, int *cbOutDataLen, LPVOID outData, OutputType *outType)
        {
            //        int outDataOrigSize = *cbOutDataLen;
            *cbOutDataLen = 0;
            *outType = AuthData;

            switch(m_serverState)
                {
                case NotInit:
                    // Send client hello

                    m_serverState = ClientHelloSent;
                    *((int*)outData) = ClientHello;
                    *cbOutDataLen = sizeof(int);

                    *cbInDataLen = 0;
                    return ERROR_SUCCESS;

                case ClientHelloSent:
                    if(*cbInDataLen < sizeof(int))
                        {
                            return ERROR_INSUFFICIENT_BUFFER;
                        }

                    if(*((int*)inData) == ServerHello)
                        {
                            m_serverState = ServerHelloReceived;
                            *((int*)outData) = ClientAuth;
                            *cbOutDataLen = sizeof(int);

                            *cbInDataLen = sizeof(int);
                            return ERROR_SUCCESS;
                        }
                    else
                        {
                            return ERROR_INVALID_PARAMETER;
                        }

                    break;


                case ServerHelloReceived:
                    if(*cbInDataLen < sizeof(int))
                        {
                            return ERROR_INSUFFICIENT_BUFFER;
                        }

                    if(*((int*)inData) == ServerAuth)
                        {
                            m_serverState = ServerAuthCompleted;

                            *cbInDataLen = sizeof(int);
                            *outType = AuthDataEnd;
                            return ERROR_SUCCESS;
                        }
                    else
                        {
                            return ERROR_INVALID_PARAMETER;
                        }

                    break;

                case ServerAuthCompleted:
                    {
                        if(*cbInDataLen < sizeof(int))
                            {
                                return ERROR_INSUFFICIENT_BUFFER;
                            }

                        int encryptedDataSize = *((int*)inData);
                        if(encryptedDataSize + (int)sizeof(int) > *cbInDataLen)
                            {
                                return ERROR_INSUFFICIENT_BUFFER;
                            }

                        memcpy(outData, (LPVOID)((LPBYTE)inData + sizeof(int)), encryptedDataSize);
                        *cbOutDataLen = encryptedDataSize;
                        *cbInDataLen = encryptedDataSize + sizeof(int);

                        *outType = DecryptedData;
                        return ERROR_SUCCESS;
                    }

                default:
                    return ERROR_INVALID_PARAMETER; // Should not call here
                    break;
                }
        }


        virtual DWORD EncryptData(int cbInDataLen, LPVOID inData, int *cbOutDataLen, LPVOID outData)
        {
            switch(m_serverState)
                {
                case ServerAuthCompleted:
                    {
                        *((int*)outData) = cbInDataLen;
                        memcpy( (LPVOID)((LPBYTE)outData + sizeof(int)), inData, cbInDataLen);
                        *cbOutDataLen = cbInDataLen + sizeof(int);
                        return ERROR_SUCCESS;
                    }

                default:
                    return ERROR_INVALID_PARAMETER; // Should not call here
                    break;
                }
        }


        virtual bool  AuthCompleted()
        {
            return m_serverState == ServerAuthCompleted;
        }
    };
#endif
}
