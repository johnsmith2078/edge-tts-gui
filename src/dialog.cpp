#include "dialog.h"
#include "ui_dialog.h"
#include <QFileDialog>
#include <QMimeData>
#include <QTimer>

Dialog::Dialog(QWidget *parent)
    : QDialog(parent)
    , ui(new Ui::Dialog)
    , m_comm()
{
    ui->setupUi(this);
    // connect this->send() to this->m_comm.start()
    connect(this, &Dialog::send, &m_comm, &Communicate::start);
    connect(&m_comm, &Communicate::finished, [&]() {
        ui->pushButtonPlay->setDisabled(false);
        ui->pushButtonPlay->setText("▶️ 播放");
        ui->pushButtonStop->setEnabled(false);
    });
    connect(this, &Dialog::stop, &m_comm, &Communicate::stop);

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
}

Dialog::~Dialog()
{
    delete ui;
}

void Dialog::playText(const QString& text)
{
    ui->plainTextEditContent->setPlainText(text);
    emit ui->pushButtonPlay->clicked(true);

    QTimer::singleShot(3000, [this, text](){
        if (manuallyStopped || text.size() <= 20) {
            return;
        }
        if (!m_comm.isPlaying() && !ui->pushButtonStop->isEnabled()) {
            playText(text);
        } else {
            setManuallyStopped(true);
        }
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

void Dialog::on_pushButtonPlay_clicked()
{
    QString text = ui->plainTextEditContent->toPlainText();
    if (text.isEmpty()) {
        return;
    }

    ui->pushButtonPlay->setDisabled(true);
    ui->pushButtonPlay->setText("⏳合成中...");
    ui->pushButtonStop->setEnabled(true);

    setCommunicate(text, voice, "");

    emit send();
}

void Dialog::on_pushButtonStop_clicked()
{
    emit stop();
    setManuallyStopped(true);
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
