#ifndef FLOATINGPLAYBUTTON_H
#define FLOATINGPLAYBUTTON_H

#include <QToolButton>

class QEnterEvent;
class QPaintEvent;

class FloatingPlayButton : public QToolButton
{
    Q_OBJECT

public:
    explicit FloatingPlayButton(QWidget *parent = nullptr);

    void setPayloadText(const QString &text);
    QString payloadText() const;

    void setPlaying(bool playing);
    bool isPlaying() const;

    void showNear(const QPoint &globalAnchorPos);

private:
    QString m_payloadText;
    bool m_isPlaying = false;

    void applyVisualStyle();

protected:
    void paintEvent(QPaintEvent *event) override;
    void enterEvent(QEnterEvent *event) override;
    void leaveEvent(QEvent *event) override;

#ifdef Q_OS_WIN
    bool nativeEvent(const QByteArray &eventType, void *message, qintptr *result) override;
#endif
};

#endif // FLOATINGPLAYBUTTON_H
