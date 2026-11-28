module;

#include <QHBoxLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QPushButton>
#include <QString>
#include <QVBoxLayout>
#include <QWidget>

export module meowlive.desktop.ui.live_panel;

import meowlive.desktop.services.server_client;
import meowlive.desktop.ui.live_settings_form;

export namespace meowlive::desktop::ui
{
    class LivePanel final : public ::QWidget
    {
    public:
        LivePanel(meowlive::desktop::services::ServerClient& server, ::QWidget* parent)
            : ::QWidget(parent), server_(server)
        {
            auto* layout = new ::QVBoxLayout(this);
            auto* actions = new ::QHBoxLayout;
            status_ = this->label();
            actions->addWidget(status_);
            actions->addStretch();
            connection_ = new ::QPushButton(this);
            actions->addWidget(connection_);
            layout->addLayout(actions);
            details_ = this->label();
            live_error_ = this->label();
            layout->addWidget(details_);
            layout->addWidget(live_error_);

            form_ = new meowlive::desktop::ui::LiveSettingsForm(this);
            layout->addWidget(form_);
            auto* settings_actions = new ::QHBoxLayout;
            save_ = new ::QPushButton(QStringLiteral("保存直播设置"), this);
            reload_ = new ::QPushButton(QStringLiteral("重新读取直播设置"), this);
            settings_actions->addWidget(save_);
            settings_actions->addWidget(reload_);
            settings_actions->addStretch();
            layout->addLayout(settings_actions);
            feedback_ = this->label();
            layout->addWidget(feedback_);
            layout->addStretch();

            this->connect(connection_, &::QPushButton::clicked, this,
                          &LivePanel::change_connection);
            this->connect(save_, &::QPushButton::clicked, this, &LivePanel::save);
            this->connect(reload_, &::QPushButton::clicked, this, &LivePanel::load);
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
            polling_ = false;
            settings_requested_ = false;
            form_->clear();
            feedback_->clear();
            this->show_status({}, access ? QStringLiteral("正在读取直播状态…") : ::QString{});
            this->controls();
            if (this->isVisible())
            {
                this->refresh();
            }
        }

        void refresh()
        {
            if (!access_ || busy_ || polling_)
            {
                return;
            }
            if (!settings_requested_)
            {
                this->load();
                return;
            }
            polling_ = true;
            const auto revision = revision_;
            server_.get_json(
                QStringLiteral("/api/live"), *this,
                [this, revision](const ::QJsonDocument& document, const ::QString& error)
                {
                    if (revision == revision_)
                    {
                        polling_ = false;
                        this->show_status(document.object(), error);
                        this->controls();
                    }
                });
        }

    private:
        ::QLabel* label()
        {
            auto* control = new ::QLabel(this);
            control->setTextFormat(::Qt::PlainText);
            control->setWordWrap(true);
            return control;
        }

        void show_status(const ::QJsonObject& snapshot, const ::QString& error)
        {
            phase_.clear();
            configured_ = false;
            details_->clear();
            live_error_->clear();
            if (!error.isEmpty() || !access_)
            {
                status_->setText(error);
                return;
            }

            const auto phase = snapshot.value(QStringLiteral("phase")).toString();
            const auto configured = snapshot.value(QStringLiteral("configured"));
            const struct
            {
                ::QString phase;
                ::QString label;
            } states[]{{QStringLiteral("disabled"), QStringLiteral("直播接入未启用")},
                       {QStringLiteral("disconnected"), QStringLiteral("直播间未连接")},
                       {QStringLiteral("connecting"), QStringLiteral("正在连接直播间…")},
                       {QStringLiteral("connected"), QStringLiteral("直播间已连接")},
                       {QStringLiteral("reconnecting"), QStringLiteral("正在重连直播间…")},
                       {QStringLiteral("disconnecting"), QStringLiteral("正在断开直播间…")},
                       {QStringLiteral("failed"), QStringLiteral("直播连接失败")}};
            status_->setText(QStringLiteral("服务返回了无效的直播状态"));
            for (const auto& state : states)
            {
                if (phase == state.phase && configured.isBool())
                {
                    phase_ = phase;
                    configured_ = configured.toBool();
                    status_->setText(state.label);
                    break;
                }
            }
            if (phase_.isEmpty())
            {
                return;
            }

            const auto room = snapshot.value(QStringLiteral("room_id")).toString();
            details_->setText(
                QStringLiteral("房间 %1　接收 %2　去重 %3　拒绝 %4　重连 %5")
                    .arg(room.isEmpty() ? QStringLiteral("待连接") : room)
                    .arg(snapshot.value(QStringLiteral("accepted_events")).toInteger())
                    .arg(snapshot.value(QStringLiteral("duplicate_events")).toInteger())
                    .arg(snapshot.value(QStringLiteral("rejected_events")).toInteger())
                    .arg(snapshot.value(QStringLiteral("reconnect_attempts")).toInteger()));
            live_error_->setText(snapshot.value(QStringLiteral("last_error")).toString());
        }

