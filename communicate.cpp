#include "communicate.h"

Communicate::Communicate(QObject *parent)
    : QObject(parent)
{
    connect(&m_webSocket, &QWebSocket::connected, this, &Communicate::onConnected);
    connect(&m_webSocket, &QWebSocket::binaryMessageReceived, this, &Communicate::onBinaryMessageReceived);
    connect(&m_webSocket, &QWebSocket::textMessageReceived, this, &Communicate::onTextMessageReceived);
    connect(&m_webSocket, &QWebSocket::disconnected, this, &Communicate::onDisconnected);
}

Communicate::~Communicate() {
    m_webSocket.close();
}

void Communicate::setText(QString text)
{
    m_text = escape(remove_incompatible_characters(text));
}

void Communicate::setVoice(QString voice)
{
    m_voice = QString("Microsoft Server Speech Text to Speech Voice (%1)").arg(voice);
}

void Communicate::setFileName(QString fileName)
{
    m_fileName = fileName;
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
    m_webSocket.open(request);
}

void Communicate::onConnected() {
    QString date = date_to_string();
    QString ssml = mkssml(m_text, m_voice, m_rate, m_volume, m_pitch);

    QString headersAndData =
        "X-Timestamp:" + date + "\r\n"
                                "Content-Type:application/json; charset=utf-8\r\n"
                                "Path:speech.config\r\n\r\n"
                                "{\"context\":{\"synthesis\":{\"audio\":{\"metadataoptions\":{"
                                "\"sentenceBoundaryEnabled\":false,\"wordBoundaryEnabled\":true},"
                                "\"outputFormat\":\"audio-24khz-48kbitrate-mono-mp3\"}}}}\r\n";

    QString ssmlAndData = ssml_headers_plus_data( connect_id(), date, ssml );

    m_webSocket.sendTextMessage(headersAndData);
    m_webSocket.sendTextMessage(ssmlAndData);
}

void Communicate::onBinaryMessageReceived(const QByteArray &message) {
    if (!m_downloadAudio) {
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
    m_audioDataReceived += audioData;
}

void Communicate::onTextMessageReceived(const QString &message) {
    auto [parameters, data] = get_headers_and_data(message);
    auto path = parameters.value("Path");
    if (path == "turn.start") {
        m_downloadAudio = true;
    } else if (path == "turn.end") {
        m_downloadAudio = false;
        // End of audio data
        // m_webSocket->close();
        m_webSocket.abort();
        return;
    } else if (path == "audio.metadata") {
        // pass
    } else if (path == "response") {
        // Do nothing
    } else {
        throw std::runtime_error("The response from the service is not recognized.\n" + message.toStdString());
    }
}

void Communicate::save() {
    QFile file(m_fileName);
    if (!file.open(QIODevice::WriteOnly)) {
        throw std::runtime_error("Could not open file to write audio.");
    }
    file.write(m_audioDataReceived);
    file.close();

    QSystemTrayIcon *trayIcon = new QSystemTrayIcon(this);
    trayIcon->setIcon(QIcon("icon.png")); // 设置图标
    trayIcon->show(); // 显示托盘图标

    // 显示通知
    trayIcon->showMessage("保存成功", "文件已保存到 " + m_fileName, QSystemTrayIcon::Information, 10000);

    // open directory of m_fileName, not file itself
    QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(m_fileName).absolutePath()));
}

void Communicate::play()
{
    // Handle disconnection
    QFile file(m_audioFile);
    if (!file.open(QIODevice::WriteOnly)) {
        throw std::runtime_error("Could not open file to write audio.");
    }
    file.write(m_audioDataReceived);
    file.close();

    auto player = new QMediaPlayer;
    auto audioOutput = new QAudioOutput;
    player->setAudioOutput(audioOutput);

    QObject::connect(player, &QMediaPlayer::mediaStatusChanged, [&](QMediaPlayer::MediaStatus status) {
        if (status == QMediaPlayer::EndOfMedia) {
            emit finished();
        }
    });

    // connect this->stop() to player->stop() and emit finished()
    QObject::connect(this, &Communicate::stop, player, &QMediaPlayer::stop);

    QObject::connect(this, &Communicate::stop, [&]() {
        emit finished();
    });

    player->setSource(QUrl::fromLocalFile(m_audioFile));
    audioOutput->setVolume(50);
    player->play();
}

void Communicate::onDisconnected() {
    if (!m_fileName.isEmpty()) {
        save();
        emit saveFinished();
    }
    else {
        play();
    }

    m_audioDataReceived.clear();
}

// Utility functions
QString Communicate::connect_id() {
    return QUuid::createUuid().toString().remove("{").remove("}").remove("-");
}

QString Communicate::date_to_string() {
    return QDateTime::currentDateTimeUtc().toString("ddd MMM dd yyyy HH:mm:ss 'GMT+0000 (Coordinated Universal Time)'");
}

QString Communicate::escape(QString data) {
    data.replace('&', ' ');
    data.replace('>', ' ');
    data.replace('<', ' ');
    return data;
}

QString Communicate::remove_incompatible_characters(QString str) {
    for (int i = 0; i < str.size(); ++i) {
        QChar ch = str.at(i);
        int code = ch.unicode();
        if ((0 <= code && code <= 8) || (10 <= code && code <= 31)) {
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
