#include "dialog.h"
#include "ui_dialog.h"
#include <QFileDialog>
#include <QMimeData>
#include <QMessageBox>
#include <QProcessEnvironment>
#include <QTimer>

Dialog::Dialog(QWidget *parent)
    : QDialog(parent)
    , ui(new Ui::Dialog)
    , m_comm()
{
    ui->setupUi(this);
    // connect this->send() to this->m_comm.start()
    connect(this, &Dialog::send, &m_comm, &Communicate::start);
    connect(&m_comm, &Communicate::finished, this, &Dialog::onPlayFinished);
    connect(&m_tts, &TextToSpeech::finished, this, &Dialog::onPlayFinished);
    connect(&m_qwen, &DashScopeTTS::finished, this, &Dialog::onPlayFinished);
    connect(&m_comm, &Communicate::finished, this, [this]() { handleAutoRetryFinished(TTSEngine::Edge); });
    connect(&m_tts, &TextToSpeech::finished, this, [this]() { handleAutoRetryFinished(TTSEngine::GPTSoVITS); });
    connect(&m_qwen, &DashScopeTTS::finished, this, [this]() { handleAutoRetryFinished(TTSEngine::Qwen); });
    connect(this, &Dialog::stop, &m_comm, &Communicate::stop);
    connect(this, &Dialog::stop, &m_tts, &TextToSpeech::stop);
    connect(this, &Dialog::stop, &m_qwen, &DashScopeTTS::stop);

    connect(&m_comm, &Communicate::saveFinished, [&]() {
        ui->pushButtonSave->setDisabled(false);
    });

    connect(ui->comboBoxLanguage, &QComboBox::currentTextChanged, this, &Dialog::onLanguageChanged);
    connect(ui->comboBoxVoiceName, &QComboBox::currentTextChanged, this, &Dialog::onVoiceNameChanged);

    setAcceptDrops(true);
    ui->plainTextEditContent->setAcceptDrops(false);

    ui->plainTextEditContent->installEventFilter(this);

    loadVoiceData();

    voice = "zh-CN, XiaoyiNeural";

    connect(ui->checkBoxUseGPTSoVITS, &QCheckBox::toggled, this, [this](bool checked) {
        if (!checked) {
            return;
        }
        ui->checkBoxUseQwenTTS->setChecked(false);
    });
    connect(ui->checkBoxUseQwenTTS, &QCheckBox::toggled, this, [this](bool checked) {
        if (!checked) {
            return;
        }
        ui->checkBoxUseGPTSoVITS->setChecked(false);
    });

    if (!DashScopeTTS::resolvedApiKey().isEmpty()) {
        ui->checkBoxUseQwenTTS->setChecked(true);
    }
}

Dialog::~Dialog()
{
    delete ui;
}

void Dialog::onPlayFinished()
{
    ui->pushButtonPlay->setDisabled(false);
    ui->pushButtonPlay->setText("▶️ 播放");
    ui->pushButtonStop->setEnabled(false);
    setPlaybackActive(false);
}

void Dialog::playText(const QString& text)
{
    constexpr int kMaxAutoRetries = 5;
    m_autoRetryText = text;
    m_autoRetriesRemaining = kMaxAutoRetries;
    m_autoRetryEnabled = !manuallyStopped;
    m_lastFinishedAttemptSerial = -1;
    startAutoRetryAttempt();
}

void Dialog::stopPlayback()
{
    if (!m_playbackActive) {
        return;
    }
    on_pushButtonStop_clicked();
}

bool Dialog::isPlaybackActive() const
{
    return m_playbackActive;
}

void Dialog::startAutoRetryAttempt()
{
    const int attemptSerial = ++m_autoAttemptSerial;
    ui->plainTextEditContent->setPlainText(m_autoRetryText);
    m_autoAttemptEngine = selectedEngine();
    emit ui->pushButtonPlay->clicked(true);
    scheduleNoPlaybackWatchdog(attemptSerial, m_autoAttemptEngine, -1);
}

