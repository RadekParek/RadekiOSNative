#pragma once
#include <string>
namespace radeki::compat {
enum class Status { Implemented, Partial, Unsupported, Blocked };
const char* statusName(Status);
const char* statusMark(Status);
enum class RuntimeState { Analyzed, PartiallyRelinked, Relinked, RuntimePartial, RuntimeReady, Blocked };
const char* runtimeStateName(RuntimeState);
}
