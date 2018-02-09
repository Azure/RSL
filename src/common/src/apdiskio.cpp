#include <windows.h> 
#include <strsafe.h>
#include <apdiskio.h>

using namespace RSLibImpl;

// Call CancelIoEx to attempt to cancel unwanted disk IO before waiting for its completion.
bool APSEQREAD::s_cancelDiskIo = true;

/*************************************************************************
**  Method: APSEQREAD::APSEQREAD
**
**  Description: Constructs a APSEQREAD object
**
**************************************************************************/
APSEQREAD::APSEQREAD()
{
    memset(m_rgReads, 0, sizeof(m_rgReads));
    m_hfile = INVALID_HANDLE_VALUE;
    m_cbLeft = 0;
    m_pbRead = NULL;
    m_cbSkip = 0;
    m_rgb = NULL;
}

/*************************************************************************
**  Method: APSEQREAD::~APSEQREAD
**
**  Description: Destructs and frees memory for a APSEQREAD object
**
**************************************************************************/
APSEQREAD::~APSEQREAD() 
{
    // Cancel any outstanding reads and wait for their completions (so that
    // it is safe to deallocate the associated OVERLAPPED structures).
    CancelReads();

    if (m_hfile != INVALID_HANDLE_VALUE)
    {
        CloseHandle(m_hfile);
    }

    for (int iRead=0; iRead< c_maxReads; iRead++)
    {
        if (m_rgReads[iRead].m_overlap.hEvent)
            CloseHandle(m_rgReads[iRead].m_overlap.hEvent);

        if (m_rgReads[iRead].m_pbRead)
            VirtualFree(m_rgReads[iRead].m_pbRead, 0, MEM_RELEASE);
    }

    if (m_rgb)
    {
        VirtualFree(m_rgb, 0, MEM_RELEASE);
        m_rgb = NULL;
    }
}

/*************************************************************************
**  Method: APSEQREAD::DoInit
**
**  Description: Initializes the object.  Allocate memory, open file and 
**      issue initial reads.
**
**  Paramaters:
**      szFile          name of file to open
**      maxReads        max number of outstanding reads
**
**  Returns:
**      NO_ERROR=0      
**      ERROR_INVAID_PARAMTER 
**      ERROR_OUTOFMEMORY
**
**  Notes:
**
**************************************************************************/
DWORD32 
APSEQREAD::DoInit(
                const CHAR*       szFile,
                int         maxReads,
                DWORD32     readBufSize,
                BOOL        readwrite)
{
    DWORD32     ec = NO_ERROR;
    int         iRead;

    this->m_readBufSize = readBufSize;
    // initialize params based on passed values
    //
    if ((maxReads <= APSEQREAD::c_maxReads) && (maxReads > 1))
        m_numReads = maxReads;
    else
    {
        ec = ERROR_INVALID_PARAMETER;
        goto Done;
    }

    // allocate memory for the temp buffer

    m_rgb = (BYTE *)VirtualAlloc(NULL, c_cbReadCopy, MEM_COMMIT, PAGE_READWRITE);
    if (m_rgb == NULL)
    {
        ec = GetLastError();
        goto Done;
    }

    // allocate events and memory for each io buffer
    //
    for (iRead=0; iRead< m_numReads; iRead++)
    {
        if ((m_rgReads[iRead].m_overlap.hEvent = 
            CreateEvent(NULL, FALSE, FALSE, NULL)) == NULL)
        {
            ec = GetLastError();
            goto Done;
        }

        if ((m_rgReads[iRead].m_pbRead = 
            VirtualAlloc(NULL, m_readBufSize, MEM_COMMIT, PAGE_READWRITE)) == NULL)
        {
            ec = GetLastError();
            goto Done;
        }
        m_rgReads[iRead].m_fWait=FALSE;
    }

    // open file
    //
    m_hfile = CreateFileA(szFile, FILE_READ_DATA,
                          readwrite ? FILE_SHARE_READ | FILE_SHARE_DELETE | FILE_SHARE_WRITE
                                    : FILE_SHARE_READ | FILE_SHARE_DELETE,
                          NULL, 
                          OPEN_EXISTING, 
                          FILE_FLAG_NO_BUFFERING | FILE_FLAG_OVERLAPPED, NULL);

    if (m_hfile == INVALID_HANDLE_VALUE)
    {
        ec = GetLastError();
        Log(LogID_Common, LogLevel_Error, "DISKIO",
            "error code - %u", ec);
        goto Done;          
    }

    // get size of file
    //
    m_size.dw.low = GetFileSize(m_hfile, &m_size.dw.high);
    if (m_size.dw.low == INVALID_FILE_SIZE)
    {
        ec = GetLastError();
        Log(LogID_Common, LogLevel_Error, "DISKIO",
            "error code - %u", ec);
        if (ec)
            goto Done;
    }

    ec = Reset();
Done:
    return ec;
}

