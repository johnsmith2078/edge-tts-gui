#include "dashscope_tts.h"

#include <QByteArray>
#include <QDebug>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkRequest>
#include <QProcessEnvironment>
#include <QSettings>
#include <QUrl>
#include <QVariant>
#include <QtGlobal>

namespace {

bool isDashScopeUrlErrorResponse(const QByteArray &responseBody)
{
    if (responseBody.isEmpty()) {
        return false;
    }

    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(responseBody, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        return false;
    }

    const QJsonObject root = doc.object();
    const QString code = root.value("code").toString();
    const QString message = root.value("message").toString();
    if (code != "InvalidParameter" || message.isEmpty()) {
        return false;
    }

    return message.contains("url error", Qt::CaseInsensitive) || message.contains("check url", Qt::CaseInsensitive);
}

} // namespace

QString DashScopeTTS::readEnvValue(const QString &name)
{
    QString value = QProcessEnvironment::systemEnvironment().value(name).trimmed();
    if (!value.isEmpty()) {
        if ((value.startsWith('"') && value.endsWith('"')) || (value.startsWith('\'') && value.endsWith('\''))) {
            if (value.size() >= 2) {
                value = value.mid(1, value.size() - 2).trimmed();
            }
        }
        return value;
    }

#ifdef Q_OS_WIN
    {
        QSettings userEnv("HKEY_CURRENT_USER\\Environment", QSettings::NativeFormat);
        value = userEnv.value(name).toString().trimmed();
        if (!value.isEmpty()) {
            if ((value.startsWith('"') && value.endsWith('"')) || (value.startsWith('\'') && value.endsWith('\''))) {
                if (value.size() >= 2) {
                    value = value.mid(1, value.size() - 2).trimmed();
                }
            }
            return value;
        }
    }
    {
        QSettings systemEnv("HKEY_LOCAL_MACHINE\\SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Environment", QSettings::NativeFormat);
        value = systemEnv.value(name).toString().trimmed();
        if (!value.isEmpty()) {
            if ((value.startsWith('"') && value.endsWith('"')) || (value.startsWith('\'') && value.endsWith('\''))) {
                if (value.size() >= 2) {
                    value = value.mid(1, value.size() - 2).trimmed();
                }
            }
            return value;
        }
    }
#endif

    return QString();
}

QString DashScopeTTS::resolvedApiKey()
{
    return readEnvValue("DASHSCOPE_API_KEY");
}

DashScopeTTS::DashScopeTTS(QObject *parent)
    : QObject(parent)
{
    m_manager = new QNetworkAccessManager(this);
    m_player = new QMediaPlayer(this);
    m_buffer = new QBuffer(this);
    m_audioOutput = new QAudioOutput(this);

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
        qDebug() << "DashScope TTS media player error:" << errorString;
        emit finished();
    });

    QObject::connect(m_player, &QMediaPlayer::mediaStatusChanged, this, [this](QMediaPlayer::MediaStatus status) {
        if (status == QMediaPlayer::EndOfMedia) {
            emit finished();
        }
    });

    QObject::connect(this, &DashScopeTTS::stop, m_player, &QMediaPlayer::stop);
    QObject::connect(this, &DashScopeTTS::stop, this, [this]() {
        abortRequests();
        emit finished();
    });
}

DashScopeTTS::~DashScopeTTS()
{
    delete m_player;
    delete m_buffer;
    delete m_audioOutput;
}

QString DashScopeTTS::baseHttpApiUrl()
{
    QString configured = readEnvValue("DASHSCOPE_BASE_HTTP_API_URL");
    if (configured.isEmpty()) {
        configured = readEnvValue("DASHSCOPE_BASE_URL");
    }
    if (!configured.isEmpty()) {
        return configured;
    }
    return "https://dashscope.aliyuncs.com/api/v1";
}

QString DashScopeTTS::apiKey()
{
    return resolvedApiKey();
}

QString DashScopeTTS::modelName()
{
    const QString configured = readEnvValue("DASHSCOPE_TTS_MODEL");
    if (!configured.isEmpty()) {
        return configured;
    }
    return "qwen3-tts-flash";
}

void DashScopeTTS::resetState()
{
    m_hasPlaybackStarted = false;
    m_playbackErrorOccurred = false;
    m_requestErrorOccurred = false;
    m_lastAudioByteCount = 0;
}

