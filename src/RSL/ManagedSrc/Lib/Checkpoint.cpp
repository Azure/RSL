/*
.........1.........2.........3.........4.........5.........6.........7.........
RSLStateMachineWrapper.cpp
This is a very simple wrapper for the RSL state machine, it is going to be
used by the ManagedRSLLib managed class to interact with the RSL.
*/

#include "ManagedRSLib.h"
#include "rsl.h"


using namespace System::Runtime::InteropServices;
using namespace ManagedRSLib;

ManagedRSLCheckpointStream::ManagedRSLCheckpointStream(RSLCheckpointStreamReader *reader)
{
    m_oReader = reader;
    m_llPos = 0;
    m_oWriter = NULL;
    m_oReaderThisWasCreatedFrom = nullptr;
    m_fileName = nullptr;

    if (reader != NULL)
    {
        char * filename = reader->GetFileName();
        if (filename != NULL)
        {
            m_fileName = gcnew String(filename);
        }
    }
}

void
ManagedRSLCheckpointStream::Close()
{
    Flush();

    if (m_oReaderThisWasCreatedFrom != nullptr)
    {
        if (m_oReaderThisWasCreatedFrom->m_oReader == NULL)
        {
            String^ msg = String::Format("readerStr should be created as 'read'");
            throw gcnew ArgumentException(msg);
        }
        if (m_fileName == nullptr)
        {
            String^ msg = String::Format("fileName should not be null");
            throw gcnew ArgumentNullException(msg);
        }

        IntPtr pStr = Marshal::StringToHGlobalAnsi(m_fileName);

        m_oReaderThisWasCreatedFrom->m_oReader->CloseWriterForThisReader((PSTR)pStr.ToPointer(), m_oWriter);
        m_oWriter = NULL;

        Marshal::FreeHGlobal(pStr);

        m_oReaderThisWasCreatedFrom = nullptr;
    }

    if (m_oWriter != NULL)
    {
        m_oWriter->Close();
        m_oWriter = NULL;
    }

    if (m_oReader != NULL)
    {
        m_oReader = NULL;
    }
    m_fileName = nullptr;
}

ManagedRSLCheckpointStream::ManagedRSLCheckpointStream(System::String^ fileName, bool reader, ManagedRSLCheckpointStream^ readerStr)
{
    if (reader)
    {
        String^ msg = String::Format("Can only create ManagedRSLCheckpointStream for writing");
        throw gcnew ArgumentException(msg);
    }

    if (readerStr == nullptr)
    {
        String^ msg = String::Format("readerStr");
        throw gcnew ArgumentNullException(msg);
    }

    if (fileName == nullptr)
    {
        String^ msg = String::Format("fileName");
        throw gcnew ArgumentNullException(msg);
    }

    if (readerStr->m_oReader == NULL)
    {
        String^ msg = String::Format("readerStr should be created as 'read'");
        throw gcnew ArgumentException(msg);
    }

    IntPtr pStr = Marshal::StringToHGlobalAnsi(fileName);

    m_fileName = fileName;
    m_oReaderThisWasCreatedFrom = readerStr;
    m_oWriter = readerStr->m_oReader->CreateWriterForThisReader((PSTR)pStr.ToPointer());
    m_llPos = 0;
    m_oReader = NULL;

    Marshal::FreeHGlobal(pStr);

    if (m_oWriter == NULL)
    {
        String^ msg = String::Format("Can not open given file");
        throw gcnew ArgumentException(msg);
    }
}

ManagedRSLCheckpointStream::ManagedRSLCheckpointStream(System::String^ fileName, bool reader)
{
    if (!reader)
    {
        String^ msg = String::Format("Can only create ManagedRSLCheckpointStream for reading");
        throw gcnew ArgumentException(msg);
    }

    IntPtr pStr = Marshal::StringToHGlobalAnsi(fileName);

    m_oReader = new RSLCheckpointStreamReader();
    m_llPos = 0;
    m_oWriter = NULL;
    m_oReaderThisWasCreatedFrom = nullptr;
    m_fileName = fileName;

    int res = m_oReader->Init((PSTR)pStr.ToPointer());

    Marshal::FreeHGlobal(pStr);

    if (res != 0)
    {
        String^ msg = String::Format("Can not open given file");
        throw gcnew ArgumentException(msg);
    }
}

ManagedRSLCheckpointStream::ManagedRSLCheckpointStream(RSLCheckpointStreamWriter *writer)
{
    m_oWriter = writer;
    m_llPos = 0;
    m_oReader = NULL;
    m_oReaderThisWasCreatedFrom = nullptr;
    m_fileName = nullptr;
}

