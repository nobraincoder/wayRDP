#include "channels/RdpCliprdrChannel.h"
#include <QDebug>
#include <QDir>
#include <QDirIterator>
#include <QDateTime>
#include <QFileInfo>
#include <QStandardPaths>
#include <QUrl>
#include <algorithm>

RdpCliprdrChannel::RdpCliprdrChannel(QObject *parent)
    : QObject(parent)
{
}

RdpCliprdrChannel::~RdpCliprdrChannel()
{
    close();
}

bool RdpCliprdrChannel::initialize(HANDLE vcm, rdpContext* rdpcontext)
{
    close();

    if (!vcm) {
        qWarning() << "RdpCliprdrChannel: Cannot initialize without Virtual Channel Manager";
        return false;
    }

    CliprdrServerContext* cliprdr = cliprdr_server_context_new(vcm);
    if (!cliprdr) {
        qWarning() << "RdpCliprdrChannel: Failed to create cliprdr server context";
        return false;
    }

    cliprdr->custom = this;
    cliprdr->rdpcontext = rdpcontext;
    cliprdr->useLongFormatNames = TRUE;
    cliprdr->streamFileClipEnabled = TRUE;
    cliprdr->fileClipNoFilePaths = TRUE;
    cliprdr->canLockClipData = FALSE;

    cliprdr->autoInitializationSequence = TRUE;
    cliprdr->ClientCapabilities = cliprdr_client_capabilities;
    cliprdr->ClientFormatList = cliprdr_client_format_list;
    cliprdr->ClientFormatListResponse = cliprdr_client_format_list_response;
    cliprdr->ClientFormatDataRequest = cliprdr_client_format_data_request;
    cliprdr->ClientFormatDataResponse = cliprdr_client_format_data_response;
    cliprdr->ClientFileContentsRequest = cliprdr_client_file_contents_request;
    cliprdr->ClientFileContentsResponse = cliprdr_client_file_contents_response;
    cliprdr->ClientLockClipboardData = cliprdr_client_lock_clipboard_data;
    cliprdr->ClientUnlockClipboardData = cliprdr_client_unlock_clipboard_data;

    if (cliprdr->Start(cliprdr) == CHANNEL_RC_OK) {
        QMutexLocker locker(&m_mutex);
        m_context = cliprdr;
        m_cliprdrReady = false;
        qInfo() << "RdpCliprdrChannel: Clipboard (cliprdr) channel initialized with file transfer enabled";
        return true;
    } else {
        qWarning() << "RdpCliprdrChannel: Failed to start cliprdr channel";
        cliprdr_server_context_free(cliprdr);
        return false;
    }
}

void RdpCliprdrChannel::close()
{
    QMutexLocker locker(&m_mutex);
    m_context = nullptr;
    m_cliprdrReady = false;

    for (auto& inf : m_incomingFiles) {
        if (inf.localFile) {
            inf.localFile->close();
            delete inf.localFile;
            inf.localFile = nullptr;
        }
    }
    m_incomingFiles.clear();
    m_completedIncomingFilePaths.clear();
    m_lastIncomingFiles.clear();
    m_outgoingFiles.clear();
    m_outgoingFgdData.clear();
    m_lastHostClipboardText.clear();
    m_currentIncomingFileIndex = 0;
    m_fileStreamId = 0;
    m_clientFileGroupDescriptorFormatId = 0;
}

UINT RdpCliprdrChannel::cliprdr_client_capabilities(CliprdrServerContext* context, const CLIPRDR_CAPABILITIES* capabilities)
{
    Q_UNUSED(capabilities);
    if (!context || !context->custom) return CHANNEL_RC_OK;
    auto* channel = static_cast<RdpCliprdrChannel*>(context->custom);

    qInfo() << "CLIPRDR: ClientCapabilities received! Clipboard channel is now ready.";
    channel->m_cliprdrReady = true;

    // If host has clipboard content, announce it now to the client
    QMutexLocker locker(&channel->m_mutex);
    if (!channel->m_outgoingFiles.isEmpty()) {
        CLIPRDR_FORMAT_LIST formatList;
        memset(&formatList, 0, sizeof(formatList));
        CLIPRDR_FORMAT formats[4];
        formats[0].formatId = CF_UNICODETEXT;
        formats[0].formatName = nullptr;
        formats[1].formatId = CF_TEXT;
        formats[1].formatName = nullptr;
        formats[2].formatId = channel->m_formatFileGroupDescriptorW;
        formats[2].formatName = const_cast<char*>("FileGroupDescriptorW");
        formats[3].formatId = channel->m_formatFileContents;
        formats[3].formatName = const_cast<char*>("FileContents");

        formatList.common.msgType = CB_FORMAT_LIST;
        formatList.common.msgFlags = 0;
        formatList.numFormats = 4;
        formatList.formats = formats;

        context->ServerFormatList(context, &formatList);
    } else if (!channel->m_lastHostClipboardText.isEmpty()) {
        CLIPRDR_FORMAT_LIST formatList;
        memset(&formatList, 0, sizeof(formatList));
        CLIPRDR_FORMAT formats[2];
        formats[0].formatId = CF_UNICODETEXT;
        formats[0].formatName = nullptr;
        formats[1].formatId = CF_TEXT;
        formats[1].formatName = nullptr;

        formatList.common.msgType = CB_FORMAT_LIST;
        formatList.common.msgFlags = 0;
        formatList.numFormats = 2;
        formatList.formats = formats;

        context->ServerFormatList(context, &formatList);
    }
    return CHANNEL_RC_OK;
}

