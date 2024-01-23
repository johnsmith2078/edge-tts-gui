#include "dialog.h"
#include "ui_dialog.h"

#include <iostream>

Dialog::Dialog(QWidget *parent)
    : QDialog(parent)
    , ui(new Ui::Dialog)
{
    ui->setupUi(this);
}

Dialog::~Dialog()
{
    delete ui;
}

void Dialog::on_pushButtonPlay_clicked()
{
    QString text = ui->plainTextEditContent->toPlainText(); // 获取QPlainTextEdit中的文本
    if (text.isEmpty()) {
        return;
    }
    std::wstring content = text.toStdWString(); // 将QString转换为std::wstring
    content.erase(std::remove(content.begin(), content.end(), L'\n'), content.end()); // 删除换行符
    content.erase(std::remove(content.begin(), content.end(), L'\r'), content.end()); // 删除回车符
    content.erase(std::remove(content.begin(), content.end(), L'-'), content.end()); // 删除横线
    content.erase(std::remove(content.begin(), content.end(), L'"'), content.end()); // 删除双引号
    content.erase(std::remove(content.begin(), content.end(), L'\\'), content.end()); // 删除反斜杠
    std::wstring cmd = L"edge-playback --text \"" + content + L"\" --voice " + voice; // 构造命令

    // std::cout << cmd.size() << std::endl;
    // std::wcout << cmd << std::endl; // 输出命令

    CommandRunner *runner = new CommandRunner(cmd, this);
    runner->start();
}


void Dialog::on_radioButtonXiaoxiao_clicked(bool checked)
{
    if (checked) {
        voice = L"zh-CN-XiaoxiaoNeural";
    }
}


void Dialog::on_radioButtonXiaoyi_clicked(bool checked)
{
    if (checked) {
        voice = L"zh-CN-XiaoyiNeural";
    }
}


void Dialog::on_radioButtonYunjian_clicked(bool checked)
{
    if (checked) {
        voice = L"zh-CN-YunjianNeural";
    }
}


void Dialog::on_radioButtonYunxi_clicked(bool checked)
{
    if (checked) {
        voice = L"zh-CN-YunxiNeural";
    }
}


void Dialog::on_radioButtonYunxia_clicked(bool checked)
{
    if (checked) {
        voice = L"zh-CN-YunxiaNeural";
    }
}


void Dialog::on_radioButtonYunyang_clicked(bool checked)
{
    if (checked) {
        voice = L"zh-CN-YunyangNeural";
    }
}

