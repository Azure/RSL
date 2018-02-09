/*
   Created by HaoyongZ on Sept. 11, 2003

   This file uses C++ template to implement
   a queue using a linked list.
*/
#pragma once

#include <windows.h>
#include <logassert.h>

namespace RSLibImpl
{

// TODO: probably should changed to the official LogAssert once it is created.

#pragma pack (push, 1)
template<class DATA_TYPE>
class CQueue_Entry
{
public:
    DATA_TYPE * m_pData;
    CQueue_Entry<DATA_TYPE> * m_pNext;

    CQueue_Entry (DATA_TYPE * pData, CQueue_Entry<DATA_TYPE> * pNext)
    {
        m_pData = pData;
        m_pNext = pNext;
    }
};
#pragma pack (pop)

template<class DATA_TYPE>
class CQueue
{
protected:
    HANDLE m_hHeap;
    CQueue_Entry<DATA_TYPE> * m_pHead;
    CQueue_Entry<DATA_TYPE> * m_pTail;
    UINT m_uiCount;

public:
    static CQueue<DATA_TYPE> * CreateNew (HANDLE hHeap)
    {
        CQueue<DATA_TYPE> * pRet = (CQueue<DATA_TYPE> *) HeapAlloc (hHeap, HEAP_ZERO_MEMORY, sizeof (CQueue<DATA_TYPE>));
        if (pRet != NULL)
        {
            pRet->m_hHeap = hHeap;
        }
        return pRet;
    }

    static void Destroy (CQueue<DATA_TYPE> * pQ)
    {
        pQ->Clear ();
        HeapFree (pQ->m_hHeap, 0, pQ);
    }

    bool Dequeue (DATA_TYPE ** ppData)
    {
        bool fSuccess = false;

        if (m_pHead != NULL)
        {
            LogAssert (m_pTail != NULL && m_uiCount > 0);

            CQueue_Entry<DATA_TYPE> * pNext = m_pHead->m_pNext;
            *ppData = m_pHead->m_pData;

            HeapFree (m_hHeap, 0, m_pHead);
            m_pHead = pNext;

            if (m_pHead == NULL)
                m_pTail = NULL;

            m_uiCount --;

            fSuccess = true;
        }

        return fSuccess;
    }

    bool Enqueue (DATA_TYPE * pData)
    {
        bool fSuccess = false;

        CQueue_Entry<DATA_TYPE> * pNew = 
            (CQueue_Entry<DATA_TYPE> *) HeapAlloc (m_hHeap, HEAP_ZERO_MEMORY, sizeof (CQueue_Entry<DATA_TYPE>));
        pNew->m_pData = pData;

        if (pNew != NULL)
        {
            if (m_pTail == NULL)
            {
                LogAssert (m_pHead == NULL && m_uiCount == 0);
                m_pHead = pNew;
            }
            else
            {
                LogAssert (m_pTail->m_pNext == NULL);
                m_pTail->m_pNext = pNew;
            }

            m_pTail = pNew;
            m_uiCount ++;

            fSuccess = true;
        }

        return fSuccess;
    }

    bool Peek (DATA_TYPE ** ppData)
    {
        bool fSuccess = false;
        if (m_pHead != NULL)
        {
            *ppData = m_pHead->m_pData;
            fSuccess = true;
        }
        return fSuccess;
    }

    UINT Count () const
    {
        return m_uiCount;
    }

    void Clear ()
    {
        DATA_TYPE * pTemp;
        while (this->Dequeue (&pTemp))
        {
            // do nothing
        }
    }

    UINT ToArray (const DATA_TYPE * rgpOutput[], UINT uiSizeLimit) const
    {
        CQueue_Entry<DATA_TYPE> * pCur = m_pHead;
        UINT i;
        for (i = 0; i < uiSizeLimit && pCur != NULL; i ++, pCur = pCur->m_pNext)
        {
            rgpOutput[i] = pCur->m_pData;
        }
        return i;
    }

private:
    CQueue () {}
    ~CQueue () {}

};
} // namespace RSLibImpl
