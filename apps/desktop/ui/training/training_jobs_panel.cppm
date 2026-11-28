module;

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <mmsystem.h>

#include <QByteArray>
#include <QCheckBox>
#include <QComboBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QSettings>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QString>
#include <QStringList>
#include <QTemporaryFile>
#include <QWidget>

#include <expected>
#include <functional>
#include <memory>
#include <utility>

export module meowlive.desktop.ui.training_jobs_panel;

import meowlive.desktop.services.server_client;
import meowlive.desktop.ui.transcription_choices;

export namespace meowlive::desktop::ui
{
    class TrainingJobsPanel final : public ::QWidget
    {
    public:
        TrainingJobsPanel(meowlive::desktop::services::ServerClient& server,
                          std::function<void()> open_models,
                          std::function<void()> open_training_models, ::QWidget* parent)
            : ::QWidget(parent), server_(server)
        {
            auto* layout = new ::QFormLayout(this);
            voice_ = new ::QComboBox(this);
            base_model_ = new ::QComboBox(this);
            base_model_->setAccessibleName(QStringLiteral("训练底模"));
            preferred_base_ =
                ::QSettings(QStringLiteral("MeowLive2D"), QStringLiteral("MeowLive2D"))
                    .value(QStringLiteral("voice/training_base_model"))
                    .toString();
            auto* base_actions = new ::QHBoxLayout;
            base_models_page_ = new ::QPushButton(QStringLiteral("管理训练底模"), this);
            base_actions->addWidget(base_model_, 1);
            base_actions->addWidget(base_models_page_);
            name_ = new ::QLineEdit(this);
            model_ = new meowlive::desktop::ui::TranscriptionChoices(
                QStringLiteral("training_clip_model"), this);
            language_ = new ::QComboBox(this);
            model_->update_languages(*language_, true);
            choose_clip_ = new ::QPushButton(QStringLiteral("选择片段 WAV"), this);
            clip_file_ = new ::QLabel(this);
            transcribe_clip_ = new ::QPushButton(QStringLiteral("转写片段"), this);
            models_page_ = new ::QPushButton(QStringLiteral("前往可选模型"), this);
            transcript_ = new ::QLineEdit(this);
            transcript_->setMaxLength(500);
            clips_ = new ::QListWidget(this);
            clips_->setMaximumHeight(100);
            auto* clip_actions = new ::QHBoxLayout;
            add_clip_ = new ::QPushButton(QStringLiteral("加入训练素材"), this);
            remove_clip_ = new ::QPushButton(QStringLiteral("移除片段"), this);
            clip_actions->addWidget(add_clip_);
            clip_actions->addWidget(remove_clip_);
            gpt_ = new ::QSpinBox(this);
            sovits_ = new ::QSpinBox(this);
            for (auto* spin : {gpt_, sovits_})
            {
                spin->setRange(1, 20);
            }
            start_ = new ::QPushButton(QStringLiteral("开始训练"), this);
            jobs_ = new ::QComboBox(this);
            preview_text_ = new ::QLineEdit(this);
            preview_text_->setMaxLength(500);
            preview_text_->setPlaceholderText(QStringLiteral("试听文本"));
            auto* actions = new ::QHBoxLayout;
            cancel_ = new ::QPushButton(QStringLiteral("取消作业"), this);
            audition_ = new ::QPushButton(QStringLiteral("试听"), this);
            activate_ = new ::QPushButton(QStringLiteral("启用"), this);
            remove_ = new ::QPushButton(QStringLiteral("删除"), this);
            for (auto* button : {cancel_, audition_, activate_, remove_})
            {
                actions->addWidget(button);
            }
            status_ = new ::QLabel(this);
            status_->setWordWrap(true);
            layout->addRow(QStringLiteral("关联音色"), voice_);
            layout->addRow(QStringLiteral("训练名称"), name_);
            layout->addRow(QStringLiteral("训练底模"), base_actions);
            layout->addRow(choose_clip_, clip_file_);
            layout->addRow(QStringLiteral("转写模型"), model_);
            auto* recognition = new ::QHBoxLayout;
            recognition->addWidget(transcribe_clip_);
            recognition->addWidget(models_page_);
            layout->addRow(recognition);
            layout->addRow(QStringLiteral("片段语言"), language_);
            layout->addRow(QStringLiteral("片段文本"), transcript_);
            layout->addRow(clip_actions);
            layout->addRow(clips_);
            layout->addRow(QStringLiteral("GPT 轮数"), gpt_);
            layout->addRow(QStringLiteral("SoVITS 轮数"), sovits_);
            layout->addRow(start_);
            layout->addRow(QStringLiteral("训练作业"), jobs_);
            layout->addRow(preview_text_);
            layout->addRow(actions);
            layout->addRow(status_);

            this->connect(choose_clip_, &::QPushButton::clicked, this,
                          &TrainingJobsPanel::choose_clip);
            this->connect(transcribe_clip_, &::QPushButton::clicked, this,
                          &TrainingJobsPanel::transcribe_clip);
            this->connect(models_page_, &::QPushButton::clicked, this, std::move(open_models));
            this->connect(model_, &::QComboBox::currentIndexChanged, this,
                          [this](int)
                          {
                              this->controls();
                          });
            this->connect(language_, &::QComboBox::currentIndexChanged, this,
                          [this](int)
                          {
                              this->controls();
                          });
            this->connect(transcript_, &::QLineEdit::textChanged, this,
                          [this]
                          {
                              this->controls();
                          });
            this->connect(add_clip_, &::QPushButton::clicked, this,
                          [this]
                          {
                              this->add_clip();
                          });
            this->connect(remove_clip_, &::QPushButton::clicked, this,
                          [this]
                          {
                              const auto index = clips_->currentRow();
                              if (index >= 0)
                              {
                                  filenames_.removeAt(index);
                                  transcripts_.removeAt(index);
                                  delete clips_->takeItem(index);
                              }
                              this->controls();
                          });
            this->connect(start_, &::QPushButton::clicked, this,
                          [this]
                          {
                              this->submit();
                          });
            this->connect(cancel_, &::QPushButton::clicked, this,
                          [this]
                          {
                              this->job_action(QStringLiteral("/api/training/cancel"));
                          });
            this->connect(activate_, &::QPushButton::clicked, this,
                          [this]
                          {
                              this->job_action(QStringLiteral("/api/training/activate"));
                          });
            this->connect(remove_, &::QPushButton::clicked, this,
                          [this]
                          {
                              this->job_action(QStringLiteral("/api/training/delete"));
                          });
            this->connect(audition_, &::QPushButton::clicked, this,
                          [this]
                          {
                              this->audition();
                          });
            this->connect(jobs_, &::QComboBox::currentIndexChanged, this,
                          [this](int)
                          {
                              this->controls();
                          });
            this->connect(voice_, &::QComboBox::currentIndexChanged, this,
                          [this](int)
                          {
                              this->controls();
                          });
            this->connect(base_models_page_, &::QPushButton::clicked, this,
                          std::move(open_training_models));
            this->connect(
                base_model_, &::QComboBox::activated, this,
                [this](int)
                {
                    preferred_base_ = base_model_->currentData()
                                          .toJsonObject()
                                          .value(QStringLiteral("id"))
                                          .toString();
                    ::QSettings(QStringLiteral("MeowLive2D"), QStringLiteral("MeowLive2D"))
                        .setValue(QStringLiteral("voice/training_base_model"), preferred_base_);
                    this->controls();
                });
            this->controls();
        }

