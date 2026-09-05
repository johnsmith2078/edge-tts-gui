#include "dialog.h"
#include "ui_dialog.h"

#include <QEasingCurve>
#include <QFileDialog>
#include <QFileInfo>
#include <QMessageBox>
#include <QMimeData>
#include <QPropertyAnimation>
#include <QRegularExpression>
#include <QTimer>

Dialog::Dialog(QWidget *parent)
    : QDialog(parent)
    , ui(new Ui::Dialog)
    , m_comm()
{
    ui->setupUi(this);
    m_saveProgressAnimation = new QPropertyAnimation(ui->progressBarSave, "value", this);
    m_saveProgressAnimation->setDuration(220);
    m_saveProgressAnimation->setEasingCurve(QEasingCurve::OutCubic);

    connect(this, &Dialog::send, &m_comm, &Communicate::start);
    connect(&m_comm, &Communicate::finished, this, &Dialog::onPlayFinished);
    connect(&m_comm, &Communicate::finished, this, [this]() { handleAutoRetryFinished(); });
    connect(this, &Dialog::stop, &m_comm, &Communicate::stop);

    const auto resetSaveUi = [this]() {
        m_savingAudio = false;
        m_saveProgressAnimation->stop();
        ui->progressBarSave->setValue(0);
        ui->progressBarSave->setVisible(false);
        ui->pushButtonSave->setDisabled(false);
        ui->pushButtonSave->setText("💾 保存");
        ui->pushButtonPlay->setDisabled(false);
    };

    connect(&m_comm, &Communicate::saveFinished, this, resetSaveUi);
    connect(&m_comm, &Communicate::saveFailed, this, [this, resetSaveUi](const QString &error) {
        resetSaveUi();
        QMessageBox::warning(this, QStringLiteral("保存失败"), error);
    });
    connect(&m_comm, &Communicate::saveProgressChanged, this, [this](int percent) {
        ui->progressBarSave->setVisible(true);
        m_saveProgressAnimation->stop();
        m_saveProgressAnimation->setStartValue(ui->progressBarSave->value());
        m_saveProgressAnimation->setEndValue(percent);
        m_saveProgressAnimation->start();
    });

    connect(ui->comboBoxLanguage, &QComboBox::currentTextChanged, this, &Dialog::onLanguageChanged);
    connect(ui->comboBoxVoiceName, &QComboBox::currentTextChanged, this, &Dialog::onVoiceNameChanged);

    setAcceptDrops(true);
    ui->plainTextEditContent->setAcceptDrops(false);
    ui->plainTextEditContent->installEventFilter(this);
    connect(ui->plainTextEditContent, &QPlainTextEdit::textChanged, this, &Dialog::updateTextCount);

    loadVoiceData();
    updateTextCount();

    if (voice.isEmpty()) {
        voice = "zh-CN, XiaoyiNeural";
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
    ui->pushButtonSave->setDisabled(false);
    ui->pushButtonStop->setEnabled(false);
    setPlaybackActive(false);
}

void Dialog::playText(const QString &text)
{
    if (m_savingAudio) {
        return;
    }

    const QString trimmedText = text.trimmed();
    if (trimmedText.isEmpty()) {
        m_autoRetryEnabled = false;
        setManuallyStopped(true);
        return;
    }

    constexpr int kMaxAutoRetries = 5;
    m_autoRetryText = text;
    m_autoRetriesRemaining = kMaxAutoRetries;
    m_autoRetryEnabled = !manuallyStopped;
    m_lastFinishedAttemptSerial = -1;
    startAutoRetryAttempt();
}

void Dialog::stopPlayback()
{
    if (m_playbackActive) {
        on_pushButtonStop_clicked();
    }
}

bool Dialog::isPlaybackActive() const
{
    return m_playbackActive;
}

void Dialog::startAutoRetryAttempt()
{
    if (!m_autoRetryEnabled || m_autoRetryText.trimmed().isEmpty()) {
        m_autoRetryEnabled = false;
        return;
    }

    const int attemptSerial = ++m_autoAttemptSerial;
    ui->plainTextEditContent->setPlainText(m_autoRetryText);
    ui->pushButtonPlay->click();
    scheduleNoPlaybackWatchdog(attemptSerial, -1);
}

void Dialog::handleAutoRetryFinished()
{
    if (!m_autoRetryEnabled || manuallyStopped) {
        m_autoRetryEnabled = false;
        return;
    }
    if (m_lastFinishedAttemptSerial == m_autoAttemptSerial) {
        return;
    }
    m_lastFinishedAttemptSerial = m_autoAttemptSerial;

    const bool playbackStarted = m_comm.hasPlaybackStarted();
    const bool hasError = m_comm.hasPlaybackError() || !m_comm.isSynthesisComplete()
                          || m_comm.audioBytesReceived() <= 0;
    if (playbackStarted && !hasError) {
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

void Dialog::scheduleNoPlaybackWatchdog(int attemptSerial, qsizetype lastEdgeBytesReceived)
{
    constexpr int kNoPlaybackTimeoutMsEdge = 8000;
    QTimer::singleShot(kNoPlaybackTimeoutMsEdge, this, [this, attemptSerial, lastEdgeBytesReceived]() {
        if (!m_autoRetryEnabled || manuallyStopped || attemptSerial != m_autoAttemptSerial) {
            return;
        }
        if (m_comm.hasPlaybackStarted()) {
            return;
        }

        const qsizetype bytesReceived = m_comm.audioBytesReceived();
        const bool stalled = lastEdgeBytesReceived >= 0 && bytesReceived == lastEdgeBytesReceived;
        if (m_comm.isSynthesisComplete() || stalled || m_comm.hasPlaybackError()) {
            emit stop();
            return;
        }
        scheduleNoPlaybackWatchdog(attemptSerial, bytesReceived);
    });
}

bool Dialog::eventFilter(QObject *obj, QEvent *event)
{
    if (obj == ui->plainTextEditContent && event->type() == QEvent::KeyPress) {
        auto *keyEvent = static_cast<QKeyEvent *>(event);
        if (keyEvent->key() == Qt::Key_Return && keyEvent->modifiers() == Qt::ControlModifier
            && ui->pushButtonPlay->isEnabled()) {
            ui->pushButtonPlay->click();
            return true;
        }
        if (keyEvent->key() == Qt::Key_S && keyEvent->modifiers() == Qt::ControlModifier
            && ui->pushButtonSave->isEnabled()) {
            ui->pushButtonSave->click();
            return true;
        }
    }
    return QWidget::eventFilter(obj, event);
}

void Dialog::dragEnterEvent(QDragEnterEvent *event)
{
    if (event->mimeData()->hasUrls()) {
        event->acceptProposedAction();
    }
}

void Dialog::dropEvent(QDropEvent *event)
{
    const QMimeData *mimeData = event->mimeData();
    if (!mimeData->hasUrls() || mimeData->urls().isEmpty()) {
        return;
    }

    QFile file(mimeData->urls().first().toLocalFile());
    if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QTextStream in(&file);
        ui->plainTextEditContent->setPlainText(in.readAll());
    }
}

void Dialog::checkDuplicate(const QString &text, const QString &voiceCode)
{
    m_comm.setDuplicated(text == m_lastText && voiceCode == m_lastVoice);
}

void Dialog::setCommunicate(const QString &text, const QString &voiceCode, const QString &fileName)
{
    m_comm.setText(text);
    m_comm.setVoice(voiceCode);
    m_comm.setFileName(fileName);
    m_lastText = text;
    m_lastVoice = voiceCode;
}

void Dialog::on_pushButtonPlay_clicked()
{
    if (m_savingAudio) {
        return;
    }
    const QString text = ui->plainTextEditContent->toPlainText();
    if (text.trimmed().isEmpty()) {
        return;
    }

    ui->pushButtonPlay->setDisabled(true);
    ui->pushButtonPlay->setText("⏳合成中...");
    ui->pushButtonSave->setDisabled(true);
    ui->pushButtonStop->setEnabled(true);
    setPlaybackActive(true);

    setCommunicate(text, voice, {});
    emit send();
}

void Dialog::on_pushButtonStop_clicked()
{
    setManuallyStopped(true);
    setPlaybackActive(false);
    emit stop();
}

void Dialog::on_pushButtonSave_clicked()
{
    const QString text = ui->plainTextEditContent->toPlainText();
    if (text.trimmed().isEmpty()) {
        return;
    }

    const QString initialPath = lastDir.isEmpty() ? QDir::currentPath() : lastDir;
    const QString fileName = QFileDialog::getSaveFileName(
        this, "保存音频文件", initialPath, "音频文件 (*.mp3)");
    if (fileName.isEmpty()) {
        return;
    }

    lastDir = QFileInfo(fileName).absolutePath();
    m_savingAudio = true;
    m_saveProgressAnimation->stop();
    ui->progressBarSave->setValue(0);
    ui->progressBarSave->setVisible(true);
    ui->pushButtonSave->setDisabled(true);
    ui->pushButtonSave->setText("⏳ 保存中");
    ui->pushButtonPlay->setDisabled(true);

    setCommunicate(text, voice, fileName);
    emit send();
}

void Dialog::setManuallyStopped(bool stopped)
{
    manuallyStopped = stopped;
}

void Dialog::setPlaybackActive(bool active)
{
    if (m_playbackActive == active) {
        return;
    }
    m_playbackActive = active;
    emit playbackActiveChanged(active);
}

void Dialog::updateTextCount()
{
    static const QRegularExpression whitespaceRegex(QStringLiteral("\\s+"));
    QString text = ui->plainTextEditContent->toPlainText();
    text.remove(whitespaceRegex);
    ui->labelCharCount->setText(QStringLiteral("字数: %1").arg(text.size()));
}

void Dialog::on_radioButtonXiaoxiao_clicked(bool checked) { if (checked) voice = "zh-CN, XiaoxiaoNeural"; }
void Dialog::on_radioButtonXiaoyi_clicked(bool checked) { if (checked) voice = "zh-CN, XiaoyiNeural"; }
void Dialog::on_radioButtonYunjian_clicked(bool checked) { if (checked) voice = "zh-CN, YunjianNeural"; }
void Dialog::on_radioButtonYunxi_clicked(bool checked) { if (checked) voice = "zh-CN, YunxiNeural"; }
void Dialog::on_radioButtonYunxia_clicked(bool checked) { if (checked) voice = "zh-CN, YunxiaNeural"; }
void Dialog::on_radioButtonYunyang_clicked(bool checked) { if (checked) voice = "zh-CN, YunyangNeural"; }

void Dialog::loadVoiceData()
{
    QFile file(":/voice_list.tsv");
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        qWarning() << "Cannot open voice list";
        return;
    }

    QTextStream in(&file);
    while (!in.atEnd()) {
        const QStringList fields = in.readLine().split('\t');
        if (fields.size() >= 3) {
            data[fields[0]][fields[1]] = fields[2];
        }
    }

    ui->comboBoxLanguage->addItems(data.keys());
    if (!data.isEmpty()) {
        const QString initialLanguage = data.keys().first();
        ui->comboBoxLanguage->setCurrentText(initialLanguage);
        onLanguageChanged(initialLanguage);
    }
}

void Dialog::onLanguageChanged(const QString &languageName)
{
    ui->comboBoxVoiceName->clear();
    const QMap<QString, QString> voiceMap = data.value(languageName);
    if (voiceMap.isEmpty()) {
        return;
    }

    ui->comboBoxVoiceName->addItems(voiceMap.keys());
    const QString initialVoiceName = voiceMap.keys().first();
    ui->comboBoxVoiceName->setCurrentText(initialVoiceName);
    onVoiceNameChanged(initialVoiceName);
}

void Dialog::onVoiceNameChanged(const QString &voiceName)
{
    const QString code = data.value(ui->comboBoxLanguage->currentText()).value(voiceName);
    if (!code.isEmpty()) {
        voice = code;
    }
}
