#ifndef COMMUNICATE_H
#define COMMUNICATE_H

#include <utility>

#include <QBuffer>
#include <QIcon>
#include <QMap>
#include <QQueue>
#include <QTimer>
#include <QVector>
#include <QWebSocket>
#include <QtMultimedia/QAudioOutput>
#include <QtMultimedia/QMediaPlayer>

class Communicate : public QObject
{
    Q_OBJECT

public:
    explicit Communicate(QObject *parent = nullptr);
    ~Communicate() override;

    void save();
    void play();
    void forcePlay();
    void setText(QString text);
    void setVoice(QString voice);
    void setFileName(QString fileName);
    void setDuplicated(bool dup);

    bool isPlaying() const;
    bool hasPlaybackStarted() const;
    bool isSynthesisComplete() const;
    qsizetype audioBytesReceived() const;
    bool hasPlaybackError() const;

public slots:
    void start();

private slots:
    void onConnected();
    void onBinaryMessageReceived(const QByteArray &message);
    void onTextMessageReceived(const QString &message);
    void onDisconnected();
    void sendNextTextPart();

signals:
    void finished();
    void stop();
    void saveFinished();
    void saveFailed(const QString &error);
    void saveProgressChanged(int percent);
    void audioDataReceived();
    void playbackStarted();

private:
    QString connect_id() const;
    QString date_to_string() const;
    QString escape(QString data) const;
    QString remove_incompatible_characters(QString str) const;
    QString mkssml(const QString &text, const QString &voice, const QString &rate,
                   const QString &volume, const QString &pitch) const;
    QString ssml_headers_plus_data(const QString &requestId, const QString &timestamp,
                                   const QString &ssml) const;
    bool get_headers_and_data(const QString &message, QMap<QString, QString> &parameters,
                              QString &data) const;
    QString generateSecMsGecToken() const;
    QString generateSecMsGecVersion() const;
    QString generateMuid() const;
    int findSafeUtf8SplitPoint(const QByteArray &text, int limit) const;
    int adjustSplitPointForXmlEntity(const QByteArray &text, int splitAt) const;
    QVector<QString> splitTextByByteLength(const QString &text, int byteLength) const;
    QVector<QString> splitTextForPlayback(const QString &text, int initialByteLength,
                                          int subsequentByteLength) const;

    void fail(const QString &error);
    void notifyFinishedOnce();
    void notifySaveFailedOnce(const QString &error);
    void tryStartOrContinuePlayback();

private:
    QString m_text;
    QString m_voice;
    QString m_fileName;
    QString m_rate = "+0%";
    QString m_volume = "+0%";
    QString m_pitch = "+0Hz";

    QWebSocket m_webSocket;
    QMediaPlayer *m_player = nullptr;
    QAudioOutput *m_audioOutput = nullptr;
    QBuffer m_audioBuffer;

    QByteArray m_audioDataReceived;
    QByteArray m_currentTurnAudio;
    QQueue<QByteArray> m_readyPlaybackChunks;
    QVector<QString> m_textParts;

    bool m_downloadAudio = false;
    bool m_synthesisComplete = false;
    bool m_stopRequested = false;
    bool m_playbackErrorOccurred = false;
    bool m_playStarted = false;
    bool m_hasPlaybackStarted = false;
    bool m_finishedEmitted = false;
    bool m_saveFailureEmitted = false;
    bool m_switchingPlaybackSource = false;
    qsizetype m_textPartIndex = 0;
    qsizetype m_audioOffset = 0;
    QString m_date;

    static constexpr int ms_maxTextByteLength = 4096;
    static constexpr int ms_initialTextByteLength = 80;
    static constexpr int ms_targetTextByteLength = 780;
};

#endif // COMMUNICATE_H
