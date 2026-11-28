module;

#include <json/json.h>
#include <cstdio>

#include <meowlive/msvc_stl.hpp>

export module meowlive.adapters.storage.reply_outbox;

import meowlive.standard;

import meowlive.adapters.storage.atomic_file;
import meowlive.adapters.storage.postgres;
import meowlive.application.reply_completion;
import meowlive.platform.exception_diagnostics;
import meowlive.protocol.json;
import meowlive.protocol.utf8;

namespace
{
    namespace app = meowlive::application;

    constexpr std::size_t outbox_record_limit = 64 * 1024;
    constexpr std::size_t outbox_event_limit = 100;
    constexpr std::size_t outbox_capacity = 128;

    struct ReplySlot
    {
        app::ReplyCompletion completion;
        std::uint64_t sequence;
        bool confirmed{};
        bool shutdown_write_failed{};
    };

    struct ReplyBuffer
    {
        std::mutex mutex;
        std::condition_variable_any ready;
        std::deque<ReplySlot> records;
        std::uint64_t sequence{};
        bool closed{};
    };

    class Reservation final : public app::ReplyReservation
    {
    public:
        Reservation(std::shared_ptr<ReplyBuffer> buffer, std::uint64_t sequence)
            : buffer_(std::move(buffer)), sequence_(sequence)
        {
        }

        ~Reservation() override
        {
            if (buffer_)
            {
                const std::lock_guard lock(buffer_->mutex);
                std::erase_if(buffer_->records,
                              [this](const auto& record)
                              {
                                  return record.sequence == sequence_;
                              });
            }
        }

        void confirm(std::uint64_t completed_at_ms) noexcept override
        {
            auto buffer = std::move(buffer_);
            if (!buffer)
            {
                return;
            }
            const std::lock_guard lock(buffer->mutex);
            const auto slot = std::ranges::find(buffer->records, sequence_, &ReplySlot::sequence);
            if (buffer->closed)
            {
                buffer->records.erase(slot);
                return;
            }
            slot->completion.completed_at_ms = completed_at_ms;
            slot->confirmed = true;
            buffer->ready.notify_one();
        }

    private:
        std::shared_ptr<ReplyBuffer> buffer_;
        std::uint64_t sequence_;
    };

    ::Json::Value encode_record(const std::string& scope,
                                const meowlive::application::ReplyCompletion& completion)
    {
        ::Json::Value document(::Json::objectValue);
        document["schema"] = 1u;
        document["scope_id"] = scope;
        document["speech_id"] = completion.speech_id;
        document["generated_at_ms"] = static_cast<::Json::UInt64>(completion.generated_at_ms);
        document["completed_at_ms"] = static_cast<::Json::UInt64>(completion.completed_at_ms);
        ::Json::Value events(::Json::arrayValue);
        for (const auto& key : completion.events)
        {
            ::Json::Value event(::Json::objectValue);
            event["source"] = key.source;
            event["id"] = key.id;
            events.append(std::move(event));
        }
        document["events"] = std::move(events);
        return document;
    }

    std::optional<meowlive::application::ReplyCompletion> decode_record(const ::Json::Value& record)
    {
        const auto& events = record["events"];
        if (!record["schema"].isUInt() || record["schema"].asUInt() != 1u ||
            !record["speech_id"].isString() || record["speech_id"].asString().empty() ||
            !record["generated_at_ms"].isUInt64() || !record["completed_at_ms"].isUInt64() ||
            !events.isArray() || events.empty() || events.size() > outbox_event_limit)
        {
            return {};
        }
        meowlive::application::ReplyCompletion completion{
            .speech_id = record["speech_id"].asString(),
            .generated_at_ms = record["generated_at_ms"].asUInt64(),
            .completed_at_ms = record["completed_at_ms"].asUInt64()};
        for (const auto& event : events)
        {
            if (!event.isObject() || !event["source"].isString() || !event["id"].isString() ||
                event["source"].asString().empty() || event["id"].asString().empty())
            {
                return {};
            }
            completion.events.push_back({event["source"].asString(), event["id"].asString()});
        }
        return completion;
    }

    std::optional<::Json::Value> read_record(const std::filesystem::path& path)
    {
        std::ifstream input(path, std::ios::binary);
        std::string bytes(outbox_record_limit + 1, '\0');
        input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        if (!input.eof() || input.bad() ||
            input.gcount() > static_cast<std::streamsize>(outbox_record_limit))
        {
            return {};
        }
        bytes.resize(static_cast<std::size_t>(input.gcount()));
        return meowlive::protocol::json::parse_object(bytes);
    }

    bool valid_key(std::string_view key)
    {
        return !key.empty() && key.size() <= outbox_record_limit &&
               key.find('\0') == std::string_view::npos && meowlive::protocol::is_utf8(key);
    }
}

