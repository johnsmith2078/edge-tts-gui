#include <QCryptographicHash>
#include <QDebug>

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

// 定义常量
const QString CHROMIUM_FULL_VERSION = "143.0.3650.75";
const QString CHROMIUM_MAJOR_VERSION = CHROMIUM_FULL_VERSION.split(".", Qt::SkipEmptyParts).value(0);

// 生成 Sec-MS-GEC Token
QString Communicate::generateSecMsGecToken() {
    // 获取当前时间戳并转换为 Windows 文件时间格式（1601年起，每 100 纳秒单位）
    qint64 ticks = (QDateTime::currentDateTimeUtc().toSecsSinceEpoch() + 11644473600) * 10000000;

    // 向下取整到最近的 5 分钟（5 分钟 = 3,000,000,000 * 100 纳秒）
    ticks -= ticks % 3000000000;

    // 将 ticks 和 TRUSTED_CLIENT_TOKEN 拼接后计算 SHA256 散列值
    QString strToHash = QString::number(ticks) + TRUSTED_CLIENT_TOKEN;
    QByteArray hash = QCryptographicHash::hash(strToHash.toUtf8(), QCryptographicHash::Sha256);

    return hash.toHex().toUpper();
}

// 生成 Sec-MS-GEC-Version
QString Communicate::generateSecMsGecVersion() {
    return QString("1-%1").arg(CHROMIUM_FULL_VERSION);
}

