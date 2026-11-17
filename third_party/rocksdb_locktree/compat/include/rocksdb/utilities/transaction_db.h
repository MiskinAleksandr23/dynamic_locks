#pragma once

// LockTree includes this path for its external mutex-factory interface only.
// No transaction database, filesystem, WAL, or background database worker is
// constructed by the standalone adapter.
#include "rocksdb/utilities/transaction_db_mutex.h"
