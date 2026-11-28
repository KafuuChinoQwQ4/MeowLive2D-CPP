module;

#include <meowlive/msvc_stl.hpp>

export module meowlive.adapters.http.response_body;

import meowlive.standard;

export namespace meowlive::adapters::http
{
    class ResponseBody
    {
    public:
        explicit ResponseBody(std::size_t limit) : limit_(limit)
        {
        }

        static std::size_t receive(char* data, std::size_t size, std::size_t count,
                                   void* context) noexcept
        {
            auto& body = *static_cast<meowlive::adapters::http::ResponseBody*>(context);
            if (!size || !count)
            {
                return 0;
            }
            if (count > (body.limit_ - body.bytes_.size()) / size)
            {
                body.exceeded_ = true;
                return 0;
            }
            if (!body.first_received_)
            {
                body.first_received_ = std::chrono::steady_clock::now();
            }
            try
            {
                body.bytes_.append(data, size * count);
                return size * count;
            }
            catch (...)
            {
                // libcurl stops on a short write. Rethrow after returning from C.
                body.failure_ = std::current_exception();
                return 0;
            }
        }

        void rethrow_failure() const
        {
            if (failure_)
            {
                std::rethrow_exception(failure_);
            }
        }

        bool exceeded() const noexcept
        {
            return exceeded_;
        }

        const std::string& bytes() const noexcept
        {
            return bytes_;
        }

        std::string take_bytes() noexcept
        {
            return std::move(bytes_);
        }

        std::optional<std::chrono::steady_clock::time_point> first_received() const noexcept
        {
            return first_received_;
        }

    private:
        std::size_t limit_;
        std::string bytes_;
        std::exception_ptr failure_;
        std::optional<std::chrono::steady_clock::time_point> first_received_;
        bool exceeded_{};
    };
}
