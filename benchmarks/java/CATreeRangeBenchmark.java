package se.uu.collection;

import java.io.BufferedWriter;
import java.io.IOException;
import java.lang.management.GarbageCollectorMXBean;
import java.lang.management.ManagementFactory;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.StandardOpenOption;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;
import java.util.concurrent.CyclicBarrier;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicReference;
import java.util.function.BiFunction;

/**
 * Write-only ordered-map experiment, entirely within one JVM implementation.
 * CA is the unchanged author CATreeMapSTDR. Fixed uses the same author AVL
 * component and write-lock primitive, partitioned into 64 fixed key regions.
 * This is not a Java-versus-C++ array-lock comparison.
 */
public final class CATreeRangeBenchmark {
    private static final int FIXED_REGIONS = 64;
    private static final class Cell { long value; }
    private static final BiFunction<Integer, Cell, Cell> INCREMENT = (key, cell) -> {
        ++cell.value;
        return cell;
    };

    private interface MapAccess {
        void insert(Integer key, Cell value);
        Cell get(Integer key);
        void update(Integer left, Integer right, BiFunction<Integer, Cell, Cell> operation);
        int routeNodes();
        int fixedRegions();
    }

    private static final class AuthorCA implements MapAccess {
        private final CATreeMapSTDR<Integer, Cell> map = new CATreeMapSTDR<>();
        public void insert(Integer key, Cell value) { map.put(key, value); }
        public Cell get(Integer key) { return map.get(key); }
        public void update(Integer left, Integer right, BiFunction<Integer, Cell, Cell> operation) {
            // The native method holds all affected base-node locks before the
            // first value update. Its callbacks must not throw.
            map.rangeUpdate(left, right, operation);
        }
        public int routeNodes() { return map.numberOfRouteNodes(); }
        public int fixedRegions() { return 0; }
    }

    private static final class FixedAVL implements MapAccess {
        private final AVLTreeMapSTD<Integer, Cell>[] maps;
        private final int width;
        private final ThreadLocal<Stack<STDAVLNode<Integer, Cell>>> traversal =
                ThreadLocal.withInitial(Stack::new);

        @SuppressWarnings("unchecked")
        FixedAVL(int keys) {
            maps = (AVLTreeMapSTD<Integer, Cell>[]) new AVLTreeMapSTD<?, ?>[Math.min(FIXED_REGIONS, keys)];
            width = 1 + (keys - 1) / maps.length;
            for (int i = 0; i < maps.length; ++i) maps[i] = new AVLTreeMapSTD<>();
        }
        public void insert(Integer key, Cell value) { maps[key / width].put(key, value); }
        public Cell get(Integer key) { return maps[key / width].get(key); }
        public void update(Integer left, Integer right, BiFunction<Integer, Cell, Cell> operation) {
            final int first = left / width, last = right / width;
            int acquired = first;
            try {
                for (; acquired <= last; ++acquired) maps[acquired].lockNoStats();
                Stack<STDAVLNode<Integer, Cell>> stack = traversal.get();
                for (int i = first; i <= last; ++i) {
                    stack.resetStack();
                    maps[i].performOperationToValuesInRange(left, right, operation, stack);
                }
            } finally {
                // Hold every intersecting region through the entire update.
                while (acquired > first) maps[--acquired].unlock();
            }
        }
        public int routeNodes() { return -1; }
        public int fixedRegions() { return maps.length; }
    }

    private static final class Options {
        String implementation = "ca", scenario = "steady_range";
        int threads = 8, keys = 1 << 20, hotWidth = 1 << 16, length = 1000;
        int traceSize = 1 << 16, timeoutSeconds = 600;
        long operations = 1_000_000, seed = 1, warmupOperations = 200_000;
        Path csv;
        boolean selfTest;
    }

