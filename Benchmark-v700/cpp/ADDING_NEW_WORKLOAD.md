# The adding entity

The software design is described on the [main page](./../README.md#software-design).  

The workloads are in the folder [./microbench/workloads/](microbench/workloads/).  
Each entity's folder has the following structure:
```shell
./microbench/workloads/<entity>/
├── builders
├── impls
├── <entity>.h
├── <entity>_builder.h
└── <entity>_json_convector.h
```

The `<entity>.h` file contains the interface that needs to be implemented, 
`<entity>_builder.h` — the interface of entity builder.
The `builders` and `impls` folders contain the corresponding implementations.  
The `<entity>_json_convector.h` file is the file is an auxiliary json convector. 

The key entities are:
+ [Distribution](microbench/workloads/distributions/distribution.h) — a distribution of a random variable;
+ [DataMap](microbench/workloads/data_maps/data_map.h) — converts a distribution's output into a key;
+ [ArgsGenerator](microbench/workloads/args_generators/args_generator.h) — creates operands for an operation;
+ [ThreadLoop](microbench/workloads/thread_loops/thread_loop.h) — the logic for interacting with a data structure.

There are builders each type of entity:
[ThreadLoopBuilder](microbench/workloads/thread_loops/thread_loop_builder.h),
[ArgsGeneratorBuilder](microbench/workloads/args_generators/args_generator_builder.h),
[DistributionBuilder](microbench/workloads/distributions/distribution_builder.h),
[DataMapBuilder](microbench/workloads/data_maps/data_map_builder.h).

There is also a [StopCondition](microbench/workloads/stop_condition/stop_condition.h)
– a condition in which the load stops working.

### The interface implementation
The first part is the implementation of an Entity:
```c++
class ExampleEntity : public Entity {
    *EntityFields*

public:
    ExampleEntity(*entityParameters*) {...}
    
    *overriding entity functions*
  
    ~ExampleEntity() {...}
};
```

### The implementation of a builder
The second part is the implementation of an EntityBuilder:
```c++
class ExampleEntityBuilder : public EntityBuilder {
    *rawParameters*
    *finalParameters*

public:
    ExampleEntityBuilder() {...}
    
    *overriding entity functions*
  
    ExampleEntityBuilder *init(*initParameters*) override {
        *initializing final parameters by processing raw parameters*
    }

    /**
     * called only after the init function
     */
    ExampleEntity *build(Random &_rng) override {
        *creating the new instance of ExampleEntity*
    }
    
    void toJson(nlohmann::json &j) const override {
        /**
         * The name of the class. 
         * Necessary for converting from json file to class
         */
        j["ClassName"] = "ExampleEntityBuilder";
        
        *converting raw parameters to json format*
    }
    
    void fromJson(const nlohmann::json &j) override {
        *converting raw parameters from json format*
    }
    
    std::string toString(size_t indents) override {
        *converting the class to string format for writing to the console*
    }
    
    ~ExampleEntityBuilder() {...}
};
```

#### Note

Do not forget to specify the class name in the `j["ClassName"]`.

#### Tools 

To convert to json format, the [nlohmann::json](https://github.com/nlohmann/json) implementation is used.

To convert the entities from json format use the `getEntityFromJson` function.


For convenient conversion to a string representation, use the  
`indented_title`, `indented_title_with_data` and `indented_title_with_str_data` functions
from [globals_extern.h](microbench/globals_extern.h). 


### Adding the new entity builder to enum and json_convector 

The last part is to extend the `getEntityFromJson` function in the `<entity>_json_convector.h` file 
using the class name you specified earlier in `j["ClassName"]`:
```c++
EntityBuilder *getEntityFromJson(const nlohmann::json &j) {    
    std::string className = j["ClassName"];
    StopCondition *stopCondition;
    if (className == "Timer") {
        stopCondition = new Timer();
        
    std::string className = j["ClassName"];
    EntityBuilder *entityBuilder;
    
    
    if (className == "ExampleEntityBuilder") {
        entityBuilder = new ExampleEntityBuilder();
    } else if (...) {
        ...
    } else {    
        setbench_error("JSON PARSER: Unknown class name StopCondition -- " + className)
    }
    
    entityBuilder->fromJson(j);
    return entityBuilder;
}
```

### StopCondition

In contrast to other entities, `StopCondition` does not have builders, so it is converted to json format on its own
and initialization occurs during the call to the `start(numThreads)` function.

## Example

### DataMap 

The `ArrayDataMap` creates an array filled with values from the entire range of keys and shuffles them randomly. 
When calling the `get(index)` method, returns the corresponding element from the array.  

The [implementation](microbench/workloads/data_maps/impls/array_data_map.h) 
and [builder](microbench/workloads/data_maps/builders/array_data_map_builder.h) of `ArrayDataMap`.

### Distribution

The `SkewedUniformDistribution` depends on two variables `hotSize` and `hotRatio`, which take values from 0 to 1. 
The range is divided into two interval: 
the random variable returns from first interval with `hotRatio` probability and from second with `1 - hotRatio`;
the size of first interval is `range * hotSize`, the size of second if `range * (1 - hotSize)`. 
The random variable in intervals chooses uniformly. 

The [implementation](microbench/workloads/distributions/impls/skewed_uniform_distribution.h)
and [builder](microbench/workloads/distributions/builders/skewed_uniform_distribution_builder.h) of `SkewedUniformDistribution`.

### ArgsGenerator

The `SkewedSetsArgsGenerator` uses two `SkewedUniformDistributions` separately for read and update operations,
and takes the following parameters:
+ `rp%` of read operations are performed on a random subset of keys of proportion `rs%` where a key is taken uniformly. 
All other read operations are performed on the rest of the set.
+ `wp%` of update operations are performed on a random subset of keys of proportion `ws%` where a key is taken uniformly.
All other update operations are performed on the rest of the set.
+ `inter%` of keys are in the intersection of the working sets of read and update operations.

The implementation and builder are presented in [SkewedSetsArgsGenerator file](microbench/workloads/args_generators/impls/skewed_sets_args_generator.h).

### TreadLoop

The `DefaultThreadLoop` selects the next operation with some fixed probability. It accepts the following parameters:
+ `ui%` of operations are insert operations;
+ `ue%` of operations are remove operations;
+ while `100 - ui - ue%` of operations are get operations.

The implementation and builder are presented in [DefaultThreadLoop file](microbench/workloads/thread_loops/impls/default_thread_loop.h).

### StopCondition

The `Timer` accepts a `workTime` parameter in milliseconds, and the `isStopped` method returns true during that time.

The implementation are presented in [Timer file](microbench/workloads/stop_condition/impls/timer.h)


[//]: # (## Example)
[//]: # ()
[//]: # (We now present a complete example, by adding the skewed read-update workload from)
[//]: # ([aksenov2023splay]&#40;https://link.springer.com/article/10.1007/s00446-022-00441-x&#41; in our suite.)
[//]: # (This workload is used for testing key-value data structures supporting three operations `insert`, `remove`, and `get`.)
[//]: # ()
[//]: # (This workload is specified by five parameters:)
[//]: # (+ `n`, the size of the working set of keys;)
[//]: # (+ `w%`, the amount of updates in the total number of operations;)
[//]: # (+ `x%` of get operations choose a key uniformly at random from a random subset of keys of proportion `y%`, )
[//]: # (while other get operations choose a random key from the rest of the set;)
[//]: # (+ `insert` and `remove` operations choose a key uniformly at random from a random subset of keys of proportion `s%`.)




