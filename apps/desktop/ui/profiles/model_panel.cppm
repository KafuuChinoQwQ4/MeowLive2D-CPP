module;

#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QString>
#include <QVBoxLayout>
#include <QWidget>

export module meowlive.desktop.ui.model_panel;

import meowlive.desktop.services.server_client;
import meowlive.desktop.ui.profile_selector;

export namespace meowlive::desktop::ui
{
    class ModelPanel final : public ::QWidget
    {
    public:
        ModelPanel(meowlive::desktop::services::ServerClient& server, ::QWidget* parent)
            : ::QWidget(parent), server_(server)
        {
            auto* layout = new ::QVBoxLayout(this);
            profiles_ = new meowlive::desktop::ui::ProfileSelector(
                QStringLiteral("模型档案"),
                [this](const ::QString& operation, const ::QString& name)
                {
                    this->profile_action(operation, name);
                },
                this);
            layout->addWidget(profiles_);

            this->build_fields(*layout);
            this->build_actions(*layout);
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
            this->render_settings({});
            status_->clear();
            this->controls();
            this->load();
        }

    private:
        void build_fields(::QVBoxLayout& layout)
        {
            form_ = new ::QWidget(this);
            auto* fields = new ::QFormLayout(form_);
            provider_ = new ::QLineEdit(form_);
            format_ = new ::QComboBox(form_);
            format_->addItem(QStringLiteral("OpenAI Chat"), QStringLiteral("openai_chat"));
            format_->addItem(QStringLiteral("OpenAI Responses"),
                             QStringLiteral("openai_responses"));
            format_->addItem(QStringLiteral("Anthropic Messages"),
                             QStringLiteral("anthropic_messages"));
            format_->addItem(QStringLiteral("Gemini GenerateContent"),
                             QStringLiteral("gemini_generate_content"));
            base_url_ = new ::QLineEdit(form_);
            model_ = new ::QComboBox(form_);
            model_->setEditable(true);
            model_->setInsertPolicy(::QComboBox::NoInsert);
            mode_ = new ::QComboBox(form_);
            mode_->addItem(QStringLiteral("云端"), QStringLiteral("cloud"));
            mode_->addItem(QStringLiteral("本地"), QStringLiteral("local"));
            timeout_ = new ::QSpinBox(form_);
            timeout_->setRange(1, 120);
            max_tokens_ = new ::QSpinBox(form_);
            max_tokens_->setRange(64, 65'536);
            json_mode_ = new ::QCheckBox(QStringLiteral("请求 JSON 输出"), form_);
            effort_ = new ::QComboBox(form_);
            effort_->addItem(QStringLiteral("default"));
            effort_->addItem(QStringLiteral("minimal"));
            effort_->addItem(QStringLiteral("low"));
            effort_->addItem(QStringLiteral("medium"));
            effort_->addItem(QStringLiteral("high"));
            effort_->addItem(QStringLiteral("xhigh"));
            effort_->addItem(QStringLiteral("max"));
            effort_->addItem(QStringLiteral("ultra"));
            api_key_ = new ::QLineEdit(form_);
            api_key_->setEchoMode(::QLineEdit::Password);
            clear_key_ = new ::QCheckBox(QStringLiteral("清除已保存密钥"), form_);
            fields->addRow(QStringLiteral("服务商"), provider_);
            fields->addRow(QStringLiteral("API 格式"), format_);
            fields->addRow(QStringLiteral("API 地址"), base_url_);
            fields->addRow(QStringLiteral("模型"), model_);
            fields->addRow(QStringLiteral("连接方式"), mode_);
            fields->addRow(QStringLiteral("超时（秒）"), timeout_);
            fields->addRow(QStringLiteral("最大输出令牌"), max_tokens_);
            fields->addRow(QStringLiteral("推理强度"), effort_);
            fields->addRow(json_mode_);
            fields->addRow(QStringLiteral("API 密钥"), api_key_);
            fields->addRow(clear_key_);
            layout.addWidget(form_);
            this->connect(clear_key_, &::QCheckBox::toggled, this,
                          [this](bool clear)
                          {
                              api_key_->setDisabled(clear);
                          });
        }

        void build_actions(::QVBoxLayout& layout)
        {
            auto* buttons = new ::QHBoxLayout;
            auto* reload = new ::QPushButton(QStringLiteral("读取模型设置"), this);
            save_ = new ::QPushButton(QStringLiteral("保存设置"), this);
            models_ = new ::QPushButton(QStringLiteral("获取模型"), this);
            test_ = new ::QPushButton(QStringLiteral("测试连接"), this);
            reasoning_ = new ::QPushButton(QStringLiteral("预览推理强度"), this);
            for (auto* button : {reload, save_, models_, test_, reasoning_})
            {
                buttons->addWidget(button);
            }
            buttons->addStretch();
            layout.addLayout(buttons);
            status_ = new ::QLabel(this);
            status_->setTextFormat(::Qt::PlainText);
            status_->setWordWrap(true);
            active_ = new ::QLabel(this);
            active_->setTextFormat(::Qt::PlainText);
            layout.addWidget(active_);
            layout.addWidget(status_);
            layout.addStretch();

            this->connect(reload, &::QPushButton::clicked, this, &ModelPanel::load);
            this->connect(save_, &::QPushButton::clicked, this,
                          [this]
                          {
                              auto body = this->draft();
                              body.insert(QStringLiteral("id"), selected_id_);
                              this->post(QStringLiteral("/api/llm/settings"), body);
                          });
            this->connect(test_, &::QPushButton::clicked, this,
                          [this]
                          {
                              this->post(QStringLiteral("/api/llm/test"), this->draft());
                          });
            this->connect(models_, &::QPushButton::clicked, this, &ModelPanel::list_models);
            this->connect(reasoning_, &::QPushButton::clicked, this,
                          &ModelPanel::preview_reasoning);
        }

        void load()
        {
            if (!access_ || busy_)
            {
                return;
            }
            busy_ = true;
            this->controls();
            const auto revision = revision_;
            server_.get_json(
                QStringLiteral("/api/llm/settings"), *this,
                [this, revision](const ::QJsonDocument& document, const ::QString& error)
                {
                    this->finish(revision, QStringLiteral("/api/llm/settings"), document, error);
                });
        }

        ::QJsonObject draft() const
        {
            return {
                {QStringLiteral("settings"),
                 ::QJsonObject{
                     {QStringLiteral("provider"), provider_->text().trimmed()},
                     {QStringLiteral("api_format"), format_->currentData().toString()},
                     {QStringLiteral("base_url"), base_url_->text().trimmed()},
                     {QStringLiteral("model"), model_->currentText().trimmed()},
                     {QStringLiteral("mode"), mode_->currentData().toString()},
                     {QStringLiteral("timeout_seconds"), timeout_->value()},
                     {QStringLiteral("max_tokens"), max_tokens_->value()},
                     {QStringLiteral("json_mode"), json_mode_->isChecked()},
                     {QStringLiteral("reasoning_effort"), effort_->currentText()},
                 }},
                {QStringLiteral("api_key"), api_key_->text().isEmpty() || clear_key_->isChecked()
                                                ? ::QJsonValue(::QJsonValue::Null)
                                                : ::QJsonValue(api_key_->text())},
                {QStringLiteral("clear_api_key"), clear_key_->isChecked()}};
        }

        void profile_action(const ::QString& operation, const ::QString& name)
        {
            auto path = QStringLiteral("/api/llm/profiles");
            if (operation == QStringLiteral("create"))
            {
                auto body = this->draft();
                body.insert(QStringLiteral("name"), name);
                this->post(path, body);
                return;
            }
            ::QJsonObject body{{QStringLiteral("id"), profiles_->id()}};
            if (operation == QStringLiteral("rename"))
            {
                body.insert(QStringLiteral("name"), name);
            }
            this->post(path + QStringLiteral("/") + operation, body);
        }

        void list_models()
        {
            auto body = this->draft();
            const auto settings = body.take(QStringLiteral("settings")).toObject();
            for (const auto& key : {QStringLiteral("provider"), QStringLiteral("api_format"),
                                    QStringLiteral("base_url"), QStringLiteral("mode")})
            {
                body.insert(key, settings.value(key));
            }
            this->post(QStringLiteral("/api/llm/models"), body);
        }

        void preview_reasoning()
        {
            const auto settings = this->draft().value(QStringLiteral("settings")).toObject();
            ::QJsonObject body;
            for (const auto& key :
                 {QStringLiteral("provider"), QStringLiteral("api_format"), QStringLiteral("model"),
                  QStringLiteral("reasoning_effort"), QStringLiteral("max_tokens")})
            {
                body.insert(key, settings.value(key));
            }
            this->post(QStringLiteral("/api/llm/reasoning"), body);
        }

        void post(const ::QString& path, const ::QJsonObject& body)
        {
            if (!access_ || busy_)
            {
                return;
            }
            busy_ = true;
            this->controls();
            const auto revision = revision_;
            status_->setText(QStringLiteral("处理中…"));
            server_.post_json(
                path, body, *this,
                [this, revision, path](const ::QJsonDocument& document, const ::QString& error)
                {
                    this->finish(revision, path, document, error);
                });
        }

        void finish(unsigned revision, const ::QString& path, const ::QJsonDocument& document,
                    const ::QString& error)
        {
            if (revision != revision_)
            {
                return;
            }
            busy_ = false;
            if (!error.isEmpty())
            {
                status_->setText(error);
                this->controls();
                return;
            }
            const auto result = document.object();
            if (path == QStringLiteral("/api/llm/models"))
            {
                const auto canonical = result.value(QStringLiteral("base_url")).toString();
                if (!canonical.isEmpty())
                {
                    base_url_->setText(canonical);
                }
                const auto model = model_->currentText();
                model_->clear();
                for (const auto& item : result.value(QStringLiteral("models")).toArray())
                {
                    const auto id = item.toObject().value(QStringLiteral("id")).toString();
                    if (!id.isEmpty())
                    {
                        model_->addItem(id);
                    }
                }
                model_->setEditText(model.isEmpty() ? model_->itemText(0) : model);
                status_->setText(QStringLiteral("找到 %1 个模型").arg(model_->count()));
            }
            else if (path == QStringLiteral("/api/llm/test"))
            {
                status_->setText(result.value(QStringLiteral("message"))
                                     .toString(QStringLiteral("连接测试通过")));
            }
            else if (path == QStringLiteral("/api/llm/reasoning"))
            {
                const auto error = result.value(QStringLiteral("error")).toString();
                const auto note = result.value(QStringLiteral("note")).toString();
                const auto strategy = result.value(QStringLiteral("strategy")).toString();
                const auto effective = result.value(QStringLiteral("effective")).toString();
                status_->setText(
                    !error.isEmpty()
                        ? error
                        : QStringLiteral("%1 · %2　%3").arg(strategy, effective, note).trimmed());
            }
            else
            {
                loaded_ = true;
                this->render_settings(result);
                status_->setText(QStringLiteral("模型设置已读取"));
            }
            this->controls();
        }

        void render_settings(const ::QJsonObject& result)
        {
            profiles_->update_profiles(result);
            selected_id_ = result.value(QStringLiteral("selected_profile_id"));
            if (selected_id_.isUndefined())
            {
                selected_id_ = ::QJsonValue::Null;
            }
            const auto settings = result.value(QStringLiteral("settings")).toObject();
            provider_->setText(
                settings.value(QStringLiteral("provider")).toString(QStringLiteral("custom")));
            format_->setCurrentIndex(
                format_->findData(settings.value(QStringLiteral("api_format"))
                                      .toString(QStringLiteral("openai_chat"))));
            base_url_->setText(settings.value(QStringLiteral("base_url")).toString());
            model_->clear();
            model_->setEditText(settings.value(QStringLiteral("model")).toString());
            mode_->setCurrentIndex(mode_->findData(
                settings.value(QStringLiteral("mode")).toString(QStringLiteral("cloud"))));
            timeout_->setValue(settings.value(QStringLiteral("timeout_seconds")).toInt(30));
            max_tokens_->setValue(settings.value(QStringLiteral("max_tokens")).toInt(1024));
            json_mode_->setChecked(settings.value(QStringLiteral("json_mode")).toBool(true));
            effort_->setCurrentText(settings.value(QStringLiteral("reasoning_effort"))
                                        .toString(QStringLiteral("default")));
            api_key_->clear();
            api_key_->setPlaceholderText(result.value(QStringLiteral("key_configured")).toBool()
                                             ? QStringLiteral("已保存密钥")
                                             : QStringLiteral("未设置密钥"));
            clear_key_->setChecked(false);
            const auto active = result.value(QStringLiteral("active_model")).toString();
            active_->setText(!active.isEmpty() ? QStringLiteral("当前模型　%1").arg(active)
                                               : ::QString{});
            if (result.value(QStringLiteral("restart_required")).toBool())
            {
                active_->setText(active_->text() + QStringLiteral("　保存后需重启主服务"));
            }
        }

        void controls()
        {
            this->setEnabled(access_ && !busy_);
            for (auto* control :
                 {static_cast<::QWidget*>(profiles_), form_, static_cast<::QWidget*>(save_),
                  static_cast<::QWidget*>(models_), static_cast<::QWidget*>(test_),
                  static_cast<::QWidget*>(reasoning_)})
            {
                control->setEnabled(loaded_);
            }
        }

        meowlive::desktop::services::ServerClient& server_;
        meowlive::desktop::ui::ProfileSelector* profiles_{};
        ::QWidget* form_{};
        ::QLineEdit* provider_{};
        ::QComboBox* format_{};
        ::QLineEdit* base_url_{};
        ::QComboBox* model_{};
        ::QComboBox* mode_{};
        ::QSpinBox* timeout_{};
        ::QSpinBox* max_tokens_{};
        ::QCheckBox* json_mode_{};
        ::QComboBox* effort_{};
        ::QLineEdit* api_key_{};
        ::QCheckBox* clear_key_{};
        ::QPushButton* save_{};
        ::QPushButton* models_{};
        ::QPushButton* test_{};
        ::QPushButton* reasoning_{};
        ::QLabel* active_{};
        ::QLabel* status_{};
        ::QJsonValue selected_id_{::QJsonValue::Null};
        unsigned revision_{};
        bool access_{};
        bool busy_{};
        bool loaded_{};
    };
}