    private static long positive(String text, String name) {
        long value = Long.parseLong(text);
        if (value <= 0) throw new IllegalArgumentException(name + " must be positive");
        return value;
    }
    private static int positiveInt(String text, String name) {
        long value = positive(text, name);
        if (value > Integer.MAX_VALUE - 8L) throw new IllegalArgumentException(name + " too large");
        return (int) value;
    }
    private static Options parse(String[] args) {
        Options o = new Options();
        for (int i = 0; i < args.length; ++i) {
            String arg = args[i];
            if (arg.equals("--self-test")) { o.selfTest = true; continue; }
            if (arg.equals("--help")) {
                System.out.println("CATreeRangeBenchmark --implementation ca|fixed_avl "
                        + "--scenario uniform_point|steady_range|islands_range --threads N --keys N "
                        + "--hot-width N --length N --operations N --trace-size N --seed N "
                        + "--warmup-operations N --timeout-seconds N --csv PATH; or --self-test");
                System.exit(0);
            }
            if (++i == args.length) throw new IllegalArgumentException("missing value for " + arg);
            String value = args[i];
            switch (arg) {
            case "--implementation": o.implementation = value; break;
            case "--scenario": o.scenario = value; break;
            case "--threads": o.threads = positiveInt(value, arg); break;
            case "--keys": o.keys = positiveInt(value, arg); break;
            case "--hot-width": o.hotWidth = positiveInt(value, arg); break;
            case "--length": o.length = positiveInt(value, arg); break;
            case "--operations": o.operations = positive(value, arg); break;
            case "--trace-size": o.traceSize = positiveInt(value, arg); break;
            case "--seed": o.seed = Long.parseLong(value); break;
            case "--warmup-operations":
                o.warmupOperations = Long.parseLong(value);
                if (o.warmupOperations < 0) throw new IllegalArgumentException("negative warmup count");
                break;
            case "--timeout-seconds": o.timeoutSeconds = positiveInt(value, arg); break;
            case "--csv": o.csv = Path.of(value); break;
            default: throw new IllegalArgumentException("unknown option " + arg);
            }
        }
        validate(o);
        if (!o.selfTest && o.csv == null) throw new IllegalArgumentException("--csv is required");
        if (o.csv != null && Files.exists(o.csv)) throw new IllegalArgumentException("refusing to overwrite " + o.csv);
        return o;
    }
    private static void validate(Options o) {
        if (!o.implementation.equals("ca") && !o.implementation.equals("fixed_avl"))
            throw new IllegalArgumentException("implementation must be ca or fixed_avl");
        if (!o.scenario.equals("uniform_point") && !o.scenario.equals("steady_range")
                && !o.scenario.equals("islands_range")) throw new IllegalArgumentException("unknown scenario");
        if (o.threads > 1024 || o.traceSize < o.threads || o.operations < o.threads)
            throw new IllegalArgumentException("threads <=1024 and trace/operations >= threads required");
        if (o.keys < 1 || o.hotWidth < 1 || o.hotWidth > o.keys || o.length < 1 || o.length > o.keys)
            throw new IllegalArgumentException("invalid key count, window or query length");
        if (!o.scenario.equals("uniform_point") && o.length > o.hotWidth)
            throw new IllegalArgumentException("range must fit inside hot window");
        if (o.scenario.equals("islands_range") && o.length > o.hotWidth / 16)
            throw new IllegalArgumentException("island query length must fit H/16");
        if (o.warmupOperations != 0 && o.warmupOperations < o.threads)
            throw new IllegalArgumentException("warmup count must be zero or at least worker count");
    }