/*************************************************************************
**  Method: APSEQREAD::IssueRead
**
**  Description: Issues a read for the next position and advance position. 
**
**  Paramaters:
**      pread           read block to use for read
**
**  Returns:
**      NO_ERROR=0      
**      ERROR_HANDLE_EOF 
**      GetLastError()  if read fails
**
**  Notes:
**
**************************************************************************/
DWORD32 
APSEQREAD::IssueRead(READBUFFER *pread)
{
    // check to see if we have passed the end of the file
    //
    pread->m_fNotEof = FALSE;
    pread->m_fWait=FALSE;
    if (m_offsetNext.ddw >= m_size.ddw)
    {
        return ERROR_HANDLE_EOF;
    }

    // setup read postion
    //
    pread->m_overlap.Offset = m_offsetNext.dw.low;
    pread->m_overlap.OffsetHigh = m_offsetNext.dw.high;

    // read data
    //
    if (!ReadFile(m_hfile, pread->m_pbRead, m_readBufSize, 
        &pread->m_cbRead, &pread->m_overlap))
    {
        if (GetLastError() == ERROR_IO_PENDING)
        {
            pread->m_fWait=TRUE;
        }
        else
        {
            DWORD ec = GetLastError(); 
            Log(LogID_Common, LogLevel_Error, "DISKIO",
                "error code - %u", ec);
            return ec;
        }
    }

    // this was not past the end of the file
    //
    pread->m_fNotEof = TRUE;

    // setup for next read
    //
    m_offsetNext.ddw += m_readBufSize;

    return NO_ERROR;
}


/*************************************************************************
**  Method: APSEQREAD::ReadNext
**
**  Description: Retrieves the next buffer and the number of bytes read. 
**
**  Paramaters:
**      ppbRead     pointer to return pointer buffer
**      pcbRead     pointer to return count of bytes set to 0 at eof
**
**  Returns:
**      NO_ERROR=0      
**
**  Notes:
**      PcbRead should only be different than the read buf size at the end
**      of the file.
**
**************************************************************************/
DWORD32 
APSEQREAD::ReadNext(BYTE** ppbRead, DWORD *pcbRead)
{
    READBUFFER *    pread;

    if (m_iRead >= 0)
    {
        DWORD32     ec = NO_ERROR;

        // issue read for previous block since we are done with it
        //
        ec = IssueRead(m_rgReads+m_iRead);
        if (ec != ERROR_HANDLE_EOF && ec != NO_ERROR)
        {
            return ec;
        }

        // advance the read postion
        m_iRead++;
        if (m_iRead >= m_numReads)
            m_iRead = 0;
    }
    else
    {
        // m_iRead will be < 0 first time through
        // there is no previous read to issue since
        // they were done at init time.
        m_iRead = 0;
    }

    pread = &m_rgReads[m_iRead];

    // see if we are past the end of file
    BYTE* pbReadRet = (BYTE*)pread->m_pbRead;
    if (!pread->m_fNotEof)
    {
        pread->m_cbRead = 0;
        pbReadRet = NULL;
    }
    else if (pread->m_fWait)
    {
        pread->m_fWait = FALSE;
        if (!GetOverlappedResult(m_hfile, &pread->m_overlap, 
            &pread->m_cbRead, TRUE))
        {
            DWORD ec = GetLastError(); 
            Log(LogID_Common, LogLevel_Error, "DISKIO",
                "error code - %u", ec);

            return ec;
        }
    }

    *pcbRead = pread->m_cbRead;
    *ppbRead = pbReadRet;
    return NO_ERROR;
}

