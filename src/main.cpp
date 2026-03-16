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

QString removeLineBreaks(QString text) {
    text.remove('\r');
    text.remove('\n');
    return text;
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
    QString program = "./RapidOCR/win-BIN-CPU-x64/RapidOcrOnnx.exe";

    // 设置参数
    QStringList arguments;
    arguments << "--models" << "./RapidOCR/models"
              << "--det" << "ch_PP-OCRv4_det_infer.onnx"
              << "--cls" << "ch_ppocr_mobile_v2.0_cls_infer.onnx"
              << "--rec" << "ch_PP-OCRv4_rec_infer.onnx"
              << "--keys" << "ppocr_keys_v1.txt"
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

    return removeLineBreaks(result);
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
            QClipboard *clipboard = QApplication::clipboard();

            // 如果剪切板里有图片，先保存一份，后续 Ctrl+C 没复制出文字时再做 OCR 兜底
            const QMimeData *mimeData = clipboard->mimeData();
            const bool hasImage = mimeData->hasImage();
            const QImage clipboardImage = hasImage ? qvariant_cast<QImage>(mimeData->imageData()) : QImage();

            // 先尝试 Ctrl+C 复制选中的文字；如果复制后剪贴板里有文字，则直接朗读
            const QString prevText = clipboard->text();
            const DWORD prevClipboardSeq = GetClipboardSequenceNumber();
            simulateCtrlC();
            for (int i = 0; i < 20; ++i) {
                sleepms(25);
                if (GetClipboardSequenceNumber() != prevClipboardSeq) {
                    break;
                }
            }

            const QString copiedText = clipboard->text();
            const QString textForTts = removeLineBreaks(copiedText).trimmed();
            const bool clipboardChanged = GetClipboardSequenceNumber() != prevClipboardSeq;
            const bool hasCopiedText = !textForTts.isEmpty() && (clipboardChanged || copiedText != prevText);
            if (hasCopiedText) {
                Dialog::getInstance().playText(textForTts);
            } else if (hasImage) {
                QString ocrResult = performOCR(clipboardImage);
                Dialog::getInstance().playText(ocrResult);
                deleteResultFiles();
            } else if (!textForTts.isEmpty()) {
                Dialog::getInstance().playText(textForTts);
            }
        }
    }
    return CallNextHookEx(g_hook, nCode, wParam, lParam);
}

// 全局鼠标钩子的回调函数
int main(int argc, char *argv[])
{
    QApplication a(argc, argv);
    Dialog::getInstance().show();

    g_hook = SetWindowsHookEx(WH_KEYBOARD_LL, KeyboardProc, NULL, 0);
    int ret = a.exec();

    if (g_hook) {
        UnhookWindowsHookEx(g_hook);
        g_hook = nullptr;
    }

    return ret;
}
