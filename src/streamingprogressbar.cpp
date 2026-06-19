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

    if (minimum() >= maximum() || value() <= minimum())
        return;

    QStyleOptionProgressBar option;
    initStyleOption(&option);
    const QRect contents = style()->subElementRect(QStyle::SE_ProgressBarContents, &option, this);

    const qreal ratio = qBound(0.0, static_cast<qreal>(value() - minimum()) / (maximum() - minimum()), 1.0);
    const int filledW = static_cast<int>(contents.width() * ratio);
    if (filledW <= 0)
        return;

    const QRect fill(contents.left(), contents.top(), filledW, contents.height());
    const qreal shineW = qBound<qreal>(28.0, contents.width() * 0.18, 90.0);
    const qreal centerX = fill.left() - shineW + (fill.width() + shineW * 2.0) * m_streamPos;

    QLinearGradient shine(centerX - shineW, 0.0, centerX + shineW, 0.0);
    shine.setColorAt(0.00, QColor(255, 255, 255, 0));
    shine.setColorAt(0.42, QColor(255, 255, 255, 18));
    shine.setColorAt(0.50, QColor(255, 255, 255, 145));
    shine.setColorAt(0.58, QColor(255, 255, 255, 18));
    shine.setColorAt(1.00, QColor(255, 255, 255, 0));

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const int shineH = qBound(4, contents.height() / 3, 7);
    const int shineY = contents.center().y() - shineH / 2;
    const QRect shineRect(contents.left(), shineY, contents.width(), shineH);

    painter.setClipRect(QRect(fill.left(), shineY, fill.width(), shineH));
    painter.setPen(Qt::NoPen);
    painter.setBrush(shine);
    painter.drawRoundedRect(shineRect, shineH / 2.0, shineH / 2.0);
}
