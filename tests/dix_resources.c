#include <dix-config.h>

#include "callback.h"
#include "dix.h"
#include "dixstruct.h"
#include "resource.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

int LimitClients = 256;

void
AttendClient(ClientPtr client)
{
    (void) client;
}

void
IgnoreClient(ClientPtr client)
{
    (void) client;
}

void *
XNFrealloc(void *pointer, unsigned long amount)
{
    void *replacement = realloc(pointer, amount);

    if (!replacement)
        abort();
    return replacement;
}

void
ErrorF(const char *format, ...)
{
    va_list arguments;

    va_start(arguments, format);
    vfprintf(stderr, format, arguments);
    va_end(arguments);
}

void
FatalError(const char *format, ...)
{
    va_list arguments;

    va_start(arguments, format);
    vfprintf(stderr, format, arguments);
    va_end(arguments);
    abort();
}

void HandleSaveSet(ClientPtr client) { (void) client; }
void MarkClientException(ClientPtr client) { (void) client; }

#define DEFINE_RESOURCE_DELETER(name) \
    int name(void *value, XID id)     \
    {                                 \
        (void) value;                 \
        (void) id;                    \
        return Success;               \
    }

DEFINE_RESOURCE_DELETER(CloseFont)
DEFINE_RESOURCE_DELETER(DeletePassiveGrab)
DEFINE_RESOURCE_DELETER(DeleteWindow)
DEFINE_RESOURCE_DELETER(FreeClientPixels)
DEFINE_RESOURCE_DELETER(FreeColormap)
DEFINE_RESOURCE_DELETER(FreeCursor)
DEFINE_RESOURCE_DELETER(FreeGC)
DEFINE_RESOURCE_DELETER(OtherClientGone)
DEFINE_RESOURCE_DELETER(dixDestroyPixmap)

typedef struct {
    int delete_calls;
    int callback_adds;
    int callback_frees;
    XID last_id;
} ResourceTestState;

static ResourceTestState state;

static int
delete_test_resource(void *value, XID id)
{
    state.delete_calls++;
    state.last_id = id;
    return value != NULL ? Success : BadValue;
}

static void
resource_callback(CallbackListPtr *list, void *closure, void *call_data)
{
    ResourceTestState *callback_state = closure;
    const ResourceStateInfoRec *info = call_data;

    (void) list;
    if (info->state == ResourceStateAdding)
        callback_state->callback_adds++;
    else if (info->state == ResourceStateFreeing)
        callback_state->callback_frees++;
    callback_state->last_id = info->id;
}

int
main(void)
{
    ClientRec client = { 0 };
    int first_value = 17;
    int replacement_value = 29;
    void *result = NULL;
    RESTYPE test_type;
    XID id;

    client.index = 0;
    client.clientAsMask = 0;
    serverClient = &client;
    clients[0] = &client;
    currentMaxClients = 1;

    InitCallbackManager();
    if (!InitClientResources(&client))
        return 1;
    if (!AddCallback(&ResourceStateCallback, resource_callback, &state))
        return 2;

    test_type = CreateNewResourceType(delete_test_resource, "XMIN_TEST");
    if (test_type == 0)
        return 3;

    id = FakeClientID(client.index);
    if (!AddResource(id, test_type, &first_value))
        return 4;
    if (dixLookupResourceByType(&result, id, test_type, &client,
                                DixReadAccess) != Success ||
        result != &first_value)
        return 5;

    if (!ChangeResourceValue(id, test_type, &replacement_value))
        return 6;
    result = NULL;
    if (dixLookupResourceByType(&result, id, test_type, &client,
                                DixReadAccess) != Success ||
        result != &replacement_value)
        return 7;

    FreeResource(id, RT_NONE);
    result = NULL;
    if (dixLookupResourceByType(&result, id, test_type, &client,
                                DixReadAccess) != BadValue)
        return 8;
    if (state.delete_calls != 1 || state.callback_adds != 1 ||
        state.callback_frees != 1 || state.last_id != id)
        return 9;

    DeleteCallback(&ResourceStateCallback, resource_callback, &state);
    FreeClientResources(&client);
    DeleteCallbackManager();
    serverClient = NULL;
    clients[0] = NULL;
    return 0;
}
