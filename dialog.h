#ifndef DIALOG_H
#define DIALOG_H

#include <QDialog>
#include <QThread>
#include <QProcess>
#include <string>

QT_BEGIN_NAMESPACE
namespace Ui {
class Dialog;
}
QT_END_NAMESPACE

class Dialog : public QDialog
{
    Q_OBJECT

public:
    Dialog(QWidget *parent = nullptr);
    ~Dialog();

private slots:
    void on_pushButtonPlay_clicked();

    void on_radioButtonXiaoxiao_clicked(bool checked);

    void on_radioButtonXiaoyi_clicked(bool checked);

    void on_radioButtonYunjian_clicked(bool checked);

    void on_radioButtonYunxi_clicked(bool checked);

    void on_radioButtonYunxia_clicked(bool checked);

    void on_radioButtonYunyang_clicked(bool checked);

private:
    Ui::Dialog *ui;
    std::wstring voice = L"zh-CN-XiaoxiaoNeural";
}; // class Dialog

class CommandRunner : public QThread
{
    Q_OBJECT

public:
    CommandRunner(std::wstring command, QObject *parent = nullptr)
        : QThread(parent), cmd(std::move(command)) {}

protected:
    void run() override {
        _wsystem(cmd.c_str());
    }

private:
    std::wstring cmd;
}; // class CommandRunner

#endif // DIALOG_H
