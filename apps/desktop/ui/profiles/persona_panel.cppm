module;

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QString>
#include <QVBoxLayout>
#include <QWidget>

export module meowlive.desktop.ui.persona_panel;

import meowlive.desktop.services.server_client;
import meowlive.desktop.ui.profile_selector;

export namespace meowlive::desktop::ui
{
    class PersonaPanel final : public ::QWidget
    {
    public:
        PersonaPanel(meowlive::desktop::services::ServerClient& server, ::QWidget* parent)
            : ::QWidget(parent), server_(server)
        {
            auto* layout = new ::QVBoxLayout(this);
            profiles_ = new meowlive::desktop::ui::ProfileSelector(
                QStringLiteral("人物卡档案"),
                [this](const ::QString& operation, const ::QString& name)
                {
                    this->edit(operation, name);
                },
                this);
            body_ = new ::QPlainTextEdit(this);
            body_->setAccessibleName(QStringLiteral("人物卡正文"));
            body_->setPlaceholderText(QStringLiteral("填写人物身份、性格和说话方式"));
            auto* reload = new ::QPushButton(QStringLiteral("读取人物卡"), this);
            save_ = new ::QPushButton(QStringLiteral("保存正文"), this);
            status_ = new ::QLabel(this);
            status_->setTextFormat(::Qt::PlainText);
            status_->setWordWrap(true);
            layout->addWidget(profiles_);
            layout->addWidget(body_, 1);
            layout->addWidget(save_, 0, ::Qt::AlignLeft);
            layout->addWidget(reload, 0, ::Qt::AlignLeft);
            layout->addWidget(status_);
            this->connect(save_, &::QPushButton::clicked, this,
                          [this]
                          {
                              this->edit(QStringLiteral("update"), selected_name_);
                          });
            this->connect(reload, &::QPushButton::clicked, this, &PersonaPanel::load);
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
            selected_id_.clear();
            selected_name_.clear();
            body_->clear();
            status_->clear();
            profiles_->update_profiles({});
            this->controls();
            this->load();
        }

    private:
        void load()
        {
            if (!access_ || busy_)
            {
                return;
            }
            busy_ = true;
            this->controls();
            const auto revision = revision_;
            server_.get_json(QStringLiteral("/api/agent/personas"), *this,
                             [this, revision](const ::QJsonDocument& result, const ::QString& error)
                             {
                                 this->finish(revision, result, error, {});
                             });
        }

        void edit(const ::QString& operation, const ::QString& name)
        {
            if (!access_ || busy_)
            {
                return;
            }
            const auto path = QStringLiteral("/api/agent/personas");
            auto endpoint = path;
            ::QJsonObject body;
            if (operation == QStringLiteral("create"))
            {
                body.insert(QStringLiteral("name"), name);
                body.insert(QStringLiteral("persona"), body_->toPlainText());
            }
            else
            {
                endpoint += QStringLiteral("/") + operation;
                const bool update = operation == QStringLiteral("update");
                body.insert(QStringLiteral("id"), update ? selected_id_ : profiles_->id());
                if (operation == QStringLiteral("rename") || update)
                {
                    body.insert(QStringLiteral("name"), name);
                }
                if (update)
                {
                    body.insert(QStringLiteral("persona"), body_->toPlainText());
                }
            }
            busy_ = true;
            this->controls();
            const auto revision = revision_;
            server_.post_json(
                endpoint, body, *this,
                [this, revision](const ::QJsonDocument& result, const ::QString& error)
                {
                    this->finish(revision, result, error, QStringLiteral("人物卡已更新"));
                });
        }

        void finish(unsigned revision, const ::QJsonDocument& result, const ::QString& error,
                    const ::QString& success)
        {
            if (revision != revision_)
            {
                return;
            }
            busy_ = false;
            if (error.isEmpty())
            {
                const auto snapshot = result.object();
                loaded_ = true;
                profiles_->update_profiles(snapshot);
                selected_id_ = snapshot.value(QStringLiteral("selected_profile_id")).toString();
                selected_name_.clear();
                body_->clear();
                const auto profiles = snapshot.value(QStringLiteral("profiles")).toArray();
                for (const auto& profile : profiles)
                {
                    const auto item = profile.toObject();
                    if (item.value(QStringLiteral("id")).toString() == selected_id_)
                    {
                        body_->setPlainText(item.value(QStringLiteral("persona")).toString());
                        selected_name_ = item.value(QStringLiteral("name")).toString();
                        break;
                    }
                }
            }
            status_->setText(error.isEmpty() ? success : error);
            this->controls();
        }

        void controls()
        {
            this->setEnabled(access_ && !busy_);
            profiles_->setEnabled(loaded_);
            save_->setEnabled(loaded_ && !selected_id_.isEmpty());
        }

        meowlive::desktop::services::ServerClient& server_;
        meowlive::desktop::ui::ProfileSelector* profiles_{};
        ::QPlainTextEdit* body_{};
        ::QPushButton* save_{};
        ::QLabel* status_{};
        ::QString selected_id_;
        ::QString selected_name_;
        unsigned revision_{};
        bool access_{};
        bool busy_{};
        bool loaded_{};
    };
}