        ~TrainingJobsPanel() override
        {
            if (audition_file_)
            {
                ::PlaySoundW(nullptr, nullptr, 0);
            }
        }

        void set_access(bool access)
        {
            if (access == access_)
            {
                return;
            }
            access_ = access;
            ++revision_;
            pending_ = false;
            writing_ = false;
            recognizing_ = false;
            this->controls();
        }

        void update_voice_models(const ::QJsonObject& catalog)
        {
            model_->update_catalog(catalog);
            ::QJsonArray bases;
            for (const auto& item : catalog.value(QStringLiteral("models")).toArray())
            {
                const auto model = item.toObject();
                if (model.value(QStringLiteral("purpose")) == QStringLiteral("training") &&
                    model.value(QStringLiteral("ready")).toBool())
                {
                    bases.append(::QJsonObject{
                        {QStringLiteral("id"), model.value(QStringLiteral("id"))},
                        {QStringLiteral("revision"), model.value(QStringLiteral("revision"))},
                        {QStringLiteral("name"), model.value(QStringLiteral("name"))},
                        {QStringLiteral("model_version"),
                         model.value(QStringLiteral("model_version"))}});
                }
            }
            if (catalog.value(QStringLiteral("legacy_training_configured")).toBool())
            {
                bases.append(::QJsonObject{
                    {QStringLiteral("id"), QStringLiteral("legacy")},
                    {QStringLiteral("name"), QStringLiteral("GPT-SoVITS v2（已有环境）")},
                    {QStringLiteral("model_version"), QStringLiteral("v2")}});
            }
            if (bases != base_choices_)
            {
                const ::QSignalBlocker blocked(base_model_);
                base_choices_ = bases;
                base_model_->clear();
                int preferred = -1;
                for (const auto& item : bases)
                {
                    const auto base = item.toObject();
                    if (base.value(QStringLiteral("id")) == preferred_base_)
                    {
                        preferred = base_model_->count();
                    }
                    base_model_->addItem(base.value(QStringLiteral("name")).toString(), base);
                }
                if (preferred >= 0)
                {
                    base_model_->setCurrentIndex(preferred);
                }
            }
            this->controls();
        }

