#include "callback.h"
#include "dix.h"
#include "dixstruct.h"
#include "resource.h"
#include "scrnintstr.h"

#include <stdlib.h>

/*
 * dixutils.c also contains server scheduling helpers.  These definitions keep
 * this unit test focused on its callback manager until the corresponding DIX
 * and OS layers are imported.
 */
ClientPtr clients[MAXCLIENTS];
int currentMaxClients;
TimeStamp currentTime;
ScreenInfo screenInfo;

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

unsigned int
ResourceClientBits(void)
{
    return 0;
}

int
dixLookupResourceByType(void **result, XID id, RESTYPE type,
                        ClientPtr client, Mask access)
{
    (void) result;
    (void) id;
    (void) type;
    (void) client;
    (void) access;
    return BadValue;
}

int
dixLookupResourceByClass(void **result, XID id, RESTYPE class,
                         ClientPtr client, Mask access)
{
    (void) result;
    (void) id;
    (void) class;
    (void) client;
    (void) access;
    return BadValue;
}

void *
XNFrealloc(void *pointer, unsigned long amount)
{
    void *replacement = realloc(pointer, amount);

    if (!replacement)
        abort();
    return replacement;
}

typedef struct {
    int calls;
    int sum;
    Bool remove_on_call;
} CallbackState;

static void
record_callback(CallbackListPtr *list, void *closure, void *call_data)
{
    CallbackState *state = closure;

    state->calls++;
    state->sum += *(const int *) call_data;
    if (state->remove_on_call)
        DeleteCallback(list, record_callback, closure);
}

int
main(void)
{
    CallbackListPtr list = NULL;
    CallbackState persistent = { 0, 0, FALSE };
    CallbackState once = { 0, 0, TRUE };
    const int first = 7;
    const int second = 11;

    InitCallbackManager();
    if (!AddCallback(&list, record_callback, &persistent) ||
        !AddCallback(&list, record_callback, &once))
        return 1;

    CallCallbacks(&list, (void *) &first);
    CallCallbacks(&list, (void *) &second);
    if (once.calls != 1 || once.sum != first)
        return 2;
    if (persistent.calls != 2 || persistent.sum != first + second)
        return 3;

    if (!DeleteCallback(&list, record_callback, &persistent))
        return 4;
    DeleteCallbackList(&list);
    if (list != NULL)
        return 5;

    DeleteCallbackManager();
    return 0;
}
