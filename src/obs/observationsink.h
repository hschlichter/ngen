#pragma once

#include "observation.h"

namespace obs {

class ObservationSink {
public:
    virtual ~ObservationSink() = default;
    virtual void write(const Observation& obs) = 0;
    virtual void flush() = 0;
};

} // namespace obs
