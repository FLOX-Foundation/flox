#pragma once

extern "C"
{
#include "quickjs.h"
}

#include <cstdint>

namespace flox
{

void registerFloxBindings(JSContext* ctx);

// Opaque handle wrapper — registered JS class for passing FloxStrategyHandle through JS
JSValue createHandleObject(JSContext* ctx, void* handle);
void* getHandlePtr(JSContext* ctx, JSValueConst val);

}  // namespace flox
