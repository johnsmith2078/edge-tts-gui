#ifndef STREAMINGPROGRESSBAR_H
#define STREAMINGPROGRESSBAR_H

#include <QProgressBar>

class QVariantAnimation;

class StreamingProgressBar : public QProgressBar
{
    Q_OBJECT
    Q_PROPERTY(double streamPos READ streamPos WRITE setStreamPos)

public:
    explicit StreamingProgressBar(QWidget *parent = nullptr);

    double streamPos() const { return m_streamPos; }
    void setStreamPos(double pos);

protected:
    void paintEvent(QPaintEvent *event) override;
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;

private:
    QVariantAnimation *m_streamAnim = nullptr;
    double m_streamPos = 0.0;
};

#endif // STREAMINGPROGRESSBAR_H