/*************************************************************************
**  Method: APSEQREAD::Reset
**
**  Description: Resets the sequential read to begining of file and 
**      reissues the initial reads.
**
**  Paramaters:
**
**  Returns:
**      NO_ERROR=0      
**      GetLastError()  if read fails
**
**  Notes:
**
**************************************************************************/
DWORD32 
APSEQREAD::Reset(DWORD64 pos)
{
    DWORD32     ec = NO_ERROR;
    int         iRead;

    m_cbSkip = (DWORD)(pos%(DWORD64)m_readBufSize);
    m_offsetNext.ddw = pos - (DWORD64)m_cbSkip;

    // Make sure that there are no outstanding IOs
    //
    CancelReads();

    // do initial reads
    //
    for (iRead=0; iRead< m_numReads; iRead++)
    {
        ec = IssueRead(m_rgReads+iRead);
        if (ec != NO_ERROR)
        {
            if (ec == ERROR_HANDLE_EOF)
            {
                ec = NO_ERROR;
                break;
            }
            else
                goto Done;
        }
    }

    // setup read index
    //
    m_iRead = -1;

    m_cbLeft = 0;
    m_pbRead = NULL;    

Done:
    return ec;
}

/*************************************************************************
**  Method: APSEQREAD::CancelReads
**
**  Description: Cancels any outstanding reads and waits for their
**      completions (ignoring the results).
**
**************************************************************************/
void
APSEQREAD::CancelReads()
{
    for (int iRead=0; iRead< m_numReads; iRead++)
    {
        if (m_rgReads[iRead].m_fWait)
        {
            if (s_cancelDiskIo)
            {
                CancelIoEx(m_hfile, &m_rgReads[iRead].m_overlap);
            }
            GetOverlappedResult(m_hfile, &m_rgReads[iRead].m_overlap, &m_rgReads[iRead].m_cbRead, TRUE);
        }
    }
}

/*************************************************************************
**  Method: APSEQREAD::GetDataPointer
**
**  Description: Gets a pointer to bytes read.  When crossing read buffers
**      this pointer goes to a temp buffer.  Data is only copied when a 
**      read crosses the read buffers.
**
**  Paramaters:
**      ppv     returns pointer to buffer with bytes of data.
**      cb      number of bytes requested.
**
**  Returns:
**      NO_ERROR=0
**      ERROR_INVALID_PARAMETER
**      ERROR_HANDLE_EOF
**      GetLastError()  if read fails
**
**  Notes:
**
**************************************************************************/
DWORD32 
APSEQREAD::GetDataPointer( void ** ppv, DWORD cb, DWORD* pcbRead )
{
    DWORD32 ec = NO_ERROR;
    DWORD   cbSaved = 0;

    // If we have more left over data than the temp buffer size, we
    // can't use the temp buffer. If the user requested more than
    // left over data, just return the left over data.
    if (m_cbLeft < cb && m_cbLeft > c_cbReadCopy)
    {
        // if caller is not getting the bytes read then fail since
        // size returned will be different
        if (pcbRead == NULL)
        {
            return ERROR_INVALID_PARAMETER;
        }
        cb = m_cbLeft;
    }
    
    if (m_cbLeft < cb)
    {
        // save left over from previous buffer if needed
        //
        cbSaved = m_cbLeft;
        if (cbSaved > 0)
        {
            if (cb > c_cbReadCopy)
            {
                if (pcbRead == NULL)
                {
                    // if caller is not getting the bytes read then fail since
                    // size returned will be different
                    return ERROR_INVALID_PARAMETER;
                }
                
                // if we need to use the temp space then limit read size
                // to size of temp buffer
                cb = c_cbReadCopy;
            }
            
            // copy remaining data to internal buffer
            memcpy(m_rgb, m_pbRead, cbSaved);
        }

        // read next buffer
        //
        ec = ReadNext(&m_pbRead, &m_cbLeft);
        if (ec != NO_ERROR)
        {
            return ec;
        }

        // handle any skip setup duing reset
        //
        if (m_cbSkip)
        {
            m_cbLeft -=m_cbSkip;
            m_pbRead +=m_cbSkip;
            m_cbSkip = 0;
        }

        if ((m_cbLeft+cbSaved) < cb)
        {
            cb = m_cbLeft+cbSaved;
            if (!pcbRead || (cb==0))
                return ERROR_HANDLE_EOF;
        }

        if (pcbRead)
            *pcbRead = cb;

        if (cbSaved > 0)
        {
            // if there were saved bytes then we return a pointer to copy
            //
            m_cbLeft -= (cb-cbSaved);
            memcpy(m_rgb+cbSaved, m_pbRead, cb-cbSaved);
            *ppv = m_rgb;
            m_pbRead+= (cb-cbSaved);
            return NO_ERROR;
        }
    }

    if (pcbRead)
        *pcbRead = cb;
    *ppv = m_pbRead;
    m_pbRead+=cb;
    m_cbLeft -=cb;
    return NO_ERROR;
}