        void refresh()
        {
            if (!access_ || pending_ || writing_)
            {
                return;
            }
            pending_ = true;
            const auto revision = revision_;
            server_.get_training_status(
                *this,
                [this, revision](
                    std::expected<meowlive::desktop::services::TrainingStatus, ::QString> status)
                {
                    if (revision != revision_)
                    {
                        return;
                    }
                    if (!status)
                    {
                        pending_ = false;
                        status_->setText(status.error());
                        return;
                    }
                    training_busy_ = status->busy;
                    this->update_jobs(status->jobs);
                    server_.get_json(QStringLiteral("/api/resources"), *this,
                                     [this, revision](const ::QJsonDocument& data, const ::QString&)
                                     {
                                         if (revision == revision_)
                                         {
                                             pending_ = false;
                                             this->update_voices(data.object());
                                         }
                                     });
                });
        }

    private:
        void choose_clip()
        {
            const auto selected = ::QFileDialog::getOpenFileName(
                this, QStringLiteral("选择 3 至 10 秒参考 WAV"), {}, QStringLiteral("WAV (*.wav)"));
            if (!access_ || selected.isEmpty())
            {
                return;
            }
            const ::QFileInfo file(selected);
            if (!file.isFile() || file.size() < 1 || file.size() > 2 * 1024 * 1024)
            {
                status_->setText(QStringLiteral("录音超过 2 MiB 或不可读"));
                return;
            }
            if (!clip_draft_.isEmpty() && selected != clip_draft_)
            {
                transcript_->clear();
            }
            clip_draft_ = selected;
            clip_file_->setText(file.fileName());
            this->controls();
        }

        void transcribe_clip()
        {
            const auto metadata = model_->request(*language_);
            const auto revision = revision_;
            recognizing_ = true;
            status_->setText(QStringLiteral("正在转写片段…"));
            this->controls();
            server_.transcribe(
                metadata, clip_draft_, *this,
                [this, metadata, revision](const ::QJsonDocument& response, const ::QString& error)
                {
                    if (revision != revision_)
                    {
                        return;
                    }
                    recognizing_ = false;
                    const auto result = response.object();
                    const auto text = result.value(QStringLiteral("text")).toString();
                    if (!error.isEmpty())
                    {
                        status_->setText(error);
                    }
                    else if (text.isEmpty() ||
                             !meowlive::desktop::ui::TranscriptionChoices::matches(metadata,
                                                                                   result))
                    {
                        status_->setText(QStringLiteral("转写结果无效"));
                    }
                    else
                    {
                        transcript_->setText(text);
                        status_->setText(QStringLiteral("请校对片段文本，再加入训练素材"));
                    }
                    this->controls();
                });
        }

        void add_clip()
        {
            const auto text = transcript_->text().trimmed();
            filenames_.append(clip_draft_);
            transcripts_.append(
                ::QJsonObject{{QStringLiteral("language"), language_->currentData().toString()},
                              {QStringLiteral("text"), text}});
            clips_->addItem(clip_file_->text() + QStringLiteral("  ") + text);
            clip_draft_.clear();
            clip_file_->clear();
            transcript_->clear();
            this->controls();
        }