int
ManagedRSLCheckpointStream::Read(array<unsigned char>^ gc_buffer, int offset, int count)
{
    if (m_oReader == NULL)
    {
        throw gcnew NotSupportedException();
    }

    if (gc_buffer == nullptr)
    {
        throw gcnew ArgumentNullException("gc_buffer is null");
    }

    if (offset < 0 || count < 0)
    {
        throw gcnew ArgumentOutOfRangeException();
    }

    pin_ptr<unsigned char> pBuffer = &gc_buffer[offset];

    DWORD totalRead = 0;
    DWORD errorCode;

    //Read the data
    errorCode = m_oReader->GetData(pBuffer, count, &totalRead);
    if (errorCode == ERROR_SUCCESS || errorCode == ERROR_HANDLE_EOF)
    {
        m_llPos += totalRead;
        return totalRead;
    }

    throw gcnew IOException("I/O operation failed", HRESULT_FROM_WIN32(errorCode));
}

int ManagedRSLCheckpointStream::ReadByte()
{
    if (m_oReader == NULL)
    {
        throw gcnew NotSupportedException();
    }

    unsigned char data = (unsigned char)-1;
    DWORD totalRead = (DWORD)-1;
    DWORD errorCode;

    //Read the data
    errorCode = m_oReader->GetData(&data, 1, &totalRead);
    if (errorCode == ERROR_SUCCESS || errorCode == ERROR_HANDLE_EOF)
    {
        m_llPos += totalRead;
        return data;
    }

    throw gcnew IOException("Read failed", HRESULT_FROM_WIN32(errorCode));
}

long long ManagedRSLCheckpointStream::Position::get()
{
    return m_llPos;
}

System::String^ ManagedRSLCheckpointStream::FileName::get()
{
    return m_fileName;
}

void ManagedRSLCheckpointStream::Position::set(long long value)
{
    this->Seek(value, SeekOrigin::Begin);
}

long long ManagedRSLCheckpointStream::Length::get()
{
    if (m_oReader != NULL)
    {
        return m_oReader->Size();
    }
    else
    {
        return m_llPos;
    }
}

bool ManagedRSLCheckpointStream::CanRead::get()
{
    return (m_oReader != NULL);
}

bool ManagedRSLCheckpointStream::CanSeek::get()
{
    return (m_oReader != NULL);
}

bool ManagedRSLCheckpointStream::CanWrite::get()
{
    return (m_oWriter != NULL);
}

bool ManagedRSLCheckpointStream::CanTimeout::get()
{
    return false;
}

long long ManagedRSLCheckpointStream::Seek(long long offset, SeekOrigin origin)
{
    switch (origin)
    {
    case SeekOrigin::Current:
        origin = SeekOrigin::Begin;
        offset += m_llPos;
        break;
    case SeekOrigin::End:
        origin = SeekOrigin::Begin;
        offset = this->Length - offset;
        break;
    }

    if (m_oReader == NULL)
    {
        throw gcnew NotSupportedException();
    }

    if (offset > this->Length)
    {
        offset = this->Length;
    }

    if (offset < 0)
    {
        offset = 0;
    }

    if (offset != this->Position)
    {
        m_oReader->Seek((unsigned long long)offset);

        m_llPos = offset;
    }

    return offset;
}

void ManagedRSLCheckpointStream::SetLength(long long /*value*/)
{
    throw gcnew NotSupportedException();
}

void ManagedRSLCheckpointStream::Write(array<unsigned char>^ gc_buffer, int offset, int count)
{
    if (m_oWriter == NULL)
    {
        throw gcnew NotSupportedException();
    }

    if (gc_buffer == nullptr)
    {
        throw gcnew ArgumentNullException("gc_buffer is null");
    }

    if (offset < 0 || count < 0)
    {
        throw gcnew ArgumentOutOfRangeException();
    }
    //Allocate enough memory for the data to be written
    pin_ptr<unsigned char> pBuffer = &gc_buffer[offset];
    DWORD errorCode;

    //Write the buffer
    errorCode = m_oWriter->Write(pBuffer, count);
    if (errorCode == NO_ERROR)
    {
        m_llPos += count;
    }
    else
    {
        throw gcnew IOException("Write failed", HRESULT_FROM_WIN32(errorCode));
    }
}

void ManagedRSLCheckpointStream::WriteByte(unsigned char value)
{
    if (m_oWriter == NULL)
    {
        throw gcnew NotSupportedException();
    }

    DWORD errorCode;

    //Write the buffer
    errorCode = m_oWriter->Write(&value, 1);
    if (errorCode == NO_ERROR)
    {
        m_llPos += 1;
    }
    else
    {
        throw gcnew IOException("Write failed", HRESULT_FROM_WIN32(errorCode));
    }
}

void ManagedRSLCheckpointStream::Flush()
{
}
