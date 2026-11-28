module;

#include <json/json.h>

#include <meowlive/msvc_stl.hpp>

export module meowlive.server.live;

import meowlive.standard;

import meowlive.application.live;
import meowlive.application.live_session;
import meowlive.server.live_settings;

namespace
{
    using Phase = meowlive::application::LivePhase;

    bool idle(Phase phase)
    {
        return phase == Phase::Disconnected || phase == Phase::Failed;
    }

    std::string phase_name(Phase phase)
    {
        switch (phase)
        {
        case Phase::Disconnected:
            return "disconnected";
        case Phase::Connecting:
            return "connecting";
        case Phase::Connected:
            return "connected";
        case Phase::Reconnecting:
            return "reconnecting";
        case Phase::Disconnecting:
            return "disconnecting";
        case Phase::Failed:
            return "failed";
        }
        std::unreachable();
    }

    ::Json::Value message(std::string_view code)
    {
        if (code == "live_disconnected")
        {
            return "直播连接已中断，正在重连。";
        }
        if (code == "live_authorization_failed")
        {
            return "开放平台授权失败，请检查应用 ID、开发者凭据和主播身份码。";
        }
        if (code == "live_cleanup_failed")
        {
            return "开放平台会话结束失败，请核对平台会话状态后重试。";
        }
        if (code == "live_source_internal_error")
        {
            return "直播接入发生内部错误，请查看服务端日志。";
        }
        if (code == "live_event_internal_error")
        {
            return "直播事件处理发生内部错误，请查看服务端日志。";
        }
        if (code == "live_event_expired")
        {
            return "收到已过期的直播事件。";
        }
        if (code == "live_event_queue_full")
        {
            return "直播事件队列已满，部分新事件被丢弃。";
        }
        if (code == "live_event_storage_failed")
        {
            return "观众存储写入失败，事件未安排回应。";
        }
        return code.empty() ? ::Json::Value{} : ::Json::Value("直播事件处理失败。");
    }
}

export namespace meowlive::server
{
    enum class LiveError
    {
        Invalid,
        Busy,
        NotConfigured,
        Disconnecting,
        Stopping,
        SaveFailed,
    };

    struct LiveRejection
    {
        meowlive::server::LiveError kind;
        std::string message;
    };

    class LiveService
    {
    public:
        using Result = std::expected<::Json::Value, meowlive::server::LiveRejection>;

        LiveService(std::filesystem::path config, meowlive::server::LiveSettings settings,
                    meowlive::application::LiveSource source,
                    meowlive::application::LiveSessionCallbacks callbacks, std::size_t capacity,
                    meowlive::application::LiveRetry retry)
            : config_(std::move(config)), settings_(std::move(settings)),
              session_(std::move(source), std::move(callbacks), capacity, retry)
        {
        }

        bool connected() const noexcept
        {
            return session_.connected();
        }

        ::Json::Value status() const
        {
            const std::lock_guard lock(mutex_);
            return status_locked(session_.snapshot());
        }

        ::Json::Value settings() const
        {
            const std::lock_guard lock(mutex_);
            return settings_.snapshot();
        }

        Result connect()
        {
            const std::lock_guard lock(mutex_);
            if (stopping_ || updating_)
            {
                return error(LiveError::Stopping, "主服务正在退出");
            }
            if (saving_)
            {
                return error(LiveError::Busy, "请等待直播设置保存完成");
            }
            if (!settings_.configured())
            {
                return error(LiveError::NotConfigured, "请设置并启用直播接入后再连接");
            }
            const auto before = session_.snapshot();
            if (before.phase == Phase::Disconnecting)
            {
                return error(LiveError::Disconnecting, "上一次直播连接正在结束");
            }
            if (idle(before.phase))
            {
                session_.connect(settings_.credentials);
            }
            return status_locked(session_.snapshot());
        }

        ::Json::Value disconnect()
        {
            const std::lock_guard lock(mutex_);
            session_.disconnect();
            return status_locked(session_.snapshot());
        }

        Result save(const ::Json::Value& input)
        {
            std::unique_lock lock(mutex_);
            if (stopping_ || updating_)
            {
                return error(LiveError::Stopping, "主服务正在退出");
            }
            if (saving_ || !idle(session_.snapshot().phase))
            {
                return error(LiveError::Busy, "请等待直播断开及设置保存完成");
            }
            auto updated = meowlive::server::edit_live_settings(settings_, input);
            if (!updated)
            {
                return error(LiveError::Invalid, std::move(updated.error()));
            }
            saving_ = true;
            lock.unlock();
            bool saved{};
            try
            {
                saved = meowlive::server::save_live_settings(config_, *updated);
            }
            catch (...)
            {
                lock.lock();
                saving_ = false;
                throw;
            }
            lock.lock();
            saving_ = false;
            if (!saved)
            {
                return error(LiveError::SaveFailed, "无法保存本机直播设置");
            }
            settings_ = std::move(*updated);
            session_.reset();
            return settings_.snapshot();
        }

        bool freeze_for_update()
        {
            const std::lock_guard lock(mutex_);
            if (stopping_ || updating_ || saving_ || !idle(session_.snapshot().phase))
            {
                return false;
            }
            updating_ = true;
            return true;
        }

        void resume_after_update_check()
        {
            const std::lock_guard lock(mutex_);
            updating_ = false;
        }

        void close()
        {
            {
                const std::lock_guard lock(mutex_);
                stopping_ = true;
            }
            session_.close();
        }

    private:
        static Result error(meowlive::server::LiveError kind, std::string detail)
        {
            return std::unexpected(meowlive::server::LiveRejection{kind, std::move(detail)});
        }

        ::Json::Value status_locked(const meowlive::application::LiveSnapshot& state) const
        {
            ::Json::Value result(::Json::objectValue);
            result["platform"] = "bilibili";
            result["configured"] = settings_.configured();
            result["phase"] = settings_.enabled ? phase_name(state.phase) : "disabled";
            result["room_id"] =
                state.room_id.empty() ? ::Json::Value{} : ::Json::Value(state.room_id);
            result["accepted_events"] = state.accepted_events;
            result["duplicate_events"] = state.duplicate_events;
            result["rejected_events"] = state.rejected_events;
            result["reconnect_attempts"] = state.reconnect_attempts;
            result["last_error"] = message(state.last_error);
            return result;
        }

        const std::filesystem::path config_;
        mutable std::mutex mutex_;
        meowlive::server::LiveSettings settings_;
        bool saving_{};
        bool stopping_{};
        bool updating_{};
        meowlive::application::LiveSession session_;
    };
}
