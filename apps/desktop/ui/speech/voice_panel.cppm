module;

#include <QComboBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QString>
#include <QVBoxLayout>
#include <QWidget>

export module meowlive.desktop.ui.voice_panel;

import meowlive.desktop.services.server_client;

export namespace meowlive::desktop::ui
{
    class VoicePanel final : public ::QWidget
    {
    public:
        VoicePanel(meowlive::desktop::services::ServerClient& server, ::QWidget* parent)
            : ::QWidget(parent), server_(server)
        {
            auto* layout = new ::QVBoxLayout(this);
            active_ = this->label();
            layout->addWidget(active_);
            voices_ = new ::QComboBox(this);
            voices_->setAccessibleName(QStringLiteral("音色"));
            layout->addWidget(voices_);

            auto* actions = new ::QHBoxLayout;
            reload_ = new ::QPushButton(QStringLiteral("刷新音色"), this);
            select_ = new ::QPushButton(QStringLiteral("设为当前音色"), this);
            preview_ = new ::QPushButton(QStringLiteral("试听"), this);
            remove_ = new ::QPushButton(QStringLiteral("删除音色"), this);
            for (auto* button : {reload_, select_, preview_, remove_})
            {
                actions->addWidget(button);
            }
            actions->addStretch();
            layout->addLayout(actions);
            preview_text_ = new ::QLineEdit(this);
            preview_text_->setAccessibleName(QStringLiteral("试听文本"));
            preview_text_->setPlaceholderText(QStringLiteral("试听文本，留空时使用参考文本"));
            layout->addWidget(preview_text_);

            auto* fields = new ::QFormLayout;
            name_ = new ::QLineEdit(this);
            language_ = new ::QLineEdit(QStringLiteral("zh"), this);
            reference_ = new ::QLineEdit(this);
            import_ = new ::QPushButton(QStringLiteral("选择并导入参考 WAV"), this);
            fields->addRow(QStringLiteral("音色名称"), name_);
            fields->addRow(QStringLiteral("语言"), language_);
            fields->addRow(QStringLiteral("参考文本"), reference_);
            fields->addRow(import_);
            layout->addLayout(fields);
            feedback_ = this->label();
            layout->addWidget(feedback_);
            layout->addStretch();

            this->connect(reload_, &::QPushButton::clicked, this, &VoicePanel::refresh);
            this->connect(select_, &::QPushButton::clicked, this,
                          [this]
                          {
                              this->update(QStringLiteral("/api/voices/select"));
                          });
            this->connect(remove_, &::QPushButton::clicked, this,
                          [this]
                          {
                              this->update(QStringLiteral("/api/voices/delete"));
                          });
            this->connect(preview_, &::QPushButton::clicked, this, &VoicePanel::preview);
            this->connect(import_, &::QPushButton::clicked, this, &VoicePanel::import_voice);
            this->connect(voices_, &::QComboBox::currentIndexChanged, this,
                          [this]
                          {
                              this->controls();
                          });
            this->controls();
        }

        void set_access(bool access)
        {
            if (access == access_)
            {
                return;
            }
            access_ = access;
            ++revision_;
            busy_ = false;
            loaded_ = false;
            voices_->clear();
            active_->clear();
            feedback_->clear();
            this->controls();
            if (this->isVisible())
            {
                this->open();
            }
        }

        void open()
        {
            if (!loaded_)
            {
                this->refresh();
            }
        }

    private:
        ::QLabel* label()
        {
            auto* result = new ::QLabel(this);
            result->setTextFormat(::Qt::PlainText);
            result->setWordWrap(true);
            return result;
        }

        void refresh()
        {
            if (!access_ || busy_)
            {
                return;
            }
            const auto revision = this->begin(QStringLiteral("正在读取音色…"));
            server_.get_json(
                QStringLiteral("/api/resources"), *this,
                [this, revision](const ::QJsonDocument& response, const ::QString& error)
                {
                    this->finish(revision, response, error, {});
                });
        }

        void update(const ::QString& path)
        {
            const auto id = this->voice().value(QStringLiteral("id")).toString();
            const auto revision = this->begin(QStringLiteral("正在保存音色…"));
            server_.post_json(
                path, {{QStringLiteral("id"), id}}, *this,
                [this, revision](const ::QJsonDocument& response, const ::QString& error)
                {
                    this->finish(revision, response, error, QStringLiteral("音色已更新"));
                });
        }

        void preview()
        {
            const auto voice = this->voice();
            auto text = preview_text_->text().trimmed();
            if (text.isEmpty())
            {
                text = voice.value(QStringLiteral("reference_text"))
                           .toString(QStringLiteral("你好，这是一段音色试听。"));
            }
            const auto revision = this->begin(QStringLiteral("正在提交音色试听…"));
            server_.post_json(QStringLiteral("/api/speech"),
                              {{QStringLiteral("text"), text},
                               {QStringLiteral("voice_id"), voice.value(QStringLiteral("id"))}},
                              *this,
                              [this, revision](const ::QJsonDocument&, const ::QString& error)
                              {
                                  if (revision == revision_)
                                  {
                                      busy_ = false;
                                      feedback_->setText(
                                          error.isEmpty() ? QStringLiteral("试听已入队") : error);
                                      this->controls();
                                  }
                              });
        }