        void update_voices(const ::QJsonObject& data)
        {
            const auto id = voice_->currentData().toString();
            voice_->blockSignals(true);
            voice_->clear();
            if (data.value(QStringLiteral("default_voice_available")).toBool())
            {
                voice_->addItem(QStringLiteral("默认音色"), QStringLiteral("default"));
            }
            for (const auto& element : data.value(QStringLiteral("voices")).toArray())
            {
                const auto entry = element.toObject();
                voice_->addItem(entry.value(QStringLiteral("name")).toString(),
                                entry.value(QStringLiteral("id")).toString());
            }
            const auto previous = voice_->findData(id);
            if (previous >= 0)
            {
                voice_->setCurrentIndex(previous);
            }
            voice_->blockSignals(false);
            this->controls();
        }

        void update_jobs(const ::QJsonArray& items)
        {
            const auto id = jobs_->currentData().toJsonObject().value(QStringLiteral("id"));
            jobs_->blockSignals(true);
            jobs_->clear();
            for (const auto& item : items)
            {
                const auto job = item.toObject();
                const auto status = job.value(QStringLiteral("save_pending")).toBool()
                                        ? job.value(QStringLiteral("message")).toString()
                                        : job.value(QStringLiteral("status")).toString();
                jobs_->addItem(
                    QStringLiteral("%1 · %2 · %3 · %4%")
                        .arg(job.value(QStringLiteral("name")).toString(),
                             job.value(QStringLiteral("model_version")).toString(), status,
                             ::QString::number(job.value(QStringLiteral("progress")).toInt())),
                    job);
                if (id == job.value(QStringLiteral("id")))
                {
                    jobs_->setCurrentIndex(jobs_->count() - 1);
                }
            }
            jobs_->blockSignals(false);
            this->controls();
        }

        void controls()
        {
            const bool editable = access_ && !writing_;
            const auto selected = jobs_->currentData().toJsonObject();
            const auto status = selected.value(QStringLiteral("status")).toString();
            const bool job_editable =
                editable && !selected.value(QStringLiteral("save_pending")).toBool();
            const bool completed = status == QStringLiteral("completed");
            const bool active =
                status == QStringLiteral("queued") || status == QStringLiteral("preparing") ||
                status == QStringLiteral("training") || status == QStringLiteral("validating");
            const bool preparing = editable && !recognizing_ && filenames_.size() < 32;
            const bool supported = model_->supports(*language_);
            base_model_->setEnabled(editable && base_model_->count() > 0);
            base_models_page_->setEnabled(access_);
            choose_clip_->setEnabled(preparing);
            model_->setEnabled(preparing && model_->count() > 0);
            models_page_->setVisible(access_ && model_->count() == 0);
            language_->setEnabled(preparing);
            transcript_->setEnabled(preparing);
            transcribe_clip_->setToolTip(model_->count() > 0 && !supported
                                             ? QStringLiteral("所选转写模型不支持片段语言")
                                             : QString{});
            transcribe_clip_->setEnabled(preparing && !clip_draft_.isEmpty() && supported);
            add_clip_->setEnabled(preparing && !clip_draft_.isEmpty() &&
                                  !transcript_->text().trimmed().isEmpty());
            remove_clip_->setEnabled(editable && !filenames_.isEmpty());
            start_->setEnabled(editable && base_model_->currentIndex() >= 0 && !training_busy_ &&
                               voice_->currentIndex() >= 0 && filenames_.size() >= 2);
            cancel_->setEnabled(job_editable && active);
            audition_->setEnabled(job_editable && completed);
            activate_->setEnabled(job_editable && completed);
            remove_->setEnabled(job_editable && !active && !selected.isEmpty());
        }

