#include <QApplication>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QImage>
#include <QOpenGLFunctions>
#include <QOpenGLWidget>
#include <QThread>

#include <cstdio>

class XminOpenGLWidget final : public QOpenGLWidget,
                               protected QOpenGLFunctions
{
public:
    using QOpenGLWidget::QOpenGLWidget;

    bool initialized() const { return m_initialized; }

protected:
    void initializeGL() override
    {
        initializeOpenGLFunctions();
        m_initialized = true;
    }

    void paintGL() override
    {
        glViewport(0, 0, width(), height());
        glClearColor(0.0F, 0.0F, 1.0F, 1.0F);
        glClear(GL_COLOR_BUFFER_BIT);
    }

private:
    bool m_initialized = false;
};

int
main(int argc, char **argv)
{
    QApplication application(argc, argv);
    XminOpenGLWidget widget;
    QElapsedTimer timer;

    if (QGuiApplication::platformName() != QStringLiteral("xcb"))
        return 2;
    widget.resize(64, 64);
    widget.show();

    timer.start();
    while (timer.elapsed() < 5000 &&
           (!widget.isVisible() || !widget.initialized())) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(1);
    }
    if (!widget.initialized()) {
        std::fprintf(stderr, "QOpenGLWidget did not initialize on Xmin\n");
        return 3;
    }

    widget.update();
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    const QImage image = widget.grabFramebuffer().convertToFormat(
        QImage::Format_RGB32);
    if (image.isNull())
        return 4;
    const QRgb pixel = image.pixel(image.width() / 2, image.height() / 2);
    if (qBlue(pixel) < 200 || qRed(pixel) > 40 || qGreen(pixel) > 40)
        return 5;
    return 0;
}
