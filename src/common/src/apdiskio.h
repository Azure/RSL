#pragma once

#include "basic_types.h"
#include "logging.h"

namespace RSLibImpl
{

// READBUFFER struct containing data for a single read
//
class READBUFFER
{
public:
    OVERLAPPED  m_overlap;
    void *      m_pbRead;   
    DWORD       m_cbRead;
    BOOL        m_fWait;
    BOOL        m_fNotEof;
};

/*************************************************************************
**  Class: APSEQREAD
**
**  Description: Sequential file reader class.  Efficiently reads the file
**      sequentially using unbuffered and asynch I/O.  Can issue many reads 
**      in order to keep many disks busy and efficiently read the disk 
**      sequentially.
**
**************************************************************************/
class APSEQREAD
{
public:
    static const int        c_maxReads = 64;
    static const int        c_maxReadsDefault = 4;
    static const DWORD32    c_readBufSize = 64*1024;
    static const int        c_cbReadCopy = c_readBufSize;

private:
    int         m_numReads;
    int         m_iRead;
    READBUFFER  m_rgReads[c_maxReads];
    DDWORD      m_offsetNext;
    HANDLE      m_hfile;
    DDWORD      m_size;
    DWORD32     m_readBufSize;

    DWORD       m_cbLeft;
    BYTE*       m_pbRead;
    DWORD       m_cbSkip;
    BYTE        *m_rgb;

    static bool s_cancelDiskIo;

    DWORD32 IssueRead(READBUFFER *pread);
    DWORD32 ReadNext(BYTE** ppbRead, DWORD *pcbRead);
    void CancelReads();

public:
    APSEQREAD();
    ~APSEQREAD();

    DWORD32 DoInit(const CHAR* szFile, int maxReads = c_maxReadsDefault, DWORD32 readBufSize = c_readBufSize, BOOL readwrite = false);
    DWORD32 Reset(DWORD64 pos = 0);
    
    DWORD32 GetDataPointer( void ** ppvBuffer, DWORD dwNumBytes, DWORD* pcbRead=NULL );
    DWORD32 GetData( void * pvBuffer, DWORD dwNumBytes );
    DWORD32 Skip(DWORD dwNumBytes);

    DWORD64 FileSize()  {return m_size.ddw;}
    HANDLE  FileHandle() {return m_hfile;}

    static void SetCancelDiskIo(bool cancelDiskIo) { s_cancelDiskIo = cancelDiskIo; }
};


// WRITEBUFFER struct containing data for a single read
//
class WRITEBUFFER
{
public:
    OVERLAPPED  m_overlap;
    void *      m_pb;   
    DWORD       m_cb;
    BOOL        m_fWait;
};

/*************************************************************************
**  Class: APSEQWRITE
**
**  Description: Sequential file writer class.  Efficiently writes the file
**      sequentially using unbuffered and asynch I/O.  
**
**************************************************************************/
class APSEQWRITE
{
public:
    static const int        c_maxWrites = 64;
    static const int        c_maxWritesDefault = 2;
    static const DWORD32    c_writeBufSizeDefault = 128*1024;
    static const int        c_cbWriteMax = 1024;

private:
    DWORD32     m_cbBufSize;
    int         m_numWrites;
    int         m_iWrite;
    WRITEBUFFER m_rgWrites[c_maxWrites];
    BYTE*       m_pbWrite;
    DWORD       m_cbUsed;
    DDWORD      m_offsetNext;
    HANDLE      m_hfile;
    HANDLE      m_hfileSetEof;

    DWORD32 IssueWrite(WRITEBUFFER *pwrite);
    DWORD32 PrepareNext();
    DWORD32 WriteInternal(const void* pbWrite, DWORD cbWrite);

	DWORD GetOverlappedResultAndCheckSize(
		HANDLE hFile, 
		LPOVERLAPPED lpOverlapped, 
		DWORD expedtedBytes, 
		LPDWORD lpWrittenBytes, 
		BOOL bWait);

public:
    APSEQWRITE();
    ~APSEQWRITE();

    DWORD32 DoInit(const CHAR* szFile, 
                   int cbWrite = c_writeBufSizeDefault, 
                   int maxWrites = c_maxWritesDefault);
    void    DoDispose();
    DWORD32 Flush();
    DWORD32 Write(const void* pbWrite, DWORD cbWrite);
    DWORD32 RandomWrite(DWORD64 offset, const void* pbWrite, DWORD cbWrite);
    DWORD32 Print(const char* szFormat, ...);

    HANDLE  RandomFileHandle()  {return m_hfileSetEof;}
    HANDLE  FileHandle()  {return m_hfile;}
    UInt64  BytesIssued() {return m_cbUsed + m_offsetNext.ddw;}

    DWORD32 GetAvailable(void **ppbAvailable, DWORD *pcbAvailable);
    DWORD32 CommitAvailable(DWORD cbAvailable);
};



/*************************************************************************
**  Class: QueueInfo
**
**  Description: Methodless class just used to pass handle and file data.
**
**************************************************************************/
class QueueInfo
{
public:
    HANDLE              hFile;
    WIN32_FIND_DATAA    fileData;
};


/*************************************************************************
**  Class: ReadDiskQueue
**
**  Description: Manages a directory of files as a queue.
**
**  Notes:
**      It is not safe to have multiple readers for the same dir.
**      No locking is performed to ensure that the same file will not be
**      processed multiple times.  It is fine to have multiple writers
**      as long as the file names are unique.
**
**************************************************************************/
class ReadDiskQueue
{
private:
    static const int c_maxQueueInfo=32;// limits the number of threads that 
                                       // can process the queue
    static const int c_maxFind=128;    // num of file entries read in batch

    CRITSEC     m_critsec;
    char        m_rgchDirectory[MAX_PATH];
    char        m_rgchPattern[32];
    QueueInfo   m_rgQueueInfo[c_maxQueueInfo];
    int         m_cfind;
    int         m_ifind;
    WIN32_FIND_DATAA    m_rgfind[c_maxFind];

    DWORD32     ReadMoreFiles();
    
public:
    ReadDiskQueue();
    ~ReadDiskQueue();
    
    char *      Directory()     {return m_rgchDirectory;}
    
    DWORD32     Initialize(const char* szDirectory, const char* szPattern="*");
    // If sleepTime == ~(DWORD)0, the function returns ERROR_NO_MORE_FILES
    // when no files are available else it will sleep and wait for files
    DWORD32     GetNextFile(QueueInfo**ppqueueInfo, DWORD sleepTime=10000);
    DWORD32     CompleteFile(QueueInfo*pqueueInfo);
    DWORD32     CompleteFileChangeExt(QueueInfo*pqueueInfo, const char *newExt);
};

/*************************************************************************
**  Class: NameGenerator
**
**  Description: Used to generate unique names for files in a directory.
**
**************************************************************************/
class NameGenerator
{
private:
    char    m_rgchPrefix[MAX_PATH];
    char    m_rgchSuffix[32];
    long   m_num;

public:    
    NameGenerator();
    ~NameGenerator();

    DWORD32     Initialize(const char* szDirectory, const char *szSuffix, const bool fUseForwardSlash = false);
    void        GetName(char*szNewName);
};

} // namespace RSLibImpl

