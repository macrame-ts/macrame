// Scope-based access on `Guarded<T>`. `auto g = co_await ts::read_write(acct)` holds an
// exclusive grant for the guard's lifetime, so a read and the write that depends on it are
// one indivisible step - the linear RAII alternative to a callback `async(fn, obj)`. A tiny
// bank exercises the three shapes:
//   - one object:         accrue interest on a single account under a held write grant
//                         (read-modify-write, atomic against any other writer);
//   - several one-by-one: interest across every account, each in its own scope. Each guard
//                         is released before the next `co_await` - holding two guards across
//                         a suspension is the ABBA shape the harness faults;
//   - several at once:    a transfer reads one balance and writes both accounts, so it needs
//                         them together. The multi-object guard `co_await ts::read_write(a, b)`
//                         holds both for the scope, acquired in canonical pipe-address order
//                         (deadlock-free); structured bindings alias the two accounts.
// Runs twice: transfers must conserve the total, and both runs must agree.

#include "ts/guarded.h"
#include "ts/coroutine_support.h"

#include <cstdio>
#include <vector>

namespace sample
{

namespace
{

// A plain thread-unsafe account, instrumented like any guarded system: every method asserts
// it runs under a grant on `this` (the scope guard, or the multi-object access, installs it).
class Account
{
public:
    long balance() const { TS_CHECK_ACCESS(); return balance_; }
    void set_balance(long b) { TS_CHECK_ACCESS(); balance_ = b; }
    void deposit(long amount) { TS_CHECK_ACCESS(); balance_ += amount; }

    bool withdraw(long amount)
    {
        TS_CHECK_ACCESS();
        if (balance_ < amount)
            return false;
        balance_ -= amount;
        return true;
    }

    // Read-modify-write: the interest depends on the balance read a line earlier, so both
    // must happen under one grant. `rate_bps` is basis points (500 = 5%).
    void accrue(int rate_bps) { TS_CHECK_ACCESS(); balance_ += balance_ * rate_bps / 10000; }

private:
    long balance_ = 0;
};

using Bank = std::vector<ts::Guarded<Account>*>;

// One object, scope-based: the guard holds the write grant across the read-modify-write, so no
// other writer can slip between reading the balance and applying the interest.
ts::Task<void> accrue(ts::Guarded<Account>& acct, int rate_bps)
{
    auto a = co_await ts::read_write(acct);
    a->accrue(rate_bps);
}

// Several accounts one-by-one: each `accrue` takes and releases its own guard, so the loop
// never holds two grants across a `co_await`.
ts::Task<void> accrue_all(Bank bank, int rate_bps)
{
    for (ts::Guarded<Account>* acct : bank)
        co_await accrue(*acct, rate_bps);
}

// Two accounts at once: a transfer withdraws from one and deposits to the other as a single
// step, so it holds both grants together. The multi-object scope guard acquires them in
// canonical (pipe-address) order - deadlock-free against any other multi-object guard, access,
// or graph node - and structured bindings give the two accounts by reference for the scope.
ts::Task<bool> transfer(ts::Guarded<Account>& from, ts::Guarded<Account>& to, long amount)
{
    auto [f, t] = co_await ts::read_write(from, to);
    if (!f.withdraw(amount))
        co_return false;
    t.deposit(amount);
    co_return true;
}

// Sum every balance through one-by-one read guards (the shared-reader form: `read_only` gives
// `const T&`, and concurrent readers do not exclude each other).
ts::Task<long> total(Bank bank)
{
    long sum = 0;
    for (ts::Guarded<Account>* acct : bank)
    {
        auto a = co_await ts::read_only(*acct);
        sum += a->balance();
    }
    co_return sum;
}

struct Totals
{
    long before = 0;
    long after_transfers = 0;
    long after_interest = 0;

    bool operator==(const Totals&) const = default;
};

ts::Task<Totals> run_bank(Bank bank)
{
    Totals t;

    // Seed 1000, 2000, 3000 through single-object write guards, one at a time.
    long seed = 1000;
    for (ts::Guarded<Account>* acct : bank)
    {
        auto a = co_await ts::read_write(*acct);
        a->set_balance(seed);
        seed += 1000;
    }
    t.before = co_await total(bank);

    // Multi-object transfers: money moves between accounts, so the total is unchanged.
    co_await transfer(*bank[0], *bank[1], 500);
    co_await transfer(*bank[1], *bank[2], 250);
    t.after_transfers = co_await total(bank);

    // One-by-one interest at 5%.
    co_await accrue_all(bank, 500);
    t.after_interest = co_await total(bank);
    co_return t;
}

Totals bank_once()
{
    ts::Guarded<Account> alice{ ts::Named{"alice"} };
    ts::Guarded<Account> bob{ ts::Named{"bob"} };
    ts::Guarded<Account> carol{ ts::Named{"carol"} };
    Bank bank{ &alice, &bob, &carol };

    // `sync()` at the boundary: `bank_once` is not a task, so blocking here is legal, and it
    // keeps the accounts alive until the frame completes (they outlive the coroutine).
    return run_bank(bank).sync();
}

} // namespace

void run_scope_access_sample()
{
    Totals a = bank_once();
    Totals b = bank_once();

    bool conserved = a.before == a.after_transfers;
    std::printf("scope_access sample: total %ld -> after transfers %ld (%s) -> after 5%% interest %ld, %s\n",
        a.before, a.after_transfers, conserved ? "conserved" : "not conserved (bug)",
        a.after_interest, a == b ? "deterministic across runs" : "differs between runs (bug)");
}

} // namespace sample
