#include "selectionoverlaycontroller.h"

#include "dialog.h"
#include "floatingplaybutton.h"

#include <QToolButton>
#include <QStringList>

#ifdef Q_OS_WIN
    #include <windows.h>
    #include <objbase.h>
    #include <UIAutomation.h>
#endif

#ifdef Q_OS_WIN
template <typename T>
class ComPtr
{
public:
    ComPtr() = default;
    ComPtr(const ComPtr &) = delete;
    ComPtr &operator=(const ComPtr &) = delete;

    ComPtr(ComPtr &&other) noexcept
        : m_ptr(other.m_ptr)
    {
        other.m_ptr = nullptr;
    }

    ComPtr &operator=(ComPtr &&other) noexcept
    {
        if (this != &other) {
            reset();
            m_ptr = other.m_ptr;
            other.m_ptr = nullptr;
        }
        return *this;
    }

    ~ComPtr() { reset(); }

    T *get() const { return m_ptr; }
    T **put()
    {
        reset();
        return &m_ptr;
    }

    explicit operator bool() const { return m_ptr != nullptr; }
    T *operator->() const { return m_ptr; }

    void reset(T *ptr = nullptr)
    {
        if (m_ptr) {
            m_ptr->Release();
        }
        m_ptr = ptr;
    }

private:
    T *m_ptr = nullptr;
};

static QString tryGetSelectionTextFromElement(IUIAutomation *automation, IUIAutomationElement *start)
{
    if (!automation || !start) {
        return {};
    }

    ComPtr<IUIAutomationTreeWalker> walker;
    if (FAILED(automation->get_ControlViewWalker(walker.put())) || !walker) {
        return {};
    }

    ComPtr<IUIAutomationElement> current;
    start->AddRef();
    current.reset(start);

    for (int depth = 0; depth < 10 && current; ++depth) {
        ComPtr<IUIAutomationTextPattern> textPattern;
        const HRESULT patternHr = current->GetCurrentPatternAs(
            UIA_TextPatternId,
            __uuidof(IUIAutomationTextPattern),
            reinterpret_cast<void **>(textPattern.put())
        );

        if (SUCCEEDED(patternHr) && textPattern) {
            ComPtr<IUIAutomationTextRangeArray> selectionRanges;
            if (SUCCEEDED(textPattern->GetSelection(selectionRanges.put())) && selectionRanges) {
                int rangeCount = 0;
                if (SUCCEEDED(selectionRanges->get_Length(&rangeCount)) && rangeCount > 0) {
                    QStringList parts;
                    parts.reserve(rangeCount);
                    for (int i = 0; i < rangeCount; ++i) {
                        ComPtr<IUIAutomationTextRange> range;
                        if (FAILED(selectionRanges->GetElement(i, range.put())) || !range) {
                            continue;
                        }

                        BSTR bstr = nullptr;
                        const HRESULT textHr = range->GetText(-1, &bstr);
                        if (SUCCEEDED(textHr) && bstr) {
                            parts.append(QString::fromWCharArray(bstr));
                        }
                        if (bstr) {
                            SysFreeString(bstr);
                        }
                    }

                    const QString text = parts.join(QStringLiteral("\n"));
                    if (!text.trimmed().isEmpty()) {
                        return text;
                    }
                }
            }
        }

        ComPtr<IUIAutomationElement> parent;
        if (FAILED(walker->GetParentElement(current.get(), parent.put())) || !parent) {
            break;
        }
        current = std::move(parent);
    }

    return {};
}
#endif // Q_OS_WIN