    private static final class Random {
        long state;
        Random(long seed) { state = seed; }
        long next() {
            long z = (state += 0x9e3779b97f4a7c15L);
            z = (z ^ (z >>> 30)) * 0xbf58476d1ce4e5b9L;
            z = (z ^ (z >>> 27)) * 0x94d049bb133111ebL;
            return z ^ (z >>> 31);
        }
        int below(int n) { return (int) Long.remainderUnsigned(next(), n); }
    }
    private static final class Trace {
        final long operations;
        final int[] left, right;
        Trace(long count, int stored) { operations = count; left = new int[stored]; right = new int[stored]; }
    }
    private static final class Input {
        final Integer[] keys;
        final Trace[] traces;
        final long[] expected;
        long uniqueQueries, hash = 0xcbf29ce484222325L;
        Input(int size, int workers) {
            keys = new Integer[size];
            for (int i = 0; i < size; ++i) keys[i] = Integer.valueOf(i);
            traces = new Trace[workers];
            expected = new long[size];
        }
        void hash(long value) { hash = (hash ^ value) * 0x100000001b3L; }
    }
    private static long share(long total, int index, int parts) {
        return total / parts + (index < total % parts ? 1 : 0);
    }
    private static Input input(Options o) {
        Input in = new Input(o.keys, o.threads);
        int id = o.scenario.equals("uniform_point") ? 0 : o.scenario.equals("steady_range") ? 25 : 27;
        long seed = new Random(o.seed + id * 0x9e3779b97f4a7c15L).next();
        final int[] islandOffsets = {0, 2, 8, 10};
        for (int worker = 0; worker < o.threads; ++worker) {
            long count = share(o.operations, worker, o.threads);
            int stored = (int) Math.min(count, share(o.traceSize, worker, o.threads));
            Trace trace = in.traces[worker] = new Trace(count, stored);
            in.uniqueQueries += stored;
            in.hash(count);
            Random random = new Random(seed ^ (0xd1b54a32d192ed03L * (worker + 1)));
            for (int i = 0; i < stored; ++i) {
                int width, begin, length;
                if (o.scenario.equals("uniform_point")) {
                    width = o.keys; begin = 0; length = 1;
                } else if (o.scenario.equals("islands_range")) {
                    width = o.hotWidth / 16;
                    begin = islandOffsets[random.below(4)] * width;
                    length = o.length;
                } else {
                    width = o.hotWidth; begin = 0; length = o.length;
                }
                int left = begin + random.below(width - length + 1), right = left + length - 1;
                trace.left[i] = left; trace.right[i] = right;
                in.hash(left); in.hash(right);
                long multiplicity = count / stored + (i < count % stored ? 1 : 0);
                in.expected[left] += multiplicity;
                if (right + 1 < o.keys) in.expected[right + 1] -= multiplicity;
            }
        }
        long prefix = 0;
        for (int i = 0; i < o.keys; ++i) in.expected[i] = (prefix += in.expected[i]);
        return in;
    }

    private static MapAccess map(String implementation, Integer[] keys) {
        MapAccess map = implementation.equals("ca") ? new AuthorCA() : new FixedAVL(keys.length);
        for (Integer key : keys) map.insert(key, new Cell());
        return map;
    }
    private static final class GC {
        long count, milliseconds;
        static GC snapshot() {
            GC value = new GC();
            for (GarbageCollectorMXBean bean : ManagementFactory.getGarbageCollectorMXBeans()) {
                if (bean.getCollectionCount() >= 0) value.count += bean.getCollectionCount();
                if (bean.getCollectionTime() >= 0) value.milliseconds += bean.getCollectionTime();
            }
            return value;
        }
    }
    private static long processCpuTime() {
        java.lang.management.OperatingSystemMXBean bean = ManagementFactory.getOperatingSystemMXBean();
        return bean instanceof com.sun.management.OperatingSystemMXBean
                ? ((com.sun.management.OperatingSystemMXBean) bean).getProcessCpuTime() : -1;
    }
    private static final class Result {
        double setupSeconds, wallSeconds, cpuSeconds;
        long gcCount, gcMilliseconds, checksum;
        int routesBefore, routesAfter, regions;
    }
    private static void join(List<Thread> workers, long deadlineNanos) throws InterruptedException {
        for (Thread worker : workers) {
            while (worker.isAlive()) {
                long remaining = deadlineNanos - System.nanoTime();
                if (remaining <= 0) throw new IllegalStateException("worker timeout; native operation may be blocked");
                worker.join(Math.max(1, remaining / 1_000_000));
            }
        }
    }
    private static Result run(Options o, Input in) throws Exception {
        long setup = System.nanoTime();
        MapAccess map = map(o.implementation, in.keys);
        Result result = new Result();
        result.setupSeconds = (System.nanoTime() - setup) / 1e9;
        result.routesBefore = map.routeNodes();
        result.regions = map.fixedRegions();
        if (o.implementation.equals("ca") && result.routesBefore != 0)
            throw new IllegalStateException("fresh uncontended CA population unexpectedly has routing nodes");
        GC gcBefore = GC.snapshot();
        long cpuBefore = processCpuTime();
        CyclicBarrier gate = new CyclicBarrier(o.threads);
        AtomicReference<Throwable> failure = new AtomicReference<>();
        List<Thread> workers = new ArrayList<>();
        long start = System.nanoTime();
        long deadline = start + o.timeoutSeconds * 1_000_000_000L;
        for (int w = 0; w < o.threads; ++w) {
            final Trace trace = in.traces[w];
            Thread worker = new Thread(() -> {
                try {
                    gate.await();
                    int cursor = 0;
                    for (long i = 0; i < trace.operations; ++i) {
                        map.update(in.keys[trace.left[cursor]], in.keys[trace.right[cursor]], INCREMENT);
                        if (++cursor == trace.left.length) cursor = 0;
                    }
                } catch (Throwable error) { failure.compareAndSet(null, error); }
            }, "range-worker-" + w);
            // A broken native method must not leave the harness JVM hanging
            // after an explicit timeout/error; this is not benchmark cancellation.
            worker.setDaemon(true);
            workers.add(worker);
            worker.start();
        }
        join(workers, deadline);
        result.wallSeconds = (System.nanoTime() - start) / 1e9;
        long cpuAfter = processCpuTime();
        result.cpuSeconds = cpuBefore >= 0 && cpuAfter >= 0 ? (cpuAfter - cpuBefore) / 1e9 : Double.NaN;
        GC gcAfter = GC.snapshot();
        result.gcCount = gcAfter.count - gcBefore.count;
        result.gcMilliseconds = gcAfter.milliseconds - gcBefore.milliseconds;
        if (failure.get() != null) throw new IllegalStateException("worker failed", failure.get());
        result.routesAfter = map.routeNodes();
        // Sequential access only: get() values are not used as concurrent range snapshots.
        for (int i = 0; i < o.keys; ++i) {
            Cell cell = map.get(in.keys[i]);
            if (cell == null || cell.value != in.expected[i])
                throw new IllegalStateException("incorrect final value at key " + i + ": expected "
                        + in.expected[i] + ", got " + (cell == null ? "absent" : cell.value));
            result.checksum += cell.value;
        }
        return result;
    }

