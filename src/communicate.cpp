#include "communicate.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDebug>
#include <QDesktopServices>
#include <QFileInfo>
#include <QNetworkRequest>
#include <QSaveFile>
#include <QSystemTrayIcon>
#include <QUuid>
#include <QUrl>

namespace {

const QString kTrustedClientToken = "6A5AA1D4EAFF4E9FB37E23D68491D6F4";
const QString kWssUrl =
    "wss://speech.platform.bing.com/consumer/speech/synthesize/"
    "readaloud/edge/v1?TrustedClientToken=" + kTrustedClientToken;
const QString kChromiumFullVersion = "143.0.3650.75";
const QString kChromiumMajorVersion = kChromiumFullVersion.section('.', 0, 0);

const QVector<QByteArray> &punctuationMarks()
{
    static const QVector<QByteArray> marks = {
        QByteArrayLiteral("\n"), QByteArrayLiteral("."), QByteArrayLiteral("!"),
        QByteArrayLiteral("?"), QByteArrayLiteral(","), QByteArrayLiteral("\xE3\x80\x82"),
        QByteArrayLiteral("\xEF\xBC\x81"), QByteArrayLiteral("\xEF\xBC\x9F"),
        QByteArrayLiteral("\xEF\xBC\x8C"), QByteArrayLiteral("\xE3\x80\x81"),
        QByteArrayLiteral("\xEF\xBC\x9B"), QByteArrayLiteral("\xEF\xBC\x9A"),
        QByteArrayLiteral("\xE2\x80\xA6")
    };
    return marks;
}

int findLastPunctuationSplitPoint(const QByteArray &text, int limit)
{
    const int safeLimit = qMin(limit, text.size());
    int best = -1;
    for (const QByteArray &mark : punctuationMarks()) {
        if (mark.isEmpty() || safeLimit < mark.size()) {
            continue;
        }
        const int index = text.lastIndexOf(mark, safeLimit - mark.size());
        if (index >= 0) {
            best = qMax(best, index + mark.size());
        }
    }
    return best;
}

int findFirstPunctuationSplitPoint(const QByteArray &text, int startAt, int hardLimit)
{
    const int safeStart = qMax(0, startAt);
    const int safeHardLimit = qMin(hardLimit, text.size());
    if (safeStart >= safeHardLimit) {
        return -1;
    }

    int best = -1;
    for (const QByteArray &mark : punctuationMarks()) {
        const int index = text.indexOf(mark, safeStart);
        if (index < 0) {
            continue;
        }
        const int candidate = index + mark.size();
        if (candidate <= safeHardLimit && (best < 0 || candidate < best)) {
            best = candidate;
        }
    }
    return best;
}

} // namespace

Communicate::Communicate(QObject *parent)
    : QObject(parent)
    , m_player(new QMediaPlayer(this))
    , m_audioOutput(new QAudioOutput(this))
{
    connect(&m_webSocket, &QWebSocket::connected, this, &Communicate::onConnected);
    connect(&m_webSocket, &QWebSocket::binaryMessageReceived, this, &Communicate::onBinaryMessageReceived);
    connect(&m_webSocket, &QWebSocket::textMessageReceived, this, &Communicate::onTextMessageReceived);
    connect(&m_webSocket, &QWebSocket::disconnected, this, &Communicate::onDisconnected);
    connect(&m_webSocket, &QWebSocket::errorOccurred, this, [this](QAbstractSocket::SocketError) {
        fail(QStringLiteral("WebSocket error: %1").arg(m_webSocket.errorString()));
    });
    connect(this, &Communicate::audioDataReceived, this, &Communicate::sendNextTextPart);
    connect(this, &Communicate::finished, this, [this]() { m_webSocket.close(); });
    connect(this, &Communicate::stop, this, [this]() {
        m_stopRequested = true;
        m_switchingPlaybackSource = false;
        m_webSocket.abort();
        m_player->stop();
        notifyFinishedOnce();
    });

    m_audioOutput->setVolume(0.5);
    m_player->setAudioOutput(m_audioOutput);

    connect(m_player, &QMediaPlayer::positionChanged, this, [this](qint64 position) {
        if (position > 0 && !m_hasPlaybackStarted) {
            m_hasPlaybackStarted = true;
            emit playbackStarted();
        }
    });

    connect(m_player, &QMediaPlayer::errorOccurred, this,
            [this](QMediaPlayer::Error error, const QString &errorString) {
        if (error != QMediaPlayer::NoError) {
            fail(QStringLiteral("Media playback error: %1").arg(errorString));
        }
    });

    connect(m_player, &QMediaPlayer::mediaStatusChanged, this, [this](QMediaPlayer::MediaStatus status) {
        if (status != QMediaPlayer::EndOfMedia || m_switchingPlaybackSource || !m_fileName.isEmpty()) {
            return;
        }
        if (m_stopRequested || m_playbackErrorOccurred) {
            notifyFinishedOnce();
        } else if (!m_readyPlaybackChunks.isEmpty()) {
            tryStartOrContinuePlayback();
        } else if (m_synthesisComplete) {
            notifyFinishedOnce();
        }
    });
}

