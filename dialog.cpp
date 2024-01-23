#include "dialog.h"
#include "ui_dialog.h"
#include "communicate.h"

Dialog::Dialog(QWidget *parent)
    : QDialog(parent)
    , ui(new Ui::Dialog)
{
    ui->setupUi(this);
    Communicate *comm = new Communicate("");
    comm->start();
}

Dialog::~Dialog()
{
    delete ui;
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
    Communicate *comm = new Communicate(text, voice);
    connect(comm, &Communicate::finished, [&]() {
        ui->pushButtonPlay->setDisabled(false);
        ui->pushButtonPlay->setText("▶️ 播放");
        ui->pushButtonStop->setEnabled(false);
    });
    // connect this->stop() to comm->stop()
    connect(this, &Dialog::stop, comm, &Communicate::stop);
    comm->start();
}

void Dialog::on_pushButtonStop_clicked()
{
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

    Communicate *comm = new Communicate(text, voice, fileName);

    connect(comm, &Communicate::saveFinished, [&]() {
        ui->pushButtonSave->setDisabled(false);
    });
    comm->start();
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