SelectionOverlayController::SelectionOverlayController(QObject *parent)
    : QObject(parent)
    , m_button(new FloatingPlayButton())
{
    m_playbackActive = Dialog::getInstance().isPlaybackActive();
    m_button->setPlaying(m_playbackActive);

    connect(&Dialog::getInstance(), &Dialog::playbackActiveChanged, this, [this](bool active) {
        m_playbackActive = active;
        if (m_button) {
            m_button->setPlaying(active);
            if (!active && m_button->payloadText().trimmed().isEmpty()) {
                hideOverlay();
            }
        }
    });

    connect(m_button, &QToolButton::clicked, this, [this]() {
        if (m_playbackActive) {
            Dialog::getInstance().stopPlayback();
            return;
        }

        const QString text = m_button ? m_button->payloadText() : QString();
        if (text.trimmed().isEmpty()) {
            hideOverlay();
            return;
        }

        Dialog::getInstance().setManuallyStopped(false);
        Dialog::getInstance().playText(text);
    });

    m_debounceTimer.setSingleShot(true);
    connect(&m_debounceTimer, &QTimer::timeout, this, &SelectionOverlayController::performSelectionCheck);

#ifdef Q_OS_WIN
    const HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    m_comInitialized = SUCCEEDED(hr);
    if (hr == RPC_E_CHANGED_MODE) {
        m_comInitialized = false;
    }

    const HRESULT uiaHr = CoCreateInstance(
        __uuidof(CUIAutomation),
        nullptr,
        CLSCTX_INPROC_SERVER,
        __uuidof(IUIAutomation),
        reinterpret_cast<void **>(&m_uiAutomation)
    );
    if (FAILED(uiaHr)) {
        m_uiAutomation = nullptr;
    }
#endif
}

SelectionOverlayController::~SelectionOverlayController()
{
    if (m_button) {
        delete m_button;
        m_button = nullptr;
    }

#ifdef Q_OS_WIN
    if (m_uiAutomation) {
        m_uiAutomation->Release();
        m_uiAutomation = nullptr;
    }

    if (m_comInitialized) {
        CoUninitialize();
    }
#endif
}

void SelectionOverlayController::handleGlobalMouseDown(const QPoint &globalPos)
{
    m_debounceTimer.stop();
    if (!m_button || !m_button->isVisible()) {
        return;
    }
    if (m_button->geometry().contains(globalPos)) {
        return;
    }
    if (m_playbackActive) {
        return;
    }
    hideOverlay();
}

void SelectionOverlayController::handleGlobalMouseUp(const QPoint &globalPos)
{
    if (m_button && m_button->geometry().contains(globalPos)) {
        return;
    }
    scheduleSelectionCheck(globalPos);
}

void SelectionOverlayController::hideOverlay()
{
    m_debounceTimer.stop();
    if (!m_button) {
        return;
    }
    m_button->hide();
    m_button->setPayloadText(QString());
}

void SelectionOverlayController::scheduleSelectionCheck(const QPoint &anchorPos)
{
    m_pendingAnchorPos = anchorPos;
    m_debounceTimer.start(80);
}

void SelectionOverlayController::performSelectionCheck()
{
#ifdef Q_OS_WIN
    const HWND foreground = GetForegroundWindow();
    const HWND self = reinterpret_cast<HWND>(Dialog::getInstance().winId());
    if (foreground && self && foreground == self) {
        hideOverlay();
        return;
    }
#endif

    const QString selectedText = selectedTextViaUiAutomation();
    if (selectedText.trimmed().isEmpty()) {
        if (m_playbackActive) {
            if (m_button) {
                m_button->setPayloadText(QString());
            }
            return;
        }
        hideOverlay();
        return;
    }

    if (!m_button) {
        return;
    }
    m_button->setPayloadText(selectedText);
    m_button->setPlaying(m_playbackActive);
    m_button->showNear(m_pendingAnchorPos);
}

QString SelectionOverlayController::selectedTextViaUiAutomation() const
{
#ifdef Q_OS_WIN
    if (!m_uiAutomation) {
        return {};
    }

    ComPtr<IUIAutomationElement> focused;
    if (SUCCEEDED(m_uiAutomation->GetFocusedElement(focused.put())) && focused) {
        const QString focusedText = tryGetSelectionTextFromElement(m_uiAutomation, focused.get());
        if (!focusedText.trimmed().isEmpty()) {
            return focusedText;
        }
    }

    POINT pt {};
    if (GetCursorPos(&pt)) {
        ComPtr<IUIAutomationElement> atPoint;
        if (SUCCEEDED(m_uiAutomation->ElementFromPoint(pt, atPoint.put())) && atPoint) {
            const QString pointText = tryGetSelectionTextFromElement(m_uiAutomation, atPoint.get());
            if (!pointText.trimmed().isEmpty()) {
                return pointText;
            }
        }
    }
#endif

    return {};
}
