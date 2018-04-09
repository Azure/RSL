#include "NetSSLCxn.h"
#include "PacketUtil.h"
#include "NetPacket.h"
#include "DynString.h"
#include <vector>

namespace RSLibImpl
{
    volatile LONG SSLAuth::s_sChannelInitialized = NotInitialized;
    PSecurityFunctionTable SSLAuth::s_pSSPIfnTbl = NULL;

    CredHandle SSLAuth::s_scClientCredHandle;
    CredHandle SSLAuth::s_scServerCredHandle;
    bool SSLAuth::s_scClientCredHandleValid = false;
    bool SSLAuth::s_scServerCredHandleValid = false;
    std::string SSLAuth::s_store;
    bool SSLAuth::s_bSslEnabled = false;
    BYTE SSLAuth::s_pThumbPrintA[SHA1_HASH_SIZE];
    BYTE SSLAuth::s_pThumbPrintB[SHA1_HASH_SIZE];
    bool SSLAuth::s_bThumbPrintAValid = false;
    bool SSLAuth::s_bThumbPrintBValid = false;
    std::string SSLAuth::s_pThumbPrintsParentA;
    std::string SSLAuth::s_pThumbPrintsParentB;
    std::string SSLAuth::s_pSubjectA;
    std::string SSLAuth::s_pSubjectB;
    bool SSLAuth::s_bSubjectAValid = false;
    bool SSLAuth::s_bSubjectBValid = false;
    bool SSLAuth::s_bValidateCAChain = true;
    bool SSLAuth::s_bCheckCertificateRevocation = true;
    bool SSLAuth::s_considerIdentitiesWhitelist = true;

    SSLAuth::SSLAuth()
    {
        m_bAuthCompleted = false;
        
        ZeroMemory(&m_ctxtHandle, sizeof(m_ctxtHandle));
        ZeroMemory(&m_secStreamSizes, sizeof(m_secStreamSizes));
        m_ctxtHandleInited = false;
        m_ctxtHandleValid = false;
        m_connectionClosed = false;
        m_bCertCAValidationPassed = false;
    }

    bool SSLAuth::IsCertCAValidationPassed()
    {
        return m_bCertCAValidationPassed;
    }

