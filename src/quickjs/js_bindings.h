#pragma once

extern "C"
{
#include "quickjs.h"
}

#include <cstddef>
#include <cstdint>

namespace lrvx
{

// Registers the lrvx globals on `ctx`. Runs the C ABI handshake
// lrvx_capi.h asks every caller to run at startup first and returns
// false without registering anything when the binding was compiled
// against a different version than the library it links -- the structs
// on that boundary carry no reserved tail, so continuing would read
// wrong numbers rather than fail. The refusal is left on the context as
// a JS exception.
bool registerLrvxBindings(JSContext* ctx);

// Opaque handle wrapper — registered JS class for passing a C handle
// through JS. `destroyFn` is the C ABI function that owns `handle` (e.g.
// lrvx_book_destroy); the finalizer calls it if the script never
// explicitly calls .destroy(). Pass nullptr only for a handle JS
// borrows rather than owns (see LrvxJsStrategy::injectHandle).
JSValue createHandleObject(JSContext* ctx, void* handle, void (*destroyFn)(void*));
void* getHandlePtr(JSContext* ctx, JSValueConst val);

// Live count of handle-bearing LrvxHandle JS objects that still have an
// undestroyed C handle attached (i.e. created with a non-null destroyFn
// and not yet destroyed, explicitly or via finalizer). Test-only hook for
// exercising the leak path without a sanitizer.
size_t lrvxJsHandleLiveCountForTesting();

}  // namespace lrvx
