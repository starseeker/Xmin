#ifndef XMIN_OSMESA_ADAPTER_H
#define XMIN_OSMESA_ADAPTER_H

#include <OSMesa/osmesa.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (GLAPIENTRY *XminOSMesaFlushHook)(void);

void XminOSMesaInstallDispatchHooks(XminOSMesaFlushHook flush_hook);
void XminOSMesaFlush(void);
void XminOSMesaCopyContext(OSMesaContext source, OSMesaContext destination,
                           unsigned long mask);

#ifdef __cplusplus
}
#endif

#endif