        void import_voice()
        {
            const auto name = name_->text().trimmed();
            const auto language = language_->text().trimmed();
            const auto reference = reference_->text().trimmed();
            if (name.isEmpty() || language.isEmpty() || reference.isEmpty())
            {
                feedback_->setText(QStringLiteral("请填写名称、语言和参考文本"));
                return;
            }
            const auto revision = revision_;
            const auto file = ::QFileDialog::getOpenFileName(this, QStringLiteral("选择参考录音"),
                                                             {}, QStringLiteral("WAV (*.wav)"));
            if (file.isEmpty() || revision != revision_ || busy_ || !access_)
            {
                return;
            }
            const auto operation = this->begin(QStringLiteral("正在导入音色…"));
            server_.import_voice(
                {{QStringLiteral("name"), name},
                 {QStringLiteral("language"), language},
                 {QStringLiteral("reference_text"), reference}},
                file, *this,
                [this, operation](const ::QJsonDocument& response, const ::QString& error)
                {
                    this->finish(operation, response, error, QStringLiteral("音色已导入"));
                });
        }

        unsigned begin(const ::QString& message)
        {
            busy_ = true;
            feedback_->setText(message);
            this->controls();
            return ++revision_;
        }

        void finish(unsigned revision, const ::QJsonDocument& response, const ::QString& error,
                    const ::QString& success)
        {
            if (revision != revision_)
            {
                return;
            }
            busy_ = false;
            if (!error.isEmpty())
            {
                feedback_->setText(error);
            }
            else if (!response.object().value(QStringLiteral("voices")).isArray())
            {
                feedback_->setText(QStringLiteral("服务返回了无效的音色列表"));
            }
            else
            {
                loaded_ = true;
                feedback_->setText(success);
                this->populate(response.object());
            }
            this->controls();
        }

        void populate(const ::QJsonObject& response)
        {
            const auto selected = this->voice().value(QStringLiteral("id")).toString();
            const auto active = response.value(QStringLiteral("active_voice_id")).toString();
            voices_->clear();
            if (response.value(QStringLiteral("default_voice_available")).toBool())
            {
                voices_->addItem(QStringLiteral("默认音色"),
                                 ::QJsonObject{{QStringLiteral("id"), QStringLiteral("default")},
                                               {QStringLiteral("available"), true}});
            }
            int selected_index = -1;
            int active_index = -1;
            const auto items = response.value(QStringLiteral("voices")).toArray();
            for (const auto& entry : items)
            {
                const auto voice = entry.toObject();
                const auto name = voice.value(QStringLiteral("name")).toString();
                const auto unavailable = !voice.value(QStringLiteral("available")).toBool();
                voices_->addItem(unavailable ? QStringLiteral("%1（不可用）").arg(name) : name,
                                 voice);
            }
            for (int index = 0; index < voices_->count(); ++index)
            {
                const auto id = voices_->itemData(index).toJsonObject().value(QStringLiteral("id"));
                if (id.toString() == selected)
                {
                    selected_index = index;
                }
                if (id.toString() == active)
                {
                    active_index = index;
                }
            }
            active_->setText(
                active_index < 0
                    ? QStringLiteral("当前音色未设置")
                    : QStringLiteral("当前音色　%1").arg(voices_->itemText(active_index)));
            voices_->setCurrentIndex(selected_index >= 0 ? selected_index
                                     : active_index >= 0 ? active_index
                                                         : voices_->currentIndex());
        }

        ::QJsonObject voice() const
        {
            return voices_->currentData().toJsonObject();
        }

        void controls()
        {
            const bool idle = access_ && !busy_;
            const auto voice = this->voice();
            const auto id = voice.value(QStringLiteral("id")).toString();
            const bool available = voice.value(QStringLiteral("available")).toBool();
            voices_->setEnabled(idle);
            reload_->setEnabled(idle);
            select_->setEnabled(idle && available);
            preview_->setEnabled(idle && available);
            remove_->setEnabled(idle && !id.isEmpty() && id != QStringLiteral("default"));
            preview_text_->setEnabled(idle && available);
            name_->setEnabled(idle);
            language_->setEnabled(idle);
            reference_->setEnabled(idle);
            import_->setEnabled(idle && loaded_);
        }

        meowlive::desktop::services::ServerClient& server_;
        ::QComboBox* voices_{};
        ::QLineEdit* name_{};
        ::QLineEdit* language_{};
        ::QLineEdit* reference_{};
        ::QLineEdit* preview_text_{};
        ::QPushButton* reload_{};
        ::QPushButton* select_{};
        ::QPushButton* preview_{};
        ::QPushButton* remove_{};
        ::QPushButton* import_{};
        ::QLabel* active_{};
        ::QLabel* feedback_{};
        unsigned revision_{};
        bool access_{};
        bool loaded_{};
        bool busy_{};
    };
}
