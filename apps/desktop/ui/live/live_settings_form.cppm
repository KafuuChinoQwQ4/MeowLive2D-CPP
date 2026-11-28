module;

#include <QCheckBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QJsonObject>
#include <QLineEdit>
#include <QString>
#include <QWidget>

export module meowlive.desktop.ui.live_settings_form;

export namespace meowlive::desktop::ui
{
    class LiveSettingsForm final : public ::QGroupBox
    {
    public:
        explicit LiveSettingsForm(::QWidget* parent)
            : ::QGroupBox(QStringLiteral("哔哩哔哩直播设置"), parent)
        {
            auto* fields = new ::QFormLayout(this);
            enabled_ = new ::QCheckBox(QStringLiteral("启用哔哩哔哩直播接入"), this);
            app_id_ = new ::QLineEdit(this);
            app_id_->setPlaceholderText(QStringLiteral("直播开放平台的数字应用 ID"));
            clear_ = new ::QCheckBox(QStringLiteral("移除已保存的全部直播凭据"), this);
            fields->addRow(enabled_);
            fields->addRow(QStringLiteral("应用 ID"), app_id_);
            for (auto& credential : credentials_)
            {
                credential.input = new ::QLineEdit(this);
                credential.input->setEchoMode(::QLineEdit::Password);
                fields->addRow(credential.label, credential.input);
            }
            fields->addRow(clear_);

            this->connect(clear_, &::QCheckBox::toggled, this,
                          [this](bool clear)
                          {
                              enabled_->setDisabled(clear);
                              if (clear)
                              {
                                  enabled_->setChecked(false);
                              }
                              for (const auto& credential : credentials_)
                              {
                                  if (clear)
                                  {
                                      credential.input->clear();
                                  }
                                  credential.input->setDisabled(clear);
                              }
                          });
            this->connect(app_id_, &::QLineEdit::textChanged, this,
                          [this]
                          {
                              this->refresh_placeholders();
                          });
        }

        bool load(const ::QJsonObject& settings)
        {
            if (!settings.value(QStringLiteral("app_id")).isString() ||
                !settings.value(QStringLiteral("enabled")).isBool() ||
                !settings.value(QStringLiteral("storage_available")).isBool())
            {
                return false;
            }
            for (const auto& credential : credentials_)
            {
                if (!settings.value(credential.key + QStringLiteral("_configured")).isBool())
                {
                    return false;
                }
            }
            this->populate(settings);
            return true;
        }

        void clear()
        {
            this->populate({});
        }

        bool storage_available() const
        {
            return storage_available_;
        }

        ::QString request(::QJsonObject& body) const
        {
            const auto app_id = app_id_->text().trimmed();
            const bool clear = clear_->isChecked();
            const bool enabled = enabled_->isChecked();
            body = {{QStringLiteral("enabled"), enabled},
                    {QStringLiteral("app_id"), app_id},
                    {QStringLiteral("clear_credentials"), clear}};

            bool saved = false;
            bool all_supplied = true;
            for (const auto& credential : credentials_)
            {
                const auto value = credential.input->text().trimmed();
                saved |= credential.configured;
                all_supplied &= !value.isEmpty();
                body.insert(credential.key,
                            value.isEmpty() ? ::QJsonValue::Null : ::QJsonValue(value));
            }
            if (!clear && this->app_changed() && saved && !all_supplied)
            {
                return QStringLiteral("更换应用 ID 时，请重新填写全部凭据或选择移除已保存凭据。");
            }
            return {};
        }

    private:
        struct Credential
        {
            ::QString key;
            ::QString label;
            ::QLineEdit* input{};
            bool configured{};
        };

        void populate(const ::QJsonObject& settings)
        {
            saved_app_id_ = settings.value(QStringLiteral("app_id")).toString();
            storage_available_ = settings.value(QStringLiteral("storage_available")).toBool();
            clear_->setChecked(false);
            enabled_->setChecked(settings.value(QStringLiteral("enabled")).toBool());
            app_id_->setText(saved_app_id_);
            for (auto& credential : credentials_)
            {
                credential.input->clear();
                credential.configured =
                    settings.value(credential.key + QStringLiteral("_configured")).toBool();
            }
            this->refresh_placeholders();
        }

        bool app_changed() const
        {
            const auto entered = app_id_->text().trimmed();
            if (entered == saved_app_id_)
            {
                return false;
            }
            bool valid = false;
            const auto id = entered.toULongLong(&valid);
            return !valid || id != saved_app_id_.toULongLong();
        }

        void refresh_placeholders()
        {
            const bool app_changed = this->app_changed();
            for (const auto& credential : credentials_)
            {
                credential.input->setPlaceholderText(credential.configured && !app_changed
                                                         ? QStringLiteral("留空保留已保存凭据")
                                                         : credential.label);
            }
        }

        ::QCheckBox* enabled_{};
        ::QLineEdit* app_id_{};
        ::QCheckBox* clear_{};
        Credential credentials_[3]{
            {QStringLiteral("access_key_id"), QStringLiteral("AccessKey ID")},
            {QStringLiteral("access_key_secret"), QStringLiteral("AccessKey Secret")},
            {QStringLiteral("identity_code"), QStringLiteral("主播身份码")}};
        ::QString saved_app_id_;
        bool storage_available_{};
    };
}