void DashScopeTTS::abortRequests()
{
    if (m_synthesisReply) {
        m_synthesisReply->abort();
        m_synthesisReply->deleteLater();
        m_synthesisReply = nullptr;
    }
    if (m_audioReply) {
        m_audioReply->abort();
        m_audioReply->deleteLater();
        m_audioReply = nullptr;
    }
}

void DashScopeTTS::getTTS(const QString &text, const QString &voice, const QString &languageType)
{
    resetState();
    abortRequests();

    m_text = text;
    m_voice = voice;
    m_languageType = languageType;

    const QString configuredEndpoint = readEnvValue("DASHSCOPE_TTS_ENDPOINT").trimmed();
    m_endpoints.clear();
    m_endpointIndex = 0;
    m_hasCustomEndpoint = !configuredEndpoint.isEmpty();
    if (m_hasCustomEndpoint) {
        m_endpoints.append(configuredEndpoint);
    } else {
        m_endpoints.append("/services/aigc/multimodal-conversation/generation");
        m_endpoints.append("/services/aigc/multimodal-generation/generation");
    }

    m_payloads.clear();
    m_payloadIndex = 0;

    // Payload variant 1: MultiModalConversation-style messages + parameters.
    {
        QJsonObject contentItem;
        contentItem.insert("text", m_text);

        QJsonArray contentArray;
        contentArray.append(contentItem);

        QJsonObject message;
        message.insert("role", "user");
        message.insert("content", contentArray);

        QJsonArray messages;
        messages.append(message);

        QJsonObject input;
        input.insert("messages", messages);

        QJsonObject parameters;
        parameters.insert("voice", m_voice);
        parameters.insert("language_type", m_languageType);

        QJsonObject root;
        root.insert("model", modelName());
        root.insert("input", input);
        root.insert("parameters", parameters);

        m_payloads.append(QJsonDocument(root).toJson(QJsonDocument::Compact));
    }

    // Payload variant 2: Simple input.text + parameters.
    {
        QJsonObject input;
        input.insert("text", m_text);

        QJsonObject parameters;
        parameters.insert("voice", m_voice);
        parameters.insert("language_type", m_languageType);

        QJsonObject root;
        root.insert("model", modelName());
        root.insert("input", input);
        root.insert("parameters", parameters);

        m_payloads.append(QJsonDocument(root).toJson(QJsonDocument::Compact));
    }

    // Payload variant 3: Simple input fields.
    {
        QJsonObject input;
        input.insert("text", m_text);
        input.insert("voice", m_voice);
        input.insert("language_type", m_languageType);

        QJsonObject root;
        root.insert("model", modelName());
        root.insert("input", input);

        m_payloads.append(QJsonDocument(root).toJson(QJsonDocument::Compact));
    }

    startSynthesisRequest();
}

void DashScopeTTS::startSynthesisRequest()
{
    if (m_payloadIndex < 0 || m_payloadIndex >= m_payloads.size()) {
        m_requestErrorOccurred = true;
        emit finished();
        return;
    }
    if (m_endpointIndex < 0 || m_endpointIndex >= m_endpoints.size()) {
        m_requestErrorOccurred = true;
        emit finished();
        return;
    }

    const QString key = apiKey();
    if (key.isEmpty()) {
        m_requestErrorOccurred = true;
        qDebug() << "DashScope API key missing (DASHSCOPE_API_KEY).";
        emit finished();
        return;
    }

    QString endpoint = m_endpoints[m_endpointIndex].trimmed();

    QUrl url;
    if (endpoint.startsWith("http://") || endpoint.startsWith("https://")) {
        url = QUrl(endpoint);
    } else {
        QString base = baseHttpApiUrl().trimmed();
        if (base.endsWith('/')) {
            base.chop(1);
        }
        if (endpoint.isEmpty()) {
            endpoint = "/services/aigc/multimodal-conversation/generation";
        } else if (!endpoint.startsWith('/')) {
            endpoint.prepend('/');
        }
        url = QUrl(base + endpoint);
    }

    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    request.setRawHeader("Authorization", ("Bearer " + key).toUtf8());

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
#endif

    m_synthesisReply = m_manager->post(request, m_payloads[m_payloadIndex]);
    QObject::connect(m_synthesisReply, &QNetworkReply::finished, this, &DashScopeTTS::onSynthesisFinished);
}

