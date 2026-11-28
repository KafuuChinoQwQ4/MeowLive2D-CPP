module;

#include <json/json.h>
#include <trantor/utils/Logger.h>

#include <meowlive/msvc_stl.hpp>

export module meowlive.server.receipt_journal;

import meowlive.standard;

import meowlive.application.scheduler;
import meowlive.application.speech_worker;
import meowlive.application.work_queue;
import meowlive.platform.exception_diagnostics;

export namespace meowlive::server
{
    class ReceiptJournal
    {
    public:
        explicit ReceiptJournal(std::filesystem::path directory) : directory_(std::move(directory))
        {
        }

        void append(const std::string& session,
                    const meowlive::application::PlaybackReceipt& receipt)
        {
            auto result = queue_.try_submit(
                [this, session, receipt]() noexcept
                {
                    try
                    {
                        write(session, receipt);
                    }
                    catch (const std::filesystem::filesystem_error& error)
                    {
                        LOG_ERROR << "playback_receipt_write_failed " << receipt.utterance_id
                                  << " filesystem_error=" << error.code().value();
                    }
                    catch (const std::ios_base::failure& error)
                    {
                        LOG_ERROR << "playback_receipt_write_failed " << receipt.utterance_id
                                  << " std::ios_base::failure=" << error.code().value()
                                  << " what=" << error.what();
                    }
                    catch (...)
                    {
                        meowlive::platform::log_current_exception(
                            "speech.receipt.write", meowlive::platform::ExceptionDetails::Omit);
                    }
                });
            if (result != meowlive::application::WorkSubmission::Accepted)
            {
                LOG_ERROR << "playback_receipt_queue_unavailable " << receipt.utterance_id;
            }
        }

    private:
        void write(const std::string& session,
                   const meowlive::application::PlaybackReceipt& receipt)
        {
            ::Json::Value entry(::Json::objectValue);
            entry["session_id"] = session;
            entry["utterance_id"] = receipt.utterance_id;
            entry["generation"] = receipt.generation;
            using Status = meowlive::application::PlaybackEvent;
            entry["status"] = receipt.status == Status::Completed ? "completed"
                              : receipt.status == Status::Failed  ? "failed"
                                                                  : "cancelled";
            ::Json::StreamWriterBuilder writer;
            writer["indentation"] = "";
            auto line = ::Json::writeString(writer, entry);
            line += '\n';
            std::filesystem::create_directories(directory_);
            std::ofstream output(directory_ / "playback.jsonl", std::ios::binary | std::ios::app);
            output.exceptions(std::ios::failbit | std::ios::badbit);
            output << line;
            output.close();
        }

        const std::filesystem::path directory_;
        meowlive::application::WorkQueue queue_{128};
    };
}