        void submit()
        {
            const auto name = name_->text().trimmed();
            if (name.isEmpty())
            {
                status_->setText(QStringLiteral("请填写训练名称"));
                return;
            }
            const auto base = base_model_->currentData().toJsonObject();
            ::QJsonObject metadata{
                {QStringLiteral("name"), name},
                {QStringLiteral("voice_id"), voice_->currentData().toString()},
                {QStringLiteral("model_version"), base.value(QStringLiteral("model_version"))},
                {QStringLiteral("reviewed"), true},
                {QStringLiteral("gpt_epochs"), gpt_->value()},
                {QStringLiteral("sovits_epochs"), sovits_->value()},
                {QStringLiteral("clips"), transcripts_}};
            const auto base_revision = base.value(QStringLiteral("revision"));
            if (base_revision.isString())
            {
                metadata.insert(QStringLiteral("base_model_id"), base.value(QStringLiteral("id")));
                metadata.insert(QStringLiteral("base_model_revision"), base_revision);
            }
            writing_ = true;
            this->controls();
            const auto revision = revision_;
            server_.submit_training(metadata, filenames_, *this,
                                    [this, revision](const ::QJsonDocument&, const ::QString& error)
                                    {
                                        this->finish(revision, error,
                                                     QStringLiteral("训练作业已入队"));
                                    });
        }

        void job_action(const ::QString& path)
        {
            const auto id = jobs_->currentData().toJsonObject().value(QStringLiteral("id"));
            writing_ = true;
            this->controls();
            const auto revision = revision_;
            server_.post_json(path, {{QStringLiteral("id"), id}}, *this,
                              [this, revision](const ::QJsonDocument&, const ::QString& error)
                              {
                                  this->finish(revision, error, QStringLiteral("已更新训练作业"));
                              });
        }

        void audition()
        {
            const auto id =
                jobs_->currentData().toJsonObject().value(QStringLiteral("id")).toString();
            const auto text = preview_text_->text().trimmed();
            if (text.isEmpty())
            {
                status_->setText(QStringLiteral("请填写试听文本"));
                return;
            }
            writing_ = true;
            this->controls();
            const auto revision = revision_;
            server_.audition_training(
                id, text, *this,
                [this, revision](::QByteArray bytes, const ::QString& error)
                {
                    if (revision != revision_)
                    {
                        return;
                    }
                    if (!error.isEmpty())
                    {
                        this->finish(revision, error, {});
                        return;
                    }
                    auto file = std::make_unique<::QTemporaryFile>();
                    if (!file->open() || file->write(bytes) != bytes.size() || !file->flush())
                    {
                        this->finish(revision, QStringLiteral("无法写入试听文件"), {});
                        return;
                    }
                    file->close();
                    const auto path = file->fileName().toStdWString();
                    if (::PlaySoundW(path.c_str(), nullptr,
                                     SND_FILENAME | SND_ASYNC | SND_NODEFAULT))
                    {
                        audition_file_ = std::move(file);
                        this->finish(revision, {}, QStringLiteral("已开始试听"));
                    }
                    else
                    {
                        this->finish(revision, QStringLiteral("无法播放试听"), {});
                    }
                });
        }

        void finish(unsigned revision, const ::QString& error, const ::QString& success)
        {
            if (revision != revision_)
            {
                return;
            }
            writing_ = false;
            status_->setText(error.isEmpty() ? success : error);
            this->controls();
            this->refresh();
        }

        meowlive::desktop::services::ServerClient& server_;
        ::QComboBox* voice_{};
        ::QComboBox* base_model_{};
        ::QComboBox* language_{};
        meowlive::desktop::ui::TranscriptionChoices* model_{};
        ::QComboBox* jobs_{};
        ::QLineEdit* name_{};
        ::QLineEdit* transcript_{};
        ::QLineEdit* preview_text_{};
        ::QListWidget* clips_{};
        ::QSpinBox* gpt_{};
        ::QSpinBox* sovits_{};
        ::QPushButton* add_clip_{};
        ::QPushButton* choose_clip_{};
        ::QPushButton* transcribe_clip_{};
        ::QPushButton* models_page_{};
        ::QPushButton* base_models_page_{};
        ::QPushButton* remove_clip_{};
        ::QPushButton* start_{};
        ::QPushButton* cancel_{};
        ::QPushButton* audition_{};
        ::QPushButton* activate_{};
        ::QPushButton* remove_{};
        ::QLabel* status_{};
        ::QLabel* clip_file_{};
        ::QString clip_draft_;
        ::QString preferred_base_;
        ::QStringList filenames_;
        ::QJsonArray transcripts_;
        ::QJsonArray base_choices_;
        std::unique_ptr<::QTemporaryFile> audition_file_;
        unsigned revision_{};
        bool access_{};
        bool pending_{};
        bool writing_{};
        bool recognizing_{};
        bool training_busy_{};
    };
}
