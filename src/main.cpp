#include "dialog.h"

#include <QApplication>
#include <QClipboard>
#include <QCoreApplication>
#include <QDebug>
#include <QImage>
#include <QMetaObject>
#include <QMimeData>
#include <QProcess>
#include <QTemporaryDir>
#include <QTimer>
#include <chrono>
#include <memory>
#include <thread>
#include <windows.h>

static HHOOK g_hook;
static HHOOK g_mouseHook;

namespace {

constexpr wchar_t kSingleInstanceMutexName[] = L"Local\\EdgeTtsGuiSingleInstanceMutex";
constexpr wchar_t kSingleInstanceMappingName[] = L"Local\\EdgeTtsGuiSingleInstanceWindow";
constexpr int kActivationRetryCount = 20;
constexpr int kActivationRetryDelayMs = 50;
constexpr int kClipboardPollIntervalMs = 25;
constexpr int kClipboardPollCount = 20;

using WindowHandleValue = UINT_PTR;

HWND readMainWindowHandle()
{
    HANDLE mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, kSingleInstanceMappingName);
    if (!mapping) {
        return nullptr;
    }

    auto view = static_cast<const WindowHandleValue *>(MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, sizeof(WindowHandleValue)));
    if (!view) {
        CloseHandle(mapping);
        return nullptr;
    }

    HWND hwnd = reinterpret_cast<HWND>(*view);
    UnmapViewOfFile(view);
    CloseHandle(mapping);

    return IsWindow(hwnd) ? hwnd : nullptr;
}

HANDLE registerMainWindowHandle(HWND hwnd)
{
    HANDLE mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(WindowHandleValue),
                                        kSingleInstanceMappingName);
    if (!mapping) {
        return nullptr;
    }

    auto view = static_cast<WindowHandleValue *>(MapViewOfFile(mapping, FILE_MAP_WRITE, 0, 0, sizeof(WindowHandleValue)));
    if (!view) {
        CloseHandle(mapping);
        return nullptr;
    }

    *view = reinterpret_cast<WindowHandleValue>(hwnd);
    FlushViewOfFile(view, sizeof(WindowHandleValue));
    UnmapViewOfFile(view);

    return mapping;
}

void bringWindowToFront(HWND hwnd)
{
    if (!IsWindow(hwnd)) {
        return;
    }

    ShowWindow(hwnd, IsIconic(hwnd) ? SW_RESTORE : SW_SHOW);

    DWORD targetThreadId = GetWindowThreadProcessId(hwnd, nullptr);
    DWORD currentThreadId = GetCurrentThreadId();
    DWORD foregroundThreadId = GetWindowThreadProcessId(GetForegroundWindow(), nullptr);

    const BOOL attachedToTarget = currentThreadId != targetThreadId
                                      ? AttachThreadInput(currentThreadId, targetThreadId, TRUE)
                                      : FALSE;
    const BOOL attachedToForeground = foregroundThreadId != 0
                                          && foregroundThreadId != currentThreadId
                                          && foregroundThreadId != targetThreadId
                                          ? AttachThreadInput(currentThreadId, foregroundThreadId, TRUE)
                                          : FALSE;

    SetWindowPos(hwnd, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    BringWindowToTop(hwnd);
    SetForegroundWindow(hwnd);
    SetFocus(hwnd);

    if (attachedToForeground) {
        AttachThreadInput(currentThreadId, foregroundThreadId, FALSE);
    }
    if (attachedToTarget) {
        AttachThreadInput(currentThreadId, targetThreadId, FALSE);
    }
}

void activateExistingInstance()
{
    for (int i = 0; i < kActivationRetryCount; ++i) {
        HWND hwnd = readMainWindowHandle();
        if (hwnd) {
            bringWindowToFront(hwnd);
            return;
        }
        Sleep(kActivationRetryDelayMs);
    }
}

void simulateCtrlC()
{
    keybd_event(VK_CONTROL, 0, 0, 0);
    keybd_event('C', 0, 0, 0);
    keybd_event('C', 0, KEYEVENTF_KEYUP, 0);
    keybd_event(VK_CONTROL, 0, KEYEVENTF_KEYUP, 0);
}

QString normalizeText(QString text)
{
    text.replace("\r\n", " ");
    text.replace('\r', ' ');
    text.replace('\n', ' ');
    return text.simplified();
}

QString performOCR(const QImage &image)
{
    QTemporaryDir tempDir;
    if (!tempDir.isValid()) {
        qWarning() << "Failed to create OCR temporary directory";
        return {};
    }

    const QString imagePath = tempDir.filePath("input.png");
    if (!image.save(imagePath)) {
        qWarning() << "Failed to save OCR input image";
        return {};
    }

    const QString appDir = QCoreApplication::applicationDirPath();
    const QString rapidOcrRoot = appDir + "/RapidOCR";
    const QString program = rapidOcrRoot + "/win-BIN-CPU-x64/RapidOcrOnnx.exe";
    const QString modelsDir = rapidOcrRoot + "/models";

    QProcess process;
    process.setWorkingDirectory(tempDir.path());
    QStringList arguments;
    arguments << "--models" << modelsDir
              << "--det" << "ch_PP-OCRv4_det_infer.onnx"
              << "--cls" << "ch_ppocr_mobile_v2.0_cls_infer.onnx"
              << "--rec" << "ch_PP-OCRv4_rec_infer.onnx"
              << "--keys" << "ppocr_keys_v1.txt"
              << "--image" << imagePath;

    process.start(program, arguments);
    if (!process.waitForStarted(3000)) {
        qWarning() << "Failed to start RapidOCR:" << process.errorString();
        return {};
    }
    if (!process.waitForFinished(30000)) {
        process.kill();
        process.waitForFinished(1000);
        qWarning() << "RapidOCR timed out";
        return {};
    }
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        qWarning() << "RapidOCR failed:" << process.readAllStandardError();
        return {};
    }

    QString result = QString::fromUtf8(process.readAllStandardOutput());
    const int startIndex = result.lastIndexOf("FullDetectTime");
    if (startIndex != -1) {
        const int nextLineIndex = result.indexOf('\n', startIndex);
        if (nextLineIndex != -1) {
            result = result.mid(nextLineIndex + 1);
        }
    }
    return normalizeText(result);
}

