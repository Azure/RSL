#pragma once

namespace RSLibImpl
{

//
// Link cell for singly-linked list of objects of type C.
//
template<class C> struct SLink {
  C * next;
  SLink();
};

template <class C> inline SLink<C>::SLink() : next(NULL) {}

//
//    Link cell for doubly-linked list of objects of type C.
//
template<class C> struct Link : SLink<C> {
  C * prev;
  Link() : prev(NULL) {}
};



//
//    List descriptor for singly-linked list of objects of type C.
//
template <class C> struct SLL
{
  C * head;

  // given an object c and its link cl, find the link for object x

  SLink<C> & link(C * x,C * c,SLink<C> & cl);

  // push objects onto the SLL

  void push(C * c);
  void push(C * c,SLink<C> & l);

  // pop objects off the SLL

  C * pop();
  C * pop(C * c,SLink<C> & l);

  SLL() : head(NULL) {}
};


template <class C> inline SLink<C> & SLL<C>::link(C * x,C * c,SLink<C> & cl)
{
  return *(SLink<C>*)(((char*)x)+(((char*)&cl)-((char*)c)));
}

template <class C> inline void SLL<C>::push(C * c)
{
  push(c,*(SLink<C>*)&c->link);
}

template <class C> inline void SLL<C>::push(C * c,SLink<C> & l)
{
  l.next = head;
  head = c;
}

template <class C> inline C * SLL<C>::pop()
{
  if (head)
    return pop(head,*(SLink<C>*)&head->link);
  else
    return NULL;
}

template <class C> inline C * SLL<C>::pop(C * c,SLink<C> & l)
{
  LogAssert(c == head);
  C * res = head;
  if (res) { 
    head = l.next;
    link(res,c,l).next = NULL;
  }
  return res;
}

//
//    List descriptor for doubly-linked list of objects of type C.
//
template <class C> struct DLL
{
  C * head;

  // given an object c and its link cl, compute the link for object x
  Link<C> & link(C * x,C * c,Link<C> & cl)
  { return *(Link<C>*)(((char*)x)+(((char*)&cl)-((char*)c))); }

  // push objects onto the DLL

  void push(C * c) { push(c,*(Link<C>*)&c->link); }
  void push(C * c,Link<C> & l);

  // pop objects off the DLL

  C * pop();
  C * pop(C * c,Link<C> & l);

  // remove object <c> from any place in the DLL
  //   (<c> must be a member of the list)

  void remove(C * c) { remove(c,*(Link<C>*)&c->link); }
  void remove(C * c,Link<C> & l);

  // insert object <c> after object <after> in the DLL
  //   (<after> must be a member of the list, and <c> must not be)

  void insert(C *c,C *after) { insert(c,*(Link<C>*)&c->link,after); }
  void insert(C *c,Link<C> &l,C *after);

  // determine if object <c> is in the list
  //   (currently does a weak test)

  bool in(C *c) { return in(c,*(Link<C>*)&c->link); }
  bool in(C *c,Link<C> &l) {
    return l.prev||l.next||head==c;
  }

  DLL() : head(NULL) {}
};

template <class C> inline void DLL<C>::push(C * c,Link<C> & l)
{
  C* chead = (C*) head;
  l.next = chead;
  if (chead) link(chead,c,l).prev = c;
  head = c;
}

template <class C> inline C * DLL<C>::pop()
{
  C* chead = (C*) head;
  if (chead)
    return pop(chead,*(Link<C>*)&chead->link);
  else
    return NULL;
}

template <class C> inline C * DLL<C>:: pop(C * c,Link<C> & l)
{
  C * res = (C*) head;
  if (res) remove(res,link(res,c,l));
  return res;
}

template <class C> inline void DLL<C>::remove(C * c,Link<C> & l)
{
  if (!head) return;
  if (c == head) head = (C*) link(head,c,l).next;
  if (l.prev) link(l.prev,c,l).next = l.next;
  if (l.next) link(l.next,c,l).prev = l.prev;
  l.prev = NULL;
  l.next = NULL;
}

template <class C> inline void DLL<C>::insert(C *c,Link<C> &l,C *after)
{
  LogAssert(l.prev == NULL);
  LogAssert(l.next == NULL);
  if (!after) { push(c,l); return; }
  l.prev = after;
  l.next = link(after,c,l).next;
  link(after,c,l).next = c;
  if (l.next) link(l.next,c,l).prev = c;
}

//
//    List descriptor for queue of objects of type C.
//
template <class C>
struct Queue : DLL<C> {
  C * tail;