    private static String quoted(String value) { return "\"" + value.replace("\"", "\"\"") + "\""; }
    private static String number(double value) { return String.format(Locale.ROOT, "%.9f", value); }
    private static void write(Options o, Input in, Result r, double warmupSeconds) throws IOException {
        try (BufferedWriter writer = Files.newBufferedWriter(o.csv, StandardCharsets.UTF_8,
                StandardOpenOption.CREATE_NEW, StandardOpenOption.WRITE)) {
            writer.write("implementation,scenario,threads,keys,hot_width,range_length,operations,trace_size,unique_queries,seed,trace_hash,warmup_operations,warmup_s,setup_s,wall_s,cpu_s,qps,gc_count,gc_ms,route_nodes_before,route_nodes_after,fixed_regions,checksum,verified,java_version,jvm_arguments,measurement\n");
            writer.write(o.implementation + "," + o.scenario + "," + o.threads + "," + o.keys + ","
                    + o.hotWidth + "," + (o.scenario.equals("uniform_point") ? 1 : o.length) + "," + o.operations + ","
                    + o.traceSize + "," + in.uniqueQueries + "," + o.seed + "," + Long.toUnsignedString(in.hash, 16) + ","
                    + o.warmupOperations + "," + number(warmupSeconds) + "," + number(r.setupSeconds) + ","
                    + number(r.wallSeconds) + "," + number(r.cpuSeconds) + "," + number(o.operations / r.wallSeconds) + ","
                    + r.gcCount + "," + r.gcMilliseconds + "," + r.routesBefore + "," + r.routesAfter + "," + r.regions + ","
                    + r.checksum + ",true," + quoted(System.getProperty("java.version")) + ","
                    + quoted(String.join(" ", ManagementFactory.getRuntimeMXBean().getInputArguments())) + ","
                    + quoted("write-only ordered map; operations and adaptation timed; data population, separate-instance JVM warmup and final verification excluded") + "\n");
        }
    }

