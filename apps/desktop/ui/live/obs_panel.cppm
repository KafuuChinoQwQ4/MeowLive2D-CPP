module;

#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QString>
#include <QVBoxLayout>
#include <QWidget>

#include <expected>

export module meowlive.desktop.ui.obs_panel;

import meowlive.desktop_runtime.obs.client;

export namespace meowlive::desktop::ui
{
    class ObsPanel final : public ::QWidget
    {
    public:
        ObsPanel(meowlive::desktop_runtime::obs::Client& obs, ::QWidget* parent)
            : ::QWidget(parent), obs_(obs)
        {
            auto* layout = new ::QVBoxLayout(this);
            auto* form = new ::QFormLayout;
            enabled_ = new ::QCheckBox(QStringLiteral("启用本机 OBS"), this);
            url_ = new ::QLineEdit(this);
            password_ = new ::QLineEdit(this);
            password_->setEchoMode(::QLineEdit::Password);
            clear_password_ = new ::QCheckBox(QStringLiteral("清除已保存密码"), this);
            form->addRow(enabled_);
            form->addRow(QStringLiteral("WebSocket 地址"), url_);
            form->addRow(QStringLiteral("WebSocket 密码"), password_);
            form->addRow(clear_password_);
            layout->addLayout(form);

            auto* settings = new ::QHBoxLayout;
            save_ = new ::QPushButton(QStringLiteral("保存 OBS 设置"), this);
            refresh_ = new ::QPushButton(QStringLiteral("查询 OBS 状态"), this);
            settings->addWidget(save_);
            settings->addWidget(refresh_);
            settings->addStretch();
            layout->addLayout(settings);

            auto* actions = new ::QHBoxLayout;
            scenes_ = new ::QComboBox(this);
            scene_ = new ::QPushButton(QStringLiteral("切换场景"), this);
            record_ = new ::QPushButton(this);
            stream_ = new ::QPushButton(this);
            actions->addWidget(scenes_, 1);
            actions->addWidget(scene_);
            actions->addWidget(record_);
            actions->addWidget(stream_);
            layout->addLayout(actions);

            status_ = new ::QLabel(this);
            status_->setTextFormat(::Qt::PlainText);
            status_->setWordWrap(true);
            layout->addWidget(status_);
            layout->addStretch();

            this->connect(clear_password_, &::QCheckBox::toggled, this,
                          [this](bool clear)
                          {
                              if (clear)
                              {
                                  password_->clear();
                              }
                              this->controls();
                          });
            this->connect(save_, &::QPushButton::clicked, this, &ObsPanel::save);
            this->connect(refresh_, &::QPushButton::clicked, this,
                          [this]
                          {
                              this->execute(meowlive::desktop_runtime::obs::Operation::Status);
                          });
            this->connect(scene_, &::QPushButton::clicked, this,
                          [this]
                          {
                              this->execute(meowlive::desktop_runtime::obs::Operation::SetScene,
                                            scenes_->currentText());
                          });
            this->connect(record_, &::QPushButton::clicked, this,
                          [this]
                          {
                              this->execute(
                                  snapshot_.recording
                                      ? meowlive::desktop_runtime::obs::Operation::StopRecording
                                      : meowlive::desktop_runtime::obs::Operation::StartRecording);
                          });
            this->connect(stream_, &::QPushButton::clicked, this,
                          [this]
                          {
                              this->execute(
                                  snapshot_.streaming
                                      ? meowlive::desktop_runtime::obs::Operation::StopStreaming
                                      : meowlive::desktop_runtime::obs::Operation::StartStreaming);
                          });

            if (const auto settings = obs_.settings())
            {
                enabled_->setChecked(settings->enabled);
                url_->setText(settings->websocket_url);
                this->password_placeholder(settings->password_configured);
            }
            else
            {
                status_->setText(settings.error());
            }
            this->controls();
        }

    private:
        void save()
        {
            const auto settings = obs_.save(enabled_->isChecked(), url_->text().trimmed(),
                                            password_->text(), clear_password_->isChecked());
            if (!settings)
            {
                status_->setText(settings.error());
                return;
            }
            password_->clear();
            clear_password_->setChecked(false);
            url_->setText(settings->websocket_url);
            this->password_placeholder(settings->password_configured);
            snapshot_ = {};
            scenes_->clear();
            status_->setText(QStringLiteral("OBS 设置已保存"));
            this->controls();
        }

        void password_placeholder(bool configured)
        {
            password_->setPlaceholderText(configured ? QStringLiteral("留空保留已保存密码")
                                                     : QStringLiteral("未设置密码"));
        }

        void execute(meowlive::desktop_runtime::obs::Operation operation,
                     const ::QString& scene = {})
        {
            busy_ = true;
            this->controls();
            status_->setText(QStringLiteral("OBS 正在处理请求…"));
            obs_.execute(
                operation, scene, *this,
                [this](const std::expected<meowlive::desktop_runtime::obs::Snapshot, ::QString>&
                           result)
                {
                    busy_ = false;
                    snapshot_ = result.value_or(meowlive::desktop_runtime::obs::Snapshot{});
                    scenes_->clear();
                    scenes_->addItems(snapshot_.scenes);
                    scenes_->setCurrentText(snapshot_.current_scene);
                    if (!result)
                    {
                        status_->setText(result.error());
                    }
                    else if (!snapshot_.connected)
                    {
                        status_->setText(QStringLiteral("OBS 接入未启用"));
                    }
                    else
                    {
                        status_->setText(QStringLiteral("场景 %1　录制 %2　推流 %3")
                                             .arg(snapshot_.current_scene,
                                                  snapshot_.recording ? QStringLiteral("进行中")
                                                                      : QStringLiteral("已停止"),
                                                  snapshot_.streaming ? QStringLiteral("进行中")
                                                                      : QStringLiteral("已停止")));
                    }
                    this->controls();
                });
        }

        void controls()
        {
            const bool connected = !busy_ && snapshot_.connected;
            enabled_->setEnabled(!busy_);
            url_->setEnabled(!busy_);
            clear_password_->setEnabled(!busy_);
            password_->setEnabled(!busy_ && !clear_password_->isChecked());
            save_->setEnabled(!busy_);
            refresh_->setEnabled(!busy_);
            scenes_->setEnabled(connected);
            scene_->setEnabled(connected && scenes_->count() > 0);
            record_->setEnabled(connected);
            stream_->setEnabled(connected);
            record_->setText(snapshot_.recording ? QStringLiteral("停止录制")
                                                 : QStringLiteral("开始录制"));
            stream_->setText(snapshot_.streaming ? QStringLiteral("停止推流")
                                                 : QStringLiteral("开始推流"));
        }

        meowlive::desktop_runtime::obs::Client& obs_;
        meowlive::desktop_runtime::obs::Snapshot snapshot_;
        ::QCheckBox* enabled_{};
        ::QLineEdit* url_{};
        ::QLineEdit* password_{};
        ::QCheckBox* clear_password_{};
        ::QPushButton* save_{};
        ::QPushButton* refresh_{};
        ::QComboBox* scenes_{};
        ::QPushButton* scene_{};
        ::QPushButton* record_{};
        ::QPushButton* stream_{};
        ::QLabel* status_{};
        bool busy_{};
    };
}
