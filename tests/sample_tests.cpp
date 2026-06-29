#include "sample_tests.h"
#include "task.h"

#include <iostream>
#include <thread>

static void print_task(const char* task_name)
{
    std::cout << std::this_thread::get_id() << ": " << task_name << "\n";
}

void run_sample_tests()
{
    Scheduler scheduler;

    Static_task static_task{ []{ print_task("static task 1"); } };
    static_task.submit(scheduler);

    Static_task static_task1{ [] { print_task("static task 2"); } };
    static_task1.submit(scheduler);

    Dynamic_task dynamic_task{ []{ print_task("dynamic task"); }, Priority::high };
    dynamic_task.submit(scheduler);

    scheduler.submit([](void*) { print_task("raw task"); }, nullptr);

    Awaitable_task awaitable_task{ []{ print_task("awaitable task"); } };
    awaitable_task.submit(scheduler);
    awaitable_task.wait();

    Ref_counted_task ref_counted_task{ []{ print_task("ref-counted task"); } };
    ref_counted_task->submit(scheduler);

    {
        Dependable_task prereq_task{ []{ print_task("prereq task 1"); } };
        prereq_task->then(scheduler, { []{ print_task("subseq task 1"); } });
        prereq_task->submit(scheduler);
    }

    {
        Dependable_task prereq_task{ []{ print_task("prereq task 2"); } };
        prereq_task->submit(scheduler);
        prereq_task->then(scheduler, { []{ print_task("subseq task 2"); } });
    }

    {
        Dependable_task prereq_task{ []{ print_task("prereq task 3"); } };
        Dependable_task subseq_task{ []{ print_task("subseq task 3"); } };
        prereq_task->then(scheduler, subseq_task);
        Dependable_task subseq_task2{ []{ print_task("subseq task 3.1"); } };
        prereq_task->then(scheduler, subseq_task2);
        prereq_task->submit(scheduler);
    }

    {
        Dependable_task prereq_task{ []{ print_task("prereq task 4"); } };
        prereq_task->submit(scheduler);
        Dependable_task subseq_task{ []{ print_task("subseq task 4"); } };
        prereq_task->then(scheduler, subseq_task);
        Dependable_task subseq_task2{ []{ print_task("subseq task 4.1"); } };
        prereq_task->then(scheduler, subseq_task2);
    }
}
