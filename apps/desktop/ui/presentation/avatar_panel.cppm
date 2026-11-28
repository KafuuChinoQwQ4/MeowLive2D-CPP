module;

#include <QComboBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QString>
#include <QTimer>
#include <QWidget>

#include <functional>
#include <utility>

export module meowlive.desktop.ui.avatar_panel;

import meowlive.desktop.services.server_client;
import meowlive.desktop_runtime.avatar.presentation;

export namespace meowlive::desktop::ui
{
    class AvatarPanel final : public ::QWidget
    {
    public:
        AvatarPanel(meowlive::desktop::services::ServerClient& server, ::QWidget* parent)
            : ::QWidget(parent), server_(server), presentation_(
                                                      [this](const ::QString& message, bool ready)
                                                      {
                                                          status_->setText(message);
                                                          this->set_connected(ready);
                                                      })
        {
            auto* layout = new ::QFormLayout(this);
            endpoint_ = new ::QLineEdit(QStringLiteral("ws://127.0.0.1:8001"), this);
            endpoint_->setAccessibleName(QStringLiteral("VTube Studio 地址"));
            layout->addRow(QStringLiteral("WebSocket 地址"), endpoint_);
            status_ = new ::QLabel(QStringLiteral("VTube Studio 未连接"), this);
            status_->setWordWrap(true);
            layout->addRow(QStringLiteral("连接状态"), status_);

            auto* connection_actions = new ::QHBoxLayout;
            auto* connect = new ::QPushButton(QStringLiteral("连接"), this);
            auto* authorize = new ::QPushButton(QStringLiteral("重新授权"), this);
            auto* disconnect = new ::QPushButton(QStringLiteral("断开"), this);
            connection_actions->addWidget(connect);
            connection_actions->addWidget(authorize);
            connection_actions->addWidget(disconnect);
            layout->addRow(connection_actions);

            model_ = new ::QComboBox(this);
            hotkey_ = new ::QComboBox(this);
            layout->addRow(QStringLiteral("角色模型"), model_);
            layout->addRow(QStringLiteral("预览快捷键"), hotkey_);
            auto* model_actions = new ::QHBoxLayout;
            refresh_ = new ::QPushButton(QStringLiteral("刷新模型"), this);
            select_ = new ::QPushButton(QStringLiteral("加载模型"), this);
            preview_ = new ::QPushButton(QStringLiteral("预览快捷键"), this);
            model_actions->addWidget(refresh_);
            model_actions->addWidget(select_);
            model_actions->addWidget(preview_);
            layout->addRow(model_actions);
            auto* hint = new ::QLabel(
                QStringLiteral("在 VTube Studio 的模型设置中将 MeowMouthOpen 映射到张嘴参数。"),
                this);
            hint->setWordWrap(true);
            layout->addRow(hint);

            this->connect(connect, &::QPushButton::clicked, this,
                          [this]
                          {
                              presentation_.start(endpoint_->text().trimmed());
                          });
            this->connect(authorize, &::QPushButton::clicked, this,
                          [this]
                          {
                              presentation_.start(endpoint_->text().trimmed(), true);
                          });
            this->connect(disconnect, &::QPushButton::clicked, this,
                          [this]
                          {
                              presentation_.stop();
                              status_->setText(QStringLiteral("VTube Studio 未连接"));
                              this->set_connected(false);
                          });
            this->connect(refresh_, &::QPushButton::clicked, this,
                          [this]
                          {
                              this->refresh_models();
                          });
            this->connect(model_, &::QComboBox::currentIndexChanged, this,
                          [this](int)
                          {
                              this->refresh_hotkeys();
                          });
            this->connect(hotkey_, &::QComboBox::currentIndexChanged, this,
                          [this](int)
                          {
                              this->update_buttons();
                          });
            this->connect(select_, &::QPushButton::clicked, this,
                          [this]
                          {
                              presentation_.select_model(model_->currentData().toString(),
                                                         [this](const ::QJsonObject&)
                                                         {
                                                             status_->setText(
                                                                 QStringLiteral("已加载模型"));
                                                             this->refresh_models();
                                                         });
                          });
            this->connect(preview_, &::QPushButton::clicked, this,
                          [this]
                          {
                              presentation_.preview_hotkey(model_->currentData().toString(),
                                                           hotkey_->currentData().toString());
                          });
            this->connect(&playback_, &::QTimer::timeout, this,
                          [this]
                          {
                              presentation_.set_mouth_level(mouth_source_ ? mouth_source_() : 0.0);
                          });
            playback_.setTimerType(::Qt::PreciseTimer);
            this->set_connected(false);
        }

