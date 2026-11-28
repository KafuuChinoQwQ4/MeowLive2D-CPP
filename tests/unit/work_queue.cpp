#include <cstdio>
#include <meowlive/msvc_stl.hpp>

import meowlive.standard;

import meowlive.application.work_queue;

namespace
{
    void require(bool condition, std::string_view message)
    {
        if (!condition)
        {
            throw std::runtime_error(std::string(message));
        }
    }

    void check_bounds_and_drain()
    {
        std::array<int, 3> order{};
        std::size_t position{};
        std::binary_semaphore first_started(0);
        std::binary_semaphore finish_first(0);
        std::binary_semaphore second_started(0);
        std::binary_semaphore finish_second(0);
        {
            meowlive::application::WorkQueue queue(2);
            const auto first = queue.try_submit(
                [&]() noexcept
                {
                    first_started.release();
                    finish_first.acquire();
                    order.at(position++) = 1;
                });
            const auto first_running = first_started.try_acquire_for(std::chrono::seconds(5));
            const auto second = queue.try_submit(
                [&]() noexcept
                {
                    second_started.release();
                    finish_second.acquire();
                    order.at(position++) = 2;
                });
            meowlive::application::WorkQueue::Task third =
                [&, value = std::make_unique<int>(3)]() noexcept
            {
                order.at(position++) = *value;
            };
            const auto overflow = queue.try_submit(std::move(third));
            const auto retained = static_cast<bool>(third);
            finish_first.release();
            const auto second_running = second_started.try_acquire_for(std::chrono::seconds(5));
            const auto retry = retained ? queue.try_submit(std::move(third)) : overflow;
            queue.close();
            queue.close();
            const auto after_close = queue.try_submit(
                []() noexcept
                {
                });
            finish_second.release();

            require(first == meowlive::application::WorkSubmission::Accepted && first_running,
                    "first task starts on the worker");
            require(second == meowlive::application::WorkSubmission::Accepted && second_running,
                    "second task follows the first");
            require(overflow == meowlive::application::WorkSubmission::Full && retained,
                    "the running task counts toward the capacity and rejection retains ownership");
            require(retry == meowlive::application::WorkSubmission::Accepted,
                    "completion opens a slot for a rejected task");
            require(after_close == meowlive::application::WorkSubmission::Closed,
                    "closed queue rejects submissions");
        }
        require(order == std::array{1, 2, 3},
                "destruction waits for all accepted tasks in FIFO order");
    }

    void check_implicit_shutdown()
    {
        bool ran{};
        {
            meowlive::application::WorkQueue queue(1);
            require(queue.try_submit(
                        [&]() noexcept
                        {
                            ran = true;
                        }) == meowlive::application::WorkSubmission::Accepted,
                    "queue accepts work before implicit shutdown");
        }
        require(ran, "destruction closes the queue and waits for accepted work");
    }
}

int main()
{
    try
    {
        check_bounds_and_drain();
        check_implicit_shutdown();
        return 0;
    }
    catch (const std::exception& error)
    {
        std::println(stderr, "{}", error.what());
        return 1;
    }
}