void Dialog::handleAutoRetryFinished(TTSEngine engine)
{
    if (!m_autoRetryEnabled || manuallyStopped) {
        m_autoRetryEnabled = false;
        return;
    }

    if (engine != m_autoAttemptEngine) {
        return;
    }

    if (m_lastFinishedAttemptSerial == m_autoAttemptSerial) {
        return;
    }
    m_lastFinishedAttemptSerial = m_autoAttemptSerial;

    bool playbackStarted = false;
    bool hasError = false;
    switch (engine) {
    case TTSEngine::Edge:
        playbackStarted = m_comm.hasPlaybackStarted();
        hasError = m_comm.hasPlaybackError() || !m_comm.isSynthesisComplete() || m_comm.audioBytesReceived() <= 0;
        break;
    case TTSEngine::GPTSoVITS:
        playbackStarted = m_tts.hasPlaybackStarted();
        hasError = m_tts.hasPlaybackError() || m_tts.hasRequestError() || m_tts.lastAudioByteCount() <= 0;
        break;
    case TTSEngine::Qwen:
        playbackStarted = m_qwen.hasPlaybackStarted();
        hasError = m_qwen.hasPlaybackError() || m_qwen.hasRequestError() || m_qwen.lastAudioByteCount() <= 0;
        break;
    }
    const bool success = playbackStarted && !hasError;
    if (success) {
        m_autoRetryEnabled = false;
        setManuallyStopped(true);
        return;
    }

    if (m_autoRetriesRemaining <= 0) {
        m_autoRetryEnabled = false;
        setManuallyStopped(true);
        return;
    }

    --m_autoRetriesRemaining;
    QTimer::singleShot(200, this, [this]() { startAutoRetryAttempt(); });
}

void Dialog::scheduleNoPlaybackWatchdog(int attemptSerial, TTSEngine engine, qsizetype lastEdgeBytesReceived)
{
    constexpr int kEdgeStartupSize = 8192 * 4;
    constexpr int kNoPlaybackTimeoutMsEdge = 8000;
    constexpr int kNoPlaybackTimeoutMsGPT = 20000;
    constexpr int kNoPlaybackTimeoutMsQwen = 25000;

    int timeoutMs = 0;
    switch (engine) {
    case TTSEngine::Edge:
        timeoutMs = kNoPlaybackTimeoutMsEdge;
        break;
    case TTSEngine::GPTSoVITS:
        timeoutMs = kNoPlaybackTimeoutMsGPT;
        break;
    case TTSEngine::Qwen:
        timeoutMs = kNoPlaybackTimeoutMsQwen;
        break;
    }

    QTimer::singleShot(timeoutMs, this, [this, attemptSerial, engine, lastEdgeBytesReceived]() {
        if (!m_autoRetryEnabled || manuallyStopped || attemptSerial != m_autoAttemptSerial) {
            return;
        }

        if (engine != m_autoAttemptEngine) {
            return;
        }

        bool playbackStarted = false;
        switch (engine) {
        case TTSEngine::Edge:
            playbackStarted = m_comm.hasPlaybackStarted();
            break;
        case TTSEngine::GPTSoVITS:
            playbackStarted = m_tts.hasPlaybackStarted();
            break;
        case TTSEngine::Qwen:
            playbackStarted = m_qwen.hasPlaybackStarted();
            break;
        }
        if (playbackStarted) {
            return;
        }

        if (engine != TTSEngine::Edge) {
            emit stop();
            return;
        }

        const qsizetype bytesReceived = m_comm.audioBytesReceived();
        const bool stalled = (lastEdgeBytesReceived >= 0 && bytesReceived == lastEdgeBytesReceived);
        if (bytesReceived == 0 || bytesReceived >= kEdgeStartupSize || stalled) {
            emit stop();
            return;
        }

        scheduleNoPlaybackWatchdog(attemptSerial, engine, bytesReceived);
    });
}

bool Dialog::eventFilter(QObject *obj, QEvent *event)
{
    if (obj == ui->plainTextEditContent && event->type() == QEvent::KeyPress) {
        QKeyEvent *keyEvent = static_cast<QKeyEvent *>(event);
        if (keyEvent->key() == Qt::Key_Return && keyEvent->modifiers() == Qt::ControlModifier && ui->pushButtonPlay->isEnabled()) {
            // Ctrl+Enter was pressed, play
            ui->pushButtonPlay->click();
            return true;
        } else if (keyEvent->key() == Qt::Key_S && keyEvent->modifiers() == Qt::ControlModifier && ui->pushButtonSave->isEnabled()) {
            // Ctrl+S was pressed, save
            ui->pushButtonSave->click();
            return true;
        }
    }

    // pass the event on to the parent class
    return QWidget::eventFilter(obj, event);
}