Communicate::~Communicate()
{
    m_webSocket.abort();
}

void Communicate::setText(QString text)
{
    m_text = escape(remove_incompatible_characters(std::move(text)));
}

void Communicate::setVoice(QString voice)
{
    m_voice = QString("Microsoft Server Speech Text to Speech Voice (%1)").arg(voice);
}

void Communicate::setFileName(QString fileName)
{
    m_fileName = std::move(fileName);
}

void Communicate::setDuplicated(bool)
{
    // Kept for source compatibility with the legacy UI. Duplicate caching is no longer used.
}

bool Communicate::isPlaying() const
{
    return m_player->playbackState() == QMediaPlayer::PlayingState;
}

bool Communicate::hasPlaybackStarted() const { return m_hasPlaybackStarted; }
bool Communicate::isSynthesisComplete() const { return m_synthesisComplete; }
qsizetype Communicate::audioBytesReceived() const { return m_audioOffset; }
bool Communicate::hasPlaybackError() const { return m_playbackErrorOccurred; }

void Communicate::start()
{
    m_webSocket.abort();
    m_player->stop();
    m_player->setSource(QUrl());
    m_audioBuffer.close();

    m_playStarted = false;
    m_hasPlaybackStarted = false;
    m_playbackErrorOccurred = false;
    m_stopRequested = false;
    m_synthesisComplete = false;
    m_finishedEmitted = false;
    m_saveFailureEmitted = false;
    m_switchingPlaybackSource = false;
    m_downloadAudio = false;
    m_textPartIndex = 0;
    m_audioOffset = 0;
    m_audioDataReceived.clear();
    m_currentTurnAudio.clear();
    m_readyPlaybackChunks.clear();

    m_textParts = m_fileName.isEmpty()
                      ? splitTextForPlayback(m_text, ms_initialTextByteLength, ms_targetTextByteLength)
                      : splitTextByByteLength(m_text, ms_maxTextByteLength);

    if (m_textParts.isEmpty()) {
        fail(QStringLiteral("Text is empty after normalization."));
        return;
    }
    if (!m_fileName.isEmpty()) {
        emit saveProgressChanged(0);
    }

    const qsizetype reserveSize = qMax<qsizetype>(1024 * 1024, m_text.size() * 500);
    m_audioDataReceived.reserve(reserveSize);

    QUrl url(kWssUrl + "&Sec-MS-GEC=" + generateSecMsGecToken()
             + "&Sec-MS-GEC-Version=" + generateSecMsGecVersion()
             + "&ConnectionId=" + connect_id());
    QNetworkRequest request(url);
    request.setRawHeader("Pragma", "no-cache");
    request.setRawHeader("Cache-Control", "no-cache");
    request.setRawHeader("Origin", "chrome-extension://jdiccldimpdaibmpdkjnbmckianbfold");
    request.setRawHeader("Accept-Encoding", "gzip, deflate, br, zstd");
    request.setRawHeader("Accept-Language", "en-US,en;q=0.9");
    request.setRawHeader("User-Agent",
                         ("Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
                          "(KHTML, like Gecko) Chrome/" + kChromiumMajorVersion
                          + ".0.0.0 Safari/537.36 Edg/" + kChromiumMajorVersion + ".0.0.0").toUtf8());
    request.setRawHeader("Cookie", ("muid=" + generateMuid() + ";").toUtf8());
    m_webSocket.open(request);
}

void Communicate::onConnected()
{
    sendNextTextPart();
}

void Communicate::sendNextTextPart()
{
    if (m_stopRequested || m_playbackErrorOccurred || m_textPartIndex >= m_textParts.size()) {
        return;
    }

    m_date = date_to_string();
    const QString config =
        "X-Timestamp:" + m_date + "\r\n"
        "Content-Type:application/json; charset=utf-8\r\n"
        "Path:speech.config\r\n\r\n"
        "{\"context\":{\"synthesis\":{\"audio\":{\"metadataoptions\":{"
        "\"sentenceBoundaryEnabled\":\"false\",\"wordBoundaryEnabled\":\"true\"},"
        "\"outputFormat\":\"audio-24khz-48kbitrate-mono-mp3\"}}}}\r\n";

    m_webSocket.sendTextMessage(config);
    const QString ssml = mkssml(m_textParts.at(m_textPartIndex), m_voice, m_rate, m_volume, m_pitch);
    m_webSocket.sendTextMessage(ssml_headers_plus_data(connect_id(), m_date, ssml));
}