    private static void check(boolean condition, String message) {
        if (!condition) throw new IllegalStateException(message);
    }
    private static void selfTest() throws Exception {
        Stack<Integer> original = new Stack<>(), copy = new Stack<>();
        original.push(7); original.push(11); copy.copyStateFrom(original);
        check(copy.pop() == 11 && copy.pop() == 7, "author traversal-stack copy failed");
        for (String implementation : new String[]{"fixed_avl", "ca"}) {
            for (String scenario : new String[]{"uniform_point", "steady_range", "islands_range"}) {
                for (int threads : new int[]{1, 4}) {
                    Options o = new Options(); o.implementation = implementation; o.scenario = scenario;
                    o.keys = 4096; o.hotWidth = 512; o.length = 31; o.threads = threads;
                    o.operations = 8191; o.traceSize = 257; o.warmupOperations = 0; o.timeoutSeconds = 20;
                    validate(o);
                    Input in = input(o);
                    long[] replay = new long[o.keys];
                    for (Trace trace : in.traces)
                        for (long i = 0; i < trace.operations; ++i) {
                            int cursor = (int) (i % trace.left.length);
                            for (int key = trace.left[cursor]; key <= trace.right[cursor]; ++key) ++replay[key];
                        }
                    check(java.util.Arrays.equals(replay, in.expected), "cyclic difference-array oracle failed");
                    run(o, in);
                }
            }
            Options whole = new Options(); whole.implementation = implementation;
            whole.keys = whole.hotWidth = whole.length = 257; whole.threads = 4;
            whole.operations = 1003; whole.traceSize = 29; whole.warmupOperations = 0; whole.timeoutSeconds = 20;
            validate(whole); run(whole, input(whole));

            Integer[] keys = new Integer[257];
            for (int i = 0; i < keys.length; ++i) keys[i] = i;
            MapAccess map = map(implementation, keys);
            AtomicBoolean inconsistent = new AtomicBoolean();
            AtomicReference<Throwable> error = new AtomicReference<>();
            CyclicBarrier gate = new CyclicBarrier(3);
            List<Thread> workers = new ArrayList<>();
            for (int worker = 0; worker < 3; ++worker) {
                final boolean observer = worker == 2;
                Thread thread = new Thread(() -> {
                    try {
                        gate.await();
                        final long[] value = {-1};
                        BiFunction<Integer, Cell, Cell> observe = (key, cell) -> {
                            if (value[0] < 0) value[0] = cell.value;
                            else if (value[0] != cell.value) inconsistent.set(true);
                            return cell;
                        };
                        for (int i = 0; i < 500; ++i) {
                            value[0] = -1;
                            // Observation intentionally uses the exclusive update
                            // API, not subSet()+get(), which is not atomic.
                            map.update(keys[0], keys[256], observer ? observe : INCREMENT);
                        }
                    } catch (Throwable failure) { error.compareAndSet(null, failure); }
                });
                thread.setDaemon(true); workers.add(thread); thread.start();
            }
            join(workers, System.nanoTime() + 20_000_000_000L);
            if (error.get() != null) throw new IllegalStateException("atomicity worker failed", error.get());
            check(!inconsistent.get(), "observer saw a partially completed range update");
            for (Integer key : keys) check(map.get(key).value == 1000, "atomicity test final value incorrect");
        }
        System.out.println("PASS cyclic oracle, dense full-array values, inclusive endpoints and atomic range updates for ca/fixed_avl");
    }

    public static void main(String[] args) {
        try {
            Options o = parse(args);
            if (o.selfTest) { selfTest(); return; }
            double warmupSeconds = 0;
            if (o.warmupOperations != 0) {
                long start = System.nanoTime();
                Options warm = new Options(); warm.implementation = o.implementation; warm.scenario = o.scenario;
                warm.keys = 65536; warm.hotWidth = 4096; warm.length = 64; warm.threads = o.threads;
                warm.traceSize = Math.max(o.threads, Math.min(o.traceSize, 65536));
                warm.operations = o.warmupOperations; warm.warmupOperations = 0;
                warm.seed = o.seed ^ 0x632be59bd9b4e019L; warm.timeoutSeconds = o.timeoutSeconds;
                validate(warm); run(warm, input(warm));
                warmupSeconds = (System.nanoTime() - start) / 1e9;
            }
            Input in = input(o);
            Result result = run(o, in);
            write(o, in, result, warmupSeconds);
            System.out.printf(Locale.ROOT, "%s %s: %.3f queries/s, wall=%.6fs, setup=%.6fs, routes=%d->%d, verified=true%n",
                    o.implementation, o.scenario, o.operations / result.wallSeconds,
                    result.wallSeconds, result.setupSeconds, result.routesBefore, result.routesAfter);
        } catch (Throwable error) {
            error.printStackTrace(System.err);
            System.exit(1);
        }
    }
}
