<!-- Quick start. Linked from the README. -->

# Quick start

This guide explains how to get the framework running. For a complete tour of every layer, see the [user guide](guide.md).

## Get the code

Macrame is not published to a package registry yet (pre-1.0). However, it builds as a standard CMake static library with no external dependencies. You can clone the repository and consume it in one of two ways.

```text
git clone https://github.com/macrame-ts/macrame.git macrame
```

*   **CMake.** You can use `add_subdirectory(macrame)` and link `macrame::macrame`. Alternatively, you can install it using `cmake --install`, then find it with `find_package(macrame CONFIG REQUIRED)` and link `macrame::macrame`. A minimal consumer project looks like this:

    ```cmake
    cmake_minimum_required(VERSION 3.25)
    project(app LANGUAGES CXX)

    add_subdirectory(macrame)   # or, after cmake --install: find_package(macrame CONFIG REQUIRED)

    add_executable(app main.cpp)
    target_link_libraries(app PRIVATE macrame::macrame)
    ```
*   **By hand.** Add the `include/` directory to your include path and compile the library `.cpp` files located under `src/` into your build. These files are `access`, `fatal`, `guarded`, `scheduler`, `static_task_graph`, and `worker_thread`. The headers are located under `include/ts/`. Other source files like `main.cpp` and `mem_profile.cpp` belong to the development driver, not the library itself.

The library requires C++23. It never throws exceptions, and it does not require your code to use them. You can compile your translation units with exceptions enabled or disabled based on your preference. The only strict rule is that an exception must not escape a task body, as this will result in a fatal error (see guide section 10.5). If you want to build Macrame itself without exception support for an entirely exceptions-off program, configure it with `-DMACRAME_NO_EXCEPTIONS=ON`. The setting rides on the `macrame::macrame` target as a usage requirement, because on MSVC it has to be program-wide.

You can include the entire library through the umbrella header, or use individual headers if preferred.

```cpp
#include "ts/ts.h" // Includes the whole public API in the ts namespace.
```

## Build

*   **Visual Studio 2022 and later.** Open the `macrame.slnx` solution file (x64) and build; the projects use the clang-cl toolset. MSVC builds via the CMake presets.
*   **CMake.** Presets are available for `windows-msvc`, `windows-clang-cl`, `windows-shipping`, `linux-clang`, and `linux-tsan`.

```text
cmake --preset windows-msvc
cmake --build --preset windows-msvc

cmake --preset linux-clang        # on Linux
cmake --build --preset linux-clang
```

As a reminder, C++23 is required. Non-recoverable failures call `ts::fatal`, which prints a message and stack trace before aborting, rather than throwing an exception — the library never throws in any configuration.

## Hello, task

Here is how you launch a unit of work and wait for its result.

```cpp
#include "ts/ts.h"
#include <cstdio>

int main()
{
    ts::create_scheduler(); // Bring the scheduler up once at startup. The default creates one worker per hardware thread.
    ts::Task<int> t = ts::launch([] { return 6 * 7; });
    std::printf("%d\n", t.sync()); // Blocks until the task is done, then prints 42.
}
```

You must bring the scheduler up explicitly with `ts::create_scheduler()` before executing any work. Because initialization is heavy, it never starts lazily. It is torn down automatically at program exit, or you can manually call `ts::destroy_scheduler()`. The `sync()` method is the only call that blocks. Launching a task never blocks.

## Guard a shared object

You can wrap a thread-unsafe object in `Guarded<T>`. The only way to interact with the object is by handing a function. The parameter's const-ness declares your intent: a non-const parameter requests exclusive write access, while a const parameter requests concurrent read access. The reference your function receives is valid only for that specific call. Storing it past the call will sidestep the safety checks.

```cpp
ts::Guarded<std::vector<int>> numbers{ ts::Named{"numbers"} }; // The name is used for diagnostics and traces.

numbers.access([](std::vector<int>& v) { v.push_back(1); }).sync(); // Exclusive write access.

size_t n = numbers.access([](const std::vector<int>& v) // Concurrent read access.
{
    return v.size();
}).sync();

std::printf("%zu\n", n);
```

Access requests are processed in submission order. Writes run alone, and reads run together. The `access` method is opportunistic. If the object is currently free, it runs your function immediately on the calling thread without scheduling it. This makes it the ideal default for short functions. The method returns a caller-owned operation handle (`ts::Access_op`), meaning it requires zero allocations. You can retrieve the result using `.sync()`, or by awaiting the handle from a coroutine.

If you have a heavy function that you prefer not to run on the calling thread, you can use `async(fn)` instead. It follows the same access rules but is always scheduled onto a worker thread and returns a free-standing `ts::Task<R>`.

## Make it safe by construction

You can add the `TS_CHECK_ACCESS()` macro to your type's methods. The runtime harness will then fault with a stack trace if any code touches the object without a declared grant. This catches issues like stray references, missed declarations, or tasks that the graph does not know about.

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

By doing this, `Guarded<Inventory>` becomes a thread-safe API for the `Inventory` class. Any access violations are caught the moment they happen instead of silently causing race conditions.

## Where to next

*   The [user guide](guide.md) contains examples for every layer, covering dependency graphs, coroutines, cancellation, `parallel_for`, and staged-write patterns like `Deferred` and `Versioned`.
*   The [design rationale](design.md) explains why the framework works this way, as well as what alternative approaches were tried and rejected.
