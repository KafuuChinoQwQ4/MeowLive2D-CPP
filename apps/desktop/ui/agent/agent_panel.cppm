module;

#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QString>
#include <QVBoxLayout>
#include <QWidget>

export module meowlive.desktop.ui.agent_panel;

import meowlive.desktop.services.server_client;

export namespace meowlive::desktop::ui
{
    class AgentPanel final : public ::QWidget
    {
    public:
        AgentPanel(meowlive::desktop::services::ServerClient& server, ::QWidget* parent)
            : ::QWidget(parent), server_(server)
        {
            auto* layout = new ::QVBoxLayout(this);
            auto* controls = new ::QHBoxLayout;
            phase_ = new ::QLabel(this);
            phase_->setTextFormat(::Qt::PlainText);
            toggle_ = new ::QPushButton(QStringLiteral("恢复 Agent"), this);
            controls->addWidget(phase_);
            controls->addStretch();
            controls->addWidget(toggle_);
            layout->addLayout(controls);

            auto* scroll = new ::QScrollArea(this);
            scroll->setWidgetResizable(true);
            form_ = new ::QWidget(scroll);
            auto* fields = new ::QFormLayout(form_);
            prompt_ = new ::QPlainTextEdit(form_);
            prompt_->setMaximumHeight(100);
            topic_ = new ::QLineEdit(form_);
            proactive_ = new ::QCheckBox(QStringLiteral("允许主动发言"), form_);
            cooldown_ = this->numeric(1'000, 3'600'000, 1'000);
            mode_ = new ::QComboBox(form_);
            mode_->addItem(QStringLiteral("自动"), QStringLiteral("auto"));
            mode_->addItem(QStringLiteral("全读"), QStringLiteral("all"));
            mode_->addItem(QStringLiteral("选择性朗读"), QStringLiteral("selective"));
            welcome_ = new ::QCheckBox(QStringLiteral("欢迎进房观众"), form_);
            chat_count_ = this->numeric(1, 1'000, 1);
            enter_count_ = this->numeric(1, 1'000, 1);
            pending_count_ = this->numeric(1, 512, 1);
            welcome_cooldown_ = this->numeric(1'000, 3'600'000, 1'000);
            viewer_cooldown_ = this->numeric(1'000, 86'400'000, 1'000);
            fields->addRow(QStringLiteral("系统提示词"), prompt_);
            fields->addRow(QStringLiteral("主题"), topic_);
            fields->addRow(proactive_);
            fields->addRow(QStringLiteral("主动发言冷却（毫秒）"), cooldown_);
            fields->addRow(QStringLiteral("弹幕朗读"), mode_);
            fields->addRow(welcome_);
            fields->addRow(QStringLiteral("繁忙弹幕数"), chat_count_);
            fields->addRow(QStringLiteral("繁忙进房数"), enter_count_);
            fields->addRow(QStringLiteral("繁忙待处理数"), pending_count_);
            fields->addRow(QStringLiteral("欢迎冷却（毫秒）"), welcome_cooldown_);
            fields->addRow(QStringLiteral("同观众欢迎冷却（毫秒）"), viewer_cooldown_);
            scroll->setWidget(form_);
            layout->addWidget(scroll, 1);

            auto* buttons = new ::QHBoxLayout;
            save_ = new ::QPushButton(QStringLiteral("保存 Agent 设置"), this);
            reload_ = new ::QPushButton(QStringLiteral("重新读取设置"), this);
            buttons->addWidget(save_);
            buttons->addWidget(reload_);
            buttons->addStretch();
            layout->addLayout(buttons);
            status_ = new ::QLabel(this);
            status_->setTextFormat(::Qt::PlainText);
            status_->setWordWrap(true);
            layout->addWidget(status_);

            this->connect(toggle_, &::QPushButton::clicked, this,
                          [this]
                          {
                              this->post(paused_ ? QStringLiteral("/api/agent/resume")
                                                 : QStringLiteral("/api/agent/pause"),
                                         {}, false);
                          });
            this->connect(reload_, &::QPushButton::clicked, this,
                          [this]
                          {
                              this->refresh(true);
                          });
            this->connect(save_, &::QPushButton::clicked, this,
                          [this]
                          {
                              this->post(QStringLiteral("/api/agent/settings"), this->settings(),
                                         true);
                          });
            this->update_controls();
        }

        void set_access(bool access)
        {
            if (access == access_)
            {
                return;
            }
            access_ = access;
            ++revision_;
            loaded_ = false;
            polling_ = false;
            busy_ = false;
            if (!access_)
            {
                this->show_settings({});
                phase_->clear();
                status_->clear();
            }
            this->update_controls();
            if (this->isVisible())
            {
                this->refresh(true);
            }
        }

        void refresh(bool reload = false)
        {
            if (!access_ || polling_ || busy_)
            {
                return;
            }
            polling_ = true;
            reload_->setEnabled(false);
            const auto revision = revision_;
            server_.get_json(
                QStringLiteral("/api/agent"), *this,
                [this, revision, reload](const ::QJsonDocument& result, const ::QString& error)
                {
                    if (revision != revision_)
                    {
                        return;
                    }
                    polling_ = false;
                    if (!busy_)
                    {
                        this->finish(result, error, reload || !loaded_, {});
                    }
                });
        }

    private:
        ::QSpinBox* numeric(int minimum, int maximum, int step)
        {
            auto* control = new ::QSpinBox(form_);
            control->setRange(minimum, maximum);
            control->setSingleStep(step);
            return control;
        }

        ::QJsonObject settings() const
        {
            return {{QStringLiteral("persona"), compatibility_persona_},
                    {QStringLiteral("system_prompt"), prompt_->toPlainText()},
                    {QStringLiteral("topic"), topic_->text()},
                    {QStringLiteral("proactive_enabled"), proactive_->isChecked()},
                    {QStringLiteral("cooldown_ms"), cooldown_->value()},
                    {QStringLiteral("interaction"),
                     ::QJsonObject{
                         {QStringLiteral("chat_read_mode"), mode_->currentData().toString()},
                         {QStringLiteral("welcome_enabled"), welcome_->isChecked()},
                         {QStringLiteral("busy_chat_count"), chat_count_->value()},
                         {QStringLiteral("busy_enter_count"), enter_count_->value()},
                         {QStringLiteral("busy_pending_count"), pending_count_->value()},
                         {QStringLiteral("welcome_cooldown_ms"), welcome_cooldown_->value()},
                         {QStringLiteral("welcome_viewer_cooldown_ms"), viewer_cooldown_->value()},
                     }}};
        }

        void show_settings(const ::QJsonObject& settings)
        {
            compatibility_persona_ = settings.value(QStringLiteral("persona")).toString();
            prompt_->setPlainText(settings.value(QStringLiteral("system_prompt")).toString());
            topic_->setText(settings.value(QStringLiteral("topic")).toString());
            proactive_->setChecked(settings.value(QStringLiteral("proactive_enabled")).toBool());
            cooldown_->setValue(settings.value(QStringLiteral("cooldown_ms")).toInt(30'000));
            const auto interaction = settings.value(QStringLiteral("interaction")).toObject();
            mode_->setCurrentIndex(
                mode_->findData(interaction.value(QStringLiteral("chat_read_mode"))
                                    .toString(QStringLiteral("auto"))));
            welcome_->setChecked(interaction.value(QStringLiteral("welcome_enabled")).toBool(true));
            chat_count_->setValue(interaction.value(QStringLiteral("busy_chat_count")).toInt(6));
            enter_count_->setValue(interaction.value(QStringLiteral("busy_enter_count")).toInt(3));
            pending_count_->setValue(
                interaction.value(QStringLiteral("busy_pending_count")).toInt(4));
            welcome_cooldown_->setValue(
                interaction.value(QStringLiteral("welcome_cooldown_ms")).toInt(30'000));
            viewer_cooldown_->setValue(
                interaction.value(QStringLiteral("welcome_viewer_cooldown_ms")).toInt(600'000));
        }

        void post(const ::QString& path, const ::QJsonObject& body, bool save)
        {
            if (!access_ || busy_)
            {
                return;
            }
            ++revision_;
            const auto revision = revision_;
            busy_ = true;
            polling_ = false;
            this->update_controls();
            server_.post_json(
                path, body, *this,
                [this, revision, save](const ::QJsonDocument& result, const ::QString& error)
                {
                    if (revision == revision_)
                    {
                        busy_ = false;
                        this->finish(result, error, save,
                                     save ? QStringLiteral("Agent 设置已保存") : ::QString{});
                    }
                });
        }

        void finish(const ::QJsonDocument& result, const ::QString& error, bool reload,
                    const ::QString& success)
        {
            if (!error.isEmpty())
            {
                status_->setText(error);
                this->update_controls();
                return;
            }
            const auto snapshot = result.object();
            const auto settings = snapshot.value(QStringLiteral("settings")).toObject();
            if (reload && !settings.isEmpty())
            {
                this->show_settings(settings);
                loaded_ = true;
            }
            if (snapshot.value(QStringLiteral("paused")).isBool())
            {
                paused_ = snapshot.value(QStringLiteral("paused")).toBool();
                const auto phase = snapshot.value(QStringLiteral("phase")).toString();
                if (paused_)
                {
                    phase_->setText(QStringLiteral("Agent 已暂停"));
                }
                else if (phase == QStringLiteral("deciding"))
                {
                    phase_->setText(QStringLiteral("Agent 正在决策"));
                }
                else if (phase == QStringLiteral("speaking"))
                {
                    phase_->setText(QStringLiteral("Agent 正在播报"));
                }
                else
                {
                    phase_->setText(QStringLiteral("Agent 等待事件"));
                }
                toggle_->setText(paused_ ? QStringLiteral("恢复 Agent")
                                         : QStringLiteral("暂停 Agent"));
            }
            if (reload || !success.isEmpty() ||
                !snapshot.value(QStringLiteral("last_error")).toString().isEmpty())
            {
                status_->setText(success.isEmpty()
                                     ? snapshot.value(QStringLiteral("last_error")).toString()
                                     : success);
            }
            this->update_controls();
        }

        void update_controls()
        {
            form_->setEnabled(access_ && loaded_ && !busy_);
            save_->setEnabled(access_ && loaded_ && !busy_);
            toggle_->setEnabled(access_ && loaded_ && !busy_);
            reload_->setEnabled(access_ && !busy_ && !polling_);
        }

        meowlive::desktop::services::ServerClient& server_;
        ::QWidget* form_{};
        ::QLabel* phase_{};
        ::QPushButton* toggle_{};
        ::QPlainTextEdit* prompt_{};
        ::QLineEdit* topic_{};
        ::QCheckBox* proactive_{};
        ::QSpinBox* cooldown_{};
        ::QComboBox* mode_{};
        ::QCheckBox* welcome_{};
        ::QSpinBox* chat_count_{};
        ::QSpinBox* enter_count_{};
        ::QSpinBox* pending_count_{};
        ::QSpinBox* welcome_cooldown_{};
        ::QSpinBox* viewer_cooldown_{};
        ::QPushButton* save_{};
        ::QPushButton* reload_{};
        ::QLabel* status_{};
        ::QString compatibility_persona_;
        unsigned revision_{};
        bool access_{};
        bool busy_{};
        bool polling_{};
        bool loaded_{};
        bool paused_{true};
    };
}
