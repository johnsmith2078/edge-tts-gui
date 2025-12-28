#include "tts.h"

TextToSpeech::TextToSpeech(QObject *parent)
    : QObject(parent)
{
    manager = new QNetworkAccessManager(this);
    player = new QMediaPlayer(this);
    buffer = new QBuffer(this);
    audioOutput = new QAudioOutput(this);

    QObject::connect(player, &QMediaPlayer::positionChanged, this, [this](qint64 position) {
        if (position <= 0 || m_hasPlaybackStarted) {
            return;
        }
        m_hasPlaybackStarted = true;
        emit playbackStarted();
    });

    QObject::connect(player, &QMediaPlayer::errorOccurred, this, [this](QMediaPlayer::Error error, const QString &errorString) {
        if (error == QMediaPlayer::NoError || m_playbackErrorOccurred) {
            return;
        }
        m_playbackErrorOccurred = true;
        qDebug() << "Media player error:" << errorString;
        emit finished();
    });

    QObject::connect(player, &QMediaPlayer::mediaStatusChanged, [&](QMediaPlayer::MediaStatus status) {
        if (status == QMediaPlayer::EndOfMedia) {
            emit finished();
        }
    });

    QObject::connect(this, &TextToSpeech::stop, player, &QMediaPlayer::stop);
    QObject::connect(this, &TextToSpeech::stop, this, [this]() {
        if (m_reply) {
            m_reply->abort();
            m_reply->deleteLater();
            m_reply = nullptr;
        }
        emit finished();
    });
}

TextToSpeech::~TextToSpeech() {
    delete player;
    delete buffer;
    delete audioOutput;
}

void TextToSpeech::getTTS(const QString &text, const QString &ref_audio_path, const QString &text_lang, const QString &prompt_lang
                          , const QString &prompt_text) {
    m_hasPlaybackStarted = false;
    m_playbackErrorOccurred = false;
    m_requestErrorOccurred = false;
    m_lastAudioByteCount = 0;
    if (m_reply) {
        m_reply->abort();
        m_reply->deleteLater();
        m_reply = nullptr;
    }
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
    m_reply = manager->get(request);
    connect(m_reply, &QNetworkReply::finished, this, &TextToSpeech::onGetFinished);
}

bool TextToSpeech::isPlaying() const
{
    return player->playbackState() == QMediaPlayer::PlayingState;
}

bool TextToSpeech::hasPlaybackStarted() const
{
    return m_hasPlaybackStarted;
}

bool TextToSpeech::hasPlaybackError() const
{
    return m_playbackErrorOccurred;
}

bool TextToSpeech::hasRequestError() const
{
    return m_requestErrorOccurred;
}

qsizetype TextToSpeech::lastAudioByteCount() const
{
    return m_lastAudioByteCount;
}

void TextToSpeech::onGetFinished() {
    QNetworkReply *reply = qobject_cast<QNetworkReply *>(sender());
    if (reply) {
        if (reply != m_reply) {
            reply->deleteLater();
            return;
        }
        m_reply = nullptr;
        if (reply->error() == QNetworkReply::NoError) {
            qDebug() << "Get Response!";

            QByteArray response = reply->readAll();
            m_lastAudioByteCount = response.size();
            if (response.isEmpty()) {
                m_requestErrorOccurred = true;
                emit finished();
                reply->deleteLater();
                return;
            }

            buffer->close();

            buffer->setData(response);
            buffer->open(QIODevice::ReadOnly);

            player->setAudioOutput(audioOutput);

            // 设置媒体源为 QBuffer
            player->setSourceDevice(buffer);

            // 播放音频
            player->play();
        } else {
            m_requestErrorOccurred = true;
            qDebug() << "GET Error:" << reply->errorString();
            emit finished();
        }
        reply->deleteLater();
    }
}
