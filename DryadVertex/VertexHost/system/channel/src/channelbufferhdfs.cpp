/*
Copyright (c) Microsoft Corporation

All rights reserved.

Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file except in 
compliance with the License.  You may obtain a copy of the License 
at http://www.apache.org/licenses/LICENSE-2.0   


THIS CODE IS PROVIDED *AS IS* BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, EITHER 
EXPRESS OR IMPLIED, INCLUDING WITHOUT LIMITATION ANY IMPLIED WARRANTIES OR CONDITIONS OF 
TITLE, FITNESS FOR A PARTICULAR PURPOSE, MERCHANTABLITY OR NON-INFRINGEMENT.  


See the Apache Version 2.0 License for specific language governing permissions and 
limitations under the License. 

*/

#include "channelbufferhdfs.h"
#include <Hadoop.h>

#include <process.h>

#pragma unmanaged

const char* RChannelBufferHdfsReader::s_hdfsPartitionPrefix = "hdfspt://";
const char* RChannelBufferHdfsReader::s_wasbPartitionPrefix = "wasbpt://";
const char* RChannelBufferHdfsWriter::s_hdfsFilePrefix = "hdfs://";
const char* RChannelBufferHdfsWriter::s_wasbFilePrefix = "wasb://";

static long s_readBufferSize = 2 * 1024 * 1024;
static long s_writeBufferSize = 256 * 1024;
static LONG s_maxBuffersOut = 4;
static UInt32 s_maxBuffersToBlockWriter = 4;

static bool
ExtractHdfsReadUri(DrStr64 uri,
                   DrStr64& schemeAndAuthority,
                   DrStr64& filePath, Int64* pOffsetStart, Int32* pLength)
{
    const char* cScheme;
    char* cAuthority;
    if (uri.StartsWith(RChannelBufferHdfsReader::s_hdfsPartitionPrefix))
    {
        cScheme = "hdfs://";
        cAuthority =
            uri.GetWritableBuffer(uri.GetLength(),
                                  strlen(RChannelBufferHdfsReader::s_hdfsPartitionPrefix));
    }
    else if (uri.StartsWith(RChannelBufferHdfsReader::s_wasbPartitionPrefix))
    {
        cScheme = "wasb://";
        cAuthority =
        uri.GetWritableBuffer(uri.GetLength(),
                                  strlen(RChannelBufferHdfsReader::s_wasbPartitionPrefix));
    }
    else
    {
        return false;
    }

    char* slash = strchr(cAuthority, '/');
    if (slash == NULL)
    {
        return false;
    }
    *slash = '\0';

    char* cPath = slash+1;

    static const char* offsetString = "?offset=";
    char* offset = strstr(cPath, offsetString);
    if (offset == NULL)
    {
        return false;
    }
    *offset = '\0';

    char* cOffset = offset + strlen(offsetString);

    static const char* lengthString = "&length=";
    char* length = strstr(cOffset, lengthString);
    if (length == NULL)
    {
        return false;
    }
    *length = '\0';

    char* cLength = length + strlen(lengthString);

    schemeAndAuthority.SetF("%s%s", cScheme, cAuthority);

    filePath.SetF("%s/%s", schemeAndAuthority.GetString(), cPath);

    DrError dre = DrStringToInt64(cOffset, pOffsetStart);
    if (dre != DrError_OK)
    {
        return false;
    }

    dre = DrStringToInt32(cLength, pLength);
    if (dre != DrError_OK)
    {
        return false;
    }

    return true;
}

RChannelBufferHdfsReader::RChannelBufferHdfsReader(const char* uri)
{
    DrLogI("Making HDFS Reader %s", uri);
    m_uri.Set(uri);
    m_handler = NULL;
    m_readThread = INVALID_HANDLE_VALUE;
    m_abortHandle = INVALID_HANDLE_VALUE;
    m_blockSemaphore = CreateSemaphore(NULL,
                                       s_maxBuffersOut,
                                       s_maxBuffersOut,
                                       NULL);

    /* it's important to initialize here since these are called
       sequentially so there's no race on the hdfs
       initialization code. Then we need to connect to the server,
       since that has to happen first on the thread that creates the
       jvm, in order for login to hdfs to work, for unexplained
       reasons. */
    bool initialized = HadoopNative::Initialize();
    if (initialized)
    {
        DrStr64 schemeAndAuthority;
        DrStr64 filePath;
        Int64 offsetStart;
        Int32 length;

        bool parsed = ExtractHdfsReadUri(m_uri,
                                         schemeAndAuthority,
                                         filePath, &offsetStart, &length);
        if (parsed)
        {
            Hdfs::Instance* bridge;
            bool openedInstance = Hdfs::OpenInstance(schemeAndAuthority.GetString(), &bridge);
            if (openedInstance)
            {
                Hdfs::InstanceAccessor ia(bridge);
                ia.Discard();
            }
        }
    }

    DrLogI("Made HDFS Reader %s", uri);
}

