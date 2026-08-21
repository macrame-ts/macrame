<!-- Quick start. Linked from the README. Name decided: Macrame; project and file names are
     now `macrame`. Only the GitHub repo and URLs stay `Andriy06/task_system` until the repo
     moves to macrame-ts/macrame (docs/going-public.md). -->

# Quick start

How to get it running. The [user guide](guide.md) has the full tour of every
layer.

## Get the code

No package registry yet (pre-1.0), but macrame builds as a normal CMake **static
library** with no external dependencies. Clone it, then consume it either way:

```
git clone <repo-url> macrame
```

- **CMake** — `add_subdirectory(macrame)` and link `macrame::macrame`; or install
  it (`cmake --install`) and `find_package(macrame CONFIG REQUIRED)` + link
  `macrame::macrame`.
- **By hand** — add `include/` to your include path and compile the six
  `src/*.cpp` into your build (headers under `include/ts/`, six `.cpp` under `src/`).

C++23, exceptions disabled. Build your own translation units the same way: the
library is exceptions-off, and a consumer must match (the config rides on the
`macrame::macrame` target).

Include everything through the umbrella header, or the individual headers if you
prefer:

```cpp
#include "ts/ts.h"   // the whole public API, in namespace ts
```

## Build

- **Visual Studio 2022+**: open `macrame.slnx` (x64), build, run. MSVC or
  clang-cl.
- **CMake**: presets for `windows-msvc`, `windows-clang-cl`, `windows-shipping`,
  `linux-clang`, and `linux-tsan`.

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
    ts::create_scheduler();        // once, at startup — one worker per hardware thread
    ts::Task<int> t = ts::launch([] { return 6 * 7; });
    std::printf("%d\n", t.sync()); // blocks until done, prints 42
}
```

Bring the scheduler up explicitly with `ts::create_scheduler()` before any work
(it is heavy, so it never starts lazily; a default gives one worker per hardware
thread). It is torn down at program exit, or call `ts::destroy_scheduler()`.
`sync()` is the only call that blocks; launching never does.

## Guard a shared object

Wrap a thread-unsafe object in `Guarded<T>`. The only way in is handing a
function to `access`. The parameter's const-ness declares what you do: a
non-`const` parameter is a write (exclusive), a `const` parameter is a read
(concurrent with other reads). The reference your function receives is valid for
that call only. Storing it past the call sidesteps the safety checks
([limits.md](limits.md) §2.3):

```cpp
ts::Guarded<std::vector<int>> numbers{ "numbers" }; // the name is for diagnostics and traces

numbers.access([](std::vector<int>& v) { v.push_back(1); }).sync(); // write: exclusive
size_t n = numbers.access([](const std::vector<int>& v) // read: concurrent
{
    return v.size();
}).sync();
std::printf("%zu\n", n);
```

Accesses run on the object in submission order: the write runs alone, reads run
together. `access` is *opportunistic*. If the object is free right now it runs
your function immediately on the calling thread (no scheduling); otherwise it
queues. That makes it the right default for the short functions typical of this
API. `access` returns a caller-owned operation handle (`ts::Access_op`), so it
allocates nothing. Take the result with `.sync()`, or `co_await` the handle
from a coroutine. For a heavy function you'd rather not run on the calling
thread, use `async(fn)` instead: same access rules, but always scheduled onto
a worker, returning a free-standing `ts::Task<R>`.

## Make it safe by construction

Add `TS_CHECK_ACCESS()` to your type's methods and the runtime harness will fault
(with a stack trace) if any code touches the object without a declared grant — a
stray reference, a missed declaration, a task the graph doesn't know about:

```cpp
#include "ts/ts.h"

class Inventory
{
public:
    void add(Item i) { TS_CHECK_ACCESS(); items_.push_back(i); }
    size_t count() const { TS_CHECK_ACCESS(); return items_.size(); }
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
