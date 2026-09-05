#ifndef DIALOG_H
#define DIALOG_H

#include <QDialog>
#include <QFile>
#include <QKeyEvent>
#include <QMap>
#include <QTextStream>

#include "communicate.h"
#include "streamingprogressbar.h"

QT_BEGIN_NAMESPACE
namespace Ui {
class Dialog;
}
QT_END_NAMESPACE

class QPropertyAnimation;

class Dialog : public QDialog
{
    Q_OBJECT

public:
    static Dialog &getInstance()
    {
        static Dialog instance {};
        return instance;
    }

    ~Dialog();

    void playText(const QString &text);
    void stopPlayback();
    bool isPlaybackActive() const;
    void setManuallyStopped(bool manuallyStopped);

signals:
    void send();
    void stop();
    void playbackActiveChanged(bool active);

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
    void onLanguageChanged(const QString &language);
    void onVoiceNameChanged(const QString &voiceName);
    void onPlayFinished();

protected:
    bool eventFilter(QObject *obj, QEvent *event) override;
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dropEvent(QDropEvent *event) override;

private:
    explicit Dialog(QWidget *parent = nullptr);

    void checkDuplicate(const QString &text, const QString &voice);
    void setCommunicate(const QString &text, const QString &voice, const QString &fileName);
    void loadVoiceData();
    void setPlaybackActive(bool active);
    void updateTextCount();
    void startAutoRetryAttempt();
    void handleAutoRetryFinished();
    void scheduleNoPlaybackWatchdog(int attemptSerial, qsizetype lastEdgeBytesReceived);

private:
    Ui::Dialog *ui;
    Communicate m_comm;
    QString m_lastText;
    QString m_lastVoice;
    QString voice;
    QString lastDir;

    bool manuallyStopped = true;
    QString m_autoRetryText;
    int m_autoRetriesRemaining = 0;
    bool m_autoRetryEnabled = false;
    int m_autoAttemptSerial = 0;
    int m_lastFinishedAttemptSerial = -1;
    bool m_playbackActive = false;
    bool m_savingAudio = false;
    QPropertyAnimation *m_saveProgressAnimation = nullptr;

    QMap<QString, QMap<QString, QString>> data;
};

#endif // DIALOG_H
