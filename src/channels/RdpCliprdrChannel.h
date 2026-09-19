#pragma once

#include <QObject>
#include <QMutex>
#include <QString>
#include <QStringList>
#include <QByteArray>
#include <QFile>
#include <QHash>
#include <QList>
#include <atomic>
#include <freerdp/freerdp.h>
#include <freerdp/server/cliprdr.h>
#include <winpr/wtsapi.h>
#include <winpr/shell.h>

class RdpCliprdrChannel : public QObject
{
    Q_OBJECT
public:
    explicit RdpCliprdrChannel(QObject *parent = nullptr);
    ~RdpCliprdrChannel() override;

    bool initialize(HANDLE vcm, rdpContext* rdpcontext);
    void close();

    bool isReady() const { return m_cliprdrReady.load(); }

public slots:
    void onHostClipboardChanged(const QString &text);
    void onHostClipboardFilesChanged(const QStringList &filePaths);

signals:
    void clientClipboardReceived(const QString &text);
    void clientFilesReceived(const QStringList &filePaths);

private:
    static UINT cliprdr_client_capabilities(CliprdrServerContext* context, const CLIPRDR_CAPABILITIES* capabilities);
    static UINT cliprdr_client_format_list(CliprdrServerContext* context, const CLIPRDR_FORMAT_LIST* formatList);
    static UINT cliprdr_client_format_list_response(CliprdrServerContext* context, const CLIPRDR_FORMAT_LIST_RESPONSE* formatListResponse);
    static UINT cliprdr_client_format_data_request(CliprdrServerContext* context, const CLIPRDR_FORMAT_DATA_REQUEST* formatDataRequest);
    static UINT cliprdr_client_format_data_response(CliprdrServerContext* context, const CLIPRDR_FORMAT_DATA_RESPONSE* formatDataResponse);
    static UINT cliprdr_client_file_contents_request(CliprdrServerContext* context, const CLIPRDR_FILE_CONTENTS_REQUEST* fileContentsRequest);
    static UINT cliprdr_client_file_contents_response(CliprdrServerContext* context, const CLIPRDR_FILE_CONTENTS_RESPONSE* fileContentsResponse);
    static UINT cliprdr_client_lock_clipboard_data(CliprdrServerContext* context, const CLIPRDR_LOCK_CLIPBOARD_DATA* lockClipboardData);
    static UINT cliprdr_client_unlock_clipboard_data(CliprdrServerContext* context, const CLIPRDR_UNLOCK_CLIPBOARD_DATA* unlockClipboardData);

    void startNextIncomingFile(CliprdrServerContext* context);
    void requestNextFileChunk(CliprdrServerContext* context);

    CliprdrServerContext* m_context{nullptr};
    mutable QMutex m_mutex;
    std::atomic<bool> m_cliprdrReady{false};

    UINT32 m_formatFileGroupDescriptorW{0xC001};
    UINT32 m_formatFileContents{0xC002};
    UINT32 m_clientFileGroupDescriptorFormatId{0};
    QStringList m_outgoingFiles;
    QByteArray m_outgoingFgdData;

    struct IncomingFileTransfer {
        QString fileName;
        QString localPath;
        uint64_t fileSize{0};
        uint64_t requestedBytes{0};
        uint64_t receivedBytes{0};
        QFile* localFile{nullptr};
    };
    QList<IncomingFileTransfer> m_incomingFiles;
    uint32_t m_currentIncomingFileIndex{0};
    uint32_t m_fileStreamId{0};
    QStringList m_completedIncomingFilePaths;
    QStringList m_lastIncomingFiles;
    QString m_lastHostClipboardText;
};
