#pragma once

#include "../adaptive_lock/adapter_impl.h"

template <typename K, typename V, class Reclaim, class Alloc, class Pool>
using ds_adapter = adaptive_lock_benchmark::AdapterImpl<K, V, adaptive_lock_benchmark::GeneticTag>;
