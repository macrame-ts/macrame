<!-- Quick start. Linked from the README. Project name undecided (placeholder "task_system"). -->

# Quick start

Get from zero to a running program. For the full tour of every layer, see the
[user guide](guide.md).

## Get the code

No package yet (pre-1.0). Clone the repository and add its `include/` directory
to your include path — the library is a set of headers (under `include/ts/`)
plus a few `.cpp` files (under `src/`), no external dependencies.

```
git clone <repo-url> task_system
```

Include everything through the umbrella header, or the individual headers if you
prefer:

```cpp
#include "ts/ts.h"   // the whole public API, in namespace ts
```

## Build

- **Visual Studio 2022+**: open `task_system.slnx` (x64), build, run. MSVC or
  clang-cl.
- **CMake**: presets for `windows-msvc`, `windows-clang-cl`, and `linux-clang`.

  ```
  cmake --preset windows-msvc
  cmake --build --preset windows-msvc
  ```

C++23 is required. Exceptions are disabled project-wide: non-recoverable failures
call `ts::fatal` (message + stack trace + abort) rather than throwing.

## Hello, task

Launch a unit of work and wait for its result:

```cpp
#include "ts/ts.h"
#include <cstdio>

int main()
{
    ts::Task<int> t = ts::launch([] { return 6 * 7; });
    std::printf("%d\n", t.sync());   // blocks until done, prints 42
}
```

The scheduler starts lazily — one worker thread per hardware thread. `sync()` is
the only call that blocks; launching never does.

## Guard a shared object

Wrap a thread-unsafe object in `Guarded<T>`. The only way in is handing a
function to `access`, and the parameter's const-ness declares what you do — a
non-`const` parameter is a write (exclusive), a `const` parameter is a read
(concurrent with other reads). The reference your function receives is for that
call only; storing it past the call sidesteps the safety checks
([limits.md](limits.md) §2.3):

```cpp
ts::Guarded<std::vector<int>> numbers;

numbers.access([](std::vector<int>& v) { v.push_back(1); });          // write: exclusive
ts::Task<size_t> n = numbers.access([](const std::vector<int>& v)     // read: concurrent
{
    return v.size();
});
std::printf("%zu\n", n.sync());
```

Accesses run on the object in submission order: the write runs alone, reads run
together. `access` is *opportunistic* — if the object is free right now it runs
your function immediately on the calling thread (no scheduling), otherwise it
queues; it is the right default for the short functions typical of this API. For
a heavy function you'd rather not run on the calling thread, use `async(fn)`
instead — same access rules, but always scheduled onto a worker. Both return a
`ts::Task<R>`.

## Make it safe by construction

Add `TS_CHECK_ACCESS()` to your type's methods and the runtime harness will fault
(with a stack trace) if any code touches the object without a declared grant — a
stray reference, a missed declaration, a task the graph doesn't know about:

```cpp
#include "ts/ts.h"

class Inventory
{
public:
    void add(Item i)       { TS_CHECK_ACCESS(); items_.push_back(i); }
    size_t count() const   { TS_CHECK_ACCESS(); return items_.size(); }
private:
    std::vector<Item> items_;
};
```

Now `Guarded<Inventory>` is a thread-safe API for `Inventory`, and violations are
caught the moment they happen instead of racing silently.

## Where to next

- The [user guide](guide.md) — every layer with examples: dependency graphs,
  coroutines, cancellation, `parallel_for`, and the staged-write patterns
  (`Deferred`/`Versioned`).
- The [design rationale](design.md) — why it works this way, and what was tried
  and rejected.
