#include "compat/status.h"
namespace radeki::compat {
const char* statusName(Status s) { switch(s) { case Status::Implemented:return "Implemented"; case Status::Partial:return "Partial"; case Status::Unsupported:return "Unsupported"; case Status::Blocked:return "Blocked"; } return "Blocked"; }
const char* statusMark(Status s) { switch(s) { case Status::Implemented:return "✓"; case Status::Partial:return "◐"; case Status::Unsupported:return "—"; case Status::Blocked:return "✕"; } return "✕"; }
const char* runtimeStateName(RuntimeState s) { switch(s) { case RuntimeState::Analyzed:return "ANALYZED"; case RuntimeState::PartiallyRelinked:return "PARTIALLY_RELINKED"; case RuntimeState::Relinked:return "RELINKED"; case RuntimeState::RuntimePartial:return "RUNTIME_PARTIAL"; case RuntimeState::RuntimeReady:return "RUNTIME_READY"; case RuntimeState::Blocked:return "BLOCKED"; } return "BLOCKED"; }
}
