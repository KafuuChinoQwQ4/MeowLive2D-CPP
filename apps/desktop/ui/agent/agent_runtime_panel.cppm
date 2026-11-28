module;

#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QString>
#include <QVBoxLayout>
#include <QWidget>

#include <array>

export module meowlive.desktop.ui.agent_runtime_panel;

import meowlive.desktop.services.server_client;

export namespace meowlive::desktop::ui
{
    class AgentRuntimePanel final : public ::QWidget
    {
    public:
        AgentRuntimePanel(meowlive::desktop::services::ServerClient& server, ::QWidget* parent)
            : ::QWidget(parent), server_(server)
        {
            auto* layout = new ::QVBoxLayout(this);
            form_ = new ::QWidget(this);
            auto* fields = new ::QFormLayout(form_);
            for (auto& flag : flags_)
            {
                flag.input = new ::QCheckBox(::QString::fromUtf8(flag.label), form_);
                fields->addRow(flag.input);
            }
            provider_ = new ::QComboBox(form_);
            provider_->addItem(QStringLiteral("Brave"), QStringLiteral("brave"));
            provider_->addItem(QStringLiteral("SearXNG"), QStringLiteral("searxng"));
            endpoint_ = new ::QLineEdit(form_);
            rounds_ = new ::QSpinBox(form_);
            rounds_->setRange(1, 3);
            timeout_ = new ::QSpinBox(form_);
            timeout_->setRange(1, 8);
            key_ = new ::QLineEdit(form_);
            key_->setEchoMode(::QLineEdit::Password);
            clear_ = new ::QCheckBox(QStringLiteral("清除已保存的搜索密钥"), form_);
            fields->addRow(QStringLiteral("搜索服务"), provider_);
            fields->addRow(QStringLiteral("搜索地址"), endpoint_);
            fields->addRow(QStringLiteral("搜索密钥"), key_);
            fields->addRow(clear_);
            fields->addRow(QStringLiteral("最大工具轮次"), rounds_);
            fields->addRow(QStringLiteral("工具超时（秒）"), timeout_);
            layout->addWidget(form_);

            auto* buttons = new ::QHBoxLayout;
            auto* reload = new ::QPushButton(QStringLiteral("读取运行设置"), this);
            save_ = new ::QPushButton(QStringLiteral("保存运行设置"), this);
            buttons->addWidget(save_);
            buttons->addWidget(reload);
            buttons->addStretch();
            layout->addLayout(buttons);
            status_ = new ::QLabel(this);
            status_->setTextFormat(::Qt::PlainText);
            status_->setWordWrap(true);
            layout->addWidget(status_);
            layout->addStretch();
            this->connect(reload, &::QPushButton::clicked, this,
                          [this]
                          {
                              this->request(false);
                          });
            this->connect(save_, &::QPushButton::clicked, this,
                          [this]
                          {
                              this->request(true);
                          });
            this->connect(clear_, &::QCheckBox::toggled, this,
                          [this](bool clear)
                          {
                              key_->setDisabled(clear);
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
            this->display({});
            status_->clear();
            this->controls();
            if (access_)
            {
                this->request(false);
            }
        }

    private:
        struct Flag
        {
            const char* key;
            const char* label;
            ::QCheckBox* input{};
        };

        void request(bool save)
        {
            if (!access_ || busy_)
            {
                return;
            }
            busy_ = true;
            this->controls();
            const auto revision = revision_;
            auto complete =
                [this, revision, save](const ::QJsonDocument& result, const ::QString& error)
            {
                if (revision != revision_)
                {
                    return;
                }
                busy_ = false;
                const auto snapshot = result.object();
                if (error.isEmpty() && snapshot.value(QStringLiteral("settings")).isObject())
                {
                    loaded_ = true;
                    this->display(snapshot);
                    status_->setText(save ? QStringLiteral("运行设置已保存") : ::QString{});
                }
                else
                {
                    status_->setText(error.isEmpty() ? QStringLiteral("运行设置响应无效") : error);
                }
                this->controls();
            };

            const auto path = QStringLiteral("/api/agent/runtime");
            if (!save)
            {
                server_.get_json(path, *this, complete);
                return;
            }
            ::QJsonObject settings{
                {QStringLiteral("search_provider"), provider_->currentData().toString()},
                {QStringLiteral("search_endpoint"), endpoint_->text().trimmed()},
                {QStringLiteral("max_tool_rounds"), rounds_->value()},
                {QStringLiteral("tool_timeout_seconds"), timeout_->value()},
            };
            for (const auto& flag : flags_)
            {
                settings.insert(::QString::fromLatin1(flag.key), flag.input->isChecked());
            }
            ::QJsonObject body{
                {QStringLiteral("settings"), settings},
                {QStringLiteral("clear_search_api_key"), clear_->isChecked()},
            };
            if (!clear_->isChecked() && !key_->text().isEmpty())
            {
                body.insert(QStringLiteral("search_api_key"), key_->text());
            }
            server_.post_json(path, body, *this, complete);
        }

        void display(const ::QJsonObject& snapshot)
        {
            const auto settings = snapshot.value(QStringLiteral("settings")).toObject();
            for (const auto& flag : flags_)
            {
                flag.input->setChecked(settings.value(::QString::fromLatin1(flag.key)).toBool());
            }
            provider_->setCurrentIndex(
                provider_->findData(settings.value(QStringLiteral("search_provider"))
                                        .toString(QStringLiteral("brave"))));
            endpoint_->setText(settings.value(QStringLiteral("search_endpoint")).toString());
            rounds_->setValue(settings.value(QStringLiteral("max_tool_rounds")).toInt(2));
            timeout_->setValue(settings.value(QStringLiteral("tool_timeout_seconds")).toInt(6));
            key_->clear();
            key_->setPlaceholderText(
                snapshot.value(QStringLiteral("search_key_configured")).toBool()
                    ? QStringLiteral("已保存密钥")
                    : QStringLiteral("未设置密钥"));
            clear_->setChecked(false);
        }

        void controls()
        {
            this->setEnabled(access_ && !busy_);
            form_->setEnabled(loaded_);
            save_->setEnabled(loaded_);
        }

        meowlive::desktop::services::ServerClient& server_;
        std::array<Flag, 5> flags_{{
            {"streaming", "启用流式响应"},
            {"cache_enabled", "启用模型缓存"},
            {"tools_enabled", "允许调用工具"},
            {"environment_enabled", "提供运行环境信息"},
            {"web_search_enabled", "允许网页搜索"},
        }};
        ::QWidget* form_{};
        ::QComboBox* provider_{};
        ::QLineEdit* endpoint_{};
        ::QSpinBox* rounds_{};
        ::QSpinBox* timeout_{};
        ::QLineEdit* key_{};
        ::QCheckBox* clear_{};
        ::QPushButton* save_{};
        ::QLabel* status_{};
        unsigned revision_{};
        bool access_{};
        bool busy_{};
        bool loaded_{};
    };
}