UINT RdpCliprdrChannel::cliprdr_client_format_list_response(CliprdrServerContext* context, const CLIPRDR_FORMAT_LIST_RESPONSE* formatListResponse)
{
    Q_UNUSED(context);
    qInfo() << "CLIPRDR: ClientFormatListResponse received with flags:" << formatListResponse->common.msgFlags;
    return CHANNEL_RC_OK;
}

UINT RdpCliprdrChannel::cliprdr_client_format_list(CliprdrServerContext* context, const CLIPRDR_FORMAT_LIST* formatList)
{
    if (!context || !context->custom) return CHANNEL_RC_OK;
    auto* channel = static_cast<RdpCliprdrChannel*>(context->custom);

    // Send ServerFormatListResponse acknowledging format list receipt (MS-RDPECLIP §3.1.5.2)
    CLIPRDR_FORMAT_LIST_RESPONSE resp;
    memset(&resp, 0, sizeof(resp));
    resp.common.msgType = CB_FORMAT_LIST_RESPONSE;
    resp.common.msgFlags = CB_RESPONSE_OK;
    context->ServerFormatListResponse(context, &resp);

    if (!formatList) return CHANNEL_RC_OK;

    qInfo() << "CLIPRDR: Client advertised" << formatList->numFormats << "clipboard formats";
    channel->m_clientFileGroupDescriptorFormatId = 0;
    UINT32 requestedId = 0;

    for (UINT32 i = 0; i < formatList->numFormats; i++) {
        const CLIPRDR_FORMAT* fmt = &formatList->formats[i];
        if (fmt->formatName) {
            QString name = QString::fromUtf8(fmt->formatName);
            qInfo() << "  Format [" << i << "]: id=" << fmt->formatId << "name=" << name;
            if (name.compare("FileGroupDescriptorW", Qt::CaseInsensitive) == 0) {
                channel->m_clientFileGroupDescriptorFormatId = fmt->formatId;
            }
        } else {
            qInfo() << "  Format [" << i << "]: id=" << fmt->formatId << "standard";
        }
    }

    // Always prefer file descriptor format if client offers files
    if (channel->m_clientFileGroupDescriptorFormatId != 0) {
        CLIPRDR_FORMAT_DATA_REQUEST req;
        memset(&req, 0, sizeof(req));
        req.common.msgType = CB_FORMAT_DATA_REQUEST;
        req.requestedFormatId = channel->m_clientFileGroupDescriptorFormatId;
        context->lastRequestedFormatId = channel->m_clientFileGroupDescriptorFormatId;
        context->ServerFormatDataRequest(context, &req);
        qInfo() << "CLIPRDR: Client has files on clipboard, requested FileGroupDescriptorW formatId:" << channel->m_clientFileGroupDescriptorFormatId;
        return CHANNEL_RC_OK;
    }

    // Fall back to text formats
    for (UINT32 i = 0; i < formatList->numFormats; i++) {
        if (formatList->formats[i].formatId == CF_UNICODETEXT) {
            requestedId = CF_UNICODETEXT;
            break;
        } else if (formatList->formats[i].formatId == CF_TEXT && requestedId == 0) {
            requestedId = CF_TEXT;
        }
    }

    if (requestedId != 0) {
        CLIPRDR_FORMAT_DATA_REQUEST req;
        memset(&req, 0, sizeof(req));
        req.common.msgType = CB_FORMAT_DATA_REQUEST;
        req.requestedFormatId = requestedId;
        context->lastRequestedFormatId = requestedId;
        context->ServerFormatDataRequest(context, &req);
        qInfo() << "CLIPRDR: Requested format data for formatId:" << requestedId;
    }

    return CHANNEL_RC_OK;
}

