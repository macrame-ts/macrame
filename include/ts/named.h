#pragma once

// `ts::Named` -- the one debug-identity type, for graph nodes, guarded objects and tasks.
//
// A name is either a static-storage literal or the CALL SITE that created the entity:
//
//   graph.add_node(ts::Named{ "physics" }, &tick_physics, world.bodies);   // or "physics"
//   ts::Guarded<Physics_world> world{ ts::Named{ "physics" }, entity_count };
//   auto t = ts::launch(fn);                                    // unnamed -> the call site
//   auto t = ts::launch(fn, { .name = "stream_textures" });     // named
//
// `ts::Named{}` is the deliberate "identify me by where I am written" spelling: the
// defaulted `std::source_location` argument is evaluated at the point of the `Named{}`
// expression, so the site is the user's, not this header's.
//
// THE RULE for library code: a defaulted `source_location` captures the CALLER of the
// function that declares it, so it must be declared on the OUTERMOST function the user
// calls. Capture once at each public boundary and pass the resulting `Named` explicitly
// down every internal layer -- an inner helper with its own defaulted parameter captures
// the library header instead, and the name is worthless. `tests/named_tests.cpp` asserts
// that captured names point into the test file; that test is the whole guarantee.
//
// The literal is referenced, not copied -- pass a literal, or something that outlives the
// entity. Only `file` and `line` are kept from the `source_location` (never
// `function_name`), so a `Named` is three words.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <source_location>

namespace ts
{

struct Named
{
    // From a literal, at the call site. Implicit, so `add_node("physics", ...)` works
    // bare; the types that REQUIRE a name constrain their parameter to a real `Named` so
    // an accidental `Guarded<std::string> g{"hello"}` cannot silently become a name.
    Named(const char* name, std::source_location site = std::source_location::current()) noexcept
        : literal(name)
        , file(site.file_name())
        , line(static_cast<std::uint32_t>(site.line()))
    {}

    // The `{}` form: identify by call site alone. Also the conversion used when a public
    // boundary forwards its own defaulted `source_location`.
    Named(std::source_location site = std::source_location::current()) noexcept
        : file(site.file_name())
        , line(static_cast<std::uint32_t>(site.line()))
    {}

    // Internal: "no identity recorded". Written `ts::Named{ nullptr }`; never produced by
    // naming an entity, so `empty()` distinguishes an un-named internal block from one the
    // user identified by site.
    constexpr Named(std::nullptr_t) noexcept {}

    bool empty() const noexcept { return literal == nullptr && file == nullptr; }

    const char* literal = nullptr;   // null when identified by call site
    const char* file = nullptr;      // null only for the internal empty state
    std::uint32_t line = 0;
};

namespace detail
{

// Last path component of `path` (both separators), for display.
inline const char* path_basename(const char* path) noexcept
{
    const char* base = path;
    for (const char* p = path; *p; ++p)
    {
        if (*p == '/' || *p == '\\')
            base = p + 1;
    }
    return base;
}

} // namespace detail

// Render `name` for a diagnostic: the literal when it has one, else `basename(file):line`,
// else `fallback`. Returns a pointer valid as long as `buf` is (or the literal itself).
inline const char* named_display(const Named& name, char* buf, std::size_t size,
                                 const char* fallback = "<unnamed>") noexcept
{
    if (name.literal != nullptr)
        return name.literal;
    if (name.file == nullptr)
        return fallback;
    std::snprintf(buf, size, "%s:%u", detail::path_basename(name.file), name.line);
    return buf;
}

} // namespace ts