RChannelBufferHdfsReader::~RChannelBufferHdfsReader()
{
    if (m_readThread != INVALID_HANDLE_VALUE)
    {
        CloseHandle(m_readThread);
    }
    if (m_abortHandle != INVALID_HANDLE_VALUE)
    {
        CloseHandle(m_abortHandle);
    }
}

void RChannelBufferHdfsReader::
Start(RChannelBufferPrefetchInfo* /*unused prefetchCookie*/,
      RChannelBufferReaderHandler* handler)
{
    LogAssert(m_handler == NULL);
    m_handler = handler;

    LogAssert(m_readThread == INVALID_HANDLE_VALUE);
    LogAssert(m_abortHandle == INVALID_HANDLE_VALUE);

    {
        AutoCriticalSection acs(&m_cs);

        m_totalLength = 0;
        m_processedLength = 0;
    }

    m_abortHandle = ::CreateEvent(NULL, TRUE, FALSE, NULL);
    LogAssert(m_abortHandle != NULL);

    m_readThread =
        (HANDLE) ::_beginthreadex(NULL,
                                  0,
                                  RChannelBufferHdfsReader::ThreadFunc,
                                  this,
                                  0,
                                  NULL);
    LogAssert(m_readThread != 0);
}

void RChannelBufferHdfsReader::Interrupt()
{
    /* tell the read thread to stop reading */
    BOOL bRet = ::SetEvent(m_abortHandle);
    LogAssert(bRet != 0);

    /* then wait for it to exit */
    DWORD dRet = ::WaitForSingleObject(m_readThread, INFINITE);
    LogAssert(dRet == WAIT_OBJECT_0);
}

void RChannelBufferHdfsReader::Drain(RChannelItem* /* unused drainItem */)
{
    Interrupt();

    for (LONG i=0; i<s_maxBuffersOut; ++i)
    {
        DrLogI("Waiting for buffer semaphore");
        DWORD dRet = WaitForSingleObject(m_blockSemaphore, INFINITE);
    }

    BOOL bRetval = CloseHandle(m_abortHandle);
    LogAssert(bRetval != 0);
    m_abortHandle = INVALID_HANDLE_VALUE;

    bRetval = CloseHandle(m_readThread);
    LogAssert(bRetval != 0);
    m_readThread = INVALID_HANDLE_VALUE;

    m_handler = NULL;
}

void RChannelBufferHdfsReader::Close()
{
}

void RChannelBufferHdfsReader::FillInStatus(DryadChannelDescription* s)
{
    AutoCriticalSection acs(&m_cs);

    s->SetChannelTotalLength(m_totalLength);
    s->SetChannelProcessedLength(m_processedLength);
}

bool RChannelBufferHdfsReader::GetTotalLength(UInt64* pLen)
{
    AutoCriticalSection acs(&m_cs);

    *pLen = m_totalLength;

    return true;
}

void RChannelBufferHdfsReader::ReturnBuffer(RChannelBuffer* buffer)
{
    /* discard buffer */
    buffer->DecRef();

    BOOL bRet = ReleaseSemaphore(m_blockSemaphore, 1, NULL);
    LogAssert(bRet != 0);
}

unsigned __stdcall RChannelBufferHdfsReader::ThreadFunc(void* arg)
{
    RChannelBufferHdfsReader* self = (RChannelBufferHdfsReader *) arg;
    self->ReadThread();
    return 0;
}


static RChannelItem*
MakeErrorItem(DrError errorCode, const char* description)
{
    RChannelItem* item =
        RChannelMarkerItem::Create(RChannelItem_Abort, true);
    DryadMetaData* metaData = item->GetMetaData();
    metaData->AddErrorWithDescription(errorCode, description);

    return item;
}

static RChannelBuffer*
MakeErrorBuffer(DrError errorCode, const char* description,
                RChannelBufferDefaultHandler* handler)
{
    RChannelItem* item = MakeErrorItem(errorCode, description);

    RChannelBuffer* errorBuffer =
        RChannelBufferMarkerDefault::Create(RChannelBuffer_Abort,
                                            item,
                                            handler);
    DryadMetaData* metaData = errorBuffer->GetMetaData();
    metaData->AddErrorWithDescription(errorCode, description);

    return errorBuffer;
}


//
// Make an end-of-stream buffer
//
static RChannelItem* MakeEndOfStreamItem()
{
    return RChannelMarkerItem::Create(RChannelItem_EndOfStream, false);
}

static RChannelBuffer*
    MakeEndOfStreamBuffer(RChannelBufferDefaultHandler* handler)
{
    RChannelItem* item = MakeEndOfStreamItem();

    RChannelBuffer* buffer =
        RChannelBufferMarkerDefault::Create(RChannelBuffer_EndOfStream,
                                            item,
                                            handler);

    return buffer;
}