void DashScopeTTS::startAudioDownload(const QUrl &url)
{
    if (!url.isValid()) {
        m_requestErrorOccurred = true;
        emit finished();
        return;
    }

    if (m_audioReply) {
        m_audioReply->abort();
        m_audioReply->deleteLater();
        m_audioReply = nullptr;
    }

    QNetworkRequest request(url);
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
#endif
    m_audioReply = m_manager->get(request);
    QObject::connect(m_audioReply, &QNetworkReply::finished, this, &DashScopeTTS::onAudioDownloadFinished);
}

void DashScopeTTS::onSynthesisFinished()
{
    QNetworkReply *reply = qobject_cast<QNetworkReply *>(sender());
    if (!reply) {
        return;
    }
    if (reply != m_synthesisReply) {
        reply->deleteLater();
        return;
    }
    m_synthesisReply = nullptr;

    const QUrl requestUrl = reply->request().url();
    const QVariant httpStatusAttribute = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute);
    const int httpStatusCode = httpStatusAttribute.isValid() ? httpStatusAttribute.toInt() : -1;
    const QString httpReasonPhrase = reply->attribute(QNetworkRequest::HttpReasonPhraseAttribute).toString();
    const QString requestId = QString::fromUtf8(reply->rawHeader("X-DashScope-Request-Id"));
    const QString traceId = QString::fromUtf8(reply->rawHeader("X-DashScope-Trace-Id"));

    const QByteArray responseBody = reply->readAll();
    const QNetworkReply::NetworkError networkError = reply->error();
    const QString networkErrorString = reply->errorString();
    reply->deleteLater();

    if (networkError != QNetworkReply::NoError) {
        qDebug() << "DashScope TTS request error:" << networkErrorString
                 << "HTTP" << httpStatusCode << httpReasonPhrase
                 << "url" << requestUrl
                 << "payload" << (m_payloadIndex + 1) << "/" << m_payloads.size()
                 << "request_id" << requestId
                 << "trace_id" << traceId;

        if (!responseBody.isEmpty()) {
            QString responsePreview = QString::fromUtf8(responseBody).trimmed();
            constexpr int kMaxPreviewChars = 2048;
            if (responsePreview.size() > kMaxPreviewChars) {
                responsePreview = responsePreview.left(kMaxPreviewChars) + "...(truncated)";
            }
            if (!responsePreview.isEmpty()) {
                qDebug() << "DashScope TTS error response:" << responsePreview;
            }

            QJsonParseError errorParseError;
            const QJsonDocument errorDoc = QJsonDocument::fromJson(responseBody, &errorParseError);
            if (errorParseError.error == QJsonParseError::NoError && errorDoc.isObject()) {
                const QJsonObject errorRoot = errorDoc.object();
                int statusCode = errorRoot.value("status_code").toInt(-1);
                if (statusCode < 0 && httpStatusCode > 0) {
                    statusCode = httpStatusCode;
                }
                const QString code = errorRoot.value("code").toString();
                const QString message = errorRoot.value("message").toString();
                const QString requestIdFromBody = errorRoot.value("request_id").toString();
                const QString traceIdFromBody = errorRoot.value("trace_id").toString();
                if (statusCode > 0 || !code.isEmpty() || !message.isEmpty() || !requestIdFromBody.isEmpty() || !traceIdFromBody.isEmpty()) {
                    qDebug() << "DashScope TTS API error response:" << statusCode << code << message
                             << "request_id" << requestIdFromBody
                             << "trace_id" << traceIdFromBody;
                }
            }
        }

        const bool looksLikeUrlError = (httpStatusCode == 404) || (httpStatusCode == 400 && isDashScopeUrlErrorResponse(responseBody));
        if (looksLikeUrlError && !m_hasCustomEndpoint && (m_endpointIndex + 1) < m_endpoints.size()) {
            ++m_endpointIndex;
            m_payloadIndex = 0;
            qDebug() << "DashScope TTS retrying with endpoint:" << m_endpoints[m_endpointIndex];
            startSynthesisRequest();
            return;
        }

        if (httpStatusCode == 401 || httpStatusCode == 403) {
            m_requestErrorOccurred = true;
            emit finished();
            return;
        }

        if (++m_payloadIndex < m_payloads.size()) {
            startSynthesisRequest();
            return;
        }
        m_requestErrorOccurred = true;
        emit finished();
        return;
    }

    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(responseBody, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        qDebug() << "DashScope TTS JSON parse error:" << parseError.errorString();
        if (!responseBody.isEmpty()) {
            QString responsePreview = QString::fromUtf8(responseBody).trimmed();
            constexpr int kMaxPreviewChars = 2048;
            if (responsePreview.size() > kMaxPreviewChars) {
                responsePreview = responsePreview.left(kMaxPreviewChars) + "...(truncated)";
            }
            if (!responsePreview.isEmpty()) {
                qDebug() << "DashScope TTS response body:" << responsePreview;
            }
        }
        if (++m_payloadIndex < m_payloads.size()) {
            startSynthesisRequest();
            return;
        }
        m_requestErrorOccurred = true;
        emit finished();
        return;
    }

    const QJsonObject root = doc.object();
    int statusCode = root.value("status_code").toInt(-1);
    if (statusCode < 0 && httpStatusCode > 0) {
        statusCode = httpStatusCode;
    }
    const QString code = root.value("code").toString();
    const QString message = root.value("message").toString();
    if (statusCode != 200 || (!code.isEmpty() && code != "SUCCESS")) {
        qDebug() << "DashScope TTS API error:" << statusCode << code << message
                 << "request_id" << root.value("request_id").toString()
                 << "trace_id" << root.value("trace_id").toString();

        if (statusCode == 400 && isDashScopeUrlErrorResponse(responseBody) && !m_hasCustomEndpoint && (m_endpointIndex + 1) < m_endpoints.size()) {
            ++m_endpointIndex;
            m_payloadIndex = 0;
            qDebug() << "DashScope TTS retrying with endpoint:" << m_endpoints[m_endpointIndex];
            startSynthesisRequest();
            return;
        }

        if (++m_payloadIndex < m_payloads.size()) {
            startSynthesisRequest();
            return;
        }
        m_requestErrorOccurred = true;
        emit finished();
        return;
    }

    const QJsonObject output = root.value("output").toObject();
    const QJsonObject audio = output.value("audio").toObject();

    const QString audioUrl = audio.value("url").toString();
    const QString audioDataBase64 = audio.value("data").toString();

    if (!audioDataBase64.isEmpty()) {
        const QByteArray audioBytes = QByteArray::fromBase64(audioDataBase64.toUtf8());
        m_lastAudioByteCount = audioBytes.size();
        if (audioBytes.isEmpty()) {
            m_requestErrorOccurred = true;
            emit finished();
            return;
        }

        m_buffer->close();
        m_buffer->setData(audioBytes);
        m_buffer->open(QIODevice::ReadOnly);

        m_player->setAudioOutput(m_audioOutput);
        m_player->setSourceDevice(m_buffer, QUrl("audio.wav"));
        m_player->play();
        return;
    }

    if (audioUrl.isEmpty()) {
        qDebug() << "DashScope TTS response missing audio url/data.";
        if (++m_payloadIndex < m_payloads.size()) {
            startSynthesisRequest();
            return;
        }
        m_requestErrorOccurred = true;
        emit finished();
        return;
    }

    startAudioDownload(QUrl(audioUrl));
}

