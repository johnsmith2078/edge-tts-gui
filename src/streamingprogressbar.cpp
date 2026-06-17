#include "streamingprogressbar.h"

#include <QLinearGradient>
#include <QPainter>
#include <QStyle>
#include <QStyleOptionProgressBar>
#include <QVariantAnimation>

StreamingProgressBar::StreamingProgressBar(QWidget *parent)
    : QProgressBar(parent)
{
    m_streamAnim = new QVariantAnimation(this);
    m_streamAnim->setStartValue(0.0);
    m_streamAnim->setEndValue(1.0);
    m_streamAnim->setDuration(1600);
    m_streamAnim->setLoopCount(-1); // infinite
    connect(m_streamAnim, &QVariantAnimation::valueChanged, this, [this](const QVariant &v) {
        m_streamPos = v.toDouble();
        update();
    });
}

void StreamingProgressBar::setStreamPos(double pos)
{
    m_streamPos = pos;
    update();
}

void StreamingProgressBar::showEvent(QShowEvent *event)
{
    QProgressBar::showEvent(event);
    m_streamAnim->start();
}

void StreamingProgressBar::hideEvent(QHideEvent *event)
{
    QProgressBar::hideEvent(event);
    m_streamAnim->stop();
}

void StreamingProgressBar::paintEvent(QPaintEvent *event)
{
    QProgressBar::paintEvent(event);

    if (minimum() >= maximum() || value() <= minimum() || value() >= maximum())
        return;

    QStyleOptionProgressBar option;
    initStyleOption(&option);
    const QRect contents = style()->subElementRect(QStyle::SE_ProgressBarContents, &option, this);

    const qreal ratio = static_cast<qreal>(value() - minimum()) / (maximum() - minimum());
    const int filledW = static_cast<int>(contents.width() * ratio);
    if (filledW <= 0)
        return;

    const int fillLeft = contents.left();
    const int fillRight = fillLeft + filledW;
    const int glowW = qBound(14, filledW / 4, 34);
    const int fillH = qBound(4, contents.height() / 3, 7);
    const int fillY = contents.center().y() - fillH / 2;
    const int drawStart = qMax(fillLeft, fillRight - glowW);
    const int drawEnd = fillRight;

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setClipRect(QRect(fillLeft, fillY, filledW, fillH));

    const qreal pulse = 0.7 + (1.0 - qAbs(m_streamPos * 2.0 - 1.0)) * 0.3;
    const QColor edge(14, 165, 233, 0);
    const QColor mid(14, 165, 233, static_cast<int>(150 * pulse));
    const QColor tip(125, 211, 252, static_cast<int>(255 * pulse));

    QLinearGradient gradient(drawStart, 0, drawEnd, 0);
    gradient.setColorAt(0.0, edge);
    gradient.setColorAt(0.55, mid);
    gradient.setColorAt(1.0, tip);

    painter.setPen(Qt::NoPen);
    painter.setBrush(gradient);
    painter.drawRoundedRect(QRectF(drawStart, fillY, drawEnd - drawStart, fillH), fillH / 2.0, fillH / 2.0);
}