UINT RdpCliprdrChannel::cliprdr_client_format_data_request(CliprdrServerContext* context, const CLIPRDR_FORMAT_DATA_REQUEST* formatDataRequest)
{
    if (!context || !context->custom) return CHANNEL_RC_OK;
    auto* channel = static_cast<RdpCliprdrChannel*>(context->custom);

    qInfo() << "CLIPRDR: ClientFormatDataRequest for formatId:" << formatDataRequest->requestedFormatId;

    CLIPRDR_FORMAT_DATA_RESPONSE resp;
    memset(&resp, 0, sizeof(resp));
    resp.common.msgType = CB_FORMAT_DATA_RESPONSE;

    if (formatDataRequest->requestedFormatId == channel->m_formatFileGroupDescriptorW) {
        QMutexLocker locker(&channel->m_mutex);
        resp.common.msgFlags = CB_RESPONSE_OK;
        resp.common.dataLen = channel->m_outgoingFgdData.size();
        resp.requestedFormatData = reinterpret_cast<const BYTE*>(channel->m_outgoingFgdData.constData());
        context->ServerFormatDataResponse(context, &resp);
        qInfo() << "CLIPRDR: Responded with FileGroupDescriptorW (" << channel->m_outgoingFgdData.size() << "bytes)";
        return CHANNEL_RC_OK;
    }

    if (formatDataRequest->requestedFormatId == CF_UNICODETEXT) {
        QByteArray utf16;
        {
            QMutexLocker locker(&channel->m_mutex);
            QString textToSend = channel->m_lastHostClipboardText;
            if (!textToSend.contains(QStringLiteral("\r\n"))) {
                textToSend.replace(QStringLiteral("\n"), QStringLiteral("\r\n"));
            }
            const ushort* utf16Data = textToSend.utf16();
            int len = (textToSend.length() + 1) * sizeof(char16_t);
            utf16 = QByteArray(reinterpret_cast<const char*>(utf16Data), len);
        }
        resp.common.msgFlags = CB_RESPONSE_OK;
        resp.common.dataLen = utf16.size();
        resp.requestedFormatData = reinterpret_cast<const BYTE*>(utf16.constData());
        context->ServerFormatDataResponse(context, &resp);
        qInfo() << "CLIPRDR: Responded with CF_UNICODETEXT (" << utf16.size() << "bytes)";
    } else if (formatDataRequest->requestedFormatId == CF_TEXT) {
        QByteArray utf8;
        {
            QMutexLocker locker(&channel->m_mutex);
            utf8 = channel->m_lastHostClipboardText.toUtf8();
            utf8.append('\0');
        }
        resp.common.msgFlags = CB_RESPONSE_OK;
        resp.common.dataLen = utf8.size();
        resp.requestedFormatData = reinterpret_cast<const BYTE*>(utf8.constData());
        context->ServerFormatDataResponse(context, &resp);
        qInfo() << "CLIPRDR: Responded with CF_TEXT (" << utf8.size() << "bytes)";
    } else {
        resp.common.msgFlags = CB_RESPONSE_FAIL;
        context->ServerFormatDataResponse(context, &resp);
        qWarning() << "CLIPRDR: Unsupported formatId requested:" << formatDataRequest->requestedFormatId;
    }

    return CHANNEL_RC_OK;
}