/*************************************************************************
**  Method: APSEQREAD::GetData
**
**  Description: Copies read bytes into passed buffer.
**
**  Paramaters:
**      pv      buffer to copy data into.
**      cb      number of bytes requested.
**
**  Returns:
**      NO_ERROR=0
**      errors from GetDataPointer
**
**  Notes:
**
**************************************************************************/
DWORD32 
APSEQREAD::GetData( void * pv,DWORD cb )
{
    DWORD32 ec = NO_ERROR;
    BYTE*   pb;

    ec = GetDataPointer((void**)&pb, cb);
    if (ec)
        return ec;
    memcpy(pv, pb, cb);

    return ec;
}

/*************************************************************************
**  Method: APSEQREAD::Skip
**
**  Description: Skips bytes in file.
**
**  Paramaters:
**      dwNumBytes      number of bytes to skip.
**
**  Returns:
**      NO_ERROR=0
**
**  Notes:
**
**************************************************************************/
DWORD32 
APSEQREAD::Skip(DWORD dwNumBytes)
{
    // if skip in current buffer then just advance m_cbLeft & m_pbRead
    //
    if (m_cbLeft > dwNumBytes)
    {
        m_cbLeft -= dwNumBytes;
        m_pbRead += dwNumBytes;
    }
    // if skip in next buffer then just advance the read
    //
    else if (dwNumBytes < m_readBufSize)
    {
        void *  pv;

        dwNumBytes -= m_cbLeft;
        m_cbLeft = 0;

        return GetDataPointer( &pv, dwNumBytes );
    }
    // skip is large and just reset to the new location
    //
    else
    {
        return Reset( m_offsetNext.ddw+m_cbLeft+dwNumBytes );
    }

    return NO_ERROR;
}

/*************************************************************************
**  Method: APSEQWRITE::APSEQWRITE
**
**  Description: Constructs a APSEQWRITE object
**
**************************************************************************/
APSEQWRITE::APSEQWRITE()
{
    memset(m_rgWrites, 0, sizeof(m_rgWrites));
    m_hfile = INVALID_HANDLE_VALUE;
    m_hfileSetEof = INVALID_HANDLE_VALUE;
    m_iWrite = 0;
    m_offsetNext.ddw=0;
    m_pbWrite=NULL;
    m_cbUsed = 0;
}

APSEQWRITE::~APSEQWRITE()
{
    this->DoDispose();
}


/*************************************************************************
**  Method: APSEQWRITE::DoDispose
**
**  Description: Destructs and frees memory for a APSEQWRITE object
**
**************************************************************************/
void APSEQWRITE::DoDispose() 
{
    if ((m_hfile != INVALID_HANDLE_VALUE) && (m_hfileSetEof != INVALID_HANDLE_VALUE))
    {
        Flush();
    }

    if (m_hfile != INVALID_HANDLE_VALUE)
    {
        CloseHandle(m_hfile);
        m_hfile = INVALID_HANDLE_VALUE;
    }

    if (m_hfileSetEof != INVALID_HANDLE_VALUE)
    {
        CloseHandle(m_hfileSetEof);
        m_hfileSetEof = INVALID_HANDLE_VALUE;
    }

    for (int iWrite=0; iWrite< c_maxWrites; iWrite++)
    {
        if (m_rgWrites[iWrite].m_overlap.hEvent)
        {
            CloseHandle(m_rgWrites[iWrite].m_overlap.hEvent);
            m_rgWrites[iWrite].m_overlap.hEvent = NULL;
        }

        if (m_rgWrites[iWrite].m_pb)
        {
            VirtualFree(m_rgWrites[iWrite].m_pb, 0, MEM_RELEASE);
            m_rgWrites[iWrite].m_pb = NULL;
        }
    }
}