void Communicate::onBinaryMessageReceived(const QByteArray &message)
{
    if (!m_downloadAudio) {
        fail(QStringLiteral("Received audio outside an active synthesis turn."));
        return;
    }
    if (message.size() < 2) {
        fail(QStringLiteral("Audio message is missing its header length."));
        return;
    }

    const int headerLength = (static_cast<unsigned char>(message[0]) << 8)
                             | static_cast<unsigned char>(message[1]);
    if (headerLength < 0 || message.size() < headerLength + 2) {
        fail(QStringLiteral("Audio message contains an invalid header length."));
        return;
    }

    const QByteArray audioData = message.mid(headerLength + 2);
    if (audioData.isEmpty()) {
        return;
    }

    m_audioDataReceived.append(audioData);
    m_audioOffset = m_audioDataReceived.size();
    if (m_fileName.isEmpty()) {
        m_currentTurnAudio.append(audioData);
    } else if (!m_textParts.isEmpty()) {
        const int percent = qBound(1, static_cast<int>(((m_textPartIndex * 100) + 50) / m_textParts.size()), 99);
        emit saveProgressChanged(percent);
    }
}

void Communicate::onTextMessageReceived(const QString &message)
{
    QMap<QString, QString> parameters;
    QString data;
    if (!get_headers_and_data(message, parameters, data)) {
        fail(QStringLiteral("Malformed response from the speech service."));
        return;
    }

    const QString path = parameters.value("Path");
    if (path == "turn.start") {
        m_downloadAudio = true;
        if (m_fileName.isEmpty()) {
            m_currentTurnAudio.clear();
        }
        return;
    }

    if (path == "turn.end") {
        m_downloadAudio = false;
        if (m_fileName.isEmpty() && !m_currentTurnAudio.isEmpty()) {
            m_readyPlaybackChunks.enqueue(m_currentTurnAudio);
            m_currentTurnAudio.clear();
            tryStartOrContinuePlayback();
        }

        ++m_textPartIndex;
        if (!m_fileName.isEmpty()) {
            emit saveProgressChanged(qBound(1, static_cast<int>((m_textPartIndex * 100) / m_textParts.size()), 100));
        }

        if (m_textPartIndex >= m_textParts.size()) {
            m_synthesisComplete = true;
            m_webSocket.close();
            if (m_fileName.isEmpty()) {
                tryStartOrContinuePlayback();
            }
        } else {
            emit audioDataReceived();
        }
        return;
    }

    if (path == "audio.metadata" || path == "response") {
        return;
    }

    fail(QStringLiteral("Unrecognized response path: %1").arg(path));
}

void Communicate::onDisconnected()
{
    if (m_stopRequested) {
        return;
    }

    if (!m_fileName.isEmpty()) {
        if (m_playbackErrorOccurred || !m_synthesisComplete || m_textPartIndex != m_textParts.size()
            || m_audioDataReceived.isEmpty()) {
            notifySaveFailedOnce(QStringLiteral("Synthesis ended before the complete audio file was received."));
            return;
        }
        save();
        return;
    }

    if (m_playbackErrorOccurred) {
        notifyFinishedOnce();
        return;
    }

    if (!m_synthesisComplete) {
        m_playbackErrorOccurred = true;
        qWarning() << "WebSocket disconnected before synthesis completed";
        notifyFinishedOnce();
        return;
    }

    tryStartOrContinuePlayback();
    if (!m_playStarted && m_audioOffset == 0) {
        notifyFinishedOnce();
    }
}