UINT RdpCliprdrChannel::cliprdr_client_format_data_response(CliprdrServerContext* context, const CLIPRDR_FORMAT_DATA_RESPONSE* formatDataResponse)
{
    if (!context || !context->custom) return CHANNEL_RC_OK;
    auto* channel = static_cast<RdpCliprdrChannel*>(context->custom);

    qInfo() << "CLIPRDR: ClientFormatDataResponse, flags:" << formatDataResponse->common.msgFlags
            << "dataLen:" << formatDataResponse->common.dataLen;

    if (!(formatDataResponse->common.msgFlags & CB_RESPONSE_OK) ||
        formatDataResponse->common.dataLen == 0 ||
        !formatDataResponse->requestedFormatData) {
        return CHANNEL_RC_OK;
    }

    if (channel->m_clientFileGroupDescriptorFormatId != 0 &&
        context->lastRequestedFormatId == channel->m_clientFileGroupDescriptorFormatId) {
        // We received FileGroupDescriptorW!
        if (formatDataResponse->common.dataLen < sizeof(UINT32)) {
            qWarning() << "CLIPRDR: Received invalid FileGroupDescriptorW payload (too short):" << formatDataResponse->common.dataLen;
            return CHANNEL_RC_OK;
        }

        const BYTE* data = formatDataResponse->requestedFormatData;
        UINT32 cItems = *reinterpret_cast<const UINT32*>(data);
        qInfo() << "CLIPRDR: FileGroupDescriptorW contains" << cItems << "files";

        // Clean up previous incoming files if any
        for (auto& inf : channel->m_incomingFiles) {
            if (inf.localFile) {
                inf.localFile->close();
                delete inf.localFile;
                inf.localFile = nullptr;
            }
        }
        channel->m_incomingFiles.clear();
        channel->m_completedIncomingFilePaths.clear();
        channel->m_currentIncomingFileIndex = 0;

        QString targetDir = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation) + "/rdp-clipboard";
        if (targetDir.isEmpty() || !QDir().mkpath(targetDir)) {
            targetDir = QDir::tempPath() + "/rdp-clipboard";
            QDir().mkpath(targetDir);
        }

        const size_t fdSize = sizeof(FILEDESCRIPTORW);
        const BYTE* currentFd = data + sizeof(UINT32);

        for (UINT32 i = 0; i < cItems; i++) {
            if ((size_t)(currentFd - data + fdSize) > (size_t)formatDataResponse->common.dataLen) {
                qWarning() << "CLIPRDR: Truncated FILEDESCRIPTORW at item" << i;
                break;
            }

            const FILEDESCRIPTORW* fd = reinterpret_cast<const FILEDESCRIPTORW*>(currentFd);
            QString baseName = QString::fromUtf16(reinterpret_cast<const char16_t*>(fd->cFileName));
            baseName = QFileInfo(baseName).fileName();
            if (baseName.isEmpty()) {
                baseName = QString("clipboard_file_%1").arg(i);
            }

            uint64_t fileSize = ((uint64_t)fd->nFileSizeHigh << 32) | fd->nFileSizeLow;
            QString localPath = targetDir + "/" + baseName;

            IncomingFileTransfer transfer;
            transfer.fileName = baseName;
            transfer.localPath = localPath;
            transfer.fileSize = fileSize;
            transfer.receivedBytes = 0;
            transfer.localFile = nullptr;

            channel->m_incomingFiles.append(transfer);
            qInfo() << "CLIPRDR: Queued incoming file [" << i << "]:" << baseName << "size:" << fileSize << "bytes -> target:" << localPath;

            currentFd += fdSize;
        }

        if (!channel->m_incomingFiles.isEmpty()) {
            channel->startNextIncomingFile(context);
        }
        return CHANNEL_RC_OK;
    }

    // Fall back to text formats
    QString text;
    if (context->lastRequestedFormatId == CF_UNICODETEXT) {
        text = QString::fromUtf16(
            reinterpret_cast<const char16_t*>(formatDataResponse->requestedFormatData),
            formatDataResponse->common.dataLen / sizeof(char16_t)
        );
    } else {
        text = QString::fromUtf8(
            reinterpret_cast<const char*>(formatDataResponse->requestedFormatData),
            formatDataResponse->common.dataLen
        );
    }
    while (!text.isEmpty() && text.endsWith(QChar('\0'))) {
        text.chop(1);
    }
    text.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    if (!text.isEmpty()) {
        qInfo() << "CLIPRDR: Received text from client (" << text.length() << "chars):" << text.left(40);
        {
            QMutexLocker locker(&channel->m_mutex);
            channel->m_lastHostClipboardText = text;
            channel->m_outgoingFiles.clear();
            channel->m_outgoingFgdData.clear();
        }
        emit channel->clientClipboardReceived(text);
    }

    return CHANNEL_RC_OK;
}

