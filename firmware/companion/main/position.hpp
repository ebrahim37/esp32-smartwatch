// Location provider abstraction.
//
// TODO(PROTOTYPE): the prototype has no GNSS receiver and reports a fixed, clearly marked
// simulated fix (43 deg 46' 48.3" N, 79 deg 24' 42.7" W). TODO(LTE): the EG912U's GNSS
// replaces SimulatedProvider; TODO(PHONE): a phone app may supply fixes instead.
#pragma once

#include "swp/messages.hpp"

namespace position {

class Provider {
public:
    virtual ~Provider() = default;
    virtual bool current(swp::Position& out) = 0;
    virtual const char* name() const = 0;
};

void init();
bool current(swp::Position& out);
const char* provider_name();

}  // namespace position
