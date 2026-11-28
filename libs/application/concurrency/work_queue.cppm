module;

#include <meowlive/msvc_stl.hpp>

export module meowlive.application.work_queue;

import meowlive.standard;

export namespace meowlive::application
{
    enum class WorkSubmission
    {
        Accepted,
        Full,
        Closed,
    };

    class WorkQueue
    {
    public:
        using Task = std::move_only_function<void() noexcept>;

        explicit WorkQueue(std::size_t capacity) : capacity_(capacity)
        {
            if (capacity_ == 0)
            {
                throw std::invalid_argument("Work queue capacity must be positive");
            }
            worker_ = std::jthread(
                [this]
                {
                    run();
                });
        }

        ~WorkQueue()
        {
            close();
        }

        // 容量包括正在执行的任务。入队成功时才接管任务。
        [[nodiscard]] meowlive::application::WorkSubmission try_submit(Task&& task)
        {
            if (!task)
            {
                throw std::invalid_argument("Work queue task must be callable");
            }
            const std::lock_guard lock(mutex_);
            if (closed_)
            {
                return meowlive::application::WorkSubmission::Closed;
            }
            if (outstanding_ == capacity_)
            {
                return meowlive::application::WorkSubmission::Full;
            }
            pending_.push_back(std::move(task));
            ++outstanding_;
            ready_.notify_one();
            return meowlive::application::WorkSubmission::Accepted;
        }

        // 关闭提交入口，已接收的工作按入队顺序执行完毕。析构时等待工作线程退出。
        void close()
        {
            const std::lock_guard lock(mutex_);
            closed_ = true;
            ready_.notify_one();
        }

        // 由拥有队列的线程调用，等待此前接收的工作执行完毕。
        void drain()
        {
            close();
            if (worker_.joinable())
            {
                worker_.join();
            }
        }

    private:
        Task take_next()
        {
            std::unique_lock lock(mutex_);
            ready_.wait(lock,
                        [this]
                        {
                            return closed_ || !pending_.empty();
                        });
            if (pending_.empty())
            {
                return {};
            }
            auto task = std::move(pending_.front());
            pending_.pop_front();
            return task;
        }

        void run()
        {
            while (auto task = take_next())
            {
                task();
                const std::lock_guard lock(mutex_);
                --outstanding_;
            }
        }

        const std::size_t capacity_;
        std::mutex mutex_;
        std::condition_variable ready_;
        std::deque<Task> pending_;
        std::size_t outstanding_{};
        bool closed_{};
        std::jthread worker_;
    };
}
