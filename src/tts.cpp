#include "tts.h"

TextToSpeech::TextToSpeech(QObject *parent)
    : QObject(parent)
{
    manager = new QNetworkAccessManager(this);
    player = new QMediaPlayer(this);
    buffer = new QBuffer(this);
    audioOutput = new QAudioOutput(this);

    QObject::connect(player, &QMediaPlayer::mediaStatusChanged, [&](QMediaPlayer::MediaStatus status) {
        if (status == QMediaPlayer::EndOfMedia) {
            emit finished();
        }
    });

    QObject::connect(this, &TextToSpeech::stop, player, &QMediaPlayer::stop);
}

TextToSpeech::~TextToSpeech() {
    delete player;
    delete buffer;
    delete audioOutput;
}

void TextToSpeech::getTTS(const QString &text, const QString &ref_audio_path, const QString &text_lang, const QString &prompt_lang
                          , const QString &prompt_text) {
    QUrl url("http://127.0.0.1:9880/tts");
    QUrlQuery query;
    query.addQueryItem("text", text);
    query.addQueryItem("text_lang", text_lang);
    query.addQueryItem("ref_audio_path", ref_audio_path);
    query.addQueryItem("prompt_lang", prompt_lang);
    if (!prompt_text.isEmpty()) {
        query.addQueryItem("prompt_text", prompt_text);
    }
    query.addQueryItem("text_split_method", "cut5");
    query.addQueryItem("batch_size", "1");
    query.addQueryItem("media_type", "wav");
    query.addQueryItem("streaming_mode", "true");
    url.setQuery(query);

    QNetworkRequest request(url);
    QNetworkReply *reply = manager->get(request);
    connect(reply, &QNetworkReply::finished, this, &TextToSpeech::onGetFinished);
}

void TextToSpeech::onGetFinished() {
    QNetworkReply *reply = qobject_cast<QNetworkReply *>(sender());
    if (reply) {
        if (reply->error() == QNetworkReply::NoError) {
            qDebug() << "Get Response!";

            QByteArray response = reply->readAll();

            buffer->close();

            buffer->setData(response);
            buffer->open(QIODevice::ReadOnly);

            player->setAudioOutput(audioOutput);

            // 设置媒体源为 QBuffer
            player->setSourceDevice(buffer);

            // 播放音频
            player->play();
        } else {
            qDebug() << "GET Error:" << reply->errorString();
        }
        reply->deleteLater();
    }
}
