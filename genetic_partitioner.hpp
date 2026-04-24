#pragma once

#include "request_workload.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <limits>
#include <random>
#include <utility>
#include <vector>

struct PartitionMetrics {
  uint64_t contention_score = 0;
  double avg_mutexes_per_query = 0.0;
  double fitness = 0.0;
};

template <size_t kLockCnt, size_t kBlocks = 1024> class GeneticPartitioner {
public:
  GeneticPartitioner(size_t array_size, uint64_t seed,
                     size_t history_limit = 12000, size_t population_size = 48,
                     size_t elite_count = 4, size_t generation_count = 36,
                     size_t max_components = 4, double component_decay = 0.90,
                     double component_learning_rate = 0.35,
                     double new_component_threshold = 0.55,
                     size_t max_cut_shift_per_batch =
                         std::max(size_t{1}, (kBlocks / kLockCnt) * size_t{4}))
      : array_size_(array_size),
        block_size_(array_size == 0 ? 1 : 1 + ((array_size - 1) / kBlocks)),
        history_limit_(history_limit),
        population_size_(std::max(population_size, size_t{1})),
        elite_count_(std::max(
            size_t{1},
            std::min(elite_count, std::max(population_size, size_t{1})))),
        generation_count_(std::max(generation_count, size_t{1})),
        max_components_(std::max(max_components, size_t{1})),
        component_query_limit_(std::max(history_limit, size_t{1})),
        component_decay_(std::clamp(component_decay, 0.0, 1.0)),
        component_learning_rate_(std::clamp(component_learning_rate, 0.0, 1.0)),
        new_component_threshold_(std::max(0.0, new_component_threshold)),
        max_cut_shift_per_batch_(std::max(max_cut_shift_per_batch, size_t{1})),
        rng_(seed), current_(MakeUniformChromosome()) {}

  void ObserveBatch(const std::vector<Query> &batch) {
    if (batch.empty()) {
      return;
    }

    const std::vector<double> profile = BuildBlockProfile(batch);
    DecayComponentMasses();
    active_component_ = AssignBatchToComponent(profile, batch);
    ActivateComponent(active_component_);
    TrainOnActiveComponent();
  }

  [[nodiscard]] PartitionMetrics
  EvaluateCurrent(const std::vector<Query> &queries) const {
    return Evaluate(current_.cuts, queries);
  }

  size_t CountLocksForRange(size_t left, size_t right) const {
    if (array_size_ == 0) {
      return 0;
    }

    left = std::min(left, array_size_ - 1);
    right = std::min(right, array_size_ - 1);
    if (left > right) {
      std::swap(left, right);
    }

    const size_t left_block = PositionToBlock(left);
    const size_t right_block = PositionToBlock(right);
    const size_t first_lock = FindLockForBlock(current_.cuts, left_block);
    const size_t last_lock = FindLockForBlock(current_.cuts, right_block);
    return last_lock - first_lock + 1;
  }

  [[nodiscard]] std::vector<size_t> PartitionWidths() const {
    std::vector<size_t> widths;
    widths.reserve(kLockCnt);
    for (size_t i = 0; i < kLockCnt; ++i) {
      widths.push_back(current_.cuts[i + 1] - current_.cuts[i]);
    }
    return widths;
  }

  [[nodiscard]] std::vector<size_t> PartitionCuts() const {
    return current_.cuts;
  }

private:
  static constexpr size_t kNoComponent = std::numeric_limits<size_t>::max();

  struct Chromosome {
    std::vector<size_t> cuts;
    PartitionMetrics metrics;
  };

  struct WorkloadComponent {
    std::vector<double> centroid;
    std::deque<Query> queries;
    Chromosome best_partition;
    double mass = 0.0;
    bool has_best_partition = false;
  };

  Chromosome MakeUniformChromosome() const {
    Chromosome chromosome;
    chromosome.cuts.resize(kLockCnt + 1);
    for (size_t i = 0; i <= kLockCnt; ++i) {
      chromosome.cuts[i] = i * kBlocks / kLockCnt;
    }
    chromosome.cuts[kLockCnt] = kBlocks;
    return chromosome;
  }

  Chromosome
  MakeProfileGuidedChromosome(const std::vector<double> &profile) const {
    double total_mass = 0.0;
    for (double value : profile) {
      total_mass += value;
    }
    if (total_mass <= 0.0) {
      return MakeUniformChromosome();
    }

    std::vector<double> prefix(kBlocks + 1, 0.0);
    for (size_t block = 0; block < kBlocks; ++block) {
      prefix[block + 1] = prefix[block] + profile[block];
    }

    Chromosome chromosome;
    chromosome.cuts.resize(kLockCnt + 1);
    chromosome.cuts[0] = 0;

    size_t previous_cut = 0;
    for (size_t lock_index = 1; lock_index < kLockCnt; ++lock_index) {
      const double target = total_mass * static_cast<double>(lock_index) /
                            static_cast<double>(kLockCnt);
      size_t cut = previous_cut + 1;
      const size_t max_cut = kBlocks - (kLockCnt - lock_index);
      while (cut < max_cut && prefix[cut] < target) {
        ++cut;
      }
      chromosome.cuts[lock_index] = cut;
      previous_cut = cut;
    }

    chromosome.cuts[kLockCnt] = kBlocks;
    return chromosome;
  }

  Chromosome MakeRandomChromosome() {
    std::vector<size_t> internal;
    internal.reserve(kLockCnt - 1);
    std::vector<bool> used(kBlocks, false);
    std::uniform_int_distribution<size_t> dist(1, kBlocks - 1);

    while (internal.size() < kLockCnt - 1) {
      const size_t cut = dist(rng_);
      if (!used[cut]) {
        used[cut] = true;
        internal.push_back(cut);
      }
    }

    std::sort(internal.begin(), internal.end());

    Chromosome chromosome;
    chromosome.cuts.reserve(kLockCnt + 1);
    chromosome.cuts.push_back(0);
    chromosome.cuts.insert(chromosome.cuts.end(), internal.begin(),
                           internal.end());
    chromosome.cuts.push_back(kBlocks);
    return chromosome;
  }

  Chromosome
  MakeChromosomeFromInternalCuts(const std::vector<size_t> &internal) const {
    Chromosome chromosome;
    chromosome.cuts.reserve(kLockCnt + 1);
    chromosome.cuts.push_back(0);
    chromosome.cuts.insert(chromosome.cuts.end(), internal.begin(),
                           internal.end());
    chromosome.cuts.push_back(kBlocks);
    return chromosome;
  }

  Chromosome MoveToward(const Chromosome &from,
                        const Chromosome &target) const {
    if (from.cuts == target.cuts) {
      return from;
    }

    std::vector<size_t> internal;
    internal.reserve(kLockCnt - 1);
    for (size_t i = 1; i < kLockCnt; ++i) {
      const size_t current_cut = from.cuts[i];
      const size_t target_cut = target.cuts[i];

      if (current_cut < target_cut) {
        internal.push_back(
            std::min(current_cut + max_cut_shift_per_batch_, target_cut));
      } else {
        const size_t lower = current_cut > max_cut_shift_per_batch_
                                 ? current_cut - max_cut_shift_per_batch_
                                 : size_t{1};
        internal.push_back(std::max(lower, target_cut));
      }
    }

    RepairInternalCuts(internal);
    return MakeChromosomeFromInternalCuts(internal);
  }

  static size_t FindLockForBlock(const std::vector<size_t> &cuts,
                                 size_t block) {
    for (size_t i = 0; i < kLockCnt; ++i) {
      if (cuts[i] <= block && block < cuts[i + 1]) {
        return i;
      }
    }
    return kLockCnt - 1;
  }

  size_t PositionToBlock(size_t position) const {
    return std::min(position / block_size_, kBlocks - 1);
  }

  static void RepairInternalCuts(std::vector<size_t> &internal) {
    if (internal.empty()) {
      return;
    }

    const size_t count = internal.size();

    internal[0] = std::clamp(internal[0], size_t{1}, kBlocks - count);
    for (size_t i = 1; i < count; ++i) {
      internal[i] = std::max(internal[i], internal[i - 1] + 1);
    }

    for (size_t offset = 0; offset < count; ++offset) {
      const size_t i = count - 1 - offset;
      const size_t upper_bound = kBlocks - (count - i);
      internal[i] = std::min(internal[i], upper_bound);
    }

    for (size_t i = 1; i < count; ++i) {
      internal[i] = std::max(internal[i], internal[i - 1] + 1);
    }
  }

  Chromosome Crossover(const Chromosome &lhs, const Chromosome &rhs) {
    std::bernoulli_distribution pick_parent(0.5);
    std::vector<size_t> internal;
    internal.reserve(kLockCnt - 1);

    for (size_t i = 1; i < kLockCnt; ++i) {
      internal.push_back(pick_parent(rng_) ? lhs.cuts[i] : rhs.cuts[i]);
    }

    std::ranges::sort(internal);
    RepairInternalCuts(internal);

    Chromosome child;
    child.cuts.reserve(kLockCnt + 1);
    child.cuts.push_back(0);
    child.cuts.insert(child.cuts.end(), internal.begin(), internal.end());
    child.cuts.push_back(kBlocks);
    return child;
  }

  void Mutate(Chromosome &chromosome) {
    if (kLockCnt <= 1) {
      return;
    }

    std::vector<size_t> internal(chromosome.cuts.begin() + 1,
                                 chromosome.cuts.end() - 1);

    std::uniform_int_distribution<size_t> mutation_count_dist(2, 5);
    std::uniform_int_distribution<size_t> index_dist(0, internal.size() - 1);
    std::uniform_int_distribution<int> shift_dist(-32, 32);

    const size_t mutation_count = mutation_count_dist(rng_);
    for (size_t i = 0; i < mutation_count; ++i) {
      const size_t index = index_dist(rng_);
      const int shift = shift_dist(rng_);
      const int mutated = static_cast<int>(internal[index]) + shift;
      internal[index] = static_cast<size_t>(
          std::clamp(mutated, 1, static_cast<int>(kBlocks - 1)));
    }

    std::sort(internal.begin(), internal.end());
    RepairInternalCuts(internal);

    chromosome.cuts.clear();
    chromosome.cuts.reserve(kLockCnt + 1);
    chromosome.cuts.push_back(0);
    chromosome.cuts.insert(chromosome.cuts.end(), internal.begin(),
                           internal.end());
    chromosome.cuts.push_back(kBlocks);
  }

  PartitionMetrics Evaluate(const std::vector<size_t> &cuts,
                            const std::vector<Query> &queries) const {
    if (queries.empty()) {
      return {};
    }

    std::vector<uint64_t> block_hits(kBlocks, 0);
    uint64_t total_locks = 0;

    for (const Query &query : queries) {
      const size_t left_block = PositionToBlock(query.left);
      const size_t right_block = PositionToBlock(query.right);

      for (size_t block = left_block; block <= right_block; ++block) {
        block_hits[block]++;
      }

      const size_t first_lock = FindLockForBlock(cuts, left_block);
      const size_t last_lock = FindLockForBlock(cuts, right_block);
      total_locks += last_lock - first_lock + 1;
    }

    uint64_t contention_score = 0;
    for (size_t lock_index = 0; lock_index < kLockCnt; ++lock_index) {
      uint64_t load = 0;
      for (size_t block = cuts[lock_index]; block < cuts[lock_index + 1];
           ++block) {
        load += block_hits[block];
      }
      contention_score += load * load;
    }

    const double avg_mutexes =
        static_cast<double>(total_locks) / static_cast<double>(queries.size());
    const double fitness =
        static_cast<double>(contention_score) + 5000.0 * avg_mutexes;

    return {contention_score, avg_mutexes, fitness};
  }

  std::vector<double>
  BuildBlockProfile(const std::vector<Query> &queries) const {
    std::vector<double> profile(kBlocks, 0.0);
    double total_hits = 0.0;

    for (const Query &query : queries) {
      const size_t left_block = PositionToBlock(query.left);
      const size_t right_block = PositionToBlock(query.right);
      for (size_t block = left_block; block <= right_block; ++block) {
        profile[block] += 1.0;
        total_hits += 1.0;
      }
    }

    if (total_hits == 0.0) {
      return profile;
    }

    for (double &value : profile) {
      value /= total_hits;
    }
    return profile;
  }

  static double ProfileDistance(const std::vector<double> &lhs,
                                const std::vector<double> &rhs) {
    double distance = 0.0;
    for (size_t i = 0; i < lhs.size(); ++i) {
      distance += std::abs(lhs[i] - rhs[i]);
    }
    return distance;
  }

  void DecayComponentMasses() {
    for (WorkloadComponent &component : components_) {
      component.mass *= component_decay_;
    }
  }

  void AppendQueries(WorkloadComponent &component,
                     const std::vector<Query> &batch) {
    for (const Query &query : batch) {
      component.queries.push_back(query);
    }
    while (component.queries.size() > component_query_limit_) {
      component.queries.pop_front();
    }
  }

  void ResetComponent(WorkloadComponent &component,
                      const std::vector<double> &profile,
                      const std::vector<Query> &batch) {
    component.centroid = profile;
    component.queries.clear();
    component.mass = 1.0;
    component.has_best_partition = false;
    AppendQueries(component, batch);
  }

  void UpdateComponent(WorkloadComponent &component,
                       const std::vector<double> &profile,
                       const std::vector<Query> &batch) {
    for (size_t block = 0; block < kBlocks; ++block) {
      component.centroid[block] =
          (1.0 - component_learning_rate_) * component.centroid[block] +
          component_learning_rate_ * profile[block];
    }
    component.mass += 1.0;
    AppendQueries(component, batch);
  }

  size_t ReplaceWeakestComponent(const std::vector<double> &profile,
                                 const std::vector<Query> &batch) {
    size_t weakest_index = 0;
    for (size_t i = 1; i < components_.size(); ++i) {
      if (components_[i].mass < components_[weakest_index].mass) {
        weakest_index = i;
      }
    }

    ResetComponent(components_[weakest_index], profile, batch);
    return weakest_index;
  }

  size_t AssignBatchToComponent(const std::vector<double> &profile,
                                const std::vector<Query> &batch) {
    if (components_.empty()) {
      WorkloadComponent component;
      ResetComponent(component, profile, batch);
      components_.push_back(std::move(component));
      return 0;
    }

    size_t best_index = 0;
    double best_distance = ProfileDistance(profile, components_[0].centroid);
    for (size_t i = 1; i < components_.size(); ++i) {
      const double distance = ProfileDistance(profile, components_[i].centroid);
      if (distance < best_distance) {
        best_distance = distance;
        best_index = i;
      }
    }

    if (best_distance > new_component_threshold_) {
      if (components_.size() < max_components_) {
        WorkloadComponent component;
        ResetComponent(component, profile, batch);
        components_.push_back(std::move(component));
        return components_.size() - 1;
      }
      return ReplaceWeakestComponent(profile, batch);
    }

    UpdateComponent(components_[best_index], profile, batch);
    return best_index;
  }

  void ActivateComponent(size_t component_index) {
    if (component_index >= components_.size()) {
      return;
    }

    WorkloadComponent &component = components_[component_index];
    if (component.has_best_partition) {
      current_ = MoveToward(current_, component.best_partition);
    }
  }

  std::vector<Query> BuildActiveQueries() const {
    if (active_component_ >= components_.size()) {
      return {};
    }

    const WorkloadComponent &component = components_[active_component_];
    return std::vector<Query>(component.queries.begin(),
                              component.queries.end());
  }

  void EvaluatePopulation(std::vector<Chromosome> &population,
                          const std::vector<Query> &queries) const {
    for (Chromosome &chromosome : population) {
      chromosome.metrics = Evaluate(chromosome.cuts, queries);
    }
  }

  void RememberBestForActiveComponent(Chromosome candidate,
                                      const std::vector<Query> &queries) {
    if (active_component_ >= components_.size()) {
      return;
    }

    WorkloadComponent &component = components_[active_component_];
    candidate.metrics = Evaluate(candidate.cuts, queries);
    if (!component.has_best_partition) {
      component.best_partition = std::move(candidate);
      component.has_best_partition = true;
      return;
    }

    component.best_partition.metrics =
        Evaluate(component.best_partition.cuts, queries);
    if (candidate.metrics.fitness <= component.best_partition.metrics.fitness) {
      component.best_partition = std::move(candidate);
    }
  }

  void SeedPopulation(std::vector<Chromosome> &population) {
    population.push_back(current_);

    const WorkloadComponent *active = active_component_ < components_.size()
                                          ? &components_[active_component_]
                                          : nullptr;

    if (active != nullptr && active->has_best_partition &&
        active->best_partition.cuts != current_.cuts &&
        population.size() < population_size_) {
      population.push_back(active->best_partition);
    }

    if (active != nullptr && population.size() < population_size_) {
      Chromosome guided = MakeProfileGuidedChromosome(active->centroid);
      if (guided.cuts != current_.cuts &&
          (!active->has_best_partition ||
           guided.cuts != active->best_partition.cuts)) {
        population.push_back(std::move(guided));
      }
    }

    if (population.size() < population_size_) {
      Chromosome uniform = MakeUniformChromosome();
      bool duplicate = false;
      for (const Chromosome &chromosome : population) {
        if (chromosome.cuts == uniform.cuts) {
          duplicate = true;
          break;
        }
      }
      if (!duplicate) {
        population.push_back(std::move(uniform));
      }
    }

    while (population.size() < elite_count_) {
      Chromosome candidate = current_;
      Mutate(candidate);
      population.push_back(std::move(candidate));
    }

    while (population.size() < population_size_) {
      if ((population.size() & 1U) == 0) {
        Chromosome candidate = current_;
        Mutate(candidate);
        population.push_back(std::move(candidate));
      } else {
        population.push_back(MakeRandomChromosome());
      }
    }
  }

  void TrainOnActiveComponent() {
    const std::vector<Query> queries = BuildActiveQueries();
    if (queries.empty()) {
      return;
    }

    const Chromosome start_partition = current_;
    std::vector<Chromosome> population;
    population.reserve(population_size_);
    SeedPopulation(population);
    EvaluatePopulation(population, queries);

    for (size_t generation = 0; generation < generation_count_; ++generation) {
      std::sort(population.begin(), population.end(),
                [](const Chromosome &lhs, const Chromosome &rhs) {
                  return lhs.metrics.fitness < rhs.metrics.fitness;
                });

      std::vector<Chromosome> next_generation;
      next_generation.reserve(population_size_);

      for (size_t i = 0; i < elite_count_ && i < population.size(); ++i) {
        next_generation.push_back(population[i]);
      }

      const size_t selectable = std::min(elite_count_, population.size());
      std::uniform_int_distribution<size_t> elite_pick(0, selectable - 1);
      std::bernoulli_distribution mutate_child(0.85);

      while (next_generation.size() < population_size_) {
        Chromosome child = Crossover(population[elite_pick(rng_)],
                                     population[elite_pick(rng_)]);
        if (mutate_child(rng_)) {
          Mutate(child);
        }
        child.metrics = Evaluate(child.cuts, queries);
        next_generation.push_back(std::move(child));
      }

      population = std::move(next_generation);
    }

    std::sort(population.begin(), population.end(),
              [](const Chromosome &lhs, const Chromosome &rhs) {
                return lhs.metrics.fitness < rhs.metrics.fitness;
              });

    RememberBestForActiveComponent(population.front(), queries);

    if (active_component_ < components_.size() &&
        components_[active_component_].has_best_partition) {
      current_ = MoveToward(start_partition,
                            components_[active_component_].best_partition);
      current_.metrics = Evaluate(current_.cuts, queries);
    }
  }

  size_t array_size_;
  size_t block_size_;
  size_t history_limit_;
  size_t population_size_;
  size_t elite_count_;
  size_t generation_count_;
  size_t max_components_;
  size_t component_query_limit_;
  double component_decay_;
  double component_learning_rate_;
  double new_component_threshold_;
  size_t max_cut_shift_per_batch_;
  std::mt19937_64 rng_;
  std::vector<WorkloadComponent> components_;
  size_t active_component_ = kNoComponent;
  Chromosome current_;
};
