module;

#include <QComboBox>
#include <QDesktopServices>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLocale>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QSignalBlocker>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QWidget>

export module meowlive.desktop.ui.voice_models_panel;

import meowlive.desktop.services.server_client;
import meowlive.desktop.ui.transcription_choices;

export namespace meowlive::desktop::ui
{
    class VoiceModelsPanel final : public ::QWidget
    {
    public:
        VoiceModelsPanel(meowlive::desktop::services::ServerClient& server, ::QWidget* parent)
            : ::QWidget(parent), server_(server)
        {
            auto* form = new ::QFormLayout(this);
            purpose_ = new ::QComboBox(this);
            purpose_->addItem(QStringLiteral("录音转写"), QStringLiteral("transcription"));
            purpose_->addItem(QStringLiteral("训练底模"), QStringLiteral("training"));
            models_ = new ::QComboBox(this);
            details_ = new ::QLabel(this);
            details_->setTextFormat(::Qt::PlainText);
            details_->setWordWrap(true);
            files_ = new ::QPlainTextEdit(this);
            files_->setReadOnly(true);
            progress_ = new ::QProgressBar(this);
            progress_->setRange(0, 100);
            status_ = new ::QLabel(this);
            status_->setTextFormat(::Qt::PlainText);
            status_->setWordWrap(true);
            install_ = new ::QPushButton(QStringLiteral("下载安装"), this);
            cancel_ = new ::QPushButton(QStringLiteral("取消下载"), this);
            auto* python = new ::QPushButton(QStringLiteral("Windows Python 安装说明"), this);
            auto* actions = new ::QHBoxLayout;
            actions->addWidget(install_);
            actions->addWidget(cancel_);
            actions->addWidget(python);
            form->addRow(QStringLiteral("用途"), purpose_);
            form->addRow(QStringLiteral("模型"), models_);
            form->addRow(details_);
            form->addRow(QStringLiteral("待下载文件"), files_);
            form->addRow(progress_);
            form->addRow(status_);
            form->addRow(actions);

            this->connect(purpose_, &::QComboBox::currentIndexChanged, this,
                          [this](int)
                          {
                              feedback_.clear();
                              this->update_model_choices();
                          });
            this->connect(models_, &::QComboBox::currentIndexChanged, this,
                          [this](int)
                          {
                              feedback_.clear();
                              this->render();
                          });
            this->connect(
                install_, &::QPushButton::clicked, this,
                [this]
                {
                    const auto model = this->selected_model();
                    this->post(
                        QStringLiteral("/api/voice-models/downloads"),
                        {{QStringLiteral("model_id"), model.value(QStringLiteral("id"))},
                         {QStringLiteral("revision"), model.value(QStringLiteral("revision"))}});
                });
            this->connect(cancel_, &::QPushButton::clicked, this,
                          [this]
                          {
                              this->post(QStringLiteral("/api/voice-models/downloads/cancel"),
                                         {{QStringLiteral("id"), current_download_}});
                          });
            this->connect(python, &::QPushButton::clicked, this,
                          []
                          {
                              ::QDesktopServices::openUrl(::QUrl(QStringLiteral(
                                  "https://docs.python.org/3.11/using/windows.html")));
                          });
            this->render();
        }

        void select_purpose(const ::QString& purpose)
        {
            purpose_->setCurrentIndex(purpose_->findData(purpose));
        }

        void set_access(bool access)
        {
            if (access_ != access)
            {
                access_ = access;
                ++session_;
                sending_ = false;
                feedback_.clear();
                if (!access_)
                {
                    this->update_catalog({}, {});
                }
            }
        }

        void update_catalog(const ::QJsonObject& value, const ::QString& error)
        {
            if (!error.isEmpty())
            {
                catalog_error_ = error;
                this->render();
                return;
            }
            catalog_error_.clear();
            catalog_ = value.value(QStringLiteral("models")).toArray();
            downloads_ = value.value(QStringLiteral("downloads")).toArray();
            this->update_model_choices();
        }

    private:
        ::QJsonObject selected_model() const
        {
            const auto selected = models_->currentData().toString();
            for (const auto& item : catalog_)
            {
                const auto model = item.toObject();
                if (model.value(QStringLiteral("id")) == selected)
                {
                    return model;
                }
            }
            return {};
        }

        void update_model_choices()
        {
            const auto purpose = purpose_->currentData().toString();
            const auto selected = models_->currentData().toString();
            const ::QSignalBlocker blocked(models_);
            ::QStringList ids;
            ::QStringList names;
            for (const auto& entry : catalog_)
            {
                const auto model = entry.toObject();
                if (model.value(QStringLiteral("purpose")) == purpose)
                {
                    ids.append(model.value(QStringLiteral("id")).toString());
                    names.append(model.value(QStringLiteral("name")).toString());
                }
            }
            if (ids != ids_ || names != names_)
            {
                ids_ = ids;
                names_ = names;
                models_->clear();
                for (::qsizetype i = 0; i < ids.size(); ++i)
                {
                    models_->addItem(names.at(i), ids.at(i));
                }
                const auto index = models_->findData(selected);
                models_->setCurrentIndex(index >= 0 ? index : (models_->count() > 0 ? 0 : -1));
            }
            this->render();
        }

