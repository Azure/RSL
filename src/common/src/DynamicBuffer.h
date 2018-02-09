#pragma once

#include "LogAssert.h"

namespace RSLibImpl
{

template <class T, int S>
struct DynamicBuffer
{

    typedef DynamicBuffer<T, S> MyT;

    DynamicBuffer (int initialSize = 0);
    ~DynamicBuffer ();

    void Copy(const MyT& copy);
    void Copy(const T *copy, size_t len);

    operator const T* () const;
    operator T* ();
    T& operator[] (int idx);
    T& operator[] (size_t idx);

    T* Begin() const { return m_Data; }

    T* End() const { m_Data + m_Size; }

    T* Detach ();
    int Size() const;
    void Clear ();

    T* Resize (size_t new_size, bool copyData = true, bool allowShrink=false);

    T *m_Data;
    T m_FastData[S];
    size_t m_Size;
};


template <class T, int S>
inline
DynamicBuffer<T, S>::DynamicBuffer (int initialSize)
    : m_Data (m_FastData),
      m_Size (S)
{
    if (initialSize > 0) {
        int i = 1;

        while (i < initialSize)
            i <<= 1;

        Resize (i, false);
    }
}

template <class T, int S>
inline
DynamicBuffer<T, S>::~DynamicBuffer ()
{
    if (m_Data != m_FastData) {
       free(m_Data);
    }
}

template <class T, int S>
inline void
DynamicBuffer<T, S>::Copy (const MyT &copy)
{
    Copy(copy.m_Data, copy.m_Size);
}

template <class T, int S>
inline void
DynamicBuffer<T, S>::Copy (const T* copy, size_t len)
{
    Resize(len, false);
    memcpy(m_Data, copy, len * sizeof(T));
}

template <class T, int S>
inline
DynamicBuffer<T, S>::operator const T* () const
{
    return m_Data;
}

template <class T, int S>
inline
DynamicBuffer<T, S>::operator T* ()
{
    return m_Data;
}

template <class T, int S>
inline T&
DynamicBuffer<T, S>::operator [] (int idx)
{
    return m_Data[idx];
}

template <class T, int S>
inline T&
DynamicBuffer<T, S>::operator [] (size_t idx)
{
    return m_Data[idx];
}

template <class T, int S>
inline T*
DynamicBuffer<T, S>::Detach ()
{
    T *d;

    d = m_Data;
    if (m_Data == m_FastData)
    {
        d = (T*) malloc(m_Size * sizeof(T));
        LogAssert(d != NULL);
        memcpy(d, m_Data, m_Size * sizeof(T));
        return d;
    }
    m_Data = m_FastData;
    m_Size = S;
    return d;
}

template <class T, int S>
inline int
DynamicBuffer<T, S>::Size() const
{
    return (int) m_Size;
}

template <class T, int S>
inline void
DynamicBuffer<T, S>::Clear ()
{
    if (m_Data != m_FastData) {
       free(m_Data);
    }
    m_Data = m_FastData;
    m_Size = S;
}

template <class T, int S>
inline T*
DynamicBuffer<T, S>::Resize (size_t newSize, bool copyData, bool allowShrink)
{
    if (allowShrink || newSize > m_Size) {

        if (newSize > m_Size && newSize < (m_Size * 2))
        {
            newSize = m_Size * 2;
        }

        if (m_Data == m_FastData)
        {
            if (newSize>m_Size)
            {
                m_Data = (T*) malloc(newSize * sizeof(T));
                LogAssert(m_Data != NULL);
                if ((m_Data != NULL) && copyData)
                {
                    memcpy(m_Data, m_FastData, m_Size * sizeof(T));
                }
            }
        }
        else
        {
            T *newAlloc = (T*) realloc(m_Data, newSize * sizeof(T));
            LogAssert(newAlloc != NULL);
            m_Data = newAlloc;
        }
        m_Size = newSize;
    }
    return m_Data;
}

} // namespace RSLibImpl

