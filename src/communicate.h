#ifndef COMMUNICATE_H
#define COMMUNICATE_H

#include <QBuffer>
#include <QDesktopServices>
#include <QFile>
#include <QFileInfo>
#include <QMap>
#include <QQueue>
#include <QSystemTrayIcon>
#include <QUuid>
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
    bool isPlaying();
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
    QString m_text;
    QString m_voice;
    QString m_fileName;
    QString m_rate = "+0%";
    QString m_volume = "+0%";
    QString m_pitch = "+0Hz";
    QWebSocket m_webSocket;
    QByteArray m_audioDataReceived;
    bool m_downloadAudio = false;
    qsizetype m_textPartIndex = 0;
    QString m_date;
    bool m_synthesisComplete = false;
    bool m_stopRequested = false;
    bool m_playbackErrorOccurred = false;
    QMediaPlayer *m_player = nullptr;
    QAudioOutput *m_audioOutput = nullptr;
    QBuffer m_audioBuffer;
    bool m_playStarted = false;
    bool m_hasPlaybackStarted = false;
    qsizetype m_audioOffset = 0;
    bool m_finishedEmitted = false;
    bool m_switchingPlaybackSource = false;
    QByteArray m_currentTurnAudio;
    QQueue<QByteArray> m_readyPlaybackChunks;
    QVector<QString> m_textParts;
    QSystemTrayIcon *m_trayIcon = nullptr;

    static const int ms_maxTextByteLength = 4096;
    static const int ms_initialTextByteLength = 80;
    static const int ms_targetTextByteLength = 780;

    QString connect_id();
    QString date_to_string();
    QString escape(QString data);
    QString remove_incompatible_characters(QString str);
    QString mkssml(QString text, QString voice, QString rate, QString volume, QString pitch);
    QString ssml_headers_plus_data(const QString &requestId, const QString &timestamp, const QString &ssml);
    bool get_headers_and_data(const QString &message, QMap<QString, QString> &parameters, QString &data) const;
    QString generateSecMsGecToken();
    QString generateSecMsGecVersion();
    QString generateMuid();
    int findSafeUtf8SplitPoint(const QByteArray &text, int limit);
    int adjustSplitPointForXmlEntity(const QByteArray &text, int splitAt);
    QVector<QString> splitTextByByteLength(const QString &text, int byteLength);
    QVector<QString> splitTextForPlayback(const QString &text, int initialByteLength, int subsequentByteLength);
    void failSession(const QString &error);
    void notifyFinishedOnce();
    void tryStartOrContinuePlayback();
};

#endif // COMMUNICATE_H