  Link<C> & tail_link(C * c,Link<C> & l) {
    return link(tail,c,l);
  }

  // push objects onto the DLL

  void push(C * c) { push(c,*(Link<C>*)&c->link); }
  void push(C * c,Link<C> & l) {
    DLL<C>::push(c,l);
    if (!tail) tail = head;
  }

  // pop objects off the DLL

  C * pop() { 
    C * ret = DLL<C>::pop();
    if (!head) tail = NULL;
    return ret;
  }
  C * pop(C * c,Link<C> & l) {
    C * ret = DLL<C>::pop(c,l);
    if (!head) tail = NULL;
    return ret;
  }

  // enqueue object <c> at end of the Queue

  void enqueue(C * c) { enqueue(c,*(Link<C>*)&c->link); }
  void enqueue(C * c,Link<C> & l);

  void remove(C * c) { remove(c,*(Link<C>*)&c->link);  }
  void remove(C * c,Link<C> & l);

  void insert(C *c,C *after) { insert(c,*(Link<C>*)&c->link,after); }
  void insert(C *c,Link<C> &l,C *after) {
    DLL<C>::insert(c,l,after);
    if (!tail) 
      tail = head;
    else if (tail == after)
      tail = c;
  }

  void append(Queue<C> q) { append(q,*(Link<C>*)&q.head->link); }
  void append(Queue<C> q, Link<C> & l) {
    if (!head) {
      head = q.head;
      tail = q.tail;
    } else {
      if (q.head) {
    link(tail,q.head,l).next = q.head;
    link(q.head,q.head,l).prev = tail;
    tail = q.tail;
      }
    }
  }

  void append(DLL<C> q) { append(q,*(Link<C>*)&q.head->link); }
  void append(DLL<C> q, Link<C> & l) {
    C *qtail = q.head;
    if (qtail) 
      while (link(qtail,q.head,l).next)
    qtail = link(qtail,q.head,l).next;
    if (!head) {
      head = q.head;
      tail = qtail;
    } else {
      if (q.head) {
    link(tail,q.head,l).next = q.head;
    link(q.head,q.head,l).prev = tail;
    tail = qtail;
      }
    }
  }

  // dequeue the object from the front of the Queue

  C * dequeue();
  C * dequeue(C * c,Link<C> & l);

  bool in(C * c) { return DLL<C>::in(c); }
  void clear() { head = NULL; tail = NULL; }

  Queue() : tail(NULL) {}
};

template <class C> inline void Queue<C>::enqueue(C * c,Link<C> & l)
{
  C* ctail = (C*) tail;
  if (ctail) insert(c,l, ctail);
  else { 
    LogAssert(!head);
    DLL<C>::push(c,l);
  }
  tail = c;
}

template <class C> inline void Queue<C>::remove(C * c,Link<C> & l)
{
  if (c==tail) tail = l.prev;
  DLL<C>::remove(c,l);
}

template <class C> inline C * Queue<C>::dequeue()
{
  C* chead = (C*) head;
  if (chead) return dequeue(chead,*(Link<C>*)&chead->link);
  else return NULL;
}

template <class C> inline C * Queue<C>::dequeue(C * c,Link<C> & l)
{
  C* chead = (C*) head;
  if (!chead) return NULL;
  remove(chead,link(chead,c,l));
  return (C*) chead;
}

} // namespace RSLibImpl
