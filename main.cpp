#include "task.h"

#include <iostream>

int main()
{
    Scheduler scheduler;

    Static_task static_task{ []{ std::cout << "static task\n"; } };
    static_task.submit(scheduler);

    Dynamic_task dynamic_task{ []{ std::cout << "dynamic task\n"; }, Priority::high };
    dynamic_task.submit(scheduler);

    scheduler.submit([](void*) { std::cout << "row task\n"; }, nullptr);

    Awaitable_task awaitable_task{ []{ std::cout << "awaitable task\n"; } };
    awaitable_task.submit(scheduler);
    awaitable_task.wait();

    Ref_counted_task ref_counted_task{ []{ std::cout << "ref-counted task\n"; } };
    ref_counted_task->submit(scheduler);

    {
        Dependable_task prereq_task{ []{ std::cout << "prereq task 1\n"; } };
        prereq_task->then(scheduler, {[]{ std::cout << "subseq task 1\n"; } });
        prereq_task->submit(scheduler);
    }

    {
        Dependable_task prereq_task{ []{ std::cout << "prereq task 2\n"; } };
        prereq_task->submit(scheduler);
        prereq_task->then(scheduler, {[]{ std::cout << "subseq task 2\n"; } });
    }
}
