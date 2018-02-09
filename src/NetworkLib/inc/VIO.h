#pragma once

#include "Action.h"
#include "NetBuffer.h"
#include "Ptr.h"

namespace RSLibImpl
{

class NetVConnection;

/////////////////////////////////////////////////////////////
//
// class VIO
//
///////////////////////////////////////////////////////////// 
class VIO 
{
public:

  static const INT64 MAX_NBYTES = ((INT64)1) << 62;

  ~VIO() {}
  ///////////////////////////////
  // interface for VConnection //
  // who owns this handle.     //
  ///////////////////////////////

  VIO(int aop);
  VIO();

  void SetOp(int op);
  int  GetOp();
    
  void SetNBytes(INT64 nbytes);
  void SetNDone(INT64 ndone);
  void AddNBytes(INT64 nbytes);
  void AddNDone(INT64 ndone);
  void SetVcServer(NetVConnection * vc);
  void SetEventHandler(EventHandler *cont);

  EventHandler   * GetEventHandler();
  INT64          GetNBytes();
  INT64          GetNDone();
  INT64          GetNTodo();
  INT64          NTodo();

  NetVConnection * GetVcServer();
  MSMutex          * GetMutex();

  /////////////////////
  // buffer settings //
  /////////////////////
  void SetBuffer(NetBuffer * buf);
  NetBuffer* GetBuffer();
  
  char *GetStart();
  char *GetEnd();
  
  char *GetBufEnd();
  int GetBufSize();
  void Consume(int bytes);
  void Fill(int bytes);
  
  int ReadAvail();
  int WriteAvail();
  int ContiguousReadAvail();
  int ContiguousWriteAvail();
  
  int GetAvail();
  int GetContiguousAvail();
  
  enum {
    NONE = 0, READ, WRITE
  };

public:
  Ptr<MSMutex>         m_Mutex;
  EventHandler       * m_Handler;
  NetVConnection   * m_Vc;

  int                  m_Op;
  INT64              m_NBytes;
  INT64              m_NDone;
  
  NetBuffer           * m_Buffer;
};

/////////////////////////////////////////////////////////////
//
//  VIO::VIO()
//
/////////////////////////////////////////////////////////////
inline VIO::VIO() :
  m_Mutex(0),
     m_Handler(NULL),
     m_Vc(NULL),
     m_Op(VIO::NONE),
     m_NBytes(0),
     m_NDone(0),
     m_Buffer(0)
{

    return;
}

inline void VIO::SetOp(int aop) { m_Op = aop; } 
inline int VIO::GetOp() { return m_Op; }

inline void VIO::SetNBytes(INT64 anbytes) { m_NBytes = anbytes; }
inline void VIO::AddNBytes(INT64 anbytes) { m_NBytes += anbytes; }
inline void VIO::SetNDone(INT64 andone) { m_NDone = andone; }
inline void VIO::AddNDone(INT64 andone) { m_NDone += andone; }
inline void VIO::SetVcServer(NetVConnection * vc) { m_Vc = vc; }
inline void VIO::SetEventHandler(EventHandler *cont) { m_Handler = cont; }

inline EventHandler * VIO::GetEventHandler() { return m_Handler; }
inline INT64 VIO::GetNBytes() { return m_NBytes; }
inline INT64 VIO::GetNDone() { return m_NDone; }
inline INT64 VIO::GetNTodo() { return (m_NBytes - m_NDone); }
inline INT64 VIO::NTodo() { return GetNTodo(); }

inline NetVConnection * VIO::GetVcServer() { return m_Vc; }
inline MSMutex * VIO::GetMutex() { return m_Mutex; }

inline void VIO::SetBuffer(NetBuffer * buf) { m_Buffer = buf; }
inline NetBuffer* VIO::GetBuffer() { return m_Buffer; }

inline char * VIO::GetStart() { return m_Buffer->GetStart(); }
inline char * VIO::GetEnd() { return m_Buffer->GetEnd(); }
inline char * VIO::GetBufEnd() { return (m_Buffer->GetBufEnd()); }
inline int VIO::GetBufSize() { return m_Buffer->GetBufSize(); }
inline void VIO::Consume(int bytes) { m_Buffer->Consume(bytes); }
inline void VIO::Fill(int bytes) { m_Buffer->Fill(bytes); }
 
inline int VIO::ReadAvail() { return m_Buffer->ReadAvail(); }
inline int VIO::WriteAvail() { return m_Buffer->WriteAvail(); }
inline int VIO::ContiguousReadAvail() { 
  return m_Buffer->ContiguousReadAvail();
}

inline int VIO::ContiguousWriteAvail() {
  return m_Buffer->ContiguousWriteAvail();
}

inline int VIO::GetAvail() {
  if (m_Op == VIO::READ) return WriteAvail();
  return ReadAvail();
}

inline int VIO::GetContiguousAvail() {
  if (m_Op == VIO::READ) return ContiguousWriteAvail();
  return ContiguousReadAvail();
}

} // namespace RSLibImpl;