void Dialog::dragEnterEvent(QDragEnterEvent *event)
{
    if (event->mimeData()->hasUrls()) {
        event->acceptProposedAction();  // 接受拖拽操作
    }
}

void Dialog::dropEvent(QDropEvent *event)
{
    const QMimeData *mimeData = event->mimeData();

    if (mimeData->hasUrls()) {
        QList<QUrl> urlList = mimeData->urls();
        if (!urlList.isEmpty()) {
            QString filePath = urlList.first().toLocalFile();  // 获取文件路径
            QFile file(filePath);

            // 检查是否是文本文件
            if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
                QTextStream in(&file);
                ui->plainTextEditContent->setPlainText(in.readAll());  // 读取文件并设置为文本框内容
                file.close();
            }
        }
    }
}

void Dialog::checkDuplicate(const QString& text, const QString& voice)
{
    if (text == m_lastText && voice == m_lastVoice) {
        m_comm.setDuplicated(true);
    } else {
        m_comm.setDuplicated(false);
    }
}

void Dialog::setCommunicate(const QString& text, const QString& voice, const QString& fileName)
{
    m_comm.setText(text);
    m_comm.setVoice(voice);
    m_comm.setFileName(fileName);
    // checkDuplicate(text, voice);
    m_lastText = text;
    m_lastVoice = voice;
}

bool Dialog::isUseGPTSoVITS()
{
    return ui->checkBoxUseGPTSoVITS->isChecked();
}

bool Dialog::isUseQwenTTS()
{
    return ui->checkBoxUseQwenTTS->isChecked();
}

Dialog::TTSEngine Dialog::selectedEngine()
{
    if (isUseQwenTTS()) {
        return TTSEngine::Qwen;
    }
    if (isUseGPTSoVITS()) {
        return TTSEngine::GPTSoVITS;
    }
    return TTSEngine::Edge;
}

bool isValidAudioFile(const QString &filePath) {
    // 检查文件扩展名
    QStringList validExtensions = {"mp3", "wav", "ogg", "flac", "aac"};
    QFileInfo fileInfo(filePath);
    if (!validExtensions.contains(fileInfo.suffix().toLower())) {
        return false;
    }

    // 尝试加载文件
    QMediaPlayer player;
    player.setSource(QUrl::fromLocalFile(filePath));
    return player.error() == QMediaPlayer::NoError;
}

void Dialog::on_pushButtonPlay_clicked()
{
    QString text = ui->plainTextEditContent->toPlainText();
    if (text.isEmpty()) {
        return;
    }

    const TTSEngine engine = selectedEngine();
    const QString refAudioFilename = ui->lineEditRefAudio->text();
    if (engine == TTSEngine::GPTSoVITS && !isValidAudioFile(refAudioFilename)) {
        QMessageBox::warning(this, "GPT-SoVITS", "请选择有效的参考音频文件");
        m_autoRetryEnabled = false;
        setManuallyStopped(true);
        return;
    }

    if (engine == TTSEngine::Qwen) {
        const QString key = DashScopeTTS::resolvedApiKey();
        if (key.isEmpty()) {
            QMessageBox::warning(this, "Qwen3-TTS-Flash", "请先设置环境变量 DASHSCOPE_API_KEY（如已设置，请重启程序/资源管理器或重新登录）");
            m_autoRetryEnabled = false;
            setManuallyStopped(true);
            return;
        }
        if (ui->lineEditQwenVoice->text().trimmed().isEmpty()) {
            ui->lineEditQwenVoice->setText("Cherry");
        }
    }

    ui->pushButtonPlay->setDisabled(true);
    ui->pushButtonPlay->setText("⏳合成中...");
    ui->pushButtonStop->setEnabled(true);
    setPlaybackActive(true);

    if (engine == TTSEngine::Edge) {
        setCommunicate(text, voice, "");
        emit send();
        return;
    }

    if (engine == TTSEngine::GPTSoVITS) {
        m_tts.getTTS(text, refAudioFilename);
        return;
    }

    m_qwen.getTTS(text, ui->lineEditQwenVoice->text().trimmed());
}

