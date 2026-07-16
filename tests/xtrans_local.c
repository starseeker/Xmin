#include <dix-config.h>

#define TRANS_SERVER
#define TRANS_REOPEN
#define XSERV_t
#include <X11/Xtrans/Xtrans.h>

#include <stdarg.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

void
VErrorF(const char *format, va_list args)
{
    (void) format;
    (void) args;
}

void
ErrorF(const char *format, ...)
{
    (void) format;
}

int
main(void)
{
    char path[96];
    struct stat status;
    XtransConnInfo local;
    XtransConnInfo tcp;

#ifndef UNIXCONN
#error "The Xmin server requires the Unix-domain xtrans backend"
#endif
#ifdef TCPCONN
#error "The default Xmin profile must not compile the TCP xtrans backend"
#endif

    if (snprintf(path, sizeof(path), "/tmp/xmin-xtrans-%ld.sock",
                 (long) getpid()) < 0)
        return 1;
    unlink(path);

    local = _XSERVTransOpenCOTSServer(path);
    if (local == NULL)
        return 2;
    if (_XSERVTransCreateListener(local, path, 0) != 0) {
        _XSERVTransClose(local);
        return 3;
    }
    if (!_XSERVTransIsLocal(local)) {
        _XSERVTransClose(local);
        return 4;
    }
    if (_XSERVTransGetConnectionNumber(local) < 0) {
        _XSERVTransClose(local);
        return 5;
    }
    if (stat(path, &status) != 0 || !S_ISSOCK(status.st_mode)) {
        _XSERVTransClose(local);
        return 6;
    }
    if (_XSERVTransClose(local) != 0 || access(path, F_OK) == 0)
        return 7;

    tcp = _XSERVTransOpenCOTSServer("tcp/127.0.0.1:6099");
    if (tcp != NULL) {
        _XSERVTransClose(tcp);
        return 8;
    }
    return 0;
}
