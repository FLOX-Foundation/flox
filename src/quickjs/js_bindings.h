#pragma once

extern "C"
{
#include "quickjs.h"
}

#include <cstddef>
#include <cstdint>

namespace flox
{

void registerFloxBindings(JSContext* ctx);

// Opaque handle wrapper — registered JS class for passing a C handle
// through JS. `destroyFn` is the C ABI function that owns `handle` (e.g.
// flox_book_destroy); the finalizer calls it if the script never
// explicitly calls .destroy(). Pass nullptr only for a handle JS
// borrows rather than owns (see FloxJsStrategy::injectHandle).
JSValue createHandleObject(JSContext* ctx, void* handle, void (*destroyFn)(void*));
void* getHandlePtr(JSContext* ctx, JSValueConst val);

// Live count of handle-bearing FloxHandle JS objects that still have an
// undestroyed C handle attached (i.e. created with a non-null destroyFn
// and not yet destroyed, explicitly or via finalizer). Test-only hook for
// exercising the leak path without a sanitizer.
size_t floxJsHandleLiveCountForTesting();

}  // namespace flox