void Dialog::on_pushButtonStop_clicked()
{
    setManuallyStopped(true);
    setPlaybackActive(false);
    emit stop();
}

void Dialog::on_pushButtonSave_clicked()
{
    QString text = ui->plainTextEditContent->toPlainText();
    if (text.isEmpty()) {
        return;
    }

    QString dir;
    if (lastDir.isEmpty()) {
        dir = QDir::currentPath();
    } else {
        dir = lastDir;
    }

    QString fileName = QFileDialog::getSaveFileName(
        this, // 父窗口
        "保存音频文件", // 对话框标题
        dir, // 默认文件路径
        "音频文件 (*.mp3)" // 文件过滤器
    );

    if (fileName.isEmpty()) {
        return;
    }

    lastDir = fileName;

    ui->pushButtonSave->setDisabled(true);

    setCommunicate(text, voice, fileName);

    emit send();
}

void Dialog::setManuallyStopped(bool manuallyStopped)
{
    this->manuallyStopped = manuallyStopped;
}

void Dialog::setPlaybackActive(bool active)
{
    if (m_playbackActive == active) {
        return;
    }
    m_playbackActive = active;
    emit playbackActiveChanged(active);
}

void Dialog::on_radioButtonXiaoxiao_clicked(bool checked)
{
    if (checked) {
        voice = "zh-CN, XiaoxiaoNeural";
    }
}


void Dialog::on_radioButtonXiaoyi_clicked(bool checked)
{
    if (checked) {
        voice = "zh-CN, XiaoyiNeural";
    }
}


void Dialog::on_radioButtonYunjian_clicked(bool checked)
{
    if (checked) {
        voice = "zh-CN, YunjianNeural";
    }
}


void Dialog::on_radioButtonYunxi_clicked(bool checked)
{
    if (checked) {
        voice = "zh-CN, YunxiNeural";
    }
}


void Dialog::on_radioButtonYunxia_clicked(bool checked)
{
    if (checked) {
        voice = "zh-CN, YunxiaNeural";
    }
}


void Dialog::on_radioButtonYunyang_clicked(bool checked)
{
    if (checked) {
        voice = "zh-CN, YunyangNeural";
    }
}

void Dialog::loadVoiceData()
{
    QFile file(":/voice_list.tsv");
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
    {
        qDebug() << "Cannot open file!";
        return;
    }

    QTextStream in(&file);

    while (!in.atEnd())
    {
        QString line = in.readLine();
        QStringList fields = line.split('\t');

        if (fields.size() >= 3)
        {
            QString language = fields[0];
            QString voiceName = fields[1];
            QString code = fields[2];

            data[language][voiceName] = code;
        }
    }

    // 填充语言的ComboBox
    ui->comboBoxLanguage->addItems(data.keys());

    // 设置初始的语言
    if (!data.isEmpty())
    {
        QString initialLanguage = data.keys().first();
        ui->comboBoxLanguage->setCurrentText(initialLanguage);
        onLanguageChanged(initialLanguage);
    }
}

void Dialog::onLanguageChanged(const QString &language)
{
    // 清空语音名的ComboBox
    ui->comboBoxVoiceName->clear();

    // 获取当前语言的语音名列表
    QMap<QString, QString> voiceMap = data.value(language);

    if (!voiceMap.isEmpty())
    {
        ui->comboBoxVoiceName->addItems(voiceMap.keys());

        // 设置初始的语音名称
        QString initialVoiceName = voiceMap.keys().first();
        ui->comboBoxVoiceName->setCurrentText(initialVoiceName);
        onVoiceNameChanged(initialVoiceName);
    }
}

void Dialog::onVoiceNameChanged(const QString &voiceName)
{
    QString language = ui->comboBoxLanguage->currentText();
    QString code = data.value(language).value(voiceName);

    voice = code;
}

void Dialog::on_pushButtonSelectRefAudio_clicked()
{
    QString filePath = QFileDialog::getOpenFileName(this, tr("Select Audio File"), "", tr("Audio Files (*.mp3 *.wav *.ogg *.flac *.aac)"));
    if (!filePath.isEmpty()) {
        ui->lineEditRefAudio->setText(filePath);
    }
}
