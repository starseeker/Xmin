#include <QBackingStore>
#include <QClipboard>
#include <QCoreApplication>
#include <QCursor>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QGuiApplication>
#include <QImage>
#include <QPainter>
#include <QPixmap>
#include <QRegion>
#include <QScreen>
#include <QThread>
#include <QWindow>

#include <cstdio>

static void
processEvents(void)
{
    for (int iteration = 0; iteration < 20; ++iteration)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
}

static bool
waitForExposure(QWindow &window)
{
    QElapsedTimer timer;

    timer.start();
    while (timer.elapsed() < 5000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        if (window.isExposed())
            return true;
        QThread::msleep(1);
    }
    return false;
}

static bool
paintAndCheck(QWindow &window, QBackingStore &store)
{
    const QRect bounds(QPoint(0, 0), window.size());

    store.resize(window.size());
    store.beginPaint(QRegion(bounds));
    {
        QPainter painter(store.paintDevice());
        painter.fillRect(bounds, QColor(0, 255, 0));
    }
    store.endPaint();
    store.flush(QRegion(bounds), &window);
    processEvents();

    QScreen *screen = window.screen();
    if (screen == nullptr)
        return false;
    const QImage image = screen->grabWindow(window.winId(),
                                             window.width() / 2,
                                             window.height() / 2, 1, 1)
                             .toImage()
                             .convertToFormat(QImage::Format_RGB32);
    if (image.isNull())
        return false;
    const QRgb pixel = image.pixel(0, 0);
    return qGreen(pixel) > 200 && qRed(pixel) < 40 && qBlue(pixel) < 40;
}

int
main(int argc, char **argv)
{
    QGuiApplication application(argc, argv);
    QWindow window;
    QBackingStore store(&window);

    if (QGuiApplication::platformName() != QStringLiteral("xcb")) {
        std::fprintf(stderr, "Qt %d did not select its xcb platform plugin\n",
                     XMIN_QT_MAJOR);
        return 2;
    }
    if (window.devicePixelRatio() <= 0.0)
        return 3;

    QClipboard *clipboard = QGuiApplication::clipboard();
    clipboard->setText(QStringLiteral("xmin-qt-clipboard"),
                       QClipboard::Clipboard);
    if (clipboard->text(QClipboard::Clipboard) !=
        QStringLiteral("xmin-qt-clipboard"))
        return 4;

    window.setTitle(QStringLiteral("Xmin Qt raster acceptance"));
    window.setCursor(QCursor(Qt::CrossCursor));
    window.resize(64, 64);
    window.show();
    if (!waitForExposure(window) || !paintAndCheck(window, store))
        return 5;

    window.resize(80, 72);
    processEvents();
    if (window.size() != QSize(80, 72) || !paintAndCheck(window, store))
        return 6;

    return 0;
}