void Communicate::save()
{
    if (!m_synthesisComplete || m_audioDataReceived.isEmpty()) {
        notifySaveFailedOnce(QStringLiteral("Refusing to save incomplete audio."));
        return;
    }

    QSaveFile file(m_fileName);
    if (!file.open(QIODevice::WriteOnly)) {
        notifySaveFailedOnce(QStringLiteral("Could not open output file: %1").arg(file.errorString()));
        return;
    }

    const qint64 written = file.write(m_audioDataReceived);
    if (written != m_audioDataReceived.size()) {
        file.cancelWriting();
        notifySaveFailedOnce(QStringLiteral("Could not write the complete audio file."));
        return;
    }
    if (!file.commit()) {
        notifySaveFailedOnce(QStringLiteral("Could not finalize output file: %1").arg(file.errorString()));
        return;
    }

    const QString iconPath = QCoreApplication::applicationDirPath() + "/icon.png";
    if (QFileInfo::exists(iconPath) && QSystemTrayIcon::isSystemTrayAvailable()) {
        auto *tray = new QSystemTrayIcon(QIcon(iconPath), this);
        tray->show();
        tray->showMessage(QStringLiteral("保存成功"), QStringLiteral("文件已保存到 %1").arg(m_fileName),
                          QSystemTrayIcon::Information, 5000);
        QTimer::singleShot(6000, tray, &QObject::deleteLater);
    }

    QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(m_fileName).absolutePath()));
    emit saveFinished();
}

void Communicate::play() { tryStartOrContinuePlayback(); }
void Communicate::forcePlay() { tryStartOrContinuePlayback(); }

void Communicate::fail(const QString &error)
{
    if (m_playbackErrorOccurred || m_stopRequested) {
        return;
    }
    m_playbackErrorOccurred = true;
    m_downloadAudio = false;
    qWarning() << error;
    m_webSocket.abort();

    if (!m_fileName.isEmpty()) {
        notifySaveFailedOnce(error);
    } else {
        notifyFinishedOnce();
    }
}

void Communicate::notifySaveFailedOnce(const QString &error)
{
    if (m_saveFailureEmitted) {
        return;
    }
    m_saveFailureEmitted = true;
    emit saveFailed(error);
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
    if (!m_fileName.isEmpty() || m_stopRequested || m_playbackErrorOccurred || m_finishedEmitted
        || m_player->playbackState() == QMediaPlayer::PlayingState) {
        return;
    }

    if (m_readyPlaybackChunks.isEmpty()) {
        if (m_synthesisComplete && m_playStarted) {
            notifyFinishedOnce();
        }
        return;
    }

    const QByteArray nextChunk = m_readyPlaybackChunks.dequeue();
    if (nextChunk.isEmpty()) {
        tryStartOrContinuePlayback();
        return;
    }

    m_playStarted = true;
    m_switchingPlaybackSource = true;
    m_player->setSource(QUrl());
    m_audioBuffer.close();
    m_audioBuffer.setData(nextChunk);
    if (!m_audioBuffer.open(QIODevice::ReadOnly)) {
        m_switchingPlaybackSource = false;
        fail(QStringLiteral("Could not open in-memory audio buffer."));
        return;
    }
    m_player->setSourceDevice(&m_audioBuffer, QUrl("audio.mp3"));
    m_player->play();
    m_switchingPlaybackSource = false;
}

QString Communicate::connect_id() const
{
    return QUuid::createUuid().toString(QUuid::WithoutBraces).remove('-');
}

QString Communicate::date_to_string() const
{
    return QDateTime::currentDateTimeUtc().toString("ddd MMM dd yyyy HH:mm:ss 'GMT+0000 (Coordinated Universal Time)'");
}

QString Communicate::escape(QString data) const
{
    data.replace('&', "&amp;");
    data.replace('<', "&lt;");
    data.replace('>', "&gt;");
    return data;
}

QString Communicate::remove_incompatible_characters(QString str) const
{
    for (int i = 0; i < str.size(); ++i) {
        const ushort code = str.at(i).unicode();
        if (code <= 8 || (code >= 11 && code <= 12) || (code >= 14 && code <= 31)) {
            str[i] = ' ';
        }
    }
    return str;
}

QString Communicate::mkssml(const QString &text, const QString &voice, const QString &rate,
                            const QString &volume, const QString &pitch) const
{
    return "<speak version='1.0' xmlns='http://www.w3.org/2001/10/synthesis' xml:lang='en-US'>"
           "<voice name='" + voice + "'><prosody pitch='" + pitch + "' rate='" + rate
           + "' volume='" + volume + "'>" + text + "</prosody></voice></speak>";
}

QString Communicate::ssml_headers_plus_data(const QString &requestId, const QString &timestamp,
                                             const QString &ssml) const
{
    return QString("X-RequestId:%1\r\nContent-Type:application/ssml+xml\r\n"
                   "X-Timestamp:%2Z\r\nPath:ssml\r\n\r\n%3")
        .arg(requestId, timestamp, ssml);
}

