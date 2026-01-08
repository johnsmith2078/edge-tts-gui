#ifndef DASHSCOPE_TTS_H
#define DASHSCOPE_TTS_H

#include <QAudioOutput>
#include <QBuffer>
#include <QList>
#include <QMediaPlayer>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QObject>
#include <QString>
#include <QStringList>

class QUrl;

class DashScopeTTS : public QObject {
    Q_OBJECT

public:
    explicit DashScopeTTS(QObject *parent = nullptr);
    ~DashScopeTTS() override;

    static QString resolvedApiKey();

    void getTTS(const QString &text, const QString &voice, const QString &languageType = "Chinese");

    bool isPlaying() const;
    bool hasPlaybackStarted() const;
    bool hasPlaybackError() const;
    bool hasRequestError() const;
    qsizetype lastAudioByteCount() const;

signals:
    void finished();
    void stop();
    void playbackStarted();

private slots:
    void onSynthesisFinished();
    void onAudioDownloadFinished();

private:
    void resetState();
    void abortRequests();
    void startSynthesisRequest();
    void startAudioDownload(const QUrl &url);

    static QString baseHttpApiUrl();
    static QString apiKey();
    static QString modelName();
    static QString readEnvValue(const QString &name);

    QNetworkAccessManager *m_manager;
    QNetworkReply *m_synthesisReply = nullptr;
    QNetworkReply *m_audioReply = nullptr;

    QMediaPlayer *m_player;
    QBuffer *m_buffer;
    QAudioOutput *m_audioOutput;

    bool m_hasPlaybackStarted = false;
    bool m_playbackErrorOccurred = false;
    bool m_requestErrorOccurred = false;
    qsizetype m_lastAudioByteCount = 0;

    QString m_text;
    QString m_voice;
    QString m_languageType;
    int m_endpointIndex = 0;
    bool m_hasCustomEndpoint = false;
    QStringList m_endpoints;
    int m_payloadIndex = 0;
    QList<QByteArray> m_payloads;
};

#endif // DASHSCOPE_TTS_H