//
// Create a data buffer for the reader to use
//
static RChannelBufferData*
MakeDataBuffer(UInt64 streamOffset, size_t blockSize,
               RChannelBufferDefaultHandler* handler)
{
    DryadAlignedReadBlock* block =
        new DryadAlignedReadBlock(blockSize, 0);
    RChannelBufferData* dataBuffer =
        RChannelBufferDataDefault::Create(block,
                                          streamOffset,
                                          handler);

    DryadMetaData* metaData = dataBuffer->GetMetaData();
    DryadMTagRef tag;
    tag.Attach(DryadMTagUInt64::Create(Prop_Dryad_BufferLength,
                                       block->GetAvailableSize()));
    metaData->Append(tag, false);

    return dataBuffer;
}

void RChannelBufferHdfsReader::SendBuffer(RChannelBuffer* buffer,
                                          bool getSemaphore)
{
    if (getSemaphore)
    {
        HANDLE h[2];
        h[0] = m_abortHandle;
        h[1] = m_blockSemaphore;

        DWORD dRet = WaitForMultipleObjects(2, h, FALSE, INFINITE);
        if (dRet == WAIT_OBJECT_0)
        {
            /* we should discard the buffer and exit since we're
               shutting down */
            buffer->DecRef();
            return;
        }
        else
        {
            LogAssert(dRet == WAIT_OBJECT_0+1);
        }
    }

    m_handler->ProcessBuffer(buffer);
}

Int64 RChannelBufferHdfsReader::
AdjustStartOffset(Hdfs::Reader* reader,
                  const char* fileName,
                  Int64 startOffset,
                  Int64 endOffset)
{
    if (startOffset > 0)
    {
        /* this isn't the first block in the file, so scan to
           the start of the next record. If the next record
           starts at offsetEnd+1 or later then it will be
           picked up by the next block reader, so don't keep
           looking past there. */
        RChannelBuffer* errorBuffer = NULL;
        Int64 offset = ScanForSync(reader, fileName,
                                   startOffset, endOffset+1, &errorBuffer);

        if (offset < -1)
        {
            /* there was a read error */
            LogAssert(errorBuffer != NULL);
            SendBuffer(errorBuffer, true);
        }
        else if (offset == -1)
        {
            /* there was no record starting in the selected range */
            LogAssert(errorBuffer == NULL);
            DrLogI("Hdfs skipped block %I64d::%I64d because no record sync was found",
                   startOffset, endOffset);
            errorBuffer = MakeEndOfStreamBuffer(this);
            SendBuffer(errorBuffer, true);
        }
        else
        {
            LogAssert(errorBuffer == NULL);
            LogAssert(offset <= endOffset);
            DrLogI("Hdfs skipped from %I64d to start at new record at %I64d",
                   startOffset, offset);
        }

        return offset;
    }
    else
    {
        LogAssert(startOffset == 0);
        DrLogI("Hdfs starting first block at offset 0");
        return startOffset;
    }
}

Int64 RChannelBufferHdfsReader::
AdjustEndOffset(Hdfs::Reader* reader,
                const char* fileName,
                Int64 endOffset)
{
    RChannelBuffer* errorBuffer = NULL;
    Int64 newOffset = ScanForSync(reader, fileName,
                                  endOffset, -1,
                                  &errorBuffer);

    if (newOffset < -1)
    {
        /* there was a read error */
        LogAssert(errorBuffer != NULL);
        SendBuffer(errorBuffer, true);
    }
    else
    {
        LogAssert(newOffset >= endOffset);
        DrLogI("HDFS file %s scanned past end of block from %I64d to %I64d",
               fileName,
               endOffset, newOffset);
    }

    return newOffset;
}

Int64 RChannelBufferHdfsReader::
ReadDataBuffer(Hdfs::ReaderAccessor& ra,
               const char* fileName,
               Int64 offset,
               Int64 endOffset)
{
    Int32 sizeToRead = s_readBufferSize;
    Int64 sizeLeft = endOffset - offset;
    if (sizeLeft < sizeToRead)
    {
        sizeToRead = (Int32) sizeLeft;
    }

    LogAssert(sizeToRead > 0);

    RChannelBuffer* buffer;

    RChannelBufferData* dataBuffer =
        MakeDataBuffer((UInt64) offset, (size_t) sizeToRead, this);

    DrMemoryBuffer* block = dataBuffer->GetData();
    Size_t available;
    void* dst = block->GetDataAddress(0, &available, NULL);
    LogAssert(available >= sizeToRead);

    DrLogI("Reading HDFS file %s range %I64d:%d",
           fileName, offset, sizeToRead);
    long bytesRead =
        ra.ReadBlock(offset, (unsigned char *) dst, sizeToRead);

    if (bytesRead < -1)
    {
        char* errorMsg = ra.GetExceptionMessage();
        DrStr64 description;
        description.SetF("Can't read HDFS file '%s' at offset %I64d:%d: %s",
                         fileName,
                         offset, sizeToRead, errorMsg);
        HadoopNative::DisposeString(errorMsg);

        buffer =
            MakeErrorBuffer(DryadError_ChannelReadError,
                            description.GetString(),
                            this);

        dataBuffer->DecRef();
        offset = -1;
    }
    else if (bytesRead == -1)
    {
        DrStr64 description;
        description.SetF("HDFS file '%s' got EOF at offset %I64d:%d",
                         fileName,
                         offset, sizeToRead);

        buffer =
            MakeErrorBuffer(DryadError_ChannelReadError,
                            description.GetString(),
                            this);

        dataBuffer->DecRef();
        offset = -1;
    }
    else if (bytesRead != sizeToRead)
    {
        DrStr64 description;
        description.SetF("HDFS file '%s' got too few bytes %d at offset %I64d:%d",
                         fileName,
                         bytesRead,
                         offset, sizeToRead);

        buffer =
            MakeErrorBuffer(DryadError_ChannelReadError,
                            description.GetString(),
                            this);

        dataBuffer->DecRef();
        offset = -1;
    }
    else
    {
        buffer = dataBuffer;
        offset += bytesRead;
    }

    SendBuffer(buffer, false);

    return offset;
}