        void set_mouth_source(std::function<double()> source)
        {
            mouth_source_ = std::move(source);
        }

        meowlive::desktop_runtime::avatar::Presentation& presentation()
        {
            return presentation_;
        }

    private:
        void set_connected(bool ready)
        {
            if (ready && !playback_.isActive())
            {
                playback_.start(34);
                this->restore_character();
            }
            else if (!ready)
            {
                playback_.stop();
                ++connection_;
                model_->clear();
                hotkey_->clear();
            }
            this->update_buttons();
        }

        void restore_character()
        {
            const auto connection = ++connection_;
            server_.get_json(
                QStringLiteral("/api/resources"), *this,
                [this, connection](const ::QJsonDocument& document, const ::QString&)
                {
                    if (connection != connection_ || !presentation_.connected())
                    {
                        return;
                    }
                    const auto resources = document.object();
                    const auto active =
                        resources.value(QStringLiteral("active_character_id")).toString();
                    for (const auto& value :
                         resources.value(QStringLiteral("characters")).toArray())
                    {
                        const auto character = value.toObject();
                        if (!active.isEmpty() && character.value(QStringLiteral("id")) == active)
                        {
                            presentation_.load_model(
                                character.value(QStringLiteral("model_id")).toString(),
                                character.value(QStringLiteral("mouth_parameter")).toString(),
                                [this](const ::QJsonObject&)
                                {
                                    this->refresh_models();
                                },
                                [this](const ::QString&)
                                {
                                    this->refresh_models();
                                });
                            return;
                        }
                    }
                    this->refresh_models();
                });
        }

        void update_buttons()
        {
            const bool ready = presentation_.connected();
            model_->setEnabled(ready);
            hotkey_->setEnabled(ready);
            refresh_->setEnabled(ready);
            select_->setEnabled(ready && model_->currentIndex() >= 0);
            preview_->setEnabled(ready && hotkey_->currentIndex() >= 0);
        }

        void refresh_models()
        {
            presentation_.refresh_models(
                [this](const ::QJsonObject& response)
                {
                    const auto items = response.value(QStringLiteral("availableModels")).toArray();
                    int active = -1;
                    model_->blockSignals(true);
                    model_->clear();
                    for (const auto& value : items)
                    {
                        const auto model = value.toObject();
                        const auto id = model.value(QStringLiteral("modelID")).toString();
                        const auto name = model.value(QStringLiteral("modelName")).toString();
                        if (!id.isEmpty() && id.size() <= 1'024 && !name.isEmpty())
                        {
                            model_->addItem(name, id);
                            if (model.value(QStringLiteral("modelLoaded")).toBool())
                            {
                                active = model_->count() - 1;
                            }
                        }
                    }
                    if (active >= 0)
                    {
                        model_->setCurrentIndex(active);
                    }
                    model_->blockSignals(false);
                    this->refresh_hotkeys();
                });
        }

        void refresh_hotkeys()
        {
            hotkey_->clear();
            this->update_buttons();
            const auto id = model_->currentData().toString();
            if (!id.isEmpty())
            {
                presentation_.list_hotkeys(
                    id,
                    [this, id](const ::QJsonObject& response)
                    {
                        // A user may select a different model while the lookup is in flight.
                        if (model_->currentData().toString() != id)
                        {
                            this->refresh_hotkeys();
                            return;
                        }
                        if (response.value(QStringLiteral("modelID")) != id)
                        {
                            return;
                        }
                        const auto hotkeys =
                            response.value(QStringLiteral("availableHotkeys")).toArray();
                        for (const auto& value : hotkeys)
                        {
                            const auto entry = value.toObject();
                            const auto key = entry.value(QStringLiteral("hotkeyID")).toString();
                            const auto name = entry.value(QStringLiteral("name")).toString();
                            if (!key.isEmpty() && key.size() <= 1'024)
                            {
                                hotkey_->addItem(
                                    name.isEmpty() ? entry.value(QStringLiteral("type")).toString()
                                                   : name,
                                    key);
                            }
                        }
                        this->update_buttons();
                    });
            }
        }

        meowlive::desktop::services::ServerClient& server_;
        meowlive::desktop_runtime::avatar::Presentation presentation_;
        unsigned connection_{};
        std::function<double()> mouth_source_;
        ::QTimer playback_;
        ::QLineEdit* endpoint_{};
        ::QLabel* status_{};
        ::QComboBox* model_{};
        ::QComboBox* hotkey_{};
        ::QPushButton* refresh_{};
        ::QPushButton* select_{};
        ::QPushButton* preview_{};
    };
}
