#pragma once

#include "locktree_port_prefix.hpp"
#include "portability/toku_external_pthread.h"

namespace locktree_port {

// The factory owns no lock table. It only supplies ordinary pthread mutexes
// and condition variables to upstream's external-wait interface. Internal
// LockTree synchronization, conflict tracking, allocation and reclamation stay
// in the upstream implementation. Allocation happens at the upstream call sites
// and remains inside any benchmark interval that includes those call sites.
toku_external_mutex_factory_t MakeMutexFactory();

} // namespace locktree_port