/*************************************************************************
**  Method: APSEQWRITE::DoInit
**
**  Description: Initializes the object.  Allocate memory, open file and 
**      prepare for writing.
**
**  Paramaters:
**      szFile          name of file to open
**      cbWrite         size of each write buffer
**      maxWrites       max number of writes
**
**  Returns:
**      NO_ERROR=0      
**      ERROR_INVAID_PARAMTER 
**      ERROR_OUTOFMEMORY
**
**  Notes:
**      File is always created and will overwrite existing files.
**
**************************************************************************/
DWORD32 
APSEQWRITE::DoInit(
                 const CHAR*       szFile,
                 int         cbWrite,
                 int         maxWrites)
{
    DWORD32     ec = NO_ERROR;
    int         iWrite;

    // initialize params based on passed values
    //
    if ((maxWrites <= APSEQWRITE::c_maxWrites) && (maxWrites >= 1))
        m_numWrites = maxWrites;
    else
    {
        ec = ERROR_INVALID_PARAMETER;
        goto Done;
    }

    m_cbBufSize = cbWrite;

    // allocate events and memory for each io buffer
    //
    for (iWrite=0; iWrite< m_numWrites; iWrite++)
    {
        if ((m_rgWrites[iWrite].m_overlap.hEvent = 
            CreateEvent(NULL, FALSE, FALSE, NULL)) == NULL)
        {
            ec = ERROR_OUTOFMEMORY;
            goto Done;
        }

        if ((m_rgWrites[iWrite].m_pb = 
            VirtualAlloc(NULL, m_cbBufSize, MEM_COMMIT, PAGE_READWRITE)) == NULL)
        {
            ec = ERROR_OUTOFMEMORY;
            goto Done;
        }
        m_rgWrites[iWrite].m_fWait=FALSE;
    }

    // open file
    //
    m_hfile = CreateFileA(szFile,
        FILE_WRITE_DATA | FILE_WRITE_ATTRIBUTES,
        FILE_SHARE_READ|FILE_SHARE_WRITE, 
        NULL, 
        OPEN_ALWAYS, 
        FILE_FLAG_NO_BUFFERING | FILE_FLAG_OVERLAPPED, NULL);

    if (m_hfile == INVALID_HANDLE_VALUE)
    {
        ec = GetLastError();
        Log(LogID_Common, LogLevel_Error, "DISKIO",
            "error code - %u", ec);

        goto Done;          
    }

    // open file handle to use for setting end of file
    //
    m_hfileSetEof = CreateFileA(szFile, FILE_READ_DATA | FILE_WRITE_DATA, 
        FILE_SHARE_READ|FILE_SHARE_WRITE, 
        NULL, 
        OPEN_EXISTING, 
        0, 
        NULL);

    if (m_hfileSetEof == INVALID_HANDLE_VALUE)
    {
        ec = GetLastError();
        Log(LogID_Common, LogLevel_Error, "DISKIO",
            "error code - %u", ec);
        goto Done;          
    }

Done:
    return ec;
}