        static ::QString describe_state(const ::QString& state)
        {
            if (state == QStringLiteral("queued"))
                return QStringLiteral("排队中");
            if (state == QStringLiteral("downloading"))
                return QStringLiteral("正在下载");
            if (state == QStringLiteral("verifying"))
                return QStringLiteral("正在校验");
            if (state == QStringLiteral("installing"))
                return QStringLiteral("正在安装并加载模型");
            if (state == QStringLiteral("cancelled"))
                return QStringLiteral("已取消");
            if (state == QStringLiteral("failed"))
                return QStringLiteral("安装未完成");
            return {};
        }

        void render()
        {
            const auto id = models_->currentData().toString();
            const auto model = this->selected_model();
            ::QJsonObject task;
            if (!model.isEmpty())
            {
                for (const auto& item : downloads_)
                {
                    const auto download = item.toObject();
                    if (download.value(QStringLiteral("model_id")) == id)
                    {
                        task = download;
                        break;
                    }
                }
            }
            const auto state = task.value(QStringLiteral("state")).toString();
            const bool active =
                state == QStringLiteral("queued") || state == QStringLiteral("downloading") ||
                state == QStringLiteral("verifying") || state == QStringLiteral("installing");
            const bool ready = model.value(QStringLiteral("ready")).toBool();
            current_download_ = task.value(QStringLiteral("id")).toString();

            const auto size_label = [](const ::QJsonObject& object, const ::QString& key)
            {
                return ::QLocale().formattedDataSize(object.value(key).toInteger(), 1);
            };
            ::QStringList languages;
            for (const auto& language : model.value(QStringLiteral("languages")).toArray())
            {
                languages.append(meowlive::desktop::ui::TranscriptionChoices::language_name(
                    language.toString()));
            }
            details_->setText(
                model.isEmpty()
                    ? QStringLiteral("当前主服务没有可下载的语音模型")
                    : QStringLiteral(
                          "%1 · %2\n%3\n需下载 %4 · 安装占用上限 %5 · 本次需要可用空间 %6")
                          .arg(model.value(QStringLiteral("requirements")).toString(),
                               languages.join(QStringLiteral("、")),
                               model.value(QStringLiteral("license")).toString(),
                               size_label(model, QStringLiteral("download_bytes")),
                               size_label(model, QStringLiteral("installed_bytes")),
                               size_label(model, QStringLiteral("required_free_bytes"))));
            ::QStringList files;
            for (const auto& item : model.value(QStringLiteral("missing")).toArray())
            {
                const auto file = item.toObject();
                files.append(
                    QStringLiteral("%1 · %2").arg(file.value(QStringLiteral("name")).toString(),
                                                  size_label(file, QStringLiteral("bytes"))));
            }
            const auto missing = files.join(u'\n');
            if (missing != missing_)
            {
                missing_ = missing;
                files_->setPlainText(missing);
            }
            files_->setVisible(!files.isEmpty());
            auto message =
                ready ? QStringLiteral("就绪") : model.value(QStringLiteral("reason")).toString();
            if (!state.isEmpty() && state != QStringLiteral("ready") && !ready)
            {
                message = meowlive::desktop::ui::VoiceModelsPanel::describe_state(state);
                const auto description = task.value(QStringLiteral("message")).toString();
                const auto file = task.value(QStringLiteral("file")).toString();
                if (!description.isEmpty() || (active && !file.isEmpty()))
                {
                    message += QStringLiteral(" · ") + (description.isEmpty() ? file : description);
                }
            }
            if (!catalog_error_.isEmpty() || !feedback_.isEmpty())
            {
                message = catalog_error_.isEmpty() ? feedback_ : catalog_error_;
            }
            status_->setText(message);
            const auto total = task.value(QStringLiteral("total_bytes")).toInteger();
            const auto received = task.value(QStringLiteral("received_bytes")).toInteger();
            progress_->setVisible(active);
            progress_->setValue(total > 0 ? static_cast<int>(received * 100 / total) : 0);
            progress_->setFormat(QStringLiteral("%1 / %2").arg(
                ::QLocale().formattedDataSize(received), ::QLocale().formattedDataSize(total)));
            purpose_->setEnabled(access_);
            models_->setEnabled(access_ && models_->count() > 0);
            install_->setEnabled(access_ && !sending_ && !active && catalog_error_.isEmpty() &&
                                 model.value(QStringLiteral("downloadable")).toBool());
            install_->setText(state == QStringLiteral("failed") ||
                                      state == QStringLiteral("cancelled")
                                  ? QStringLiteral("重新检测并重试")
                                  : QStringLiteral("下载安装"));
            cancel_->setEnabled(access_ && !sending_ && active);
        }

        void post(const ::QString& path, const ::QJsonObject& body)
        {
            sending_ = true;
            feedback_.clear();
            this->render();
            const auto session = session_;
            server_.post_json(path, body, *this,
                              [this, session](const ::QJsonDocument&, const ::QString& error)
                              {
                                  if (session == session_)
                                  {
                                      sending_ = false;
                                      feedback_ = error;
                                      this->render();
                                  }
                              });
        }

        meowlive::desktop::services::ServerClient& server_;
        ::QComboBox* purpose_{};
        ::QComboBox* models_{};
        ::QPlainTextEdit* files_{};
        ::QLabel* details_{};
        ::QLabel* status_{};
        ::QProgressBar* progress_{};
        ::QPushButton* install_{};
        ::QPushButton* cancel_{};
        ::QJsonArray catalog_;
        ::QJsonArray downloads_;
        ::QStringList ids_;
        ::QStringList names_;
        ::QString current_download_;
        ::QString missing_;
        ::QString catalog_error_;
        ::QString feedback_;
        unsigned session_{};
        bool access_{};
        bool sending_{};
    };
}