Communicate::Communicate(QObject *parent)
    : QObject(parent)
{
    connect(&m_webSocket, &QWebSocket::connected, this, &Communicate::onConnected);
    connect(&m_webSocket, &QWebSocket::binaryMessageReceived, this, &Communicate::onBinaryMessageReceived);
    connect(&m_webSocket, &QWebSocket::textMessageReceived, this, &Communicate::onTextMessageReceived);
    connect(&m_webSocket, &QWebSocket::disconnected, this, &Communicate::onDisconnected);
    connect(this, &Communicate::audioDataReceived, this, &Communicate::sendNextTextPart);
    connect(this, &Communicate::finished, [&]() {
        m_webSocket.close();
    });
    connect(this, &Communicate::stop, this, [this]() {
        m_stopRequested = true;
    });

    m_player = new QMediaPlayer(this);
    m_audioOutput = new QAudioOutput(this);
    m_audioOutput->setVolume(50);
    m_player->setAudioOutput(m_audioOutput);

    QObject::connect(m_player, &QMediaPlayer::positionChanged, this, [this](qint64 position) {
        if (position <= 0 || m_hasPlaybackStarted) {
            return;
        }
        m_hasPlaybackStarted = true;
        emit playbackStarted();
    });

    QObject::connect(m_player, &QMediaPlayer::errorOccurred, this, [this](QMediaPlayer::Error error, const QString &errorString) {
        if (error == QMediaPlayer::NoError || m_playbackErrorOccurred) {
            return;
        }
        m_playbackErrorOccurred = true;
        m_switchingPlaybackSource = false;
        qDebug() << "Media player error:" << errorString;
        notifyFinishedOnce();
    });

    QObject::connect(m_player, &QMediaPlayer::mediaStatusChanged, this, [this](QMediaPlayer::MediaStatus status) {
        if (status != QMediaPlayer::EndOfMedia) {
            return;
        }

        if (m_switchingPlaybackSource) {
            // Ignore transient EndOfMedia fired while we are replacing the source.
            return;
        }

        if (!m_fileName.isEmpty()) {
            return;
        }

        if (m_stopRequested || m_playbackErrorOccurred) {
            notifyFinishedOnce();
            return;
        }

        if (!m_readyPlaybackChunks.isEmpty()) {
            // Continue with next fully-synthesized segment.
            tryStartOrContinuePlayback();
            return;
        }

        if (!m_synthesisComplete) {
            // Producer has not completed current/next segment yet. Wait silently.
            return;
        }

        notifyFinishedOnce();
    });

    // connect this->stop() to player->stop() and emit finished()
    QObject::connect(this, &Communicate::stop, m_player, &QMediaPlayer::stop);

    QObject::connect(this, &Communicate::stop, this, [this]() {
        m_switchingPlaybackSource = false;
        notifyFinishedOnce();
    });
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

bool Communicate::isPlaying()
{
    return m_player->playbackState() == QMediaPlayer::PlayingState;
}

bool Communicate::hasPlaybackStarted() const
{
    return m_hasPlaybackStarted;
}

bool Communicate::isSynthesisComplete() const
{
    return m_synthesisComplete;
}

qsizetype Communicate::audioBytesReceived() const
{
    return m_audioOffset;
}

bool Communicate::hasPlaybackError() const
{
    return m_playbackErrorOccurred;
}


void Communicate::start() {
    m_player->stop();
    m_player->setSource(QUrl());
    m_audioBuffer.close();

    m_playStarted = false;
    m_hasPlaybackStarted = false;
    m_playbackErrorOccurred = false;
    m_stopRequested = false;
    m_synthesisComplete = false;
    m_finishedEmitted = false;
    m_switchingPlaybackSource = false;
    m_downloadAudio = false;
    m_textPartIndex = 0;
    m_audioOffset = 0;
    m_audioDataReceived.clear();
    m_currentTurnAudio.clear();
    m_readyPlaybackChunks.clear();

    if (m_fileName.isEmpty()) {
        m_textParts = splitTextForPlayback(m_text, ms_initialTextByteLength, ms_targetTextByteLength);
    } else {
        m_textParts = splitTextByByteLength(m_text, ms_maxTextByteLength);
    }

    qsizetype scaleSize = m_text.size() * 500;
    qsizetype minSize = 1024 * 1024;
    qsizetype defaultSize = scaleSize > minSize ? scaleSize : minSize;
    m_audioDataReceived.reserve(defaultSize);

    // 添加 Sec-MS-GEC 和 Sec-MS-GEC-Version 参数
    QUrl url(WSS_URL + "&Sec-MS-GEC=" + generateSecMsGecToken() +
             "&Sec-MS-GEC-Version=" + generateSecMsGecVersion() +
             "&ConnectionId=" + connect_id());

    QNetworkRequest request(url);

    // 设置必要的 headers
    request.setRawHeader("Pragma", "no-cache");
    request.setRawHeader("Cache-Control", "no-cache");
    request.setRawHeader("Origin", "chrome-extension://jdiccldimpdaibmpdkjnbmckianbfold");
    request.setRawHeader("Accept-Encoding", "gzip, deflate, br, zstd");
    request.setRawHeader("Accept-Language", "en-US,en;q=0.9");
    request.setRawHeader("User-Agent", ("Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/" + CHROMIUM_MAJOR_VERSION + ".0.0.0 Safari/537.36 Edg/" + CHROMIUM_MAJOR_VERSION + ".0.0.0").toUtf8());
    request.setRawHeader("Cookie", ("muid=" + generateMuid() + ";").toUtf8());

    // 打开 WebSocket 连接
    m_webSocket.open(request);
}

void Communicate::sendNextTextPart() {
    if (m_textPartIndex < m_textParts.size()) {
        m_date = date_to_string();

        QString headersAndData =
            "X-Timestamp:" + m_date + "\r\n"
                                      "Content-Type:application/json; charset=utf-8\r\n"
                                      "Path:speech.config\r\n\r\n"
                                      "{\"context\":{\"synthesis\":{\"audio\":{\"metadataoptions\":{"
                                      "\"sentenceBoundaryEnabled\":\"false\",\"wordBoundaryEnabled\":\"true\"},"
                                      "\"outputFormat\":\"audio-24khz-48kbitrate-mono-mp3\"}}}}\r\n";
        m_webSocket.sendTextMessage(headersAndData);

        QString part = m_textParts.at(m_textPartIndex);
        QString ssml = mkssml(part, m_voice, m_rate, m_volume, m_pitch);
        QString ssmlAndData = ssml_headers_plus_data( connect_id(), m_date, ssml );
        m_webSocket.sendTextMessage(ssmlAndData);
    }
}

void Communicate::onConnected() {
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
    if (audioData.isEmpty()) {
        return;
    }

    m_audioDataReceived.append(audioData);
    m_audioOffset = m_audioDataReceived.size();
    if (m_fileName.isEmpty()) {
        m_currentTurnAudio.append(audioData);
    }
}

void Communicate::onTextMessageReceived(const QString &message) {
    auto [parameters, data] = get_headers_and_data(message);
    auto path = parameters.value("Path");
    if (path == "turn.start") {
        m_downloadAudio = true;
        if (m_fileName.isEmpty()) {
            m_currentTurnAudio.clear();
        }
    } else if (path == "turn.end") {
        m_downloadAudio = false;

        if (m_fileName.isEmpty() && !m_currentTurnAudio.isEmpty()) {
            m_readyPlaybackChunks.enqueue(m_currentTurnAudio);
            m_currentTurnAudio.clear();
            tryStartOrContinuePlayback();
        }

        ++m_textPartIndex;
        if (m_textPartIndex >= m_textParts.size()) {
            m_synthesisComplete = true;
            m_webSocket.close();
            if (m_fileName.isEmpty()) {
                tryStartOrContinuePlayback();
            }
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
    // Find the position of the last non-zero byte
    int n = byteArray.size() - 1;
    while (n >= 0 && byteArray.at(n) == '\0') {
        --n;
    }
    // Truncate the array to remove trailing zero bytes
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
    tryStartOrContinuePlayback();
}

void Communicate::forcePlay()
{
    tryStartOrContinuePlayback();
}

void Communicate::notifyFinishedOnce()
{
    if (m_finishedEmitted) {
        return;
    }

    m_finishedEmitted = true;
    emit finished();
}

void Communicate::tryStartOrContinuePlayback()
{
    if (!m_fileName.isEmpty() || m_stopRequested || m_playbackErrorOccurred || m_finishedEmitted) {
        return;
    }

    if (m_player->playbackState() == QMediaPlayer::PlayingState) {
        return;
    }

    if (m_readyPlaybackChunks.isEmpty()) {
        if (m_synthesisComplete && m_playStarted) {
            notifyFinishedOnce();
        }
        return;
    }

    QByteArray nextChunk = m_readyPlaybackChunks.dequeue();
    if (nextChunk.isEmpty()) {
        if (m_synthesisComplete && m_readyPlaybackChunks.isEmpty()) {
            notifyFinishedOnce();
        }
        return;
    }

    m_playStarted = true;
    m_switchingPlaybackSource = true;
    m_player->setSource(QUrl());
    m_audioBuffer.close();
    m_audioBuffer.setData(nextChunk);
    m_audioBuffer.open(QIODevice::ReadOnly);
    m_player->setSourceDevice(&m_audioBuffer, QUrl("audio.mp3"));
    m_player->play();
    m_switchingPlaybackSource = false;
}

void Communicate::onDisconnected() {
    if (!m_fileName.isEmpty()) {
        save();
        return;
    }

    if (m_stopRequested || m_playbackErrorOccurred) {
        // Do not auto-play if user stopped or playback already failed.
        return;
    }

    if (!m_synthesisComplete) {
        // WebSocket disconnected unexpectedly; treat current data as final snapshot.
        m_synthesisComplete = true;
        if (!m_currentTurnAudio.isEmpty()) {
            m_readyPlaybackChunks.enqueue(m_currentTurnAudio);
            m_currentTurnAudio.clear();
        }
    }

    tryStartOrContinuePlayback();

    if (!m_playStarted && m_audioOffset == 0) {
        notifyFinishedOnce();
    }
}

// Utility functions
QString Communicate::connect_id() {
    return QUuid::createUuid().toString().remove("{").remove("}").remove("-");
}

QString Communicate::date_to_string() {
    return QDateTime::currentDateTimeUtc().toString("ddd MMM dd yyyy HH:mm:ss 'GMT+0000 (Coordinated Universal Time)'");
}

QString Communicate::escape(QString data) {
    data.replace('&', "&amp;");
    data.replace('<', "&lt;");
    data.replace('>', "&gt;");
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

QString Communicate::generateMuid() {
    return QUuid::createUuid().toString(QUuid::WithoutBraces).remove("-").toUpper();
}

int Communicate::findSafeUtf8SplitPoint(const QByteArray &text, int limit) {
    int splitAt = qMin(limit, text.size());
    if (splitAt <= 0) {
        return splitAt;
    }

    int leadIndex = splitAt - 1;
    int continuationBytes = 0;
    while (leadIndex >= 0 && (static_cast<unsigned char>(text.at(leadIndex)) & 0xC0) == 0x80) {
        --leadIndex;
        ++continuationBytes;
    }

    if (continuationBytes == 0) {
        return splitAt;
    }

    if (leadIndex < 0) {
        return splitAt - continuationBytes;
    }

    unsigned char lead = static_cast<unsigned char>(text.at(leadIndex));
    int expectedLength = 1;
    if ((lead & 0xE0) == 0xC0) {
        expectedLength = 2;
    } else if ((lead & 0xF0) == 0xE0) {
        expectedLength = 3;
    } else if ((lead & 0xF8) == 0xF0) {
        expectedLength = 4;
    } else {
        return leadIndex;
    }

    int actualLength = splitAt - leadIndex;
    if (actualLength < expectedLength) {
        return leadIndex;
    }

    return splitAt;
}

int Communicate::adjustSplitPointForXmlEntity(const QByteArray &text, int splitAt) {
    int adjustedSplitAt = splitAt;
    while (adjustedSplitAt > 0) {
        int ampersandIndex = text.lastIndexOf('&', adjustedSplitAt - 1);
        if (ampersandIndex < 0) {
            break;
        }
        int semicolonIndex = text.indexOf(';', ampersandIndex);
        if (semicolonIndex >= 0 && semicolonIndex < adjustedSplitAt) {
            break;
        }
        adjustedSplitAt = ampersandIndex;
    }
    return adjustedSplitAt;
}

namespace {

const QVector<QByteArray> &punctuationMarks() {
    static const QVector<QByteArray> marks = {
        QByteArrayLiteral("\n"),
        QByteArrayLiteral("."),
        QByteArrayLiteral("!"),
        QByteArrayLiteral("?"),
        QByteArrayLiteral(","),
        QByteArrayLiteral("\xE3\x80\x82"), // CJK period
        QByteArrayLiteral("\xEF\xBC\x81"), // CJK exclamation mark
        QByteArrayLiteral("\xEF\xBC\x9F"), // CJK question mark
        QByteArrayLiteral("\xEF\xBC\x8C"), // CJK comma
        QByteArrayLiteral("\xE3\x80\x81"), // ideographic comma
        QByteArrayLiteral("\xEF\xBC\x9B"), // CJK semicolon
        QByteArrayLiteral("\xEF\xBC\x9A"), // CJK colon
        QByteArrayLiteral("\xE2\x80\xA6")  // horizontal ellipsis
    };
    return marks;
}

int findLastPunctuationSplitPoint(const QByteArray &text, int limit) {
    const int safeLimit = qMin(limit, text.size());
    if (safeLimit <= 0) {
        return -1;
    }

    int bestSplitAt = -1;
    const QVector<QByteArray> &marks = punctuationMarks();
    for (const QByteArray &mark : marks) {
        if (mark.isEmpty() || safeLimit < mark.size()) {
            continue;
        }

        const int searchFrom = safeLimit - mark.size();
        const int index = text.lastIndexOf(mark, searchFrom);
        if (index < 0) {
            continue;
        }

        const int candidateSplitAt = index + mark.size();
        if (candidateSplitAt > bestSplitAt) {
            bestSplitAt = candidateSplitAt;
        }
    }

    return bestSplitAt;
}

int findFirstPunctuationSplitPoint(const QByteArray &text, int startAt, int hardLimit) {
    const int safeStart = qMax(0, startAt);
    const int safeHardLimit = qMin(hardLimit, text.size());
    if (safeStart >= safeHardLimit) {
        return -1;
    }

    int bestSplitAt = -1;
    const QVector<QByteArray> &marks = punctuationMarks();
    for (const QByteArray &mark : marks) {
        if (mark.isEmpty()) {
            continue;
        }

        const int index = text.indexOf(mark, safeStart);
        if (index < 0) {
            continue;
        }

        const int candidateSplitAt = index + mark.size();
        if (candidateSplitAt > safeHardLimit) {
            continue;
        }

        if (bestSplitAt < 0 || candidateSplitAt < bestSplitAt) {
            bestSplitAt = candidateSplitAt;
        }
    }

    return bestSplitAt;
}

} // namespace

QVector<QString> Communicate::splitTextByByteLength(const QString &text, int byteLength) {
    QByteArray bytes = text.toUtf8();
    QVector<QString> parts;

    while (bytes.size() > byteLength) {
        int splitAt = findLastPunctuationSplitPoint(bytes, byteLength);
        if (splitAt < 0) {
            splitAt = findSafeUtf8SplitPoint(bytes, byteLength);
        }
        splitAt = adjustSplitPointForXmlEntity(bytes, splitAt);
        if (splitAt <= 0) {
            splitAt = qMin(byteLength, bytes.size());
        }

        QByteArray chunk = bytes.left(splitAt).trimmed();
        if (!chunk.isEmpty()) {
            parts.append(QString::fromUtf8(chunk));
        }
        bytes = bytes.mid(splitAt);
    }

    QByteArray tail = bytes.trimmed();
    if (!tail.isEmpty()) {
        parts.append(QString::fromUtf8(tail));
    }

    return parts;
}

QVector<QString> Communicate::splitTextForPlayback(const QString &text, int initialByteLength, int subsequentByteLength) {
    QByteArray bytes = text.toUtf8();
    QVector<QString> parts;
    const int safeInitialLength = qMax(64, qMin(initialByteLength, ms_maxTextByteLength));
    const int safeMaxLength = qMax(safeInitialLength, qMin(subsequentByteLength, ms_maxTextByteLength));
    int currentLimit = safeInitialLength;

    while (!bytes.isEmpty()) {
        if (bytes.size() <= currentLimit) {
            QByteArray tail = bytes.trimmed();
            if (!tail.isEmpty()) {
                parts.append(QString::fromUtf8(tail));
            }
            break;
        }

        int splitAt = findLastPunctuationSplitPoint(bytes, currentLimit);
        if (splitAt < 0 && safeMaxLength > currentLimit) {
            splitAt = findFirstPunctuationSplitPoint(bytes, currentLimit, safeMaxLength);
        }
        if (splitAt < 0) {
            const int fallbackLimit = qMin(safeMaxLength, bytes.size());
            splitAt = findSafeUtf8SplitPoint(bytes, fallbackLimit);
        }
        splitAt = adjustSplitPointForXmlEntity(bytes, splitAt);
        if (splitAt <= 0) {
            splitAt = qMin(currentLimit, bytes.size());
        }

        QByteArray chunk = bytes.left(splitAt).trimmed();
        if (!chunk.isEmpty()) {
            parts.append(QString::fromUtf8(chunk));
        }

        bytes = bytes.mid(splitAt);
        // Extreme-fast-start profile: keep early growth small and smooth,
        // and still ramp slowly in later chunks to avoid long synthesis gaps.
        const int growthStep = qMax(16, qMin(40, currentLimit / 8));
        currentLimit = qMin(safeMaxLength, currentLimit + growthStep);
    }

    return parts;
}