void RdpCliprdrChannel::startNextIncomingFile(CliprdrServerContext* context)
{
    while (m_currentIncomingFileIndex < (uint32_t)m_incomingFiles.size()) {
        IncomingFileTransfer& item = m_incomingFiles[m_currentIncomingFileIndex];

        if (!item.localFile) {
            item.localFile = new QFile(item.localPath);
            if (!item.localFile->open(QIODevice::ReadWrite | QIODevice::Truncate)) {
                qWarning() << "CLIPRDR: Failed to open target file for writing:" << item.localPath;
                delete item.localFile;
                item.localFile = nullptr;
                m_currentIncomingFileIndex++;
                continue;
            }
        }

        if (item.fileSize == 0) {
            item.localFile->close();
            delete item.localFile;
            item.localFile = nullptr;
            m_completedIncomingFilePaths.append(item.localPath);
            m_currentIncomingFileIndex++;
            continue;
        }

        item.requestedBytes = 0;
        item.receivedBytes = 0;

        m_fileStreamId++;
        requestNextFileChunk(context);
        return;
    }

    // All files completed!
    if (!m_completedIncomingFilePaths.isEmpty()) {
        qInfo() << "CLIPRDR: All incoming files received (" << m_completedIncomingFilePaths.size() << "files):" << m_completedIncomingFilePaths;
        {
            QMutexLocker locker(&m_mutex);
            m_lastIncomingFiles = m_completedIncomingFilePaths;
        }
        emit clientFilesReceived(m_completedIncomingFilePaths);
        m_incomingFiles.clear();
        m_completedIncomingFilePaths.clear();
    }
}

void RdpCliprdrChannel::requestNextFileChunk(CliprdrServerContext* context)
{
    if (m_currentIncomingFileIndex >= (uint32_t)m_incomingFiles.size()) return;
    IncomingFileTransfer& item = m_incomingFiles[m_currentIncomingFileIndex];

    if (item.receivedBytes >= item.fileSize) return;

    uint64_t offset = item.receivedBytes;
    uint64_t remaining = item.fileSize - item.receivedBytes;
    UINT32 chunkSize = (UINT32)std::min((uint64_t)65536, remaining);

    CLIPRDR_FILE_CONTENTS_REQUEST req;
    memset(&req, 0, sizeof(req));
    req.common.msgType = CB_FILECONTENTS_REQUEST;
    req.streamId = m_fileStreamId;
    req.listIndex = m_currentIncomingFileIndex;
    req.dwFlags = FILECONTENTS_RANGE;
    req.nPositionLow = (UINT32)(offset & 0xFFFFFFFF);
    req.nPositionHigh = (UINT32)(offset >> 32);
    req.cbRequested = chunkSize;
    req.haveClipDataId = FALSE;
    req.clipDataId = 0;

    item.requestedBytes = offset + chunkSize;

    qInfo() << "CLIPRDR: Requesting chunk for file [" << m_currentIncomingFileIndex << "]"
            << item.fileName << "offset:" << offset << "size:" << req.cbRequested << "streamId:" << req.streamId;
    context->ServerFileContentsRequest(context, &req);
}