/*************************************************************************
**  Method: APSEQWRITE::IssueWrite
**
**  Description: Issues a write for a write buffer. 
**
**  Paramaters:
**      pwrite          write block to use for write
**
**  Returns:
**      NO_ERROR=0      
**      GetLastError()  if read fails
**
**  Notes:
**
**************************************************************************/
DWORD32 
APSEQWRITE::IssueWrite(WRITEBUFFER *pwrite)
{
    // setup read postion
    //
    pwrite->m_overlap.Offset = m_offsetNext.dw.low;
    pwrite->m_overlap.OffsetHigh = m_offsetNext.dw.high;

    // write data
    //
    if (!WriteFile(m_hfile, pwrite->m_pb, m_cbBufSize, 
        &pwrite->m_cb, &pwrite->m_overlap))
    {
        if (GetLastError() == ERROR_IO_PENDING)
        {
            pwrite->m_fWait=TRUE;
        }
        else
        {
            DWORD ec = GetLastError(); 
            Log(LogID_Common, LogLevel_Error, "DISKIO",
                "error code - %u", ec);

            return ec;
        }
    }

    return NO_ERROR;
}

/*************************************************************************
**  Method: APSEQWRITE::PrepareNext
**
**  Description: Prepare for next write.  Issue previous write if needed.
**      advance the location and wait for a buffer for next writes.
**
**  Paramaters:
**      pwrite          write block to use for write
**
**  Returns:
**      NO_ERROR=0      
**      GetLastError()  if read fails
**
**  Notes:
**
**************************************************************************/
DWORD32 
APSEQWRITE::PrepareNext()
{
    DWORD32         ec = NO_ERROR;
    WRITEBUFFER*    pwrite;

    if (m_pbWrite)
    {
        m_rgWrites[m_iWrite].m_cb = m_cbUsed;
        ec = IssueWrite(m_rgWrites+m_iWrite);
        if (ec)
            return ec;

        // setup for next write
        //
        m_offsetNext.ddw += m_cbUsed;
    }

    m_iWrite++;
    if (m_iWrite >= m_numWrites)
        m_iWrite = 0;
    pwrite = &m_rgWrites[m_iWrite];

    if (pwrite->m_fWait)
    {
        pwrite->m_fWait = FALSE;

		ec = GetOverlappedResultAndCheckSize(
			m_hfile, 
			&pwrite->m_overlap, 
			m_cbBufSize,
			&pwrite->m_cb, 
			TRUE);

        if (NO_ERROR != ec )
        {
            Log(LogID_Common, LogLevel_Error, "DISKIO",
                "error code - %u", ec);
            return ec;
        }
    }

    m_pbWrite = (BYTE*)pwrite->m_pb;
    m_cbUsed = 0;
    return ec;
}

DWORD 
APSEQWRITE::GetOverlappedResultAndCheckSize(
    HANDLE hFile, 
	LPOVERLAPPED lpOverlapped, 
	DWORD expedtedBytes, 
	LPDWORD lpWrittenBytes, 
	BOOL bWait)
{
    if (!GetOverlappedResult(hFile, lpOverlapped, 
        lpWrittenBytes, bWait))
    {
        return GetLastError();
    }

	if (*lpWrittenBytes != expedtedBytes)
	{
		return ERROR_DISK_FULL;
	}

	return NO_ERROR;
}

/*************************************************************************
**  Method: APSEQWRITE::WriteInternal
**
**  Description: Write bytes to internal buffer.  If buffer is full then
**      prepare for next write and copy any remaining bytes
**
**  Paramaters:
**      pbWrite         data to write
**      cbWrite         number of bytes of data to write
**
**  Returns:
**      NO_ERROR=0      
**      GetLastError()  if read fails
**
**  Notes:
**
**************************************************************************/
DWORD32 
APSEQWRITE::WriteInternal(const void* pbWrite, DWORD cbWrite)
{
    DWORD32 ec = NO_ERROR;

    if (cbWrite > m_cbBufSize)
        return ERROR_INVALID_PARAMETER;

    if (!m_pbWrite)
        PrepareNext();

    if (cbWrite > (m_cbBufSize-m_cbUsed))
    {
        int     cbUsed = m_cbBufSize-m_cbUsed;

        memcpy(m_pbWrite+m_cbUsed, pbWrite, cbUsed);
        m_cbUsed += cbUsed;
        ec = PrepareNext();
        if (!ec)
        {
            memcpy(m_pbWrite, (const BYTE*)pbWrite+cbUsed, cbWrite-cbUsed);
        }
        m_cbUsed+= (cbWrite-cbUsed);
    }
    else
    {
        memcpy(m_pbWrite+m_cbUsed, pbWrite, cbWrite);
        m_cbUsed+=cbWrite;
    }

    return ec;
}


