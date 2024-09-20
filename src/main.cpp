#include "dialog.h"

#include <QApplication>
#include <QClipboard>
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

// 全局键盘钩子的回调函数
LRESULT CALLBACK KeyboardProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode >= 0 && wParam == WM_KEYDOWN) {
        KBDLLHOOKSTRUCT *pKeyBoard = (KBDLLHOOKSTRUCT *)lParam;
        int key = pKeyBoard->vkCode;

        // 检测到 F9 键
        if (key == VK_F9) {
            // 模拟 Ctrl+C 组合键按下
            simulateCtrlC();
            sleepms(100);
            QString copiedText = QApplication::clipboard()->text(); // 获取剪贴板文本
            sleepms(100);
            Dialog::getInstance().playText(copiedText);
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