UINT RdpCliprdrChannel::cliprdr_client_file_contents_request(CliprdrServerContext* context, const CLIPRDR_FILE_CONTENTS_REQUEST* fileContentsRequest)
{
    if (!context || !context->custom) return CHANNEL_RC_OK;
    auto* channel = static_cast<RdpCliprdrChannel*>(context->custom);

    qInfo() << "CLIPRDR: ClientFileContentsRequest: listIndex=" << fileContentsRequest->listIndex
            << "dwFlags=" << fileContentsRequest->dwFlags
            << "streamId=" << fileContentsRequest->streamId
            << "cbRequested=" << fileContentsRequest->cbRequested;

    CLIPRDR_FILE_CONTENTS_RESPONSE resp;
    memset(&resp, 0, sizeof(resp));
    resp.common.msgType = CB_FILECONTENTS_RESPONSE;
    resp.streamId = fileContentsRequest->streamId;

    QMutexLocker locker(&channel->m_mutex);
    if (fileContentsRequest->listIndex >= (UINT32)channel->m_outgoingFiles.size()) {
        qWarning() << "CLIPRDR: Invalid listIndex in ClientFileContentsRequest:" << fileContentsRequest->listIndex;
        resp.common.msgFlags = CB_RESPONSE_FAIL;
        context->ServerFileContentsResponse(context, &resp);
        return CHANNEL_RC_OK;
    }

    QString filePath = channel->m_outgoingFiles[fileContentsRequest->listIndex];
    QFileInfo fi(filePath);
    if (!fi.exists()) {
        qWarning() << "CLIPRDR: File does not exist:" << filePath;
        resp.common.msgFlags = CB_RESPONSE_FAIL;
        context->ServerFileContentsResponse(context, &resp);
        return CHANNEL_RC_OK;
    }

    if (fileContentsRequest->dwFlags & FILECONTENTS_SIZE) {
        uint64_t fileSize = fi.isDir() ? 0 : fi.size();
        resp.common.msgFlags = CB_RESPONSE_OK;
        if (fileContentsRequest->cbRequested == sizeof(UINT32)) {
            UINT32 sz32 = static_cast<UINT32>(fileSize);
            resp.cbRequested = sizeof(UINT32);
            resp.requestedData = reinterpret_cast<const BYTE*>(&sz32);
            context->ServerFileContentsResponse(context, &resp);
        } else {
            resp.cbRequested = sizeof(uint64_t);
            resp.requestedData = reinterpret_cast<const BYTE*>(&fileSize);
            context->ServerFileContentsResponse(context, &resp);
        }
        qInfo() << "CLIPRDR: Responded to FILECONTENTS_SIZE for file [" << fileContentsRequest->listIndex << "]:" << fileSize << "bytes";
        return CHANNEL_RC_OK;
    }

    if (fileContentsRequest->dwFlags & FILECONTENTS_RANGE) {
        if (fi.isDir()) {
            resp.common.msgFlags = CB_RESPONSE_OK;
            resp.cbRequested = 0;
            resp.requestedData = nullptr;
            context->ServerFileContentsResponse(context, &resp);
            return CHANNEL_RC_OK;
        }

        QFile file(filePath);
        if (!file.open(QIODevice::ReadOnly)) {
            qWarning() << "CLIPRDR: Could not open file for reading:" << filePath;
            resp.common.msgFlags = CB_RESPONSE_FAIL;
            context->ServerFileContentsResponse(context, &resp);
            return CHANNEL_RC_OK;
        }

        uint64_t offset = ((uint64_t)fileContentsRequest->nPositionHigh << 32) | fileContentsRequest->nPositionLow;
        if (!file.seek(offset)) {
            qWarning() << "CLIPRDR: Could not seek to offset:" << offset << "in" << filePath;
            resp.common.msgFlags = CB_RESPONSE_FAIL;
            context->ServerFileContentsResponse(context, &resp);
            return CHANNEL_RC_OK;
        }

        QByteArray buffer = file.read(fileContentsRequest->cbRequested);
        resp.common.msgFlags = CB_RESPONSE_OK;
        resp.cbRequested = buffer.size();
        resp.requestedData = reinterpret_cast<const BYTE*>(buffer.constData());
        context->ServerFileContentsResponse(context, &resp);
        qInfo() << "CLIPRDR: Responded to FILECONTENTS_RANGE at offset:" << offset << "sent:" << buffer.size() << "bytes";
        return CHANNEL_RC_OK;
    }

    resp.common.msgFlags = CB_RESPONSE_FAIL;
    context->ServerFileContentsResponse(context, &resp);
    return CHANNEL_RC_OK;
}

UINT RdpCliprdrChannel::cliprdr_client_file_contents_response(CliprdrServerContext* context, const CLIPRDR_FILE_CONTENTS_RESPONSE* fileContentsResponse)
{
    if (!context || !context->custom) return CHANNEL_RC_OK;
    auto* channel = static_cast<RdpCliprdrChannel*>(context->custom);

    if (channel->m_currentIncomingFileIndex >= (uint32_t)channel->m_incomingFiles.size()) {
        qWarning() << "CLIPRDR: Unexpected file contents response, no active file (streamId=" << fileContentsResponse->streamId << ")";
        return CHANNEL_RC_OK;
    }

    IncomingFileTransfer& item = channel->m_incomingFiles[channel->m_currentIncomingFileIndex];

    if (!(fileContentsResponse->common.msgFlags & CB_RESPONSE_OK)) {
        qWarning() << "CLIPRDR: File contents response failed for file" << item.fileName
                   << "flags:" << fileContentsResponse->common.msgFlags
                   << "streamId:" << fileContentsResponse->streamId;
        if (item.localFile) {
            item.localFile->close();
            delete item.localFile;
            item.localFile = nullptr;
        }
        channel->m_currentIncomingFileIndex++;
        channel->startNextIncomingFile(context);
        return CHANNEL_RC_OK;
    }

    if (fileContentsResponse->streamId != channel->m_fileStreamId) {
        qWarning() << "CLIPRDR: Stale or mismatched streamId:" << fileContentsResponse->streamId
                   << "expected:" << channel->m_fileStreamId;
        return CHANNEL_RC_OK;
    }

    if (fileContentsResponse->cbRequested == 0 || !fileContentsResponse->requestedData) {
        qWarning() << "CLIPRDR: Empty data in FileContentsResponse for file" << item.fileName;
        if (item.localFile) {
            item.localFile->close();
            delete item.localFile;
            item.localFile = nullptr;
        }
        channel->m_currentIncomingFileIndex++;
        channel->startNextIncomingFile(context);
        return CHANNEL_RC_OK;
    }

    if (item.localFile) {
        item.localFile->seek(item.receivedBytes);
        qint64 written = item.localFile->write(reinterpret_cast<const char*>(fileContentsResponse->requestedData),
                                               fileContentsResponse->cbRequested);
        if (written > 0) {
            item.receivedBytes += written;
        }
    }

    if (item.receivedBytes >= item.fileSize) {
        // File is complete!
        qInfo() << "CLIPRDR: Completed transfer of file:" << item.localPath << "(" << item.receivedBytes << "bytes)";
        if (item.localFile) {
            item.localFile->close();
            delete item.localFile;
            item.localFile = nullptr;
        }
        channel->m_completedIncomingFilePaths.append(item.localPath);
        channel->m_currentIncomingFileIndex++;
        channel->startNextIncomingFile(context);
    } else {
        // Request next chunk sequentially (stop-and-wait)
        channel->requestNextFileChunk(context);
    }

    return CHANNEL_RC_OK;
}

