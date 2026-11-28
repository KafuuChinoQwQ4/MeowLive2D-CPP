module;

#include <drogon/HttpAppFramework.h>
#include <drogon/HttpFilter.h>
#include <drogon/WebSocketController.h>
#include <drogon/utils/Utilities.h>
#include <trantor/net/EventLoop.h>

#include <meowlive/msvc_stl.hpp>

export module meowlive.server.transport.audio_transfer;

import meowlive.standard;
import meowlive.application.obs_status;
import meowlive.application.scheduler;
import meowlive.application.speech;
import meowlive.application.speech_worker;
import meowlive.protocol.control;
import meowlive.protocol.resources;
import meowlive.server.transport.access;

export namespace meowlive::server::transport::bridge_detail
{
    struct Endpoint
    {
        drogon::WebSocketConnectionPtr socket;
        trantor::EventLoop* loop{};

        void send_control(const meowlive::protocol::ServerCommand& command) const
        {
            loop->queueInLoop(
                [socket = this->socket,
                 message = meowlive::protocol::encode_server_command(command)]
                {
                    if (!socket->disconnected())
                    {
                        socket->send(message);
                    }
                });
        }

        void close(drogon::CloseCode code) const
        {
            if (!socket)
            {
                return;
            }
            loop->runInLoop(
                [socket = this->socket, loop = this->loop, code]
                {
                    if (socket->disconnected())
                    {
                        return;
                    }
                    socket->shutdown(code);
                    loop->runAfter(std::chrono::seconds{1},
                                   [weak_socket = std::weak_ptr(socket)]
                                   {
                                       if (const auto active = weak_socket.lock();
                                           active && !active->disconnected())
                                       {
                                           active->forceClose();
                                       }
                                   });
                });
        }
    };

    class AudioTransfer : public std::enable_shared_from_this<AudioTransfer>
    {
    public:
        AudioTransfer(Endpoint audio, const meowlive::protocol::ServerSpeak& command,
                      meowlive::application::PcmAudio pcm, std::stop_token stop)
            : audio_(std::move(audio)), id_(command.utterance_id), generation_(command.generation),
              samples_(std::move(pcm.samples)), stop_(std::move(stop))
        {
        }

        void start()
        {
            audio_.loop->queueInLoop(
                [transfer = shared_from_this()]
                {
                    transfer->pump();
                });
        }

    private:
        void pump()
        {
            // Limit each turn so heartbeat and disconnect work can run on the audio loop.
            constexpr std::size_t batch = 4;
            constexpr std::size_t chunk_samples = 8192;
            for (std::size_t index = 0; index < batch; ++index)
            {
                if (stop_.stop_requested() || audio_.socket->disconnected())
                {
                    return;
                }
                const auto count = (std::min)(chunk_samples, samples_.size() - offset_);
                const auto first = samples_.begin() + offset_;
                offset_ += count;
                const bool end = offset_ == samples_.size();
                const auto frame = meowlive::protocol::encode_audio_chunk(
                    {id_, generation_, sequence_++, {first, first + count}, end});
                if (!frame)
                {
                    audio_.close(drogon::CloseCode::kProtocolError);
                    return;
                }
                audio_.socket->send(reinterpret_cast<const char*>(frame->data()), frame->size(),
                                    drogon::WebSocketMessageType::Binary);
                if (end)
                {
                    return;
                }
            }
            audio_.loop->runAfter(std::chrono::milliseconds{2},
                                  [transfer = shared_from_this()]
                                  {
                                      transfer->pump();
                                  });
        }

        const Endpoint audio_;
        const std::string id_;
        const std::uint32_t generation_;
        const std::vector<std::int16_t> samples_;
        const std::stop_token stop_;
        std::size_t offset_{};
        std::uint32_t sequence_{};
    };
}