void RChannelBufferHdfsReader::ReadThread()
{
    bool initialized = HadoopNative::Initialize();
    if (!initialized)
    {
        DrStr64 description;
        description.Set("Can't initialize HDFS bridge");
        RChannelBuffer* error =
            MakeErrorBuffer(DryadError_ChannelOpenError,
                            description.GetString(),
                            this);
        SendBuffer(error, true);
        return;
    }

    DrStr64 schemeAndAuthority;
    DrStr64 filePath;
    Int64 offsetStart;
    Int32 length;

    bool parsed = ExtractHdfsReadUri(m_uri,
                                     schemeAndAuthority,
                                     filePath, &offsetStart, &length);
    if (!parsed)
    {
        DrStr64 description;
        description.SetF("Can't parse HDFS URI '%s'", m_uri.GetString());
        RChannelBuffer* error =
            MakeErrorBuffer(DryadError_InvalidChannelURI,
                            description.GetString(),
                            this);
        SendBuffer(error, true);
        return;
    }

    {
        AutoCriticalSection acs(&m_cs);
        m_totalLength = length;
    }

    Hdfs::Instance* bridge;
    bool openedInstance = Hdfs::OpenInstance(schemeAndAuthority.GetString(), &bridge);
    if (!openedInstance)
    {
        DrStr64 description;
        description.SetF("Can't open HDFS Bridge '%s'", schemeAndAuthority.GetString());
        RChannelBuffer* error =
            MakeErrorBuffer(DryadError_ChannelOpenError,
                            description.GetString(),
                            this);
        SendBuffer(error, true);
        return;
    }

    Hdfs::InstanceAccessor ia(bridge);

    Hdfs::Reader* reader;
    bool openedReader = ia.OpenReader(filePath.GetString(), &reader);
    if (!openedReader)
    {
        char* errorMsg = ia.GetExceptionMessage();
        DrStr64 description;
        description.SetF("Can't open HDFS file '%s': %s",
                         m_uri.GetString(), errorMsg);
        DrLogI("%s", description.GetString());
        HadoopNative::DisposeString(errorMsg);

        RChannelBuffer* error =
            MakeErrorBuffer(DryadError_ChannelOpenError,
                            description.GetString(),
                            this);
        SendBuffer(error, true);

        ia.Discard();
        return;
    }

    Int64 offsetEnd = offsetStart + length;
    Int64 offset = AdjustStartOffset(reader, m_uri.GetString(),
                                     offsetStart, offsetEnd);
    bool scannedFinal = false;

    if (offset < 0)
    {
        /* nothing to read here: AdjustStartOffset already sent
           the termination item so we can exit */
        ia.Discard();
        return;
    }

    if (offset == offsetEnd)
    {
        offsetEnd = AdjustEndOffset(reader, m_uri.GetString(),
                                    offsetEnd);
        if (offsetEnd < 0)
        {
            /* there was a read error: AdjustEndOffset already sent
               the termination item so we can exit */
            ia.Discard();
            return;
        }

        scannedFinal = true;
    }

    {
        AutoCriticalSection acs(&m_cs);

        offsetStart = offset;
        LogAssert(offsetEnd >= offsetStart);
        m_totalLength = offsetEnd - offsetStart;
    }

    Hdfs::ReaderAccessor ra(reader);

    while (offset >=0 && offset < offsetEnd)
    {
        HANDLE h[2];
        h[0] = m_abortHandle;
        h[1] = m_blockSemaphore;

        DWORD dRet = WaitForMultipleObjects(2, h, FALSE, INFINITE);
        if (dRet == WAIT_OBJECT_0)
        {
            /* we should exit */
            offset = -1;
            break;
        }
        else
        {
            LogAssert(dRet == WAIT_OBJECT_0+1);
        }

        /* just check we aren't aborted anyway */
        dRet = WaitForSingleObject(m_abortHandle, 0);
        if (dRet == WAIT_OBJECT_0)
        {
            /* give back the semaphore we just took */
            BOOL bRet = ReleaseSemaphore(m_blockSemaphore, 1, NULL);
            LogAssert(bRet != 0);

            /* we should exit */
            offset = -1;
            break;
        }
        else
        {
            LogAssert(dRet == WAIT_TIMEOUT);
        }

        offset = ReadDataBuffer(ra, m_uri.GetString(),
                                offset, offsetEnd);
        if (offset >= 0)
        {
            AutoCriticalSection acs(&m_cs);

            m_processedLength = offset - offsetStart;
        }

        if (offset == offsetEnd && !scannedFinal)
        {
            offsetEnd = AdjustEndOffset(reader, m_uri.GetString(),
                                        offsetEnd);
            if (offsetEnd < 0)
            {
                /* there was a read error: AdjustEndOffset already
                   sent the termination item so we can exit */
                ia.Discard();
                return;
            }

            scannedFinal = true;
        }
    } /* while (offset >=0 && offset < offsetEnd) */

    ra.Discard();

    if (offset >= 0)
    {
        RChannelBuffer* buffer = MakeEndOfStreamBuffer(this);
        SendBuffer(buffer, true);
    }

    ia.Discard();
}