bool Communicate::get_headers_and_data(const QString &message, QMap<QString, QString> &parameters,
                                       QString &data) const
{
    const int separator = message.indexOf("\r\n\r\n");
    if (separator < 0) {
        return false;
    }

    const QString headerBlock = message.left(separator);
    data = message.mid(separator + 4);
    for (const QString &header : headerBlock.split("\r\n", Qt::SkipEmptyParts)) {
        const int colon = header.indexOf(':');
        if (colon <= 0) {
            return false;
        }
        const QString key = header.left(colon).trimmed();
        const QString value = header.mid(colon + 1).trimmed();
        if (key.isEmpty()) {
            return false;
        }
        parameters.insert(key, value);
    }
    return !parameters.isEmpty();
}

QString Communicate::generateMuid() const
{
    return connect_id().toUpper();
}

QString Communicate::generateSecMsGecToken() const
{
    qint64 ticks = (QDateTime::currentDateTimeUtc().toSecsSinceEpoch() + 11644473600LL) * 10000000LL;
    ticks -= ticks % 3000000000LL;
    const QByteArray hash = QCryptographicHash::hash(
        (QString::number(ticks) + kTrustedClientToken).toUtf8(), QCryptographicHash::Sha256);
    return hash.toHex().toUpper();
}

QString Communicate::generateSecMsGecVersion() const
{
    return QString("1-%1").arg(kChromiumFullVersion);
}

int Communicate::findSafeUtf8SplitPoint(const QByteArray &text, int limit) const
{
    int splitAt = qMin(limit, text.size());
    while (splitAt > 0 && splitAt < text.size()
           && (static_cast<unsigned char>(text.at(splitAt)) & 0xC0) == 0x80) {
        --splitAt;
    }
    return splitAt;
}

int Communicate::adjustSplitPointForXmlEntity(const QByteArray &text, int splitAt) const
{
    int adjusted = splitAt;
    while (adjusted > 0) {
        const int amp = text.lastIndexOf('&', adjusted - 1);
        if (amp < 0) {
            break;
        }
        const int semi = text.indexOf(';', amp);
        if (semi >= 0 && semi < adjusted) {
            break;
        }
        adjusted = amp;
    }
    return adjusted;
}

QVector<QString> Communicate::splitTextByByteLength(const QString &text, int byteLength) const
{
    QByteArray bytes = text.toUtf8();
    QVector<QString> parts;

    while (bytes.size() > byteLength) {
        int splitAt = findLastPunctuationSplitPoint(bytes, byteLength);
        if (splitAt < 0) {
            splitAt = findSafeUtf8SplitPoint(bytes, byteLength);
        }
        splitAt = adjustSplitPointForXmlEntity(bytes, splitAt);
        if (splitAt <= 0) {
            splitAt = findSafeUtf8SplitPoint(bytes, qMin(byteLength, bytes.size()));
        }
        if (splitAt <= 0) {
            break;
        }

        const QByteArray chunk = bytes.left(splitAt).trimmed();
        if (!chunk.isEmpty()) {
            parts.append(QString::fromUtf8(chunk));
        }
        bytes = bytes.mid(splitAt);
    }

    const QByteArray tail = bytes.trimmed();
    if (!tail.isEmpty()) {
        parts.append(QString::fromUtf8(tail));
    }
    return parts;
}

QVector<QString> Communicate::splitTextForPlayback(const QString &text, int initialByteLength,
                                                    int subsequentByteLength) const
{
    QByteArray bytes = text.toUtf8();
    QVector<QString> parts;
    const int safeInitialLength = qBound(64, initialByteLength, ms_maxTextByteLength);
    const int safeMaxLength = qBound(safeInitialLength, subsequentByteLength, ms_maxTextByteLength);
    int currentLimit = safeInitialLength;

    while (!bytes.isEmpty()) {
        if (bytes.size() <= currentLimit) {
            const QByteArray tail = bytes.trimmed();
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
            splitAt = findSafeUtf8SplitPoint(bytes, qMin(safeMaxLength, bytes.size()));
        }
        splitAt = adjustSplitPointForXmlEntity(bytes, splitAt);
        if (splitAt <= 0) {
            splitAt = findSafeUtf8SplitPoint(bytes, qMin(currentLimit, bytes.size()));
        }
        if (splitAt <= 0) {
            break;
        }

        const QByteArray chunk = bytes.left(splitAt).trimmed();
        if (!chunk.isEmpty()) {
            parts.append(QString::fromUtf8(chunk));
        }
        bytes = bytes.mid(splitAt);
        currentLimit = qMin(safeMaxLength, currentLimit + qBound(16, currentLimit / 8, 40));
    }

    return parts;
}
