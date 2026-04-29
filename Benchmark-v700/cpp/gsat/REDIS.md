# Redis trees

To run the following implementations of [Redis sorted sets](https://redis.io/docs/data-types/sorted-sets/):
- [redis_zset](../ds/redis_zset/) - the original implementation of ZSET rewritten in C++;
- [redis_sabt](../ds/redis_sabt/) - self-adjusting B-Tree with faster modify operations (use this by default);
- [redis_sabpt](../ds/redis_sabpt/) - self-adjusting B-Tree with faster read-only operations;
- [redis_sait](../ds/redis_sait/) - self-adjusting IST;
- [redis_salt](../ds/redis_salt/) - self-adjusting Logarithmic Tree;

[//]: # (please add the following line in [microbench/Makefile]&#40;../microbench/Makefile&#41; to build executable with defined flag **REDIS**:)
[//]: # ()
[//]: # (```)
[//]: # (FLAGS += -DREDIS)
[//]: # (```)