static long s_lineRecordScanSize = 4*1024;

RChannelBufferHdfsReaderLineRecord::
RChannelBufferHdfsReaderLineRecord(const char* uri) : RChannelBufferHdfsReader(uri)
{
}

Int64 RChannelBufferHdfsReaderLineRecord::
ScanForSync(Hdfs::Reader* reader,
            const char* fileName,
            Int64 startOffset, Int64 endOffset,
            RChannelBuffer** pErrorBuffer)
{
    *pErrorBuffer = NULL;

    unsigned char* scanBuffer = new unsigned char[s_lineRecordScanSize];

    /* endOffset is -1 if we're scanning indefinitely, otherwise it
       must designate a range that ends after startOffset */
    LogAssert(endOffset != 0);

    Int64 foundOffset = -1;
    bool foundReturn = false;

    {
        Hdfs::ReaderAccessor ra(reader);

        do
        {
            long bytesToRead = s_lineRecordScanSize;
            if (endOffset > 0)
            {
                /* there's a known endStop: let's not go past it */
                Int64 bytesLeft = endOffset - startOffset;
                if (bytesLeft < bytesToRead)
                {
                    bytesToRead = (long) bytesLeft;
                }
            }

            long bytesRead = ra.ReadBlock(startOffset, scanBuffer,
                                          bytesToRead);

            if (bytesRead < -1)
            {
                char* errorMsg = ra.GetExceptionMessage();
                DrStr64 description;
                description.SetF("Can't read HDFS file '%s' at offset %I64d:%d: %s",
                                 fileName,
                                 startOffset, s_lineRecordScanSize, errorMsg);
                HadoopNative::DisposeString(errorMsg);

                *pErrorBuffer =
                    MakeErrorBuffer(DryadError_ChannelReadError,
                                    description.GetString(),
                                    this);

                /* break from while loop */
                foundOffset = -2;
            }
            else if (bytesRead == -1)
            {
                if (endOffset > 0)
                {
                    /* we were supposed to be able to read as far as
                       endOffset, but hit EOF early */
                    char* errorMsg = ra.GetExceptionMessage();
                    DrStr64 description;
                    description.SetF("Got HDFS EOF early for '%s' at offset %I64d, expecting data up to %I64d: %s",
                                     fileName,
                                     startOffset, endOffset, errorMsg);
                    HadoopNative::DisposeString(errorMsg);

                    *pErrorBuffer =
                        MakeErrorBuffer(DryadError_ChannelReadError,
                                        description.GetString(),
                                        this);

                    /* break from while loop */
                    foundOffset = -2;
                }
                else
                {
                    /* we were scanning indefinitely and hit EOF,
                       which just means we found the end of the last
                       record. */
                    /* break from while loop */
                    foundOffset = startOffset;
                }
            }
            else
            {
                LogAssert(bytesRead > 0);

                for (long i=0; i<bytesRead; ++i)
                {
                    if (scanBuffer[i] == '\n')
                    {
                        /* the next character is the first character
                           in a new line */
                        foundOffset = startOffset + i + 1;
                        if (endOffset > 0 && foundOffset >= endOffset)
                        {
                            /* we got to the end of the range we were
                               scanning without finding a new
                               record */
                            LogAssert(foundOffset == endOffset);
                            LogAssert(startOffset + bytesRead == endOffset);
                            foundOffset = -1;
                        }
                        break;
                    }
                    else if (foundReturn)
                    {
                        /* we saw a return character the previous char, so
                           this is the first character in a new line */
                        foundOffset = startOffset + i;
                        break;
                    }
                    else if (scanBuffer[i] == '\r')
                    {
                        foundReturn = true;
                    }
                }

                startOffset += bytesRead;
            }
        } while (foundOffset == -1 &&
                 (endOffset < 0 || startOffset < endOffset));

        if (endOffset > 0)
        {
            LogAssert(startOffset <= endOffset);
        }
    }

    delete [] scanBuffer;

    return foundOffset;
}