        bool disconnected() const
        {
            return phase_ == QStringLiteral("disconnected") || phase_ == QStringLiteral("failed");
        }

        bool can_disconnect() const
        {
            return phase_ == QStringLiteral("connecting") ||
                   phase_ == QStringLiteral("connected") ||
                   phase_ == QStringLiteral("reconnecting");
        }

        void controls()
        {
            const bool idle = access_ && !busy_;
            const bool editable = idle && form_->storage_available() &&
                                  (this->disconnected() || phase_ == QStringLiteral("disabled"));
            form_->setEnabled(editable);
            save_->setEnabled(editable);
            reload_->setEnabled(idle);
            const bool disconnect = this->can_disconnect();
            const bool disconnecting = phase_ == QStringLiteral("disconnecting");
            connection_->setEnabled(idle && (disconnect || (configured_ && this->disconnected())));
            connection_->setText(disconnecting ? QStringLiteral("正在断开…")
                                 : disconnect  ? QStringLiteral("断开直播间")
                                               : QStringLiteral("连接直播间"));
        }

        unsigned begin(const ::QString& message)
        {
            busy_ = true;
            polling_ = false;
            feedback_->setText(message);
            this->controls();
            return ++revision_;
        }

        void load()
        {
            settings_requested_ = true;
            const auto revision = this->begin(QStringLiteral("正在读取直播设置…"));
            server_.get_json(
                QStringLiteral("/api/live/settings"), *this,
                [this, revision](const ::QJsonDocument& document, const ::QString& error)
                {
                    this->finish_settings(revision, document, error, {});
                });
        }

        void save()
        {
            ::QJsonObject body;
            const auto validation_error = form_->request(body);
            if (!validation_error.isEmpty())
            {
                feedback_->setText(validation_error);
                return;
            }
            const auto revision = this->begin(QStringLiteral("正在保存直播设置…"));
            server_.post_json(
                QStringLiteral("/api/live/settings"), body, *this,
                [this, revision](const ::QJsonDocument& document, const ::QString& error)
                {
                    this->finish_settings(revision, document, error,
                                          QStringLiteral("直播设置已保存"));
                });
        }

        void finish_settings(unsigned revision, const ::QJsonDocument& document,
                             const ::QString& error, const ::QString& success)
        {
            if (revision != revision_)
            {
                return;
            }
            busy_ = false;
            auto message = error;
            if (message.isEmpty())
            {
                if (!form_->load(document.object()))
                {
                    message = QStringLiteral("服务返回了无效的直播设置");
                }
                else
                {
                    message = form_->storage_available() ? success
                                                         : QStringLiteral("主服务无法保存直播设置");
                }
            }
            feedback_->setText(message);
            this->show_status({}, QStringLiteral("正在读取直播状态…"));
            this->controls();
            this->refresh();
        }

        void change_connection()
        {
            const bool disconnect = this->can_disconnect();
            const auto revision = this->begin(disconnect ? QStringLiteral("正在请求断开…")
                                                         : QStringLiteral("正在请求连接…"));
            server_.post_json(
                disconnect ? QStringLiteral("/api/live/disconnect")
                           : QStringLiteral("/api/live/connect"),
                {}, *this,
                [this, revision, disconnect](const ::QJsonDocument& document,
                                             const ::QString& error)
                {
                    if (revision != revision_)
                    {
                        return;
                    }
                    busy_ = false;
                    if (error.isEmpty())
                    {
                        this->show_status(document.object(), {});
                    }
                    feedback_->setText(
                        error.isEmpty() && disconnect
                            ? QStringLiteral("重新连接后，可在 Agent 设置中手动恢复互动。")
                            : error);
                    this->controls();
                    this->refresh();
                });
        }

        meowlive::desktop::services::ServerClient& server_;
        meowlive::desktop::ui::LiveSettingsForm* form_{};
        ::QLabel* status_{};
        ::QLabel* details_{};
        ::QLabel* live_error_{};
        ::QLabel* feedback_{};
        ::QPushButton* connection_{};
        ::QPushButton* save_{};
        ::QPushButton* reload_{};
        ::QString phase_;
        unsigned revision_{};
        bool access_{};
        bool busy_{};
        bool polling_{};
        bool settings_requested_{};
        bool configured_{};
    };
}