struct ClipboardReadState {
    QString previousText;
    QImage fallbackImage;
    DWORD sequence = 0;
    int pollsRemaining = kClipboardPollCount;
};

void finishSelectionRead(const std::shared_ptr<ClipboardReadState> &state)
{
    QClipboard *clipboard = QApplication::clipboard();
    const QString copiedText = clipboard->text();
    const QString textForTts = normalizeText(copiedText);
    const bool clipboardChanged = GetClipboardSequenceNumber() != state->sequence;
    const bool hasCopiedText = !textForTts.isEmpty() && (clipboardChanged || copiedText != state->previousText);

    if (hasCopiedText) {
        Dialog::getInstance().playText(textForTts);
        return;
    }

    if (!state->fallbackImage.isNull()) {
        const QImage image = state->fallbackImage;
        std::thread([image]() {
            const QString ocrResult = performOCR(image);
            if (ocrResult.isEmpty()) {
                return;
            }
            QMetaObject::invokeMethod(&Dialog::getInstance(), [ocrResult]() {
                Dialog::getInstance().playText(ocrResult);
            }, Qt::QueuedConnection);
        }).detach();
        return;
    }

    if (!textForTts.isEmpty()) {
        Dialog::getInstance().playText(textForTts);
    }
}

void pollClipboard(const std::shared_ptr<ClipboardReadState> &state)
{
    if (GetClipboardSequenceNumber() != state->sequence || state->pollsRemaining-- <= 0) {
        finishSelectionRead(state);
        return;
    }
    QTimer::singleShot(kClipboardPollIntervalMs, [state]() { pollClipboard(state); });
}

void readSelectedText()
{
    Dialog::getInstance().setManuallyStopped(false);
    QClipboard *clipboard = QApplication::clipboard();
    const QMimeData *mimeData = clipboard->mimeData();

    auto state = std::make_shared<ClipboardReadState>();
    state->previousText = clipboard->text();
    state->sequence = GetClipboardSequenceNumber();
    if (mimeData && mimeData->hasImage()) {
        state->fallbackImage = qvariant_cast<QImage>(mimeData->imageData());
    }

    simulateCtrlC();
    QTimer::singleShot(kClipboardPollIntervalMs, [state]() { pollClipboard(state); });
}

void queueReadSelectedText()
{
    QMetaObject::invokeMethod(&Dialog::getInstance(), []() { readSelectedText(); }, Qt::QueuedConnection);
}

} // namespace

LRESULT CALLBACK KeyboardProc(int nCode, WPARAM wParam, LPARAM lParam)
{
    if (nCode >= 0 && wParam == WM_KEYDOWN) {
        auto *keyboard = reinterpret_cast<KBDLLHOOKSTRUCT *>(lParam);
        if (keyboard->vkCode == VK_F9) {
            queueReadSelectedText();
        }
    }
    return CallNextHookEx(g_hook, nCode, wParam, lParam);
}

LRESULT CALLBACK MouseProc(int nCode, WPARAM wParam, LPARAM lParam)
{
    Q_UNUSED(lParam);
    if (nCode >= 0 && wParam == WM_MBUTTONDOWN) {
        queueReadSelectedText();
    }
    return CallNextHookEx(g_mouseHook, nCode, wParam, lParam);
}

int main(int argc, char *argv[])
{
    HANDLE singleInstanceMutex = CreateMutexW(nullptr, TRUE, kSingleInstanceMutexName);
    if (!singleInstanceMutex) {
        return 1;
    }

    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        activateExistingInstance();
        CloseHandle(singleInstanceMutex);
        return 0;
    }

    QApplication app(argc, argv);
    Dialog &dialog = Dialog::getInstance();
    dialog.show();

    HANDLE windowMapping = registerMainWindowHandle(reinterpret_cast<HWND>(dialog.winId()));

    g_hook = SetWindowsHookEx(WH_KEYBOARD_LL, KeyboardProc, nullptr, 0);
    g_mouseHook = SetWindowsHookEx(WH_MOUSE_LL, MouseProc, nullptr, 0);
    const int ret = app.exec();

    if (g_hook) {
        UnhookWindowsHookEx(g_hook);
        g_hook = nullptr;
    }
    if (g_mouseHook) {
        UnhookWindowsHookEx(g_mouseHook);
        g_mouseHook = nullptr;
    }
    if (windowMapping) {
        CloseHandle(windowMapping);
    }
    ReleaseMutex(singleInstanceMutex);
    CloseHandle(singleInstanceMutex);

    return ret;
}