static bool
ExtractHdfsWriteUri(DrStr64 uri,
                    DrStr64& schemeAndAuthority,
                    DrStr64& filePath)
{
    const char* cScheme;
    char* cAuthority;
    if (uri.StartsWith(RChannelBufferHdfsWriter::s_hdfsFilePrefix))
    {
        cScheme = RChannelBufferHdfsWriter::s_hdfsFilePrefix;
        cAuthority = uri.GetWritableBuffer(uri.GetLength(),
                                           strlen(RChannelBufferHdfsWriter::s_hdfsFilePrefix));
    }
    else if (uri.StartsWith(RChannelBufferHdfsWriter::s_wasbFilePrefix))
    {
        cScheme = RChannelBufferHdfsWriter::s_wasbFilePrefix;
        cAuthority = uri.GetWritableBuffer(uri.GetLength(),
                                           strlen(RChannelBufferHdfsWriter::s_wasbFilePrefix));
    }
    else
    {
        return false;
    }

    char* slash = strchr(cAuthority, '/');
    if (slash == NULL)
    {
        return false;
    }
    *slash = '\0';

    char* cPath = slash+1;

    schemeAndAuthority.SetF("%s%s", cScheme, cAuthority);

    filePath.SetF("%s/%s", schemeAndAuthority.GetString(), cPath);

    return true;
}

RChannelBufferHdfsWriter::RChannelBufferHdfsWriter(const char* uri)
{
    m_uri.Set(uri);
    m_queueHandle = INVALID_HANDLE_VALUE;
    m_writeThread = INVALID_HANDLE_VALUE;
    m_queueLength = 0;

    /* it's important to initialize here since these are called
       sequentially so there's no race on the hdfs
       initialization code. Then we need to connect to the server,
       since that has to happen first on the thread that creates the
       jvm, in order for login to hdfs to work, for unexplained
       reasons. */
    bool initialized = HadoopNative::Initialize();
    if (initialized)
    {
        DrStr64 schemeAndAuthority;
        DrStr64 filePath;

        bool parsed = ExtractHdfsWriteUri(m_uri,
                                          schemeAndAuthority,
                                          filePath);
        if (parsed)
        {
            Hdfs::Instance* bridge;
            bool openedInstance = Hdfs::OpenInstance(schemeAndAuthority.GetString(), &bridge);
            if (openedInstance)
            {
                Hdfs::InstanceAccessor ia(bridge);
                ia.Discard();
            }

            char envUserName[128];
            DWORD ret = ::GetEnvironmentVariableA("USER", envUserName, sizeof(envUserName));
            if (ret > 0 && ret < sizeof(envUserName)-1)
            {
                m_user.Set(envUserName);
                DrLogI("Hdfs writer set user to %s from environment", m_user.GetString());
            }
            else
            {
                if (ret == 0 && GetLastError() != ERROR_ENVVAR_NOT_FOUND)
                {
                    DrLogW("Can't get USER environment variable: %s", DRERRORSTRING(DrGetLastError()));
                }
                if (ret >= sizeof(envUserName)-1)
                {
                    DrLogW("USER environment is too long: %d", ret);
                }

                DWORD bufSize = sizeof(envUserName);
                BOOL bRet = ::GetUserNameA(envUserName, &bufSize);
                if (bRet == 0)
                {
                    DrLogW("Can't get username: %s", DRERRORSTRING(DrGetLastError()));
                }
                else
                {
                    m_user.Set(envUserName);
                    DrLogI("Hdfs writer set user to %s from username", m_user.GetString());
                }
            }
        }
    }
}

DryadFixedMemoryBuffer* RChannelBufferHdfsWriter::GetNextWriteBuffer()
{
    return GetCustomWriteBuffer(s_writeBufferSize);
}

DryadFixedMemoryBuffer* RChannelBufferHdfsWriter::
GetCustomWriteBuffer(Size_t bufferSize)
{
    return new DryadAlignedWriteBlock(bufferSize, 0);
}

void RChannelBufferHdfsWriter::Start()
{
    LogAssert(m_writeThread == INVALID_HANDLE_VALUE);
    LogAssert(m_queueHandle == INVALID_HANDLE_VALUE);
    LogAssert(m_queue.IsEmpty());
    LogAssert(m_queueLength == 0);

    {
        AutoCriticalSection acs(&m_cs);

        m_processedLength = 0;
    }

    m_queueHandle = ::CreateEvent(NULL, TRUE, FALSE, NULL);
    LogAssert(m_queueHandle != NULL);

    m_writeThread =
        (HANDLE) ::_beginthreadex(NULL,
                                  0,
                                  RChannelBufferHdfsWriter::ThreadFunc,
                                  this,
                                  0,
                                  NULL);
    LogAssert(m_writeThread != 0);
}

unsigned __stdcall RChannelBufferHdfsWriter::ThreadFunc(void* arg)
{
    RChannelBufferHdfsWriter* self = (RChannelBufferHdfsWriter *) arg;
    self->WriteThread();
    return 0;
}

