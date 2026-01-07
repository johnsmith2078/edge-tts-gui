#ifndef SELECTIONOVERLAYCONTROLLER_H
#define SELECTIONOVERLAYCONTROLLER_H

#include <QObject>
#include <QPoint>
#include <QTimer>

struct IUIAutomation;

class FloatingPlayButton;

class SelectionOverlayController : public QObject
{
    Q_OBJECT

public:
    explicit SelectionOverlayController(QObject *parent = nullptr);
    ~SelectionOverlayController() override;

    void handleGlobalMouseDown(const QPoint &globalPos);
    void handleGlobalMouseUp(const QPoint &globalPos);
    void hideOverlay();

private:
    void scheduleSelectionCheck(const QPoint &anchorPos);
    void performSelectionCheck();
    QString selectedTextViaUiAutomation() const;

private:
    FloatingPlayButton *m_button = nullptr;
    QPoint m_pendingAnchorPos;
    QTimer m_debounceTimer;
    bool m_playbackActive = false;

#ifdef Q_OS_WIN
    IUIAutomation *m_uiAutomation = nullptr;
    bool m_comInitialized = false;
#endif
};

#endif // SELECTIONOVERLAYCONTROLLER_H
