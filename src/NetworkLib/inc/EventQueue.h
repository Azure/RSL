//////////////////////////////////////////////////////////////////////////////
//
//    The PriorityEventQueue Class
//
//////////////////////////////////////////////////////////////////////////////

#pragma once

#include "HiResTime.h"
#include "List.h"
#include "CompletionEvent.h"

namespace RSLibImpl
{

//<5ms, 10, 20, 40, 80, 160, 320, 640, 1280, 2560, 5120
#define PQ_BUCKET_TIME(_i) (HRTIME_MSECONDS(5) << (_i))

class PriorityEventQueue {
 public:
  static const int NUM_BUCKETS = 10;

  Queue<CompletionEvent> m_Eventq[NUM_BUCKETS];
  HiResTime    m_TimeBase;
  int          m_LastChecked;

  void Enqueue(CompletionEvent * e, HiResTime now) {
    HiResTime t = e->m_Timeout - now;
    int i = 0;
    while (t > PQ_BUCKET_TIME(i) && i < NUM_BUCKETS-1)
      i++;
    e->m_InBucket = i;
    m_Eventq[i].enqueue(e);
  }

  void Remove(CompletionEvent * e) {
    LogAssert(e->m_InBucket >= 0);
    m_Eventq[e->m_InBucket].remove(e);
    e->m_InBucket = -1;

  }

  CompletionEvent * Dequeue() {
    CompletionEvent * e = m_Eventq[0].dequeue();
    if (e) {
      LogAssert(e->m_InBucket >= 0);
      e->m_InBucket = -1;
    }
    return e;
  }

  void Check(HiResTime now) {
    int i = 1, k = 0;
    int check = (int)(now / PQ_BUCKET_TIME(0));
    int todo = check ^ m_LastChecked;
    m_TimeBase = now;
    m_LastChecked = check;
    todo &= ((1 << (NUM_BUCKETS - 1)) - 1);
    while (todo) { k++; todo >>= 1 ; }
    for (; i <= k; i++) {
      CompletionEvent * e = NULL;
      Queue<CompletionEvent> q = m_Eventq[i];
      m_Eventq[i].clear();
      while ((e = q.dequeue()) != NULL) {
        if (e->m_Cancelled) {
          m_Eventq[0].enqueue(e);
        } else {
          HiResTime tt = e->m_Timeout - now;
          int j = i;
          while (j > 0 && tt <= PQ_BUCKET_TIME(j-1))
            j--;
          e->m_InBucket = j;
          m_Eventq[j].enqueue(e);
        }
      }
    }
  }

  HiResTime EarliestTimeout() {
    for (int i = 0; i < NUM_BUCKETS; i++) {
      if (m_Eventq[i].head)
    return m_TimeBase + (PQ_BUCKET_TIME(i)/2);
    }
    return m_TimeBase + HRTIME_FOREVER;
  }

  PriorityEventQueue() {
    m_TimeBase = GetHiResTime();
    m_LastChecked =(int) (m_TimeBase / PQ_BUCKET_TIME(0));
  }
};

} // namespace RSLibImpl