export namespace meowlive::adapters::storage
{
    class ReplyOutbox
    {
    public:
        using Settle = std::function<void(const meowlive::application::ReplyCompletion&)>;
        using Report = std::function<void(std::string_view)>;

        ReplyOutbox(std::filesystem::path directory, std::filesystem::path recovery_directory,
                    std::string scope, std::string instance_id, Settle settle, Report report = {})
            : directory_(std::move(directory)), recovery_directory_(std::move(recovery_directory)),
              scope_(std::move(scope)), instance_id_(std::move(instance_id)),
              settle_(std::move(settle)), report_(std::move(report)),
              replay_worker_(
                  [this](std::stop_token stop)
                  {
                      replay(stop);
                  }),
              write_worker_(
                  [this](std::stop_token stop)
                  {
                      write_pending(stop);
                  })
        {
        }

        ~ReplyOutbox()
        {
            (void)close();
        }

        // Call from the owning thread after the speech worker releases unconfirmed reservations.
        [[nodiscard]] bool close()
        {
            {
                const std::lock_guard lock(buffer_->mutex);
                if (buffer_->closed)
                {
                    return !std::ranges::any_of(buffer_->records, &ReplySlot::confirmed);
                }
                buffer_->closed = true;
            }
            write_worker_.request_stop();
            write_worker_.join();
            replay_worker_.request_stop();
            replay_worker_.join();
            std::size_t pending{};
            {
                const std::lock_guard lock(buffer_->mutex);
                pending = static_cast<std::size_t>(
                    std::ranges::count_if(buffer_->records, &ReplySlot::confirmed));
            }
            if (pending != 0)
            {
                std::fprintf(stderr, "reply_completion_outbox_shutdown_pending count=%zu\n",
                             pending);
                note("reply_completion_outbox_shutdown_pending");
            }
            return pending == 0;
        }

        app::ReplyReserveResult reserve(meowlive::application::ReplyCompletion completion)
        {
            if (!valid_key(completion.speech_id) || completion.events.empty() ||
                completion.events.size() > outbox_event_limit ||
                std::ranges::any_of(completion.events,
                                    [](const auto& event)
                                    {
                                        return !valid_key(event.source) || !valid_key(event.id);
                                    }))
            {
                return std::unexpected(app::ReplyReservationError::Invalid);
            }
            // Reserve room for the largest possible completion timestamp before admitting speech.
            completion.completed_at_ms = std::numeric_limits<std::uint64_t>::max();
            if (meowlive::protocol::json::serialize(encode_record(scope_, completion)).size() >
                outbox_record_limit)
            {
                return std::unexpected(app::ReplyReservationError::Invalid);
            }
            const std::lock_guard lock(buffer_->mutex);
            if (buffer_->closed)
            {
                return std::unexpected(app::ReplyReservationError::Closed);
            }
            if (buffer_->records.size() >= outbox_capacity)
            {
                return std::unexpected(app::ReplyReservationError::Full);
            }
            const auto sequence = ++buffer_->sequence;
            buffer_->records.push_back({std::move(completion), sequence});
            try
            {
                return std::make_unique<Reservation>(buffer_, sequence);
            }
            catch (...)
            {
                buffer_->records.pop_back();
                throw;
            }
        }

    private:
        void note(std::string_view code) const noexcept
        try
        {
            if (report_)
            {
                report_(code);
            }
        }
        catch (...)
        {
            const auto kind = meowlive::platform::log_current_exception(
                "reply.outbox.report", meowlive::platform::ExceptionDetails::Omit);
            if (kind == meowlive::platform::ExceptionKind::OutOfMemory)
            {
                std::terminate();
            }
        }

        bool persist(const std::filesystem::path& directory, std::uint64_t sequence,
                     const meowlive::application::ReplyCompletion& completion)
        try
        {
            const auto base = instance_id_ + "-" + std::to_string(sequence);
            const auto bytes =
                meowlive::protocol::json::serialize(encode_record(scope_, completion));
            return meowlive::adapters::storage::write_atomic_file(
                directory / (base + ".json"), directory / (base + ".tmp"), bytes);
        }
        catch (const std::filesystem::filesystem_error&)
        {
            meowlive::platform::log_current_exception("reply.outbox.persist",
                                                      meowlive::platform::ExceptionDetails::Omit);
            return false;
        }

        void notify_replay()
        {
            const std::lock_guard lock(wait_mutex_);
            changed_ = true;
            ready_.notify_one();
        }

