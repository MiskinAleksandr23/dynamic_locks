#pragma once

// The extracted upstream sources previously obtained standard declarations
// transitively from RocksDB's transaction_db.h. Keep upstream files unchanged
// and provide those declarations explicitly for the standalone target.
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <exception>
#include <limits>
#include <memory>
#include <new>
#include <set>
#include <stdexcept>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>