UINT RdpCliprdrChannel::cliprdr_client_lock_clipboard_data(CliprdrServerContext* context, const CLIPRDR_LOCK_CLIPBOARD_DATA* lockClipboardData)
{
    Q_UNUSED(context);
    Q_UNUSED(lockClipboardData);
    return CHANNEL_RC_OK;
}

UINT RdpCliprdrChannel::cliprdr_client_unlock_clipboard_data(CliprdrServerContext* context, const CLIPRDR_UNLOCK_CLIPBOARD_DATA* unlockClipboardData)
{
    Q_UNUSED(context);
    Q_UNUSED(unlockClipboardData);
    return CHANNEL_RC_OK;
}

void RdpCliprdrChannel::onHostClipboardChanged(const QString &text)
{
    QMutexLocker locker(&m_mutex);
    if (!m_incomingFiles.isEmpty()) {
        return;
    }
    if (!m_outgoingFiles.isEmpty()) {
        QStringList paths = m_outgoingFiles;
        QString pathsJoinedN = paths.join(QLatin1Char('\n'));
        QString pathsJoinedRN = paths.join(QStringLiteral("\r\n"));
        if (text == pathsJoinedN || text == pathsJoinedRN) {
            return;
        }
        if (paths.size() == 1) {
            if (text == paths.first() || text == QUrl::fromLocalFile(paths.first()).toString()) {
                return;
            }
        }
    }
    if (m_lastHostClipboardText == text && m_outgoingFiles.isEmpty()) return;
    m_lastHostClipboardText = text;
    m_outgoingFiles.clear();
    m_outgoingFgdData.clear();
    m_lastIncomingFiles.clear();

    if (!m_context || !m_cliprdrReady) return;

    qInfo() << "CLIPRDR: Host clipboard changed (" << text.length() << "chars), announcing ServerFormatList";

    CLIPRDR_FORMAT_LIST formatList;
    memset(&formatList, 0, sizeof(formatList));
    CLIPRDR_FORMAT formats[2];
    formats[0].formatId = CF_UNICODETEXT;
    formats[0].formatName = nullptr;
    formats[1].formatId = CF_TEXT;
    formats[1].formatName = nullptr;

    formatList.common.msgType = CB_FORMAT_LIST;
    formatList.common.msgFlags = 0;
    formatList.numFormats = 2;
    formatList.formats = formats;

    m_context->ServerFormatList(m_context, &formatList);
}

