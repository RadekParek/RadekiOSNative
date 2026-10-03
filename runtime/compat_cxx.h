#pragma once
#include "relinker/relinker.h"
namespace radeki::runtime {
// Process-wide destructor registry; thread-local destructor semantics are not emulated.
int registerCxxExit(void (*fn)(void*), void* arg, void* dso);
void finalizeCxx(void* dso);
int acquireCxxGuard(uint64_t* guard);
void releaseCxxGuard(uint64_t* guard);
void abortCxxGuard(uint64_t* guard);
void addCxxCompat(relinker::CompatRegistry&);
}
