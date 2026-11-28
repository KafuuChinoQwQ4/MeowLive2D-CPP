module;

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

export module meowlive.desktop_runtime.audio.playback;

import meowlive.desktop_runtime.audio.wasapi;
import meowlive.protocol.control;

export namespace meowlive::desktop_runtime::audio
{
    class Playback
    {
    public:
        void reset(std::uint32_t generation = 0)
        {
            output_.stop();
            generation_ = generation;
            active_.reset();
            pending_.clear();
            retired_.clear();
            receipts_.clear();
            pending_samples_ = 0;
        }

        double level() const noexcept
        {
            return output_.level();
        }

        bool speak(const meowlive::protocol::ServerSpeak& command)
        {
            if (command.generation < generation_)
            {
                return true;
            }
            if (command.generation != generation_ || active_ ||
                this->retired(command.utterance_id) || command.utterance_id.empty() ||
                command.utterance_id.size() > meowlive::protocol::max_utterance_id_bytes ||
                !meowlive::protocol::valid_audio_format(command.format))
            {
                return false;
            }

            active_ = Active{.id = command.utterance_id, .channels = command.format.channels};
            output_.start(command.format);
            for (auto chunk = pending_.begin(); chunk != pending_.end();)
            {
                if (chunk->generation == generation_ && chunk->utterance_id == command.utterance_id)
                {
                    pending_samples_ -= chunk->samples.size();
                    auto selected = std::move(*chunk);
                    chunk = pending_.erase(chunk);
                    if (!this->receive(std::move(selected)))
                    {
                        return false;
                    }
                }
                else
                {
                    ++chunk;
                }
            }
            return true;
        }

        void stop(std::uint32_t generation)
        {
            if (generation <= generation_)
            {
                return;
            }
            output_.stop();
            active_.reset();
            generation_ = generation;
            retired_.clear();
            receipts_.clear();
            std::erase_if(pending_,
                          [this](const meowlive::protocol::AudioChunk& chunk)
                          {
                              if (chunk.generation < generation_)
                              {
                                  pending_samples_ -= chunk.samples.size();
                                  return true;
                              }
                              return false;
                          });
        }

        bool receive(meowlive::protocol::AudioChunk chunk)
        {
            if (chunk.generation < generation_ ||
                (chunk.generation == generation_ && this->retired(chunk.utterance_id)))
            {
                return true;
            }

            if (!active_ || chunk.generation != generation_ || chunk.utterance_id != active_->id)
            {
                constexpr std::size_t maximum_pending_frames = 64;
                constexpr std::size_t maximum_pending_samples = 1'048'576;
                if (pending_.size() >= maximum_pending_frames ||
                    chunk.samples.size() > maximum_pending_samples - pending_samples_)
                {
                    return false;
                }
                pending_samples_ += chunk.samples.size();
                pending_.push_back(std::move(chunk));
                return true;
            }

            if (active_->ended || active_->sequence != chunk.sequence ||
                chunk.samples.size() % active_->channels != 0 ||
                active_->sequence == std::numeric_limits<std::uint32_t>::max())
            {
                return false;
            }
            ++active_->sequence;
            active_->ended = chunk.end;
            active_->has_samples |= !chunk.samples.empty();
            if (chunk.end && !active_->has_samples)
            {
                this->finish(meowlive::protocol::ExecutionStatus::Failed,
                             "utterance contains no PCM samples");
            }
            else if (!output_.push(std::move(chunk.samples), chunk.end))
            {
                // Preserve a specific device error if the worker already stopped.
                this->collect_events();
                if (active_)
                {
                    this->finish(meowlive::protocol::ExecutionStatus::Failed,
                                 "PCM exceeds playback buffer capacity");
                }
            }
            return true;
        }

        std::vector<meowlive::protocol::ExecutionReceipt> poll()
        {
            this->collect_events();
            return std::exchange(receipts_, {});
        }

    private:
        struct Active
        {
            std::string id;
            std::uint16_t channels;
            std::uint32_t sequence{};
            bool ended{};
            bool has_samples{};
        };

        bool retired(const std::string& id) const
        {
            return std::ranges::find(retired_, id) != retired_.end();
        }

        void collect_events()
        {
            for (auto& event : output_.poll())
            {
                if (!active_)
                {
                    break;
                }
                if (event.status == meowlive::protocol::ExecutionStatus::Started)
                {
                    receipts_.push_back({active_->id, generation_, event.status, std::nullopt});
                }
                else
                {
                    this->finish(event.status, std::move(event.error));
                }
            }
        }

        void finish(meowlive::protocol::ExecutionStatus status, std::optional<std::string> error)
        {
            output_.stop();
            retired_.push_back(active_->id);
            if (retired_.size() > 128)
            {
                retired_.pop_front();
            }
            receipts_.push_back({std::move(active_->id), generation_, status, std::move(error)});
            active_.reset();
        }

        meowlive::desktop_runtime::audio::WasapiOutput output_;
        std::uint32_t generation_{};
        std::optional<Active> active_;
        std::deque<meowlive::protocol::AudioChunk> pending_;
        std::size_t pending_samples_{};
        std::deque<std::string> retired_;
        std::vector<meowlive::protocol::ExecutionReceipt> receipts_;
    };
}
