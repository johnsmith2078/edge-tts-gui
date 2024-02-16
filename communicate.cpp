#include "communicate.h"

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

Communicate::Communicate(QObject *parent)
    : QObject(parent)
{
    connect(&m_webSocket, &QWebSocket::connected, this, &Communicate::onConnected);
    connect(&m_webSocket, &QWebSocket::binaryMessageReceived, this, &Communicate::onBinaryMessageReceived);
    connect(&m_webSocket, &QWebSocket::textMessageReceived, this, &Communicate::onTextMessageReceived);
    connect(&m_webSocket, &QWebSocket::disconnected, this, &Communicate::onDisconnected);
    connect(this, &Communicate::audioDataReceived, this, &Communicate::sendNextTextPart);
    connect(this, &Communicate::duplicated, &m_webSocket, &QWebSocket::disconnected);

    m_player = new QMediaPlayer(this);
    m_audioOutput = new QAudioOutput(this);
    m_audioOutput->setVolume(50);
    m_player->setAudioOutput(m_audioOutput);

    qsizetype scaleSize = m_text.size() * 500;
    qsizetype minSize = 1024 * 1024;
    qsizetype defaultSize = scaleSize > minSize ? scaleSize : minSize;
    m_audioDataReceived.resize(defaultSize, 0);

    QObject::connect(m_player, &QMediaPlayer::mediaStatusChanged, [&](QMediaPlayer::MediaStatus status) {
        if (status == QMediaPlayer::EndOfMedia) {
            if (m_audioOffset == m_audioDataReceived.size()) {
                m_audioOffset = 0;
                emit finished();
            } else {
                play();
            }
        }
    });

    // connect this->stop() to player->stop() and emit finished()
    QObject::connect(this, &Communicate::stop, m_player, &QMediaPlayer::stop);

    QObject::connect(this, &Communicate::stop, [&]() {
        emit finished();
    });

    // QObject::connect(m_player, &QMediaPlayer::mediaStatusChanged,
    //                  [=](QMediaPlayer::MediaStatus status)
    //                  { qDebug() << "MediaStatus:" << m_player->mediaStatus() << "|" << status; });
}

Communicate::~Communicate() {
    m_webSocket.close();
    delete m_player;
    delete m_audioOutput;
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

void Communicate::setDuplicated(bool dup)
{
    m_isDuplicated = dup;
    m_audioDataReceived.clear();
}


void Communicate::start() {
    m_playStarted = false;
    m_audioOffset = 0;
    m_audioDataReceived.clear();

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

void Communicate::sendNextTextPart() {
    if (m_textPartIndex < m_text.length()) {
        m_date = date_to_string();

        QString headersAndData =
            "X-Timestamp:" + m_date + "\r\n"
                                      "Content-Type:application/json; charset=utf-8\r\n"
                                      "Path:speech.config\r\n\r\n"
                                      "{\"context\":{\"synthesis\":{\"audio\":{\"metadataoptions\":{"
                                      "\"sentenceBoundaryEnabled\":false,\"wordBoundaryEnabled\":true},"
                                      "\"outputFormat\":\"audio-24khz-48kbitrate-mono-mp3\"}}}}\r\n";
        m_webSocket.sendTextMessage(headersAndData);

        QString part = m_text.mid(m_textPartIndex,  ms_maxMessageSize);
        QString ssml = mkssml(part, m_voice, m_rate, m_volume, m_pitch);
        QString ssmlAndData = ssml_headers_plus_data( connect_id(), m_date, ssml );
        m_webSocket.sendTextMessage(ssmlAndData);
    }
}

void Communicate::onConnected() {
    m_textPartIndex = 0;

    sendNextTextPart();
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
    m_audioDataReceived.replace(m_audioOffset, audioData.size(), audioData);
    m_audioOffset += audioData.size();

    if (!m_playStarted && m_audioDataReceived.size() >= ms_trunkSize && m_fileName.isEmpty()) {
        play();
        m_playStarted = true;
    }
}

void Communicate::onTextMessageReceived(const QString &message) {
    auto [parameters, data] = get_headers_and_data(message);
    auto path = parameters.value("Path");
    if (path == "turn.start") {
        m_downloadAudio = true;
    } else if (path == "turn.end") {
        m_downloadAudio = false;
        m_textPartIndex += ms_maxMessageSize;
        if (m_textPartIndex >= m_text.length()) {
            m_webSocket.close();
        }
        // End of audio data
        emit audioDataReceived();
    } else if (path == "audio.metadata") {
        // pass
    } else if (path == "response") {
        // Do nothing
    } else {
        throw std::runtime_error("The response from the service is not recognized.\n" + message.toStdString());
    }
}

void Communicate::removeTrailingZeros(QByteArray &byteArray) {
    // 查找最后一个非零字节的位置
    int n = byteArray.size() - 1;
    while (n >= 0 && byteArray.at(n) == '\0') {
        --n;
    }
    // 截断数组以去除末尾的零字节
    byteArray.truncate(n + 1);
}

void Communicate::save() {
    QFile file(m_fileName);
    if (!file.open(QIODevice::WriteOnly)) {
        throw std::runtime_error("Could not open file to write audio.");
    }
    removeTrailingZeros(m_audioDataReceived);
    file.write(m_audioDataReceived);
    file.close();

    // if icon.png exists, show notification
    if (QFile("icon.png").exists()) {
        QSystemTrayIcon *trayIcon = new QSystemTrayIcon(this);
        trayIcon->setIcon(QIcon("icon.png")); // 设置图标
        trayIcon->show(); // 显示托盘图标

        // show notification
        trayIcon->showMessage("保存成功", "文件已保存到 " + m_fileName, QSystemTrayIcon::Information, 10000);
    }

    // open directory of m_fileName, not file itself
    QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(m_fileName).absolutePath()));

    emit saveFinished();
}

void Communicate::play()
{
    if (m_audioDataReceived.size() < ms_trunkSize) {
        return;
    }
    m_player->setSource(QUrl());
    m_audioBuffer.close();
    m_audioBuffer.setBuffer(&m_audioDataReceived);
    m_player->setSourceDevice(&m_audioBuffer, QUrl("audio.mp3"));
    m_player->play();
}

void Communicate::forcePlay()
{
    // qDebug() << "force play";
    m_player->setSource(QUrl());
    m_audioBuffer.close();
    m_audioBuffer.setData(m_audioDataReceived);
    m_player->setSourceDevice(&m_audioBuffer, QUrl("audio.mp3"));
    m_player->play();
}

void Communicate::onDisconnected() {
    if (!m_fileName.isEmpty()) {
        save();
    }
    else if (m_audioDataReceived.size() < ms_trunkSize) {
        forcePlay();
        m_audioOffset += m_audioDataReceived.size();
    }/* else if (m_isDuplicated) {
        m_isDuplicated = false;
        start();
    }*/
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
        if ((0 <= code && code <= 8) || (11 <= code && code <= 31 && code != 13)) {
            str.replace(i, 1, ' ');
        } else if (code == 10 || code == 13) {
            str.replace(i, 1, '\0');
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
