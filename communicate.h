#ifndef COMMUNICATE_H
#define COMMUNICATE_H

#include <QDir>
#include <QUuid>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QWebSocket>
#include <QSystemTrayIcon>
#include <QDesktopServices>
#include <QtMultimedia/QMediaPlayer>
#include <QtMultimedia/QAudioOutput>

// Class for communicating with the service
class Communicate : public QObject
{
    Q_OBJECT

public:
    Communicate(QObject *parent = nullptr);

    ~Communicate();

    void save();

    void play();

    void setText(QString text);

    void setVoice(QString voice);

    void setFileName(QString fileName);

    void setDuplicated(bool dup);

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

    void audioDataReceived();

    void duplicated();

private:
    QString m_text;
    QString m_voice;
    QString m_fileName;
    QString m_rate = "+0%";
    QString m_volume = "+0%";
    QString m_pitch = "+0Hz";
    QWebSocket m_webSocket;
    QByteArray m_audioDataReceived = "";
    QString m_audioFile = "audio.mp3";
    bool m_downloadAudio = false;
    qsizetype m_textPartIndex;
    QString m_date;
    bool m_isDuplicated = false;
    QMediaPlayer* m_player;
    QAudioOutput* m_audioOutput;

    static const qsizetype maxMessageSize = 8192;

private:
    // Utility functions
    QString connect_id();

    QString date_to_string();

    QString escape(QString data);

    QString remove_incompatible_characters(QString str);

    QString mkssml(QString text, QString voice, QString rate, QString volume, QString pitch);

    QString ssml_headers_plus_data(const QString& requestId, const QString& timestamp, const QString& ssml);

    QPair<QMap<QString, QString>, QString> get_headers_and_data(const QString& message);

    void showInFolder(const QString& path);
};

#endif // COMMUNICATE_H
