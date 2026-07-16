#include <dix-config.h>

#include "xfixesint.h"

#include <stdlib.h>
#include <string.h>

char dispatchExceptionAtReset;
static unsigned int registered_private_size;

int (*ProcXFixesVector[XFixesNumberRequests]) (ClientPtr);

Bool
dixRegisterPrivateKey(DevPrivateKey key, DevPrivateType type, unsigned int size)
{
    key->offset = 0;
    key->size = (int) size;
    key->initialized = TRUE;
    key->allocated = TRUE;
    key->type = type;
    key->next = NULL;
    registered_private_size = size;
    return TRUE;
}

int
WriteToClient(ClientPtr client, int count, const void *buffer)
{
    (void) client;
    (void) buffer;
    return count;
}

int
main(void)
{
    xXFixesSetClientDisconnectModeReq request;
    ClientRec client;

    memset(&client, 0, sizeof(client));
    memset(&request, 0, sizeof(request));
    if (!XFixesClientDisconnectInit())
        return 1;
    client.devPrivates = calloc(1, registered_private_size);
    if (client.devPrivates == NULL)
        return 2;

    request.length = bytes_to_int32(sizeof(request));
    request.disconnect_mode = XFixesClientDisconnectFlagTerminate;
    client.requestBuffer = &request;
    client.req_len = request.length;

    if (ProcXFixesSetClientDisconnectMode(&client) != Success)
        return 3;

    dispatchExceptionAtReset = DE_RESET;
    if (XFixesShouldDisconnectClient(&client))
        return 4;
    dispatchExceptionAtReset = DE_TERMINATE;
    if (!XFixesShouldDisconnectClient(&client))
        return 5;

    free(client.devPrivates);
    dispatchExceptionAtReset = 0;
    return 0;
}
