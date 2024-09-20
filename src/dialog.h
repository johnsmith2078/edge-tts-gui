#ifndef DIALOG_H
#define DIALOG_H

#include <QDialog>
#include <QKeyEvent>
#include <QMap>
#include "communicate.h"

QT_BEGIN_NAMESPACE
namespace Ui {
class Dialog;
}
QT_END_NAMESPACE

class Dialog : public QDialog
{
    Q_OBJECT

public:
    static Dialog& getInstance() {
        static Dialog instance {};
        return instance;
    }


private:
    Dialog(QWidget *parent = nullptr);
public:
    ~Dialog();

signals:
    void send();

    void stop();

private:
    void checkDuplicate(const QString& text, const QString& voice);

    void setCommunicate(const QString& text, const QString& voice, const QString& fileName);

public:
    void playText(const QString& text);

private slots:
    void on_pushButtonPlay_clicked();

    void on_radioButtonXiaoxiao_clicked(bool checked);

    void on_radioButtonXiaoyi_clicked(bool checked);

    void on_radioButtonYunjian_clicked(bool checked);

    void on_radioButtonYunxi_clicked(bool checked);

    void on_radioButtonYunxia_clicked(bool checked);

    void on_radioButtonYunyang_clicked(bool checked);

    void on_pushButtonStop_clicked();

    void on_pushButtonSave_clicked();

private:
    Ui::Dialog *ui;
    Communicate m_comm;
    QString m_lastText;
    QString m_lastVoice;
    QString voice;
    QString lastDir = "";

private:
    void loadVoiceData();

    QMap<QString, QMap<QString, QString>> data; // 存储语言、语音名称和代码

private slots:
    void onLanguageChanged(const QString &language);

    void onVoiceNameChanged(const QString &voiceName);


protected:
    bool eventFilter(QObject *obj, QEvent *event) override;

    void dragEnterEvent(QDragEnterEvent *event) override;

    void dropEvent(QDropEvent *e) override;
}; // class Dialog

#endif // DIALOG_H
