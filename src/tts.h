#ifndef TTS_H
#define TTS_H

#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QBuffer>
#include <QFile>
#include <QUrl>
#include <QUrlQuery>
#include <QObject>
#include <QDebug>
#include <QtMultimedia/QMediaPlayer>
#include <QtMultimedia/QAudioOutput>

class TextToSpeech : public QObject {
    Q_OBJECT

public:
    TextToSpeech(QObject *parent = nullptr);

    ~TextToSpeech();

    void getTTS(const QString &text, const QString &ref_audio_path, const QString &text_lang="zh", const QString &prompt_lang="zh"
                , const QString &prompt_text="");

    bool isPlaying() const;

    bool hasPlaybackStarted() const;

    bool hasPlaybackError() const;

    bool hasRequestError() const;

    qsizetype lastAudioByteCount() const;

private slots:
    void onGetFinished();

signals:
    void finished();

    void stop();

    void playbackStarted();


private:
    QNetworkAccessManager *manager;
    QNetworkReply *m_reply = nullptr;
    QMediaPlayer* player;
    QBuffer *buffer;
    QAudioOutput *audioOutput;
    bool m_hasPlaybackStarted = false;
    bool m_playbackErrorOccurred = false;
    bool m_requestErrorOccurred = false;
    qsizetype m_lastAudioByteCount = 0;
};


#endif // TTS_H