void DashScopeTTS::onAudioDownloadFinished()
{
    QNetworkReply *reply = qobject_cast<QNetworkReply *>(sender());
    if (!reply) {
        return;
    }
    if (reply != m_audioReply) {
        reply->deleteLater();
        return;
    }
    m_audioReply = nullptr;

    if (reply->error() != QNetworkReply::NoError) {
        m_requestErrorOccurred = true;
        qDebug() << "DashScope TTS audio download error:" << reply->errorString();
        reply->deleteLater();
        emit finished();
        return;
    }

    const QByteArray audioBytes = reply->readAll();
    reply->deleteLater();

    m_lastAudioByteCount = audioBytes.size();
    if (audioBytes.isEmpty()) {
        m_requestErrorOccurred = true;
        emit finished();
        return;
    }

    m_buffer->close();
    m_buffer->setData(audioBytes);
    m_buffer->open(QIODevice::ReadOnly);

    m_player->setAudioOutput(m_audioOutput);
    m_player->setSourceDevice(m_buffer, QUrl("audio.wav"));
    m_player->play();
}

bool DashScopeTTS::isPlaying() const
{
    return m_player->playbackState() == QMediaPlayer::PlayingState;
}

bool DashScopeTTS::hasPlaybackStarted() const
{
    return m_hasPlaybackStarted;
}

bool DashScopeTTS::hasPlaybackError() const
{
    return m_playbackErrorOccurred;
}

bool DashScopeTTS::hasRequestError() const
{
    return m_requestErrorOccurred;
}

qsizetype DashScopeTTS::lastAudioByteCount() const
{
    return m_lastAudioByteCount;
}