/*************************************************************************
**  Method: APSEQWRITE::Write
**
**  Description: Calls WriteInternal for the right size buffers.
**
**  Paramaters:
**      pbWrite         data to write
**      cbWrite         number of bytes of data to write
**
**  Returns:
**      NO_ERROR=0      
**      GetLastError()  if read fails
**
**  Notes:
**
**************************************************************************/
DWORD32 
APSEQWRITE::Write(const void* pbWrite, DWORD cbWrite)
{
    DWORD32 ec = NO_ERROR;

    const BYTE* pCurr = (const BYTE*)pbWrite;

    while (cbWrite > this->m_cbBufSize)
    {
        ec = this->WriteInternal(pCurr, this->m_cbBufSize);
        if (NO_ERROR != ec)
        {
            return ec;
        }
        pCurr += this->m_cbBufSize;
        cbWrite -= this->m_cbBufSize;
    }
    ec = this->WriteInternal(pCurr, cbWrite);
    if (NO_ERROR != ec)
    {
        return ec;
    }

    return ec;
}

/*************************************************************************
**  Method: APSEQWRITE::RandomWrite
**
**  Description: Write bytes to a random location in file.
**
**  Paramaters:
**      pbWrite         data to write
**      cbWrite         number of bytes of data to write
**
**  Returns:
**      NO_ERROR=0      
**      GetLastError()  if read fails
**
**  Notes:
**      Performance of this call will NOT be good. All pending writes
**      are flushed and the IO is performed on an buffered file handle.
**      This will typically require a read in addition to the write.
**
**************************************************************************/
DWORD32 
APSEQWRITE::RandomWrite(DWORD64 offset, const void* pbWrite, DWORD cbWrite)
{
    DDWORD  pos;
    DWORD   cbWritten;
    DWORD32 ec = NO_ERROR;

    if (offset+cbWrite >= m_offsetNext.ddw)
    {
        // write too close to the current end of file
        return ERROR_INVALID_PARAMETER;
    }

    Flush();

    pos.ddw = offset;

    if (SetFilePointer(m_hfileSetEof, pos.dw.low, 
        (PLONG)&pos.dw.high, FILE_BEGIN)
        == INVALID_SET_FILE_POINTER)
    {
        ec = GetLastError();
        Log(LogID_Common, LogLevel_Error, "DISKIO",
            "error code - %u", ec);

        if (ec)
            return ec;
    }

    if (!WriteFile(m_hfileSetEof, pbWrite, cbWrite, &cbWritten, NULL))
    {
        ec = GetLastError();
        Log(LogID_Common, LogLevel_Error, "DISKIO",
            "error code - %u", ec);

    }

    return ec;
}


/*************************************************************************
**  Method: APSEQWRITE::Print
**
**  Description: Print formatted data into an internal buffer and then
**      write the data.
**
**  Paramaters:
**      szFormat        format string
**      ...             params for print statement
**
**  Returns:
**      NO_ERROR=0      
**      GetLastError()  if read fails
**
**  Notes:
**
**************************************************************************/
DWORD32
APSEQWRITE::Print(const char* szFormat, ...)
{
    char    rgch[c_cbWriteMax + 1];
    char*   end = NULL;
    DWORD   cb = 0;
    va_list args;
    va_start(args, szFormat);

    HRESULT hr = StringCchVPrintfExA(
        rgch, 
        NUMELEM(rgch), 
        &end, 
        NULL, 
        0, 
        szFormat, 
        args);

    if (FAILED(hr))
    {
        return ERROR_INVALID_PARAMETER;
    }

    cb = (DWORD) (end - rgch);

    return Write(rgch, cb);
}

