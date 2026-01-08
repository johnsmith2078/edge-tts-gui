#ifndef DIALOG_H
#define DIALOG_H

#include <QDialog>
#include <QKeyEvent>
#include <QMap>
#include "communicate.h"
#include "tts.h"
#include "dashscope_tts.h"

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

    void playbackActiveChanged(bool active);

private:
    void checkDuplicate(const QString& text, const QString& voice);

    void setCommunicate(const QString& text, const QString& voice, const QString& fileName);

    bool isUseGPTSoVITS();

    bool isUseQwenTTS();

    enum class TTSEngine {
        Edge,
        GPTSoVITS,
        Qwen,
    };

    TTSEngine selectedEngine();

public:
    void playText(const QString& text);

    void stopPlayback();

    bool isPlaybackActive() const;

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

    void on_pushButtonSelectRefAudio_clicked();

private:
    Ui::Dialog *ui;
    Communicate m_comm;
    TextToSpeech m_tts;
    DashScopeTTS m_qwen;
    QString m_lastText;
    QString m_lastVoice;
    QString voice;
    QString lastDir = "";

    bool manuallyStopped = true;
    QString m_autoRetryText;
    int m_autoRetriesRemaining = 0;
    bool m_autoRetryEnabled = false;
    TTSEngine m_autoAttemptEngine = TTSEngine::Edge;
    int m_autoAttemptSerial = 0;
    int m_lastFinishedAttemptSerial = -1;
    bool m_playbackActive = false;

public:
    void setManuallyStopped(bool manuallyStopped);

private:
    void loadVoiceData();

    void setPlaybackActive(bool active);

    QMap<QString, QMap<QString, QString>> data; // 存储语言、语音名称和代码

private slots:
    void onLanguageChanged(const QString &language);

    void onVoiceNameChanged(const QString &voiceName);

    void onPlayFinished();

protected:
    bool eventFilter(QObject *obj, QEvent *event) override;

    void dragEnterEvent(QDragEnterEvent *event) override;

    void dropEvent(QDropEvent *e) override;

private:
    void startAutoRetryAttempt();

    void handleAutoRetryFinished(TTSEngine engine);

    void scheduleNoPlaybackWatchdog(int attemptSerial, TTSEngine engine, qsizetype lastEdgeBytesReceived);
}; // class Dialog

#endif // DIALOG_H
