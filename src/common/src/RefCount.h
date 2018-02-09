#pragma once

#include <windows.h>

namespace RSLibImpl
{

class RefCount
{
protected:
    mutable volatile long m_Crefs;

public:
    RefCount(void) { m_Crefs = 0; }
    
protected:
    virtual ~RefCount()
    { 
        if (m_Crefs != 0)
        {
            // if we are here then some code is trying to delete object with active references.
            DebugBreak();
        }
    }
public:

    long UpCount(void) const
    {
        return InterlockedIncrement(&m_Crefs);
    }
    
    long DownCount(void) const
    {
        long val = InterlockedDecrement(&m_Crefs);
        if (!val) 
        {
            delete this;
        }
        return val;
    }

    void OnRefCountError();
    static void SetErrorMode(bool AssertOnError);
};

template <class T> class Ptr
{
    T* m_Ptr;
    public:

    Ptr(const Ptr<T>& other) : m_Ptr(other.m_Ptr)
    {
        if (m_Ptr)
        {
            m_Ptr->UpCount();
        }
    }
    
    Ptr(T* ptr = 0) : m_Ptr(ptr)
    {
        if (m_Ptr)
        {
            m_Ptr->UpCount();
        }
    }
    
    ~Ptr(void)
    {
        if (m_Ptr)
        {
            m_Ptr->DownCount();
        }
    }
    
    operator T*(void) const { return m_Ptr; }
    
    operator T*(void) { return m_Ptr; }
    
    T& operator*(void) const { return *m_Ptr; }
    
    T& operator*(void) { return *m_Ptr; }

    T* operator->(void) const { return m_Ptr; }
    
    T* operator->(void) { return m_Ptr; }
    
    bool operator == (const T* ptr) const { return (m_Ptr == ptr); }
    
    bool operator == (const Ptr<T> &ptr) const { return (m_Ptr == ptr.m_Ptr); }
    
    bool operator != (const T * ptr) const { return (m_Ptr != ptr); }
    
    bool operator != (const Ptr<T> &ptr) const { return (m_Ptr != ptr.m_Ptr); }

    bool operator !() const { return (m_Ptr == 0); }
    
    Ptr& operator=(Ptr<T> &ptr) {return operator=((T *) ptr);}
    
    Ptr& operator=(T* ptr)
    {
        //
        // Be careful with the ordering of operations here. The invariant we wish to preserve is that we should never publish a pointer (by having it assigned
        // to m_Ptr) unless we hold a ref count on it. Other threads can access m_Ptr asynchronously so if we break this invariant they may make a copy of a
        // pointer to an object that is destructed or in the process of being destructed.
        //

        // Increment ref count on the new pointer *before* we publish it in m_Ptr.
        if (ptr)
            ptr->UpCount();

        // Atomically publish the new pointer and unpublish the old pointer.
        T* oldPtr = (T*)InterlockedExchangePointer((volatile PVOID*)&m_Ptr, ptr);

        // If we had an old pointer it is now safe to decrement the ref count.
        if (oldPtr)
            oldPtr->DownCount();

        return *this;
    }
};

} // namespace RSLibImpl
