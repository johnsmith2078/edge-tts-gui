#ifndef COMMUNICATE_H
#define COMMUNICATE_H

#include <QUuid>
#include <QFile>
#include <QWebSocket>
#include <QtMultimedia/QMediaPlayer>
#include <QtMultimedia/QAudioOutput>

// Constants
const QString TRUSTED_CLIENT_TOKEN = "6A5AA1D4EAFF4E9FB37E23D68491D6F4";
const QString WSS_URL =
    "wss://speech.platform.bing.com/consumer/speech/synthesize/"
    "readaloud/edge/v1?TrustedClientToken="
    + TRUSTED_CLIENT_TOKEN;
const QString VOICE_LIST =
    "https://speech.platform.bing.com/consumer/speech/synthesize/"
    "readaloud/voices/list?trustedclienttoken="
    + TRUSTED_CLIENT_TOKEN;

// Class for communicating with the service
class Communicate : public QObject
{
    Q_OBJECT

public:
    Communicate(QString text, QString voice = "zh-CN, XiaoyiNeural", QObject *parent = nullptr);

    ~Communicate();

    void start();

private slots:
    void onConnected();

    void onBinaryMessageReceived(const QByteArray &message);

    void onTextMessageReceived(const QString &message);

    void onDisconnected();

signals:
    void finished();

private:
    QString m_text;
    QString m_voice;
    QString m_rate = "+0%";
    QString m_volume = "+0%";
    QString m_pitch = "+0Hz";
    QWebSocket *m_webSocket;
    QByteArray m_audioDataReceived = "";
    QString m_audioFile = "audio.mp3";
    bool m_downloadAudio = false;

private:
    // Utility functions
    QString connect_id();

    QString date_to_string();

    QString escape(QString data);

    QString remove_incompatible_characters(QString str);

    QString mkssml(QString text, QString voice, QString rate, QString volume, QString pitch);

    QString ssml_headers_plus_data(const QString& requestId, const QString& timestamp, const QString& ssml);

    QPair<QMap<QString, QString>, QString> get_headers_and_data(const QString& message);
};

#endif // COMMUNICATE_H
