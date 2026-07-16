#include <QClipboard>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QGuiApplication>
#include <QThread>
#include <QWindow>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <poll.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

static const char clipboardText[] = "xmin-qt-cross-process-clipboard";

static int
parseFd(const char *name)
{
    const char *value = std::getenv(name);
    char *end = nullptr;
    const long fd = value == nullptr ? -1 : std::strtol(value, &end, 10);
    return value != nullptr && end != value && *end == '\0' && fd >= 0
               ? static_cast<int>(fd)
               : -1;
}

static bool
writeByte(int fd, char value)
{
    while (write(fd, &value, 1) < 0) {
        if (errno != EINTR)
            return false;
    }
    return true;
}

static bool
readByte(int fd, char *value)
{
    ssize_t count;
    do {
        count = read(fd, value, 1);
    } while (count < 0 && errno == EINTR);
    return count == 1;
}

static int
runOwner(int argc, char **argv)
{
    const int readyFd = parseFd("XMIN_QT_READY_FD");
    const int doneFd = parseFd("XMIN_QT_DONE_FD");
    if (readyFd < 0 || doneFd < 0)
        return 10;

    QGuiApplication application(argc, argv);
    QWindow window;
    window.resize(48, 48);
    window.show();
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    QGuiApplication::clipboard()->setText(QString::fromLatin1(clipboardText));
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    if (!writeByte(readyFd, 'R'))
        return 11;

    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < 5000) {
        struct pollfd descriptor = { doneFd, POLLIN, 0 };
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        if (poll(&descriptor, 1, 0) > 0) {
            char status = 0;
            return readByte(doneFd, &status) && status == 'P' ? 0 : 12;
        }
        QThread::msleep(1);
    }
    return 13;
}

static int
runReader(int argc, char **argv)
{
    const int readyFd = parseFd("XMIN_QT_READY_FD");
    const int doneFd = parseFd("XMIN_QT_DONE_FD");
    char ready = 0;
    if (readyFd < 0 || doneFd < 0 || !readByte(readyFd, &ready) || ready != 'R')
        return 20;

    QGuiApplication application(argc, argv);
    QElapsedTimer timer;
    timer.start();
    bool matched = false;
    while (timer.elapsed() < 5000) {
        if (QGuiApplication::clipboard()->text() == QString::fromLatin1(clipboardText)) {
            matched = true;
            break;
        }
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(1);
    }
    if (!writeByte(doneFd, matched ? 'P' : 'F'))
        return 21;
    return matched ? 0 : 22;
}

static int
runParent(const char *executable)
{
    int ready[2];
    int done[2];
    if (pipe(ready) != 0 || pipe(done) != 0)
        return 30;

    const pid_t owner = fork();
    if (owner == 0) {
        char readyFd[32];
        char doneFd[32];
        close(ready[0]);
        close(done[1]);
        std::snprintf(readyFd, sizeof(readyFd), "%d", ready[1]);
        std::snprintf(doneFd, sizeof(doneFd), "%d", done[0]);
        setenv("XMIN_QT_ROLE", "owner", 1);
        setenv("XMIN_QT_READY_FD", readyFd, 1);
        setenv("XMIN_QT_DONE_FD", doneFd, 1);
        execl(executable, executable, static_cast<char *>(nullptr));
        _exit(127);
    }
    if (owner < 0)
        return 31;

    const pid_t reader = fork();
    if (reader == 0) {
        char readyFd[32];
        char doneFd[32];
        close(ready[1]);
        close(done[0]);
        std::snprintf(readyFd, sizeof(readyFd), "%d", ready[0]);
        std::snprintf(doneFd, sizeof(doneFd), "%d", done[1]);
        setenv("XMIN_QT_ROLE", "reader", 1);
        setenv("XMIN_QT_READY_FD", readyFd, 1);
        setenv("XMIN_QT_DONE_FD", doneFd, 1);
        execl(executable, executable, static_cast<char *>(nullptr));
        _exit(127);
    }
    if (reader < 0)
        return 32;

    close(ready[0]);
    close(ready[1]);
    close(done[0]);
    close(done[1]);
    int ownerStatus = 0;
    int readerStatus = 0;
    if (waitpid(reader, &readerStatus, 0) != reader ||
        waitpid(owner, &ownerStatus, 0) != owner)
        return 33;
    return WIFEXITED(ownerStatus) && WEXITSTATUS(ownerStatus) == 0 &&
                   WIFEXITED(readerStatus) && WEXITSTATUS(readerStatus) == 0
               ? 0
               : 34;
}

int
main(int argc, char **argv)
{
    const char *role = std::getenv("XMIN_QT_ROLE");
    if (role == nullptr)
        return runParent(argv[0]);
    if (std::strcmp(role, "owner") == 0)
        return runOwner(argc, argv);
    if (std::strcmp(role, "reader") == 0)
        return runReader(argc, argv);
    return 40;
}