bool RChannelBufferHdfsWriter::Open(Hdfs::Instance** pInstance,
                                    Hdfs::Writer** pWriter)
{
    DrLogI("Opening Hdfs writer for %s", m_uri.GetString());
    bool initialized = HadoopNative::Initialize();
    if (!initialized)
    {
        DrStr64 description;
        description.Set("Can't initialize HDFS bridge");
        m_completionItem.Attach(MakeErrorItem(DryadError_ChannelOpenError,
                                description.GetString()));
        return false;
    }

    DrStr64 schemeAndAuthority;
    DrStr64 filePath;

    bool parsed = ExtractHdfsWriteUri(m_uri,
                                      schemeAndAuthority,
                                      filePath);
    if (!parsed)
    {
        DrStr64 description;
        description.SetF("Can't parse HDFS URI '%s'", m_uri.GetString());
        m_completionItem.Attach(MakeErrorItem(DryadError_InvalidChannelURI,
                                description.GetString()));
        return false;
    }

    Hdfs::Instance* instance;
    bool openedInstance = Hdfs::OpenInstance(schemeAndAuthority.GetString(), &instance);
    if (!openedInstance)
    {
        DrStr64 description;
        description.SetF("Can't open HDFS Bridge '%s'", schemeAndAuthority.GetString());
        m_completionItem =
            MakeErrorItem(DryadError_ChannelOpenError,
                          description.GetString());
        return false;
    }

    Hdfs::InstanceAccessor ia(instance);

    Hdfs::Writer* writer;
    bool openedWriter = ia.OpenCreate(filePath.GetString(), 1024 * 1024, -1, &writer);
    if (!openedWriter)
    {
        char* errorMsg = ia.GetExceptionMessage();
        DrStr64 description;
        description.SetF("Can't open HDFS file '%s': %s",
                         m_uri.GetString(), errorMsg);
        HadoopNative::DisposeString(errorMsg);

        m_completionItem.Attach(MakeErrorItem(DryadError_ChannelOpenError,
                                description.GetString()));

        ia.Discard();
        return false;
    }
    bool setPermissions = ia.SetOwnerAndPermission(filePath.GetString(), m_user.GetString(), NULL, 0644);
    if (!setPermissions)
    {
        char* errorMsg = ia.GetExceptionMessage();
        DrStr64 description;
        description.SetF("Can't set HDFS file permissions '%s': %s",
                         m_uri.GetString(), errorMsg);
        HadoopNative::DisposeString(errorMsg);

        m_completionItem.Attach(MakeErrorItem(DryadError_ChannelOpenError,
                                description.GetString()));

        ia.Discard();
        return false;
    }

    DrLogI("Opened Hdfs writer for %s", m_uri.GetString());

    *pInstance = instance;
    *pWriter = writer;

    return true;
}

void RChannelBufferHdfsWriter::WriteThread()
{
    Hdfs::Instance* instance = NULL;
    Hdfs::Writer* writer = NULL;

    bool opened = Open(&instance, &writer);

    do
    {
        DWORD dRet = WaitForSingleObject(m_queueHandle, INFINITE);
        LogAssert(dRet == WAIT_OBJECT_0);

        WriteEntry* entry = NULL;
        {
            AutoCriticalSection acs(&m_cs);

            entry = m_queue.CastOut(m_queue.RemoveHead());
            /* the event shouldn't have been signaled unless the queue
               is non-empty */
            LogAssert(entry != NULL);
            LogAssert(m_queueLength > 0);
            --m_queueLength;
        }

        do
        {
            if (m_completionItem != NULL)
            {
                /* we've had a write error: we'll just reply below
                   with another error */
                LogAssert(m_completionItem->GetType() !=
                          RChannelItem_EndOfStream);
            }
            else
            {
                if (entry->m_type == RChannelItem_Data)
                {
                    /* we got a data item */
                    LogAssert(entry->m_buffer != NULL);

                    LogAssert(writer != NULL);
                    Hdfs::WriterAccessor wa(writer);

                    size_t dataSize;
                    void *dataAddr = entry->m_buffer->
                        GetDataAddress(0, &dataSize, NULL);
                    Size_t dataToWrite = entry->m_buffer->GetAvailableSize();
                    LogAssert(dataToWrite <= dataSize);
                    bool ret =
                        wa.WriteBlock((unsigned char *)dataAddr, (long) dataToWrite,
                                      entry->m_flush);
                    if (ret)
                    {
                        AutoCriticalSection acs(&m_cs);

                        m_processedLength += dataToWrite;
                    }
                    else
                    {
                        char* errorMsg = wa.GetExceptionMessage();
                        DrStr64 description;
                        description.SetF("Got HDFS error on write: %s",
                                         errorMsg);
                        HadoopNative::DisposeString(errorMsg);

                        DrLogE(description.GetString());

                        m_completionItem.Attach(MakeErrorItem(DryadError_ChannelWriteError,
                                                description.GetString()));
                    }
                }
                else
                {
                    /* we got a termination item */
                    LogAssert(entry->m_buffer == NULL);

                    DrLogI("Got hdfs termination item");

                    LogAssert(writer != NULL);
                    Hdfs::WriterAccessor wa(writer);

                    bool ret = wa.Close();
                    if (!ret)
                    {
                        char* errorMsg = wa.GetExceptionMessage();
                        DrStr64 description;
                        description.SetF("Got HDFS error on close: %s",
                                         errorMsg);
                        HadoopNative::DisposeString(errorMsg);

                        DrLogE(description.GetString());
                        m_completionItem.Attach(MakeErrorItem(DryadError_ChannelWriteError,
                                                description.GetString()));
                    }
                    else
                    {
                        DrLogI("Closed HDFS writer");
                        m_completionItem.Attach(RChannelMarkerItem::Create(entry->m_type, false));
                    }
                }
            }

            RChannelItemType status;
            if (m_completionItem == NULL)
            {
                status = RChannelItem_Data;
            }
            else
            {
                status = m_completionItem->GetType();
                LogAssert(status != RChannelItem_Data);
            }

            entry->m_handler->ProcessWriteCompleted(status);
            delete entry;
            entry = NULL;

            if (status == RChannelItem_Data)
            {
                /* we haven't had an error or termination, so see if
                   there's another entry in the queue */
                AutoCriticalSection acs(&m_cs);

                entry = m_queue.CastOut(m_queue.RemoveHead());
                if (entry == NULL)
                {
                    /* go to sleep until someone puts another buffer
                       in the queue */
                    LogAssert(m_queueLength == 0);
                    BOOL bRet = ResetEvent(m_queueHandle);
                    LogAssert(bRet != 0);
                }
                else
                {
                    LogAssert(m_queueLength > 0);
                    --m_queueLength;
                }
            }
        } while (entry != NULL);
    } while (m_completionItem == NULL);

    if (opened)
    {
        /* discard the java objects we're holding onto */
        Hdfs::WriterAccessor wa(writer);
        wa.Discard();

        Hdfs::InstanceAccessor ia(instance);
        ia.Discard();
    }
}

