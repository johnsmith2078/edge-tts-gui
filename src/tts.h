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

private slots:
    void onGetFinished();

signals:
    void finished();

    void stop();


private:
    QNetworkAccessManager *manager;
    QMediaPlayer* player;
    QBuffer *buffer;
    QAudioOutput *audioOutput;
};


#endif // TTS_H
