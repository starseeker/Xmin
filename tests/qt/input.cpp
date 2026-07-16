#include <QCloseEvent>
#include <QCoreApplication>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QWindow>

#include <cmath>

class InputWindow : public QWindow
{
public:
    bool gotClick = false;
    bool gotDrag = false;
    bool gotKey = false;

protected:
    void mousePressEvent(QMouseEvent *event) override
    {
        const QPointF position = event->position();
        if (event->button() == Qt::LeftButton &&
            event->modifiers().testFlag(Qt::ControlModifier) &&
            std::abs(position.x() - 20.0) <= 1.0 &&
            std::abs(position.y() - 20.0) <= 1.0)
            gotClick = true;
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        const QPointF position = event->position();
        if (event->buttons().testFlag(Qt::LeftButton) &&
            event->modifiers().testFlag(Qt::ControlModifier) &&
            std::abs(position.x() - 30.0) <= 1.0 &&
            std::abs(position.y() - 30.0) <= 1.0)
            gotDrag = true;
    }

    void keyPressEvent(QKeyEvent *event) override
    {
        if (event->key() == Qt::Key_A && event->text() == QStringLiteral("a"))
            gotKey = true;
    }

    void closeEvent(QCloseEvent *event) override
    {
        event->accept();
        QCoreApplication::exit(gotClick && gotDrag && gotKey ? 0 : 1);
    }
};

int
main(int argc, char **argv)
{
    QGuiApplication application(argc, argv);
    InputWindow window;
    window.setTitle(QStringLiteral("xmin-qt-input-target"));
    window.resize(80, 60);
    window.show();
    return application.exec();
}