bool RChannelBufferHdfsWriter::AddToQueue(WriteEntry* entry)
{
    {
        AutoCriticalSection acs(&m_cs);

        BOOL wasEmpty = m_queue.IsEmpty();

        m_queue.InsertAsTail(m_queue.CastIn(entry));
        ++m_queueLength;

        if (wasEmpty)
        {
            LogAssert(m_queueLength == 1);
            BOOL bRet = SetEvent(m_queueHandle);
            LogAssert(bRet != 0);
        }

        /* should block if the queue gets too deep */
        return (m_queueLength > s_maxBuffersToBlockWriter);
    }
}

bool RChannelBufferHdfsWriter::
WriteBuffer(DryadFixedMemoryBuffer* buffer,
            bool flushAfter,
            RChannelBufferWriterHandler* handler)
{
    WriteEntry* entry = new WriteEntry;
    entry->m_buffer.Attach(buffer);
    entry->m_flush = flushAfter;
    entry->m_type = RChannelItem_Data;
    entry->m_handler = handler;

    return AddToQueue(entry);
}

void RChannelBufferHdfsWriter::
ReturnUnusedBuffer(DryadFixedMemoryBuffer* buffer)
{
    buffer->DecRef();
}

void RChannelBufferHdfsWriter::
WriteTermination(RChannelItemType reasonCode,
                 RChannelBufferWriterHandler* handler)
{
    WriteEntry* entry = new WriteEntry;
    /* NULL entry->m_buffer */
    entry->m_flush = false;
    entry->m_type = reasonCode;
    entry->m_handler = handler;

    AddToQueue(entry);
}

void RChannelBufferHdfsWriter::FillInStatus(DryadChannelDescription* status)
{
    AutoCriticalSection acs(&m_cs);

    status->SetChannelTotalLength(0);
    status->SetChannelProcessedLength(m_processedLength);
}

void RChannelBufferHdfsWriter::Drain(RChannelItemRef* pReturnItem)
{
    /* Drain shouldn't have been called unless a termination item has
       been sent, so eventually the writer thread will exit... */
    DWORD dRet = WaitForSingleObject(m_writeThread, INFINITE);
    LogAssert(dRet == WAIT_OBJECT_0);

    LogAssert(m_queue.IsEmpty());
    LogAssert(m_queueLength == 0);

    /* and that's it, nothing more to do */
    LogAssert(m_completionItem != NULL);
    *pReturnItem = m_completionItem;
    m_completionItem = NULL;
}

void RChannelBufferHdfsWriter::Close()
{
    LogAssert(m_queueHandle != INVALID_HANDLE_VALUE);
    LogAssert(m_writeThread != INVALID_HANDLE_VALUE);

    BOOL bRetval = CloseHandle(m_queueHandle);
    LogAssert(bRetval != 0);
    m_queueHandle = INVALID_HANDLE_VALUE;

    bRetval = CloseHandle(m_writeThread);
    LogAssert(bRetval != 0);
    m_writeThread = INVALID_HANDLE_VALUE;
    
    LogAssert(m_completionItem == NULL);
}

UInt64 RChannelBufferHdfsWriter::GetInitialSizeHint()
{
    return 0;
}

void RChannelBufferHdfsWriter::SetInitialSizeHint(UInt64 /*hint*/)
{
}