void RdpCliprdrChannel::onHostClipboardFilesChanged(const QStringList &filePaths)
{
    struct OutgoingFileEntry {
        QString fullPath;
        QString relativePath;
        bool isDirectory;
        uint64_t fileSize;
        QDateTime lastModified;
    };

    QList<OutgoingFileEntry> entries;
    QStringList validRootFiles;

    for (const QString& path : filePaths) {
        QFileInfo fi(path);
        if (!fi.exists()) continue;

        validRootFiles.append(path);

        OutgoingFileEntry entry;
        entry.fullPath = path;
        entry.relativePath = fi.fileName();
        entry.isDirectory = fi.isDir();
        entry.fileSize = fi.isDir() ? 0 : fi.size();
        entry.lastModified = fi.lastModified();
        entries.append(entry);

        if (fi.isDir()) {
            QDir rootDir(path);
            QDirIterator it(path, QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden, QDirIterator::Subdirectories);
            while (it.hasNext()) {
                it.next();
                QFileInfo childFi = it.fileInfo();
                QString rel = fi.fileName() + QLatin1Char('\\') + rootDir.relativeFilePath(childFi.absoluteFilePath());
                rel.replace(QLatin1Char('/'), QLatin1Char('\\'));

                OutgoingFileEntry childEntry;
                childEntry.fullPath = childFi.absoluteFilePath();
                childEntry.relativePath = rel;
                childEntry.isDirectory = childFi.isDir();
                childEntry.fileSize = childFi.isDir() ? 0 : childFi.size();
                childEntry.lastModified = childFi.lastModified();
                entries.append(childEntry);
            }
        }
    }

    if (entries.isEmpty()) return;

    QMutexLocker locker(&m_mutex);
    if (!m_incomingFiles.isEmpty()) return;

    if (m_lastIncomingFiles == validRootFiles) return;

    QStringList allFullPaths;
    for (const auto& e : entries) {
        allFullPaths.append(e.fullPath);
    }
    if (m_outgoingFiles == allFullPaths) return;

    m_outgoingFiles = allFullPaths;
    m_lastIncomingFiles.clear();

    // Build FileGroupDescriptorW payload
    UINT32 cItems = entries.size();
    size_t headerSize = sizeof(UINT32);
    size_t itemSize = sizeof(FILEDESCRIPTORW);
    size_t totalSize = headerSize + cItems * itemSize;

    m_outgoingFgdData.resize(totalSize);
    m_outgoingFgdData.fill(0);

    BYTE* ptr = reinterpret_cast<BYTE*>(m_outgoingFgdData.data());
    *reinterpret_cast<UINT32*>(ptr) = cItems;
    ptr += headerSize;

    for (int i = 0; i < entries.size(); i++) {
        const auto& item = entries[i];
        FILEDESCRIPTORW* fd = reinterpret_cast<FILEDESCRIPTORW*>(ptr + i * itemSize);
        fd->dwFlags = FD_FILESIZE | FD_WRITESTIME | FD_ATTRIBUTES;
        if (item.isDirectory) {
            fd->dwFileAttributes = 0x00000010; // FILE_ATTRIBUTE_DIRECTORY
            fd->nFileSizeLow = 0;
            fd->nFileSizeHigh = 0;
        } else {
            fd->dwFileAttributes = 0x00000080; // FILE_ATTRIBUTE_NORMAL
            fd->nFileSizeLow = (DWORD)(item.fileSize & 0xFFFFFFFF);
            fd->nFileSizeHigh = (DWORD)(item.fileSize >> 32);
        }

        uint64_t mtimeMs = item.lastModified.toMSecsSinceEpoch();
        uint64_t winTime = (mtimeMs + 11644473600000ULL) * 10000ULL;
        fd->ftLastWriteTime.dwLowDateTime = (DWORD)(winTime & 0xFFFFFFFF);
        fd->ftLastWriteTime.dwHighDateTime = (DWORD)(winTime >> 32);

        int copyLen = static_cast<int>(std::min<qsizetype>(item.relativePath.length(), 259));
        memcpy(fd->cFileName, item.relativePath.utf16(), copyLen * sizeof(char16_t));
        fd->cFileName[copyLen] = 0;
    }

    m_lastHostClipboardText = validRootFiles.join(QStringLiteral("\r\n"));

    if (!m_context || !m_cliprdrReady) return;

    qInfo() << "CLIPRDR: Host clipboard files changed (" << entries.size() << "entries, root files:" << validRootFiles.size() << "), announcing ServerFormatList";

    CLIPRDR_FORMAT_LIST formatList;
    memset(&formatList, 0, sizeof(formatList));
    CLIPRDR_FORMAT formats[4];
    formats[0].formatId = CF_UNICODETEXT;
    formats[0].formatName = nullptr;
    formats[1].formatId = CF_TEXT;
    formats[1].formatName = nullptr;
    formats[2].formatId = m_formatFileGroupDescriptorW;
    formats[2].formatName = const_cast<char*>("FileGroupDescriptorW");
    formats[3].formatId = m_formatFileContents;
    formats[3].formatName = const_cast<char*>("FileContents");

    formatList.common.msgType = CB_FORMAT_LIST;
    formatList.common.msgFlags = 0;
    formatList.numFormats = 4;
    formatList.formats = formats;

    m_context->ServerFormatList(m_context, &formatList);
}