        void write_pending(std::stop_token stop)
        try
        {
            const auto next = [this]
            {
                return std::ranges::find_if(buffer_->records,
                                            [](const auto& slot)
                                            {
                                                return slot.confirmed &&
                                                       !slot.shutdown_write_failed;
                                            });
            };
            for (;;)
            {
                std::unique_lock lock(buffer_->mutex);
                if (!buffer_->ready.wait(lock, stop,
                                         [&]
                                         {
                                             return next() != buffer_->records.end();
                                         }))
                {
                    return;
                }
                const auto record = *next();
                lock.unlock();
                const bool closing = stop.stop_requested();
                bool persisted = persist(directory_, record.sequence, record.completion);
                if (!persisted)
                {
                    note("reply_completion_outbox_write_failed");
                    if (closing)
                    {
                        persisted =
                            persist(recovery_directory_, record.sequence, record.completion);
                        if (persisted)
                        {
                            note("reply_completion_outbox_saved_to_recovery");
                        }
                        else
                        {
                            note("reply_completion_outbox_recovery_write_failed");
                            lock.lock();
                            auto slot = std::ranges::find(buffer_->records, record.sequence,
                                                          &ReplySlot::sequence);
                            slot->shutdown_write_failed = true;
                            lock.unlock();
                        }
                    }
                }
                if (persisted)
                {
                    lock.lock();
                    buffer_->records.erase(
                        std::ranges::find(buffer_->records, record.sequence, &ReplySlot::sequence));
                    lock.unlock();
                    notify_replay();
                }
                else if (!closing)
                {
                    lock.lock();
                    buffer_->ready.wait_for(lock, stop, std::chrono::seconds(1),
                                            []
                                            {
                                                return false;
                                            });
                }
            }
        }
        catch (...)
        {
            meowlive::platform::log_current_exception("reply.outbox.write",
                                                      meowlive::platform::ExceptionDetails::Omit);
            std::terminate();
        }

        void settle_record(const std::filesystem::path& path)
        {
            const auto record = read_record(path);
            if (!record || !(*record)["scope_id"].isString())
            {
                note("reply_completion_outbox_record_invalid");
                return;
            }
            if ((*record)["scope_id"].asString() != scope_)
            {
                return;
            }
            const auto completion = decode_record(*record);
            if (!completion)
            {
                note("reply_completion_outbox_record_invalid");
                return;
            }
            settle_(*completion);
            std::error_code error;
            std::filesystem::remove(path, error);
            if (error)
            {
                note("reply_completion_outbox_ack_failed");
            }
        }

        void replay_directory(const std::filesystem::path& directory, std::stop_token stop)
        {
            std::error_code error;
            auto entry = std::filesystem::directory_iterator(directory, error);
            const std::filesystem::directory_iterator end;
            while (!stop.stop_requested() && !error && entry != end)
            {
                try
                {
                    const auto& path = entry->path();
                    std::error_code status_error;
                    if (path.extension() == ".json" &&
                        std::filesystem::is_regular_file(entry->symlink_status(status_error)))
                    {
                        settle_record(path);
                    }
                    if (status_error && status_error != std::errc::no_such_file_or_directory)
                    {
                        note("reply_completion_outbox_read_failed");
                    }
                }
                catch (const meowlive::adapters::storage::PostgresUnavailable&)
                {
                    meowlive::platform::log_current_exception("reply.outbox.settle");
                }
                entry.increment(error);
            }
            if (error && error != std::errc::no_such_file_or_directory)
            {
                note("reply_completion_outbox_read_failed");
            }
        }

        void replay(std::stop_token stop)
        try
        {
            while (!stop.stop_requested())
            {
                replay_directory(directory_, stop);
                replay_directory(recovery_directory_, stop);
                std::unique_lock lock(wait_mutex_);
                ready_.wait_for(lock, stop, std::chrono::seconds(1),
                                [this]
                                {
                                    return changed_;
                                });
                changed_ = false;
            }
        }
        catch (const meowlive::adapters::storage::PostgresError&)
        {
            meowlive::platform::log_current_exception("reply.outbox.replay");
        }
        catch (...)
        {
            const auto kind = meowlive::platform::log_current_exception(
                "reply.outbox.replay", meowlive::platform::ExceptionDetails::Omit);
            if (kind == meowlive::platform::ExceptionKind::OutOfMemory)
            {
                std::terminate();
            }
        }

        const std::filesystem::path directory_;
        const std::filesystem::path recovery_directory_;
        const std::string scope_;
        const std::string instance_id_;
        const Settle settle_;
        const Report report_;
        const std::shared_ptr<ReplyBuffer> buffer_ = std::make_shared<ReplyBuffer>();
        std::mutex wait_mutex_;
        std::condition_variable_any ready_;
        bool changed_{};
        std::jthread replay_worker_;
        std::jthread write_worker_;
    };
}