/*************************************************************************
**  Method: APSEQWRITE::Flush
**
**  Description: Issue writes for any data not written, wait for writes
**      to complete and adjust the size of the file if it is not a 
**      multiple of the write size.
**
**  Paramaters:
**
**  Returns:
**      NO_ERROR=0      
**      GetLastError()  if read fails
**
**  Notes:
**
**************************************************************************/
DWORD32 
APSEQWRITE::Flush()
{
    DWORD32         ec = NO_ERROR;
    WRITEBUFFER*    pwrite;
    int             i;

    // write data in current buffer
    // 
    if (m_pbWrite && m_cbUsed)
    {
        m_rgWrites[m_iWrite].m_cb = m_cbUsed;
        ec = IssueWrite(m_rgWrites+m_iWrite);
        if (ec)
            return ec;
    }

    // make sure that all writes have completed
    //
    for (i=0; i< m_numWrites; i++)
    {
        pwrite = &m_rgWrites[i];

        if (pwrite->m_fWait)
        {
            pwrite->m_fWait = FALSE;

			ec = GetOverlappedResultAndCheckSize(
				m_hfile, 
				&pwrite->m_overlap, 
				m_cbBufSize,
				&pwrite->m_cb, 
				TRUE);

			if (NO_ERROR != ec )
			{
				Log(LogID_Common, LogLevel_Error, "DISKIO",
					"error code - %u", ec);
				return ec;
			}
        }
    }

    // Long time ago, APSEQWRITE truncated files when opening them, and
    // only had to call explicit SetEndOfFile for odd-size files.
    // Now, APSEQWRITE does not truncate existing files to avoid
    // fragmentation, and SetEndOfFile should be called for files
    // of any sizes.

    DDWORD  len;

    len.ddw = m_offsetNext.ddw+(DWORD64)m_cbUsed;

    if (SetFilePointer(m_hfileSetEof, len.dw.low, (PLONG)&len.dw.high, FILE_BEGIN)
        == INVALID_SET_FILE_POINTER)
    {
        ec = GetLastError();
        Log(LogID_Common, LogLevel_Error, "DISKIO",
            "error code - %u", ec);

    }

    if (ec)
        return ec;

    if (!SetEndOfFile(m_hfileSetEof))
    {
        ec = GetLastError();
        Log(LogID_Common, LogLevel_Error, "DISKIO",
            "error code - %u", ec);

    }

    return ec;
}

/*************************************************************************
**  Method: APSEQWRITE::GetAvailable
**
**  Description: Some users of APSEQWRITE may avoid one round of memcpy by
**      writing their data directly to an available APSEQWRITE's buffer.
**      GetAvailable and CommitAvailable methods allow user to query
**      APSEQWRITE for an available buffer, and commit the data after writing it.
**
**  Paramaters:
**      ppbAvailable    pointer to return available buffer pointer
**      pcbAvailable    pointer to return available buffer size
**
**  Returns:
**      NO_ERROR=0      
**      GetLastError()  if fails
**
**  Notes:
**
**************************************************************************/
DWORD32 
APSEQWRITE::GetAvailable(void **ppbAvailable, DWORD *ccbAvailable)
{
    DWORD32 ec = NO_ERROR;
    if ((0 == m_pbWrite) || (m_cbBufSize == m_cbUsed ))
    {
        ec = PrepareNext();
    }
    *ppbAvailable = m_pbWrite + m_cbUsed;
    *ccbAvailable = m_cbBufSize - m_cbUsed;

    return ec;
}

/*************************************************************************
**  Method: APSEQWRITE::CommitAvailable
**
**  Description: Some users of APSEQWRITE may avoid one round of memcpy by
**      writing their data directly to an available APSEQWRITE's buffer.
**      GetAvailable and CommitAvailable methods allow user to query
**      APSEQWRITE for an available buffer, and commit the data after writing it.
**
**  Paramaters:
**      cbAvailable     number of bytes in available buffer to commit.
**
**  Returns:
**      NO_ERROR=0      
**      GetLastError()  if fails
**
**  Notes:
**
**************************************************************************/
DWORD32 
APSEQWRITE::CommitAvailable(DWORD cbAvailable)
{
    DWORD32 ec = NO_ERROR;

    if (cbAvailable > m_cbBufSize - m_cbUsed)
    {
        return ERROR_INVALID_PARAMETER;
    }
    
    m_cbUsed += cbAvailable;

    return ec;
}
