#pragma once

#include "observationsink.h"

#include <fstream>
#include <string>

namespace obs {

class JsonLinesFileSink : public ObservationSink {
public:
    // Opens `path` for writing. Writes a schema metadata record as the first line.
    // Returns false if the file couldn't be opened.
    bool open(const std::string& path);

    void write(const Observation& obs) override;
    void flush() override;

    bool isOpen() const { return m_stream.is_open(); }

private:
    std::ofstream m_stream;
};

} // namespace obs
