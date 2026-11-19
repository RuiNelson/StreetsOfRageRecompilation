#pragma once

#include "data_types.hpp"
#include "system/memory/SystemMemory.hpp"

#include <vector>

namespace SoRDecompress {

/// The tiles of the Nemesis stream at @p source (ROM), 32 bytes each, for host-side drawing.
std::vector<m_byte> nemesis(SystemMemory &memory, m_long source);

} // namespace SoRDecompress
