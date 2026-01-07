#include "floatingplaybutton.h"

#include <QEnterEvent>
#include <QGuiApplication>
#include <QCursor>
#include <QPainter>
#include <QPainterPath>
#include <QScreen>
#include <QtGlobal>

#ifdef Q_OS_WIN
    #include <windows.h>
#endif

FloatingPlayButton::FloatingPlayButton(QWidget *parent)
    : QToolButton(parent)
{
    setWindowFlags(Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::WindowDoesNotAcceptFocus);
    setAttribute(Qt::WA_ShowWithoutActivating);
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_NoSystemBackground);
    setFocusPolicy(Qt::NoFocus);
    setAutoRaise(true);
    setCursor(Qt::PointingHandCursor);
    setMouseTracking(true);
    setAutoFillBackground(false);

    setFixedSize(QSize(38, 38));

    applyVisualStyle();

    hide();
}

void FloatingPlayButton::setPayloadText(const QString &text)
{
    m_payloadText = text;
}

QString FloatingPlayButton::payloadText() const
{
    return m_payloadText;
}

void FloatingPlayButton::setPlaying(bool playing)
{
    if (m_isPlaying == playing) {
        return;
    }
    m_isPlaying = playing;
    applyVisualStyle();
}

bool FloatingPlayButton::isPlaying() const
{
    return m_isPlaying;
}

void FloatingPlayButton::applyVisualStyle()
{
    if (m_isPlaying) {
        setToolTip(QStringLiteral("停止朗读"));
    } else {
        setToolTip(QStringLiteral("播放朗读选中文本"));
    }
    update();
}

void FloatingPlayButton::paintEvent(QPaintEvent *event)
{
    (void)event;

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::TextAntialiasing, true);

    const QRectF circleRect(0.5, 0.5, width() - 1.0, height() - 1.0);

    QColor top;
    QColor bottom;
    if (m_isPlaying) {
        top = QColor("#f87171");
        bottom = QColor("#dc2626");
    } else {
        top = QColor("#34d399");
        bottom = QColor("#16a34a");
    }

    if (isDown()) {
        top = top.darker(120);
        bottom = bottom.darker(135);
    } else if (underMouse()) {
        top = top.lighter(108);
        bottom = bottom.lighter(108);
    }

    QLinearGradient gradient(circleRect.topLeft(), circleRect.bottomLeft());
    gradient.setColorAt(0.0, top);
    gradient.setColorAt(1.0, bottom);

    QPainterPath circlePath;
    circlePath.addEllipse(circleRect);
    painter.fillPath(circlePath, gradient);

    QPen borderPen(QColor(255, 255, 255, underMouse() ? 64 : 48));
    borderPen.setWidthF(1.0);
    painter.setPen(borderPen);
    painter.setBrush(Qt::NoBrush);
    painter.drawEllipse(circleRect);

    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(255, 255, 255, 240));

    const QPointF center(width() / 2.0, height() / 2.0);
    const qreal size = qMin(width(), height());

    if (m_isPlaying) {
        const qreal side = size * 0.34;
        const QRectF stopRect(center.x() - side / 2.0, center.y() - side / 2.0, side, side);
        painter.drawRoundedRect(stopRect, 2.2, 2.2);
        return;
    }

    QPainterPath playPath;
    const qreal triW = size * 0.34;
    const qreal triH = size * 0.38;
    const QPointF p1(center.x() - triW * 0.35, center.y() - triH / 2.0);
    const QPointF p2(center.x() - triW * 0.35, center.y() + triH / 2.0);
    const QPointF p3(center.x() + triW * 0.65, center.y());
    playPath.moveTo(p1);
    playPath.lineTo(p2);
    playPath.lineTo(p3);
    playPath.closeSubpath();
    painter.drawPath(playPath);
}

void FloatingPlayButton::enterEvent(QEnterEvent *event)
{
    QToolButton::enterEvent(event);
    update();
}

void FloatingPlayButton::leaveEvent(QEvent *event)
{
    QToolButton::leaveEvent(event);
    update();
}

#ifdef Q_OS_WIN
bool FloatingPlayButton::nativeEvent(const QByteArray &eventType, void *message, qintptr *result)
{
    const bool isWindowsMsg = (eventType == "windows_generic_MSG" || eventType == "windows_dispatcher_MSG");
    if (isWindowsMsg && message && result) {
        const MSG *msg = static_cast<const MSG *>(message);
        if (msg->message == WM_NCHITTEST) {
            const QPoint globalPos = QCursor::pos();
            const QPoint localPos = mapFromGlobal(globalPos);
            if (!rect().contains(localPos)) {
                *result = HTTRANSPARENT;
                return true;
            }

            const QPointF center(width() / 2.0, height() / 2.0);
            const QPointF pt(localPos.x() + 0.5, localPos.y() + 0.5);
            const qreal dx = pt.x() - center.x();
            const qreal dy = pt.y() - center.y();
            const qreal radius = qMin(width(), height()) / 2.0;

            if ((dx * dx + dy * dy) > (radius * radius)) {
                *result = HTTRANSPARENT;
                return true;
            }

            *result = HTCLIENT;
            return true;
        }
    }

    return QToolButton::nativeEvent(eventType, message, result);
}
#endif

void FloatingPlayButton::showNear(const QPoint &globalAnchorPos)
{
    const QPoint offset(10, 14);
    QPoint pos = globalAnchorPos + offset;

    const QScreen *screen = QGuiApplication::screenAt(globalAnchorPos);
    const QRect bounds = screen ? screen->availableGeometry() : QRect();
    if (!bounds.isNull()) {
        const int margin = 4;
        pos.setX(qBound(bounds.left() + margin, pos.x(), bounds.right() - width() - margin));
        pos.setY(qBound(bounds.top() + margin, pos.y(), bounds.bottom() - height() - margin));
    }

    move(pos);
    show();
    raise();
}
