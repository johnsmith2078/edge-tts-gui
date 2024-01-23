#include "communicate.h"

Communicate::Communicate(QString text, QString voice, QObject *parent)
    : QObject(parent), text(text)
    , voice(QString("Microsoft Server Speech Text to Speech Voice (%1)").arg(voice))
{
    text = escape(remove_incompatible_characters(text));
    webSocket = new QWebSocket();
    connect(webSocket, &QWebSocket::connected, this, &Communicate::onConnected);
    connect(webSocket, &QWebSocket::binaryMessageReceived, this, &Communicate::onBinaryMessageReceived);
    connect(webSocket, &QWebSocket::textMessageReceived, this, &Communicate::onTextMessageReceived);
    connect(webSocket, &QWebSocket::disconnected, this, &Communicate::onDisconnected);
}

Communicate::~Communicate() {
    delete webSocket;
}

void Communicate::start() {
    QUrl url(WSS_URL + "&ConnectionId=" + connect_id());
    QNetworkRequest request(url);

    // Set headers
    request.setRawHeader("Pragma", "no-cache");
    request.setRawHeader("Cache-Control", "no-cache");
    request.setRawHeader("Origin", "chrome-extension://jdiccldimpdaibmpdkjnbmckianbfold");
    request.setRawHeader("Accept-Encoding", "gzip, deflate, br");
    request.setRawHeader("Accept-Language", "en-US,en;q=0.9");
    request.setRawHeader("User-Agent", "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/91.0.4472.77 Safari/537.36 Edg/91.0.864.41");

    // Open WebSocket connection
    webSocket->open(request);
}

void Communicate::onConnected() {
    QString date = date_to_string();
    QString ssml = mkssml(text, voice, rate, volume, pitch);

    QString headersAndData =
        "X-Timestamp:" + date + "\r\n"
                                "Content-Type:application/json; charset=utf-8\r\n"
                                "Path:speech.config\r\n\r\n"
                                "{\"context\":{\"synthesis\":{\"audio\":{\"metadataoptions\":{"
                                "\"sentenceBoundaryEnabled\":false,\"wordBoundaryEnabled\":true},"
                                "\"outputFormat\":\"audio-24khz-48kbitrate-mono-mp3\"}}}}\r\n";

    QString ssmlAndData = ssml_headers_plus_data( connect_id(), date, ssml );

    webSocket->sendTextMessage(headersAndData);
    webSocket->sendTextMessage(ssmlAndData);
}

void Communicate::onBinaryMessageReceived(const QByteArray &message) {
    if (!downloadAudio) {
        throw std::runtime_error("We received a binary message, but we are not expecting one.");
    }

    if (message.size() < 2) {
        throw std::runtime_error("We received a binary message, but it is missing the header length.");
    }

    // See: https://github.com/microsoft/cognitive-services-speech-sdk-js/blob/d071d11/src/common.speech/WebsocketMessageFormatter.ts#L46
    int headerLength = (static_cast<unsigned char>(message[0]) << 8) | static_cast<unsigned char>(message[1]);
    if (message.size() < headerLength + 2) {
        throw std::runtime_error("We received a binary message, but it is missing the audio data.");
    }

    QByteArray audioData = message.mid(headerLength + 2);
    // Emit a signal or do something with the audio data
    // emit audioDataReceived({"audio", audioData});
    audioDataReceived += audioData;
}

void Communicate::onTextMessageReceived(const QString &message) {
    auto [parameters, data] = get_headers_and_data(message);
    auto path = parameters.value("Path");
    if (path == "turn.start") {
        downloadAudio = true;
    } else if (path == "turn.end") {
        downloadAudio = false;
        // End of audio data
        webSocket->close();
        return;
    } else if (path == "audio.metadata") {
        // pass
    } else if (path == "response") {
        // Do nothing
    } else {
        throw std::runtime_error("The response from the service is not recognized.\n" + message.toStdString());
    }
}

void Communicate::onDisconnected() {
    // Handle disconnection
    QFile file(audioFile);
    if (!file.open(QIODevice::WriteOnly)) {
        throw std::runtime_error("Could not open file to write audio.");
    }
    file.write(audioDataReceived);
    file.close();

    auto player = new QMediaPlayer;
    auto audioOutput = new QAudioOutput;
    player->setAudioOutput(audioOutput);

    QObject::connect(player, &QMediaPlayer::mediaStatusChanged, [&](QMediaPlayer::MediaStatus status) {
        if (status == QMediaPlayer::EndOfMedia) {
            emit finished();
        }
    });

    player->setSource(QUrl::fromLocalFile(audioFile));
    audioOutput->setVolume(50);
    player->play();

}

// Utility functions
QString Communicate::connect_id() {
    return QUuid::createUuid().toString().remove("{").remove("}").remove("-");
}

QString Communicate::date_to_string() {
    return QDateTime::currentDateTimeUtc().toString("ddd MMM dd yyyy HH:mm:ss 'GMT+0000 (Coordinated Universal Time)'");
}

QString Communicate::escape(QString data) {
    data.replace("&", "&amp;");
    data.replace(">", "&gt;");
    data.replace("<", "&lt;");
    return data;
}

QString Communicate::remove_incompatible_characters(QString str) {
    for (int i = 0; i < str.size(); ++i) {
        QChar ch = str.at(i);
        int code = ch.unicode();
        if ((0 <= code && code <= 8) || (11 <= code && code <= 12) || (14 <= code && code <= 31)) {
            str.replace(i, 1, ' ');
        }
    }
    return str;
}

QString Communicate::mkssml(QString text, QString voice, QString rate, QString volume, QString pitch) {
    QString ssml =
        "<speak version='1.0' xmlns='http://www.w3.org/2001/10/synthesis' xml:lang='en-US'>"
        "<voice name='" + voice + "'><prosody pitch='" + pitch + "' rate='" + rate + "' volume='" + volume + "'>"
        + text + "</prosody></voice></speak>";
    return ssml;
}

QString Communicate::ssml_headers_plus_data(const QString& requestId, const QString& timestamp, const QString& ssml) {
    return QString("X-RequestId:%1\r\n"
                    "Content-Type:application/ssml+xml\r\n"
                    "X-Timestamp:%2Z\r\n"  // This is not a mistake, Microsoft Edge bug.
                    "Path:ssml\r\n\r\n"
                    "%3")
        .arg(requestId)
        .arg(timestamp)
        .arg(ssml);
}

QPair<QMap<QString, QString>, QString> Communicate::get_headers_and_data(const QString& message) {
    auto parts = message.split("\r\n\r\n");
    auto headers = parts[0].split("\r\n");
    QMap<QString, QString> parameters;
    for (const auto& header : headers) {
        auto key_value = header.split(":");
        parameters[key_value[0].trimmed()] = key_value[1].trimmed();
    }
    return qMakePair(parameters, parts[1]);
}
