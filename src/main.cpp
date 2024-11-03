#include "dialog.h"

#include <QApplication>
#include <QClipboard>
#include <QMimeData>
#include <QImage>
#include <QTemporaryFile>
#include <QProcess>
#include <QDir>
#include <thread>
#include <chrono>
#include <windows.h>

void sleepms(uint64_t ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

static HHOOK g_hook;

// 模拟 Ctrl+C 组合键按下
void simulateCtrlC() {
    keybd_event(VK_CONTROL, 0, 0, 0);   // 按下 Ctrl 键
    keybd_event('C', 0, 0, 0);          // 按下 C 键
    keybd_event('C', 0, KEYEVENTF_KEYUP, 0);   // 释放 C 键
    keybd_event(VK_CONTROL, 0, KEYEVENTF_KEYUP, 0);   // 释放 Ctrl 键
}

QString performOCR(const QImage &image) {
    // 将剪切板中的图片保存到临时文件
    QTemporaryFile tempFile;
    tempFile.setFileTemplate("temp_image_XXXXXX.png");
    tempFile.open();
    QString tempFilePath = tempFile.fileName();
    image.save(tempFilePath);

    // 创建 QProcess 对象
    QProcess process;

    // 设置程序路径
    QString program = "./OcrLiteOnnx-1.6.1/win-BIN-x64/OcrLiteOnnx.exe";

    // 设置参数
    QStringList arguments;
    arguments << "--models" << "./OcrLiteOnnx-1.6.1/models"
              << "--det" << "dbnet.onnx"
              << "--cls" << "angle_net.onnx"
              << "--rec" << "crnn_lite_lstm.onnx"
              << "--keys" << "keys.txt"
              << "--image" << tempFilePath;

    // 启动程序
    process.start(program, arguments);
    process.waitForFinished();

    // 获取输出结果
    QString result = process.readAllStandardOutput();

    // 使用正则表达式或字符串操作提取“FullDetectTime”后面的内容
    int startIndex = result.lastIndexOf("FullDetectTime");
    if (startIndex != -1) {
        // 找到“FullDetectTime”之后的换行符的位置
        int nextLineIndex = result.indexOf('\n', startIndex);
        if (nextLineIndex != -1) {
            // 截取从下一行开始的内容
            result = result.mid(nextLineIndex + 1);
        }
    }

    return result;
}

void deleteResultFiles() {
    QDir dir;

    // 查找并删除所有结果图片
    QStringList imageFiles = dir.entryList(QStringList() << "*-result.jpg", QDir::Files);
    for (const QString &file : imageFiles) {
        if (dir.remove(file)) {
            // qDebug() << "Deleted" << file;
        } else {
            // qDebug() << "Failed to delete" << file;
        }
    }

    // 查找并删除所有结果文本文件
    QStringList textFiles = dir.entryList(QStringList() << "*-result.txt", QDir::Files);
    for (const QString &file : textFiles) {
        if (dir.remove(file)) {
            // qDebug() << "Deleted" << file;
        } else {
            // qDebug() << "Failed to delete" << file;
        }
    }
}

// 全局键盘钩子的回调函数
LRESULT CALLBACK KeyboardProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode >= 0 && wParam == WM_KEYDOWN) {
        KBDLLHOOKSTRUCT *pKeyBoard = (KBDLLHOOKSTRUCT *)lParam;
        int key = pKeyBoard->vkCode;

        // 检测到 F9 键
        if (key == VK_F9) {
            Dialog::getInstance().setManuallyStopped(false);
            // 检测剪切板内容是否为图片
            const QMimeData *mimeData = QApplication::clipboard()->mimeData();
            if (mimeData->hasImage()) {
                QImage image = qvariant_cast<QImage>(mimeData->imageData());
                QString ocrResult = performOCR(image);
                Dialog::getInstance().playText(ocrResult);
                deleteResultFiles();
            } else {
                // 模拟 Ctrl+C 组合键按下
                simulateCtrlC();
                sleepms(100);
                QString copiedText = QApplication::clipboard()->text(); // 获取剪贴板文本
                Dialog::getInstance().playText(copiedText);
            }
        }
    }
    return CallNextHookEx(g_hook, nCode, wParam, lParam);
}

int main(int argc, char *argv[])
{
    QApplication a(argc, argv);
    Dialog::getInstance().show();

    SetWindowsHookEx(WH_KEYBOARD_LL, KeyboardProc, NULL, 0);
    int ret = a.exec();
    UnhookWindowsHookEx(g_hook);

    return ret;
}