    HRESULT SSLAuth::DecryptData(int *cbInDataLen, LPVOID inData, int *cbOutDataLen, LPVOID outData, OutputType *outType)
    {
        *outType = DecryptedData;

        SecBufferDesc   inputBufferDesc;
        SecBuffer       inputBuffers[4];
        SECURITY_STATUS secStatus;

        inputBufferDesc.ulVersion = SECBUFFER_VERSION;
        inputBufferDesc.cBuffers = 4;
        inputBufferDesc.pBuffers = inputBuffers;

        secStatus = SEC_E_OK;

        // init input buffer
        inputBuffers[0].pvBuffer = inData;
        inputBuffers[0].cbBuffer = *cbInDataLen;
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

        if (NULL != pExtraDataBuffer)
        {
            *cbInDataLen -= pExtraDataBuffer->cbBuffer;
        }

        if (SEC_E_INCOMPLETE_MESSAGE == secStatus)
        {
            *cbOutDataLen = 0; // No data as input is not complete
            return SEC_E_INCOMPLETE_MESSAGE;
        }

        if (secStatus != SEC_E_OK)
        {
            SSLError("SSPI decrypt failed",
                LogTag_StatusCode, secStatus);

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

        if ((UINT32)*cbOutDataLen < pDataBuffer->cbBuffer)
        {
            return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
        }

        memcpy_s(outData, *cbOutDataLen, (LPVOID)pDataBuffer->pvBuffer, pDataBuffer->cbBuffer);
        *cbOutDataLen = pDataBuffer->cbBuffer;

        return S_OK;
    }

    DWORD SSLAuth::ProcessData(int *cbInDataLen, LPVOID inData, int *cbOutDataLen, LPVOID outData, OutputType *outType)
    {
        if (!AuthCompleted())
        {
            DWORD err = RunSSPILoop(cbInDataLen, inData, cbOutDataLen, outData, outType);
            if (err != ERROR_SUCCESS && err != SEC_E_INCOMPLETE_MESSAGE)
            {
                SSLError("SSPI Loop failed", LogTag_StatusCode, err);
            }

            return err;
        }
        else
        {
            return DecryptData(cbInDataLen, inData, cbOutDataLen, outData, outType);
        }
    }
    
    DWORD SSLAuth::EncryptData(int cbInDataLen, LPVOID inData, int *cbOutDataLen, LPVOID outData)
    {
        if(!AuthCompleted())
        {
            return ERROR_INVALID_HANDLE;
        }

        SecBufferDesc    inputBufferDesc;
        SecBuffer        inputBuffers[4];
        SECURITY_STATUS  secStatus;
        
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
        errno_t error = memcpy_s((LPBYTE)outData + m_secStreamSizes.cbHeader, *cbOutDataLen - m_secStreamSizes.cbHeader - m_secStreamSizes.cbTrailer, inData, cbInDataLen);
        if (0 != error)
        {
            return HRESULT_FROM_WIN32(error);
        }
        
        // Fillup the input buffer's header, data and trailor
        inputBuffers[0].pvBuffer = outData;
        inputBuffers[0].cbBuffer = m_secStreamSizes.cbHeader;
        inputBuffers[0].BufferType = SECBUFFER_STREAM_HEADER;
        
        inputBuffers[1].pvBuffer = (LPVOID)((LPBYTE)outData + m_secStreamSizes.cbHeader);
        inputBuffers[1].cbBuffer = cbInDataLen;
        inputBuffers[1].BufferType = SECBUFFER_DATA;
        
        inputBuffers[2].pvBuffer = (LPVOID)((LPBYTE)outData + m_secStreamSizes.cbHeader + cbInDataLen);
        inputBuffers[2].cbBuffer = m_secStreamSizes.cbTrailer;
        inputBuffers[2].BufferType = SECBUFFER_STREAM_TRAILER;
        
        inputBuffers[3].BufferType = SECBUFFER_EMPTY;
        inputBuffers[3].cbBuffer = 0;
        
        secStatus = s_pSSPIfnTbl->EncryptMessage(&m_ctxtHandle, 0, &inputBufferDesc, 0);

        if (secStatus != SEC_E_OK)
        {
            SSLError("SSPI EncryptMessage failed", LogTag_StatusCode, secStatus);

            return HRESULT_FROM_WIN32(secStatus);
        }
        
        *cbOutDataLen = inputBuffers[0].cbBuffer + inputBuffers[1].cbBuffer + inputBuffers[2].cbBuffer;
        
        return S_OK;
    }
    
    bool  SSLAuth::AuthCompleted()
    {
        return m_bAuthCompleted;
    }

    SSLAuth::~SSLAuth()
    {
        Close();
    }
    
    DWORD SSLAuth::Close()
    {
        if (!m_ctxtHandleInited)
        {
            return S_OK;
        }

        if (!m_connectionClosed)
        {
            DWORD           messageType;
            SecBufferDesc   outputBufferDesc;
            SecBuffer       outputBuffer;
            SECURITY_STATUS secStatus;

            messageType = SCHANNEL_SHUTDOWN;

            outputBufferDesc.cBuffers = 1;
            outputBufferDesc.pBuffers = &outputBuffer;
            outputBufferDesc.ulVersion = SECBUFFER_VERSION;

            outputBuffer.pvBuffer = &messageType;
            outputBuffer.BufferType = SECBUFFER_TOKEN;
            outputBuffer.cbBuffer = sizeof(messageType);

            // best effort.
            secStatus = s_pSSPIfnTbl->ApplyControlToken(&m_ctxtHandle, &outputBufferDesc);

            m_connectionClosed = true;
        }

        s_pSSPIfnTbl->DeleteSecurityContext(&m_ctxtHandle);
        m_ctxtHandleInited = false;

        return S_OK;
    }

    bool SSLAuth::HasAnyThumbprint()
    {
        return SSLAuth::s_bThumbPrintAValid || SSLAuth::s_bThumbPrintBValid;
    }

    HRESULT SSLAuth::ValidateCertificateAndGetContextAttributes(bool authServer)
    {
        PCERT_CONTEXT pClientCertContext = NULL;
        HRESULT hr;

        hr = s_pSSPIfnTbl->QueryContextAttributes(&m_ctxtHandle, SECPKG_ATTR_REMOTE_CERT_CONTEXT, &pClientCertContext);

        if (FAILED(hr))
        {
            return hr;
        }

        ON_BLOCK_EXIT(CertFreeCertificateContext, pClientCertContext);

        hr = IsCertificateTrusted(pClientCertContext, authServer);

        if (FAILED(hr))
        {
            if (s_bValidateCAChain)
            {
                SSLError("Certificate validation failed", LogTag_StatusCode, hr);
                return hr;
            }
            else
            {
                SSLError("Cert chain validation failed, skipping due to config", LogTag_StatusCode, hr);
                OutputDebugString("Cert chain validation failed. But Skipping from config.\r\n");
                m_bCertCAValidationPassed = false;
            }
        }
        else
        {
            m_bCertCAValidationPassed = true;
        }

        LPVOID ar[] = { s_pThumbPrintA, s_pThumbPrintB };

        std::string msg("certificate");

        hr = IsCertificateThumbprintAcceptable(pClientCertContext, ar, 2, &msg);
        
        if (FAILED(hr))
        {
            hr = IsCertificateSubjectAcceptable(pClientCertContext, authServer, &msg);
        }

        if (FAILED(hr))
        {
            msg = std::string("cert auth failed: ").append(msg);
            SSLError(msg.c_str());

            m_bCertCAValidationPassed = false;
            return hr;
        }

        msg = std::string("cert auth succeeded: ").append(msg);
        SSLWarning(msg.c_str());

        m_bAuthCompleted = true; // cert passed verification
        m_ctxtHandleValid = true;

        OutputDebugString("Cert is trusted, connected\r\n");

        hr = s_pSSPIfnTbl->QueryContextAttributes(&m_ctxtHandle, SECPKG_ATTR_STREAM_SIZES, &m_secStreamSizes);

        return S_OK;
    }

    HRESULT SSLAuth::IsCertificateTrusted(PCCERT_CONTEXT pCertContext, bool authServer)
    {
        CERT_CHAIN_PARA          certChainParam;
        PCCERT_CHAIN_CONTEXT     pCertChainContext = NULL;
        LPSTR certUsages[3];
        int usagesCount;
        if(authServer)
        {
            certUsages[0] = szOID_PKIX_KP_CLIENT_AUTH;
            usagesCount = 1;
        }
        else
        {
            certUsages[0] = szOID_PKIX_KP_SERVER_AUTH;
            certUsages[1] = szOID_SERVER_GATED_CRYPTO;
            certUsages[2] = szOID_SGC_NETSCAPE;
            usagesCount = 3;
        }

        // Get certificate chain
        ZeroMemory(&certChainParam, sizeof(certChainParam));
        certChainParam.cbSize = sizeof(certChainParam);
        certChainParam.RequestedUsage.dwType = USAGE_MATCH_TYPE_OR;
        certChainParam.RequestedUsage.Usage.cUsageIdentifier = usagesCount;
        certChainParam.RequestedUsage.Usage.rgpszUsageIdentifier = certUsages;

        if (!CertGetCertificateChain(
                                     NULL,
                                     pCertContext,
                                     NULL,
                                     pCertContext->hCertStore,
                                     &certChainParam,
                                     s_bCheckCertificateRevocation ? CERT_CHAIN_REVOCATION_CHECK_CHAIN_EXCLUDE_ROOT : 0,
                                     NULL,
                                     &pCertChainContext))
        {
            return HRESULT_FROM_WIN32(GetLastError());
        }
        ON_BLOCK_EXIT(CertFreeCertificateChain, pCertChainContext);

        // now validate
        HTTPSPolicyCallbackData  httpsPolicyCallbackData;
        CERT_CHAIN_POLICY_PARA   certChainPolicyParam;
        CERT_CHAIN_POLICY_STATUS policyStatus;
        ZeroMemory(&httpsPolicyCallbackData, sizeof(HTTPSPolicyCallbackData));
        httpsPolicyCallbackData.cbStruct = sizeof(HTTPSPolicyCallbackData);
        httpsPolicyCallbackData.dwAuthType = authServer ? AUTHTYPE_CLIENT : AUTHTYPE_SERVER; // who is being authenticated. For server, the client is being authenticated here
        httpsPolicyCallbackData.fdwChecks = 0;    // no checks ignored
        httpsPolicyCallbackData.pwszServerName = NULL; // if server name is given, validated in InitializeSecurityContext

        ZeroMemory(&certChainPolicyParam, sizeof(certChainPolicyParam));
        certChainPolicyParam.cbSize = sizeof(certChainPolicyParam);
        certChainPolicyParam.pvExtraPolicyPara = &httpsPolicyCallbackData;

        ZeroMemory(&policyStatus, sizeof(policyStatus));
        policyStatus.cbSize = sizeof(policyStatus);

        if (!CertVerifyCertificateChainPolicy(
                                              CERT_CHAIN_POLICY_SSL,
                                              pCertChainContext,
                                              &certChainPolicyParam,
                                              &policyStatus))
        {
            return HRESULT_FROM_WIN32(GetLastError());
        }

        if (ERROR_SUCCESS != policyStatus.dwError)
        {
            // Note that we ignore error of being unable to check CRL due to CRL being offline which we'll investigate below.
            // If the error is not one of these, this cert is just no good and we should bail immediately.
            if (CRYPT_E_NO_REVOCATION_CHECK != policyStatus.dwError && CRYPT_E_REVOCATION_OFFLINE != policyStatus.dwError)
            {
                return HRESULT_FROM_WIN32(policyStatus.dwError);
            }
            else
            {
                // If there is a revocation server offline error we'll verify that the revocation offline error is actually the only error here.
                // CertVerifyCertificateChainPolicy only reports 1 error out of possibly several so something else could technically be hidden behind it 
                // that we could miss if we just go off the CertVerifyCertificateChainPolicy error result. dwErrorStatus in the cert chain context provides 
                // flags to sort it out.
                // Note that if we were doing name validation as part of CertVerifyCertificateChainPolicy we would actually need to call it back a 2nd time
                // just to do name check since CertGetCertificateChain doesn't necessarily validate that properly, but right now we always pass
                // NULL for name in httpsPolicyCallbackData as we validate name in InitializeSecurityContext so the below is fine for our use case
                int certChainErrorStatus = pCertChainContext->TrustStatus.dwErrorStatus;

                // we only tolerate it if the errors contained are in the set { CERT_TRUST_REVOCATION_STATUS_UNKNOWN , CERT_TRUST_IS_OFFLINE_REVOCATION }
                // anything else will not be allowed
                int invalidMask = ~(CERT_TRUST_REVOCATION_STATUS_UNKNOWN | CERT_TRUST_IS_OFFLINE_REVOCATION);

                if ((certChainErrorStatus & invalidMask) != 0)
                {
                    return HRESULT_FROM_WIN32(policyStatus.dwError);
                }

                Log(LogID_Common, LogLevel_Warning, "Certificate revocation server unavailable. Skipping revocation check.", LogTag_StatusCode, policyStatus.dwError, LogTag_End);
            }
        }

        return S_OK;
    }

    HRESULT SSLAuth::IsCertificateThumbprintAcceptable(PCCERT_CONTEXT pCertContext, LPVOID pThumbprints[], int numThumbprints, std::string* debugMsg)
    {
        BYTE rgbSha1Hash[SHA1_HASH_SIZE];

        HRESULT hr = GetCertificateThumbprint(pCertContext, rgbSha1Hash);
        if (FAILED(hr) || pThumbprints == NULL || numThumbprints <= 0)
        {
            return hr;
        }

        if (!s_considerIdentitiesWhitelist)
        {
            if (debugMsg != NULL)
            {
                debugMsg->append(" not accepted as identity as whitelist is disabled ").append(HexToString(rgbSha1Hash)).c_str();
            }

            return E_FAIL;
        }

        for (int i = 0; i < numThumbprints; i++)
        {
            if (pThumbprints[i] != NULL)
            {
                if (memcmp((LPVOID)rgbSha1Hash, pThumbprints[i], SHA1_HASH_SIZE) == 0)
                {
                    if (debugMsg != NULL)
                    {
                        debugMsg->append(" accepted ").append(HexToString(rgbSha1Hash)).c_str();
                    }
                    return S_OK;
                }
            }
        }

        if (debugMsg != NULL)
        {
            debugMsg->append(" not accepted ").append(HexToString(rgbSha1Hash)).c_str();
        }

        return E_FAIL;
    }

    HRESULT SSLAuth::IsCertificateSubjectAcceptable(PCCERT_CONTEXT pCertContext, bool authServer, std::string*msg)
    {
        PCCERT_CONTEXT parentCertContext;
        std::string subject;

        HRESULT hr = GetCertificateSubject(pCertContext, authServer, subject, parentCertContext);

        if (FAILED(hr))
        {
            if (msg != NULL)
            {
                msg->append("Cert subject not retrievable.\r\n");
            }

            return hr;
        }

        if (SSLAuth::s_bSubjectAValid)
        {
            if (SSLAuth::s_pSubjectA.compare(subject) == 0)
            {
                PBYTE *arrayOfThumbprints = NULL;
                int num = 0;

                hr = ConvertStringThumbprintsToArrayOfByteArray(SSLAuth::s_pThumbPrintsParentA, &arrayOfThumbprints, &num);

                if (FAILED(hr))
                {
                    if (msg != NULL)
                    {
                        msg->append("Acceptable cert thumbprintA is invalid");
                    }
                    return hr;
                }

                if (msg != NULL)
                {
                    msg->append("Cert subject is acceptable: ").append(subject).append("parent's cert: ");
                }

                hr = IsCertificateThumbprintAcceptable(parentCertContext, (LPVOID*)arrayOfThumbprints, num, msg);
                if (SUCCEEDED(hr))
                {
                    return S_OK;
                }
            }
        }

        if (SSLAuth::s_bSubjectBValid)
        {
            if (SSLAuth::s_pSubjectB.compare(subject) == 0)
            {
                PBYTE *arrayOfThumbprints = NULL;
                int num = 0;

                hr = ConvertStringThumbprintsToArrayOfByteArray(SSLAuth::s_pThumbPrintsParentB, &arrayOfThumbprints, &num);

                if (FAILED(hr))
                {
                    if (msg != NULL)
                    {
                        msg->append("Acceptable cert thumbprintB is invalid");
                    }
                    return hr;
                }

                if (msg != NULL)
                {
                    msg->append("Cert subject is acceptable: ").append(subject).append("parent's cert: ");
                }

                hr = IsCertificateThumbprintAcceptable(parentCertContext, (LPVOID*)arrayOfThumbprints, num, msg);
                if (SUCCEEDED(hr))
                {
                    return S_OK;
                }
            }
        }

        return E_FAIL;
    }

    HRESULT SSLAuth::ConvertStringThumbprintsToArrayOfByteArray(std::string thumbprintSequence, PBYTE **arrayOfThumbprints, int *num)
    {
        std::vector<BYTE*> output;

        std::string::size_type prev_pos = 0;
        std::string::size_type pos = 0;

        BYTE *oneThumb;

        *arrayOfThumbprints = NULL;
        *num = 0;

        HRESULT hr;

        while ((pos = thumbprintSequence.find(';', prev_pos)) != std::string::npos)
        {
            std::string substring(thumbprintSequence.substr(prev_pos, pos - prev_pos));

            oneThumb = new BYTE[SHA1_HASH_SIZE];

            hr = ConvertStringThumbprintToByteArray(substring.c_str(), oneThumb);

            if (S_OK == hr)
            {
                output.push_back(oneThumb);
            }
            else
            {
                Log(LogID_Netlib, LogLevel_Error, std::string("ConvertStringThumbprintToByteArray failed for ").append(substring).c_str(),
                    LogTag_StatusCode, hr, LogTag_End);

                delete[] oneThumb;

                for (int i = 0; i < (int)output.size(); i++)
                {
                    delete[] output[i];
                }

                return E_FAIL;
            }

            prev_pos = pos + 1;
        }

        std::string substring(thumbprintSequence.substr(prev_pos));
        
        oneThumb = new BYTE[SHA1_HASH_SIZE];
        hr = ConvertStringThumbprintToByteArray(substring.c_str(), oneThumb);

        if (S_OK == hr)
        {
            output.push_back(oneThumb);
        }
        else
        {
            Log(LogID_Netlib, LogLevel_Error, std::string("ConvertStringThumbprintToByteArray failed for ").append(substring).c_str(),
                LogTag_StatusCode, hr, LogTag_End);

            delete[] oneThumb;

            for (int i = 0; i < (int)output.size(); i++)
            {
                delete[] output[i];
            }

            return E_FAIL;
        }

        *num = (int)output.size();
        
        if (*num != 0)
        {
            *arrayOfThumbprints = new PBYTE[*num];
            for (int i = 0; i < *num; i++)
            {
                (*arrayOfThumbprints)[i] = output[i];
            }
        }

        return S_OK;
    }

    CHAR SSLAuth::QuadToChar(BYTE b)
    {
        if (b >= 0 && b <= 9)
        {
            return '0' + b;
        }
        if (b >= 10 && b <= 15)
        {
            return 'a' + b - 10;
        }
        return '?';
    }

    std::string SSLAuth::HexToString(BYTE pThumbBytes[SHA1_HASH_SIZE])
    {
        CHAR bytes[SHA1_HASH_SIZE * 2 + 1];
        int i;

        for (i = 0; i < SHA1_HASH_SIZE; i++)
        {
            bytes[i * 2] = QuadToChar(pThumbBytes[i] >> 4);
            bytes[i * 2 + 1] = QuadToChar(pThumbBytes[i] & 0x0f);
        }
        bytes[i] = 0;

        return std::string(bytes);
    }

    DWORD SSLAuth::ConvertStringThumbprintToByteArray(LPCSTR thumbprint, BYTE *pThumbBytes)
    {
        int nelem = (int)(strlen(thumbprint) / 2);
        if(nelem != SHA1_HASH_SIZE)
            {
                return ERROR_INVALID_PARAMETER;
            }

        for (int i = 0, j = 0; i < nelem; i++)
            {
                pThumbBytes[i] = HexToDec(thumbprint[j]) * 16 + HexToDec(thumbprint[j + 1]);
                j += 2;
            }

        return ERROR_SUCCESS;
    }


    BYTE SSLAuth::HexToDec(char ch)
    {
        if (ch >= '0' && ch <= '9')
        {
            return ch - '0';
        }
        if (ch >= 'a' && ch <= 'f')
        {
            return ch - 'a' + 10;
        }
        if (ch >= 'A' && ch <= 'F')
        {
            return ch - 'A' + 10;
        }

        // non-hex translate to zero
        return 0;
    }

    HRESULT SSLAuth::GetCertificateThumbprint(PCCERT_CONTEXT pCertContext, BYTE pbSha1Hash[SHA1_HASH_SIZE])
    {
        DWORD cbSha1Hash = SHA1_HASH_SIZE;

        if (!CertGetCertificateContextProperty(
            pCertContext,
            CERT_SHA1_HASH_PROP_ID,
            pbSha1Hash,
            &cbSha1Hash))
        {
            return HRESULT_FROM_WIN32(GetLastError());
        }

        if (cbSha1Hash != SHA1_HASH_SIZE)
        {
            return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
        }

        return S_OK;
    }

    HRESULT SSLAuth::GetCertificateSubject(PCCERT_CONTEXT pCertContext, bool authServer, std::string &subject, PCCERT_CONTEXT &pParentCertContext)
    {
        char pszNameString[256];

        if (!CertGetNameString(
            pCertContext,
            CERT_NAME_SIMPLE_DISPLAY_TYPE,
            0,
            NULL,
            pszNameString,
            sizeof(pszNameString)))
        {
            return HRESULT_FROM_WIN32(GetLastError());
        }

        subject = std::string(pszNameString);

        // now get the chain
        CERT_CHAIN_PARA          certChainParam;
        PCCERT_CHAIN_CONTEXT     pCertChainContext = NULL;
        LPSTR certUsages[3];
        int usagesCount;
        if (!authServer)
        {
            certUsages[0] = szOID_PKIX_KP_CLIENT_AUTH;
            usagesCount = 1;
        }
        else
        {
            certUsages[0] = szOID_PKIX_KP_SERVER_AUTH;
            certUsages[1] = szOID_SERVER_GATED_CRYPTO;
            certUsages[2] = szOID_SGC_NETSCAPE;
            usagesCount = 3;
        }

        // Get certificate chain
        ZeroMemory(&certChainParam, sizeof(certChainParam));
        certChainParam.cbSize = sizeof(certChainParam);
        certChainParam.RequestedUsage.dwType = USAGE_MATCH_TYPE_OR;
        certChainParam.RequestedUsage.Usage.cUsageIdentifier = usagesCount;
        certChainParam.RequestedUsage.Usage.rgpszUsageIdentifier = certUsages;

        if (!CertGetCertificateChain(
            NULL,
            pCertContext,
            NULL,
            pCertContext->hCertStore,
            &certChainParam,
            s_bCheckCertificateRevocation ? CERT_CHAIN_REVOCATION_CHECK_CHAIN_EXCLUDE_ROOT : 0,
            NULL,
            &pCertChainContext))
        {
            return HRESULT_FROM_WIN32(GetLastError());
        }
        ON_BLOCK_EXIT(CertFreeCertificateChain, pCertChainContext);

        // now we check if we have a parent cert:

        // go through all chains, until we find one with a parent signing cert to this one
        for (unsigned int i = 0; i < pCertChainContext->cChain; i++)
        {
            CERT_SIMPLE_CHAIN *simpleCertificateChainWithinContext = pCertChainContext->rgpChain[i];

            if (simpleCertificateChainWithinContext->cElement >= 2)
            {
                // get the CertContext from the array
                pParentCertContext = simpleCertificateChainWithinContext->rgpElement[1]->pCertContext;
                return S_OK;
            }
        }

        return E_FAIL;
    }

    PCCERT_CONTEXT SSLAuth::GetCertificate(LPCSTR store, BYTE *pThumbPrint)
    {
        wchar_t Lstore[256];
        size_t sizeConverted;
        DWORD err = mbstowcs_s(&sizeConverted, Lstore, ARRAYSIZE(Lstore), store, SHA1_HASH_SIZE*2 + 1);

        if(err)
            {
                return NULL;
            }

        HCERTSTORE hStore = CertOpenStore(CERT_STORE_PROV_SYSTEM, 0, 0, CERT_SYSTEM_STORE_LOCAL_MACHINE, Lstore);

        PCCERT_CONTEXT pCertCtx = NULL;
        for (
             pCertCtx = CertEnumCertificatesInStore(hStore, NULL);
             pCertCtx != NULL;
             pCertCtx = CertEnumCertificatesInStore(hStore, pCertCtx))
            {
                BYTE                rgbSha1Hash[SHA1_HASH_SIZE];

                HRESULT hr = GetCertificateThumbprint(pCertCtx, rgbSha1Hash);
                if (SUCCEEDED(hr))
                    {
                        if (memcmp(rgbSha1Hash, pThumbPrint, sizeof(rgbSha1Hash)) == 0)
                            {
                                break;
                            }
                    }
            }

        CertCloseStore(hStore, 0);
        return pCertCtx;
    }

    
    DWORD SSLAuth::CreateCertificateCredential(PCCERT_CONTEXT pCertCtx)
    {
        {
            SCHANNEL_CRED   scCred = { 0 };
            scCred.dwVersion = SCHANNEL_CRED_VERSION;
            scCred.cCreds = 1;
            scCred.paCred = &pCertCtx;
            scCred.grbitEnabledProtocols = SP_PROT_TLS1_2_CLIENT;
            scCred.dwFlags = SCH_USE_STRONG_CRYPTO;

            SECURITY_STATUS status = s_pSSPIfnTbl->AcquireCredentialsHandle(
                                                                            NULL,
                                                                            UNISP_NAME,
                                                                            SECPKG_CRED_OUTBOUND,
                                                                            NULL,
                                                                            &scCred,
                                                                            NULL,
                                                                            NULL,
                                                                            &s_scClientCredHandle,
                                                                            NULL
                                                                            );

            s_scClientCredHandleValid = (SEC_E_OK == status );

            if (status != SEC_E_OK)
            {
                return status;
            }
        }

        {
            SCHANNEL_CRED    scCred = { 0 };
            scCred.dwVersion = SCHANNEL_CRED_VERSION;
            scCred.cCreds = 1;
            scCred.paCred = &pCertCtx;
            scCred.grbitEnabledProtocols = SP_PROT_TLS1_2_SERVER;
            scCred.dwFlags = SCH_USE_STRONG_CRYPTO;
        
            SECURITY_STATUS status = s_pSSPIfnTbl->AcquireCredentialsHandle(
                                                                            NULL,
                                                                            UNISP_NAME,
                                                                            SECPKG_CRED_INBOUND,
                                                                            NULL,
                                                                            &scCred,
                                                                            NULL,
                                                                            NULL,
                                                                            &s_scServerCredHandle,
                                                                            NULL
                                                                            );
        
            s_scServerCredHandleValid = (SEC_E_OK == status );
        
            if (status != SEC_E_OK)
            {
                return status;
            }

            return status;
        }
    }

    DWORD SSLAuth::GetCredential(LPCSTR store)
    {
        if (s_scClientCredHandleValid)
        {
            s_pSSPIfnTbl->FreeCredentialsHandle(&s_scClientCredHandle);
            ZeroMemory(&s_scClientCredHandle, sizeof(s_scClientCredHandle));
            s_scClientCredHandleValid = false;
        }

        if (s_scServerCredHandleValid)
        {
            s_pSSPIfnTbl->FreeCredentialsHandle(&s_scServerCredHandle);
            ZeroMemory(&s_scServerCredHandle, sizeof(s_scServerCredHandle));
            s_scServerCredHandleValid = false;
        }

        PCCERT_CONTEXT pCertCtx = NULL;

        // it might be no cert is valid, and hence, we are just done
        if (!s_bThumbPrintAValid && !s_bThumbPrintBValid)
        {
            return ERROR_SUCCESS;
        }

        if (s_bThumbPrintAValid)
        {
            pCertCtx = GetCertificate(store, s_pThumbPrintA);

            if (s_bThumbPrintBValid && NULL == pCertCtx)
            {
                pCertCtx = GetCertificate(store, s_pThumbPrintB);
            }
        }

        if (NULL == pCertCtx)
        {
            return ERROR_INVALID_PARAMETER;
        }
        else
        {
            ON_BLOCK_EXIT(CertFreeCertificateContext, pCertCtx);
            return CreateCertificateCredential(pCertCtx);
        }
    }

    DWORD SSLAuth::SetSSLSubjectNames(LPCSTR subjectA, LPCSTR thumbPrintsParentA, LPCSTR subjectB, LPCSTR thumbPrintsParentB, bool considerIdentitiesWhitelist)
    {
        if (subjectA != NULL && thumbPrintsParentA != NULL)
        {
            SSLAuth::s_pSubjectA = subjectA;
            SSLAuth::s_pThumbPrintsParentA = thumbPrintsParentA;
            SSLAuth::s_bSubjectAValid = true;
        }
        else
        {
            SSLAuth::s_bSubjectAValid = false;
        }

        if (subjectB != NULL && thumbPrintsParentB != NULL)
        {
            SSLAuth::s_pSubjectB = subjectB;
            SSLAuth::s_pThumbPrintsParentB = thumbPrintsParentB;
            SSLAuth::s_bSubjectBValid = true;
        }
        else 
        {
            SSLAuth::s_bSubjectBValid = false;
        }

        SSLAuth::s_bSslEnabled = SSLAuth::s_bSslEnabled || SSLAuth::s_bSubjectAValid || SSLAuth::s_bSubjectBValid;
        SSLAuth::s_considerIdentitiesWhitelist = considerIdentitiesWhitelist;

        return ERROR_SUCCESS;
    }

    DWORD SSLAuth::SetSSLThumbprints(LPCSTR store, LPCSTR thumbPrintA, LPCSTR thumbPrintB, bool validateCAChain, bool checkCertificateRevocation)
    {
        if(!InitializeSChannel())
        {
            return ERROR_INVALID_HANDLE;
        }
    
        if(SSLAuth::s_scClientCredHandleValid)
        {
            s_pSSPIfnTbl->FreeCredentialsHandle(&SSLAuth::s_scClientCredHandle);
            ZeroMemory(&SSLAuth::s_scClientCredHandle, sizeof(SSLAuth::s_scClientCredHandle));
            SSLAuth::s_scClientCredHandleValid = false;
        }

        if(SSLAuth::s_scServerCredHandleValid)
        {
            s_pSSPIfnTbl->FreeCredentialsHandle(&SSLAuth::s_scClientCredHandle);
            ZeroMemory(&SSLAuth::s_scClientCredHandle, sizeof(SSLAuth::s_scClientCredHandle));
            SSLAuth::s_scServerCredHandleValid = false;
        }
        
        DWORD err = ERROR_SUCCESS;

        if (thumbPrintA != NULL)
        {
            err = ConvertStringThumbprintToByteArray(thumbPrintA, SSLAuth::s_pThumbPrintA);
            if (err != ERROR_SUCCESS)
            {
                Log(LogID_Netlib, LogLevel_Error, "SSL init failed. Thumbprint not valid",
                    LogTag_String1, thumbPrintA, LogTag_End);
                return err;
            }

            SSLAuth::s_bThumbPrintAValid = true;
        }
        else
        {
            SSLAuth::s_bThumbPrintAValid = false;
        }

        if (thumbPrintB != NULL)
        {
            err = ConvertStringThumbprintToByteArray(thumbPrintB, SSLAuth::s_pThumbPrintB);
            if (err != ERROR_SUCCESS)
            {
                Log(LogID_Netlib, LogLevel_Error, "SSL init failed. Thumbprint not valid",
                    LogTag_String1, thumbPrintB, LogTag_End);
                return err;
            }

            SSLAuth::s_bThumbPrintBValid = true;
        }
        else
        {
            SSLAuth::s_bThumbPrintBValid = false;
        }

        SSLAuth::s_bSslEnabled = thumbPrintA != NULL || thumbPrintB != NULL;
        SSLAuth::s_bValidateCAChain = validateCAChain;
        SSLAuth::s_bCheckCertificateRevocation = checkCertificateRevocation;
        SSLAuth::s_store = store;

        err = GetCredential(store);
        if(err != ERROR_SUCCESS)
        {
            Log(LogID_Netlib, LogLevel_Error, "SSL init failed. GetCredential failed",
                LogTag_StatusCode, err, LogTag_End);
        }

        return err;
    }

    bool SSLAuth::IsSSLEnabled()
    {
        return SSLAuth::s_bSslEnabled;
    }

    BOOL SSLAuth::InitializeSChannel()
    {
        for (;;)
        {
            LONG result = InterlockedCompareExchange(&SSLAuth::s_sChannelInitialized, Initializing, NotInitialized);

            if (NotInitialized == result)
            {
                // Initialize the SChannel
                SSLAuth::s_pSSPIfnTbl = InitSecurityInterface();

                if (NULL != SSLAuth::s_pSSPIfnTbl)
                {
                    InterlockedCompareExchange(&SSLAuth::s_sChannelInitialized, Initialized, Initializing);
                }
                else
                {
                    // Init failed
                    InterlockedCompareExchange(&SSLAuth::s_sChannelInitialized, NotInitialized, Initializing);
                    return FALSE;
                }
            }
            else if (Initialized == result)
            {
                return TRUE;
            }

            // Being initialized wait
            Sleep(300);
        }
    }

    HRESULT SSLAuth::Authenticate(bool authServer, SEC_CHAR* validateUsingName, int *cbInDataLen, LPVOID inData, int *cbOutDataLen, LPVOID outData, OutputType *outType)
    {
        SECURITY_STATUS      secStatus;
        SecBufferDesc        inputBufferDesc;
        SecBufferDesc        outputBufferDesc;
        SecBuffer            inputBuffers[2];
        SecBuffer            outputBuffer;
        DWORD                dwSspiOutputFlags = 0;
        TimeStamp            expiryTimeStamp;

        DWORD dwSspiInFlags = 
            ISC_REQ_SEQUENCE_DETECT |
            ISC_REQ_REPLAY_DETECT |
            ISC_REQ_CONFIDENTIALITY |
            ISC_REQ_EXTENDED_ERROR |
            ISC_REQ_ALLOCATE_MEMORY |
            ISC_REQ_STREAM;

        if (authServer && validateUsingName == NULL)
        {
            dwSspiInFlags |= ISC_REQ_MANUAL_CRED_VALIDATION; // we need to manually validate the cert
        }

        if (!authServer)
        {
            dwSspiInFlags |= ASC_REQ_MUTUAL_AUTH;
        }

        secStatus = SEC_E_INCOMPLETE_MESSAGE;

        *outType = AuthData;

        inputBufferDesc.cBuffers = 2;
        inputBufferDesc.pBuffers = inputBuffers;
        inputBufferDesc.ulVersion = SECBUFFER_VERSION;

        outputBufferDesc.cBuffers = 1;
        outputBufferDesc.pBuffers = &outputBuffer;
        outputBufferDesc.ulVersion = SECBUFFER_VERSION;

        // input buffer 0 contains the data from the client
        // input buffer 1 is reserved and will return any excess data that is not processed in this call
        inputBuffers[0].pvBuffer = inData;
        inputBuffers[0].cbBuffer = *cbInDataLen;
        inputBuffers[0].BufferType = SECBUFFER_TOKEN;

        inputBuffers[1].pvBuffer = NULL;
        inputBuffers[1].cbBuffer = 0;
        inputBuffers[1].BufferType = SECBUFFER_EMPTY;

        // Initialize output buffer
        outputBuffer.pvBuffer = NULL;
        outputBuffer.BufferType = SECBUFFER_TOKEN;
        outputBuffer.cbBuffer = 0;

        ON_BLOCK_EXIT(s_pSSPIfnTbl->FreeContextBuffer, ByRef(outputBuffer.pvBuffer));

        if (authServer)
        {
            secStatus = s_pSSPIfnTbl->InitializeSecurityContext(
                                                                &s_scClientCredHandle,
                                                                m_ctxtHandleInited ? &m_ctxtHandle : NULL,
                                                                validateUsingName,
                                                                dwSspiInFlags,
                                                                0,
                                                                SECURITY_NATIVE_DREP,
                                                                m_ctxtHandleInited ? &inputBufferDesc : NULL,
                                                                0,
                                                                m_ctxtHandleInited ? NULL : &m_ctxtHandle,
                                                                &outputBufferDesc,
                                                                &dwSspiOutputFlags,
                                                                &expiryTimeStamp);
        }
        else 
        {
            secStatus = s_pSSPIfnTbl->AcceptSecurityContext(
                                                            &s_scServerCredHandle,
                                                            m_ctxtHandleInited ? &m_ctxtHandle : NULL,
                                                            &inputBufferDesc,
                                                            dwSspiInFlags,
                                                            0,
                                                            m_ctxtHandleInited ? NULL : &m_ctxtHandle,
                                                            &outputBufferDesc,
                                                            &dwSspiOutputFlags,
                                                            &expiryTimeStamp);
        }

        m_ctxtHandleInited = true;

        if (SEC_E_INCOMPLETE_MESSAGE != secStatus && SECBUFFER_EXTRA == inputBuffers[1].BufferType)
        {
            // Move over the processed data
            *cbInDataLen -= inputBuffers[1].cbBuffer;
        }

        if (outputBuffer.cbBuffer != 0 && outputBuffer.pvBuffer != NULL)
        {
            // SSPI created token to be sent to client

            if ((UINT32)*cbOutDataLen < outputBuffer.cbBuffer)
            {
                return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
            }

            *cbOutDataLen = (int)outputBuffer.cbBuffer;

            memcpy(outData, outputBuffer.pvBuffer, outputBuffer.cbBuffer);
        }
        else
        {
            *cbOutDataLen = 0;
        }

        if (secStatus == SEC_I_CONTINUE_NEEDED)
        {
            return S_OK;
        }
        else if (secStatus == SEC_E_OK)
        {
            HRESULT hr = ValidateCertificateAndGetContextAttributes(authServer);

            if (SUCCEEDED(hr))
            {
                *outType = AuthDataEnd;
            }

            return hr;
        }
        else if (secStatus == SEC_E_INCOMPLETE_MESSAGE)
        {
            return secStatus;    // More data is needed
        }

        return secStatus;
    }

    SSLAuthServer::SSLAuthServer(LPCTSTR authEndPointName)
    {
        if(authEndPointName != NULL)
        {
            m_remoteEndpointName = authEndPointName;
        }
    }

    HRESULT SSLAuthServer::RunSSPILoop(int *cbInDataLen, LPVOID inData, int *cbOutDataLen, LPVOID outData, OutputType *outType)
    {
        SEC_CHAR* validateUsingName = NULL;

        if (m_remoteEndpointName.length() != 0)
        {
            validateUsingName = (SEC_CHAR*)m_remoteEndpointName.c_str(); // Let the system validate the endpoint name
        }

        HRESULT hr = Authenticate(true, validateUsingName, cbInDataLen, inData, cbOutDataLen, outData, outType);

        return hr;
    }

    // Used by server to authenticate the client
    HRESULT SSLAuthClient::RunSSPILoop(int *cbInDataLen, LPVOID inData, int *cbOutDataLen, LPVOID outData, OutputType *outType)
    {
        HRESULT hr = Authenticate(false, NULL, cbInDataLen, inData, cbOutDataLen, outData, outType);

        return hr;
    }
}


