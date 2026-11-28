module;

#include <QInputDialog>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMainWindow>
#include <QMessageBox>
#include <QPushButton>
#include <QString>
#include <QTabWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include <functional>
#include <utility>

export module meowlive.desktop.ui.main_window;

import meowlive.desktop.services.server_client;
import meowlive.desktop.services.managed_server;
import meowlive.desktop.ui.speech_panel;
import meowlive.desktop.ui.agent_panel;
import meowlive.desktop.ui.agent_runtime_panel;
import meowlive.desktop.ui.agent_observability_panel;
import meowlive.desktop.ui.avatar_panel;
import meowlive.desktop.ui.character_panel;
import meowlive.desktop.ui.training_panel;
import meowlive.desktop.ui.training_jobs_panel;
import meowlive.desktop.ui.voice_models_panel;
import meowlive.desktop.ui.update_panel;
import meowlive.desktop.ui.live_panel;
import meowlive.desktop.ui.log_panel;
import meowlive.desktop.ui.memory_panel;
import meowlive.desktop.ui.model_panel;
import meowlive.desktop.ui.obs_panel;
import meowlive.desktop.ui.persona_panel;
import meowlive.desktop.ui.relationship_panel;
import meowlive.desktop.ui.voice_panel;
import meowlive.desktop.ui.viewer_panel;
import meowlive.desktop_runtime.obs.client;
import meowlive.desktop_runtime.avatar.presentation;

export namespace meowlive::desktop::ui
{
    class MainWindow final : public ::QMainWindow
    {
    public:
        MainWindow(meowlive::desktop::services::ServerClient& server,
                   meowlive::desktop_runtime::obs::Client& obs, const ::QString& data_directory)
            : server_(server)
        {
            this->setWindowTitle(QStringLiteral("MeowLive2D"));
            this->resize(960, 640);

            auto* content = new ::QWidget(this);
            auto* layout = new ::QVBoxLayout(content);
            health_ = new ::QLabel(QStringLiteral("正在检测主服务"), content);
            bridge_ = new ::QLabel(QStringLiteral("执行端未连接"), content);
            admin_ = new ::QLabel(content);
            login_ = new ::QPushButton(content);
            layout->addWidget(health_);
            managed_status_ = new ::QLabel(content);
            managed_retry_ = new ::QPushButton(QStringLiteral("重试启动主服务"), content);
            managed_status_->hide();
            managed_retry_->hide();
            layout->addWidget(managed_status_);
            layout->addWidget(managed_retry_, 0, ::Qt::AlignLeft);
            layout->addWidget(bridge_);
            layout->addWidget(admin_);
            layout->addWidget(login_, 0, ::Qt::AlignLeft);
            tabs_ = new ::QTabWidget(content);
            speech_ = new meowlive::desktop::ui::SpeechPanel(server_, tabs_);
            voices_ = new meowlive::desktop::ui::VoicePanel(server_, tabs_);
            agent_ = new meowlive::desktop::ui::AgentPanel(server_, tabs_);
            runtime_ = new meowlive::desktop::ui::AgentRuntimePanel(server_, tabs_);
            model_ = new meowlive::desktop::ui::ModelPanel(server_, tabs_);
            persona_ = new meowlive::desktop::ui::PersonaPanel(server_, tabs_);
            observations_ = new meowlive::desktop::ui::AgentObservabilityPanel(server_, tabs_);
            live_ = new meowlive::desktop::ui::LivePanel(server_, tabs_);
            avatar_ = new meowlive::desktop::ui::AvatarPanel(server_, tabs_);
            characters_ = new meowlive::desktop::ui::CharacterPanel(server_, tabs_);
            voice_models_ = new meowlive::desktop::ui::VoiceModelsPanel(server_, tabs_);
            const auto open_voice_models = [this]
            {
                voice_models_->select_purpose(QStringLiteral("transcription"));
                tabs_->setCurrentWidget(voice_models_);
            };
            const auto open_training_models = [this]
            {
                voice_models_->select_purpose(QStringLiteral("training"));
                tabs_->setCurrentWidget(voice_models_);
            };
            training_ = new meowlive::desktop::ui::TrainingPanel(server_, open_voice_models, tabs_);
            training_jobs_ = new meowlive::desktop::ui::TrainingJobsPanel(
                server_, open_voice_models, open_training_models, tabs_);
            viewers_ = new meowlive::desktop::ui::ViewerPanel(server_, tabs_);
            memories_ = new meowlive::desktop::ui::MemoryPanel(server_, tabs_);
            relationships_ = new meowlive::desktop::ui::RelationshipPanel(server_, tabs_);
            auto* logs = new meowlive::desktop::ui::LogPanel(data_directory, tabs_);
            tabs_->addTab(speech_, QStringLiteral("手动播报"));
            tabs_->addTab(voices_, QStringLiteral("音色"));
            tabs_->addTab(agent_, QStringLiteral("Agent 设置"));
            tabs_->addTab(runtime_, QStringLiteral("Agent 运行"));
            tabs_->addTab(model_, QStringLiteral("模型"));
            tabs_->addTab(persona_, QStringLiteral("人物卡"));
            tabs_->addTab(observations_, QStringLiteral("观测"));
            tabs_->addTab(live_, QStringLiteral("直播连接"));
            tabs_->addTab(viewers_, QStringLiteral("观众"));
            tabs_->addTab(memories_, QStringLiteral("记忆"));
            tabs_->addTab(relationships_, QStringLiteral("关系"));
            tabs_->addTab(avatar_, QStringLiteral("VTube Studio"));
            tabs_->addTab(characters_, QStringLiteral("角色"));
            tabs_->addTab(training_, QStringLiteral("参考录音"));
            tabs_->addTab(voice_models_, QStringLiteral("可选模型"));
            tabs_->addTab(training_jobs_, QStringLiteral("声音训练"));
            tabs_->addTab(new meowlive::desktop::ui::ObsPanel(obs, tabs_), QStringLiteral("OBS"));
            tabs_->addTab(logs, QStringLiteral("运行日志"));
            layout->addWidget(tabs_, 1);
            this->setCentralWidget(content);

            this->set_admin(meowlive::desktop::services::AdminSession::Unavailable);
            this->connect(login_, &::QPushButton::clicked, this, &MainWindow::change_admin);
            this->connect(&refresh_, &::QTimer::timeout, this, &MainWindow::refresh);
            this->connect(tabs_, &::QTabWidget::currentChanged, this,
                          [this, logs]
                          {
                              if (tabs_->currentWidget() == observations_)
                              {
                                  observations_->refresh();
                              }
                              else if (tabs_->currentWidget() == voices_)
                              {
                                  voices_->open();
                              }
                              else if (tabs_->currentWidget() == characters_)
                              {
                                  characters_->open();
                              }
                              else if (tabs_->currentWidget() == viewers_)
                              {
                                  viewers_->refresh();
                              }
                              else if (tabs_->currentWidget() == memories_)
                              {
                                  memories_->refresh_status();
                              }
                              else if (tabs_->currentWidget() == relationships_)
                              {
                                  relationships_->refresh();
                              }
                              else if (tabs_->currentWidget() == logs)
                              {
                                  logs->refresh();
                              }
                              this->refresh_selected_page();
                          });
            refresh_.start(2'000);
            this->refresh();
        }

        void set_server_retry(std::function<void()> action)
        {
            this->connect(managed_retry_, &::QPushButton::clicked, this, std::move(action));
        }

        void enable_updates(meowlive::desktop::services::ManagedServer& managed,
                            const ::QString& data_directory)
        {
            tabs_->addTab(
                new meowlive::desktop::ui::UpdatePanel(server_, managed, data_directory, tabs_),
                QStringLiteral("更新"));
        }

        void set_managed_server(const ::QString& status, bool retryable)
        {
            managed_status_->setText(status);
            managed_status_->show();
            managed_retry_->setVisible(retryable);
        }

        void set_bridge(const ::QString& status)
        {
            bridge_->setText(status);
        }

        void set_mouth_source(std::function<double()> source)
        {
            avatar_->set_mouth_source(std::move(source));
        }

        meowlive::desktop_runtime::avatar::Presentation& avatar_presentation()
        {
            return avatar_->presentation();
        }

    private:
        void refresh()
        {
            this->refresh_selected_page();
            if (!health_pending_)
            {
                health_pending_ = true;
                server_.get_health(
                    [this](const ::QString& status)
                    {
                        health_pending_ = false;
                        health_->setText(status);
                    });
            }
            if (!admin_pending_)
            {
                admin_pending_ = true;
                login_->setEnabled(false);
                server_.get_admin_session(
                    [this](meowlive::desktop::services::AdminSession status)
                    {
                        admin_pending_ = false;
                        login_->setEnabled(true);
                        this->set_admin(status);
                    });
            }
        }

        void refresh_selected_page()
        {
            const auto* page = tabs_->currentWidget();
            if (page == training_ || page == voice_models_ || page == training_jobs_)
            {
                this->refresh_voice_models();
            }
            if (tabs_->currentWidget() == speech_)
            {
                speech_->refresh();
            }
            else if (tabs_->currentWidget() == agent_)
            {
                agent_->refresh();
            }
            else if (tabs_->currentWidget() == live_)
            {
                live_->refresh();
            }
            else if (tabs_->currentWidget() == training_jobs_)
            {
                training_jobs_->refresh();
            }
        }

        void refresh_voice_models()
        {
            if (!voice_models_access_ || voice_models_pending_)
            {
                return;
            }
            voice_models_pending_ = true;
            const auto session = voice_models_session_;
            server_.get_json(
                QStringLiteral("/api/voice-models"), *this,
                [this, session](const ::QJsonDocument& response, const ::QString& error)
                {
                    if (session != voice_models_session_)
                    {
                        return;
                    }
                    voice_models_pending_ = false;
                    const auto catalog = response.object();
                    voice_models_->update_catalog(catalog, error);
                    if (error.isEmpty())
                    {
                        training_->update_voice_models(catalog);
                        training_jobs_->update_voice_models(catalog);
                    }
                });
        }

        void set_admin(meowlive::desktop::services::AdminSession status)
        {
            admin_status_ = status;
            const bool access = status == meowlive::desktop::services::AdminSession::Disabled ||
                                status == meowlive::desktop::services::AdminSession::SignedIn;
            speech_->set_access(access);
            voices_->set_access(access);
            characters_->set_access(access);
            agent_->set_access(access);
            runtime_->set_access(access);
            model_->set_access(access);
            persona_->set_access(access);
            observations_->set_access(access);
            live_->set_access(access);
            training_->set_access(access);
            training_jobs_->set_access(access);
            voice_models_->set_access(access);
            if (voice_models_access_ != access)
            {
                voice_models_access_ = access;
                ++voice_models_session_;
                voice_models_pending_ = false;
                if (!access)
                {
                    training_->update_voice_models({});
                    training_jobs_->update_voice_models({});
                }
                this->refresh_selected_page();
            }
            viewers_->set_access(access);
            memories_->set_access(access);
            relationships_->set_access(access);
            switch (status)
            {
            case meowlive::desktop::services::AdminSession::Unavailable:
                admin_->setText(QStringLiteral("管理员会话状态不可用"));
                login_->hide();
                break;
            case meowlive::desktop::services::AdminSession::Disabled:
                admin_->setText(QStringLiteral("主服务未启用管理员鉴权"));
                login_->hide();
                break;
            case meowlive::desktop::services::AdminSession::SignedOut:
                admin_->setText(QStringLiteral("管理员未登录"));
                login_->setText(QStringLiteral("登录"));
                login_->show();
                break;
            case meowlive::desktop::services::AdminSession::SignedIn:
                admin_->setText(QStringLiteral("管理员已登录"));
                login_->setText(QStringLiteral("退出登录"));
                login_->show();
                break;
            }
        }

        void change_admin()
        {
            if (admin_pending_)
            {
                return;
            }
            admin_pending_ = true;
            login_->setEnabled(false);
            if (admin_status_ == meowlive::desktop::services::AdminSession::SignedIn)
            {
                server_.logout(
                    [this](bool success)
                    {
                        this->finish_admin(success, QStringLiteral("退出登录失败"));
                    });
                return;
            }

            bool accepted = false;
            const auto token = ::QInputDialog::getText(this, QStringLiteral("管理员登录"),
                                                       QStringLiteral("管理员凭据"),
                                                       ::QLineEdit::Password, {}, &accepted);
            if (!accepted || token.isEmpty())
            {
                admin_pending_ = false;
                login_->setEnabled(true);
                return;
            }
            server_.login(token,
                          [this](bool success)
                          {
                              this->finish_admin(success, QStringLiteral("管理员登录失败"));
                          });
        }

        void finish_admin(bool success, const ::QString& failure)
        {
            admin_pending_ = false;
            login_->setEnabled(true);
            if (!success)
            {
                ::QMessageBox::warning(this, QStringLiteral("管理员会话"), failure);
            }
            this->refresh();
        }

        meowlive::desktop::services::ServerClient& server_;
        ::QLabel* health_{};
        ::QLabel* managed_status_{};
        ::QPushButton* managed_retry_{};
        ::QLabel* bridge_{};
        ::QLabel* admin_{};
        ::QPushButton* login_{};
        ::QTabWidget* tabs_{};
        meowlive::desktop::ui::SpeechPanel* speech_{};
        meowlive::desktop::ui::VoicePanel* voices_{};
        meowlive::desktop::ui::AgentPanel* agent_{};
        meowlive::desktop::ui::AgentRuntimePanel* runtime_{};
        meowlive::desktop::ui::ModelPanel* model_{};
        meowlive::desktop::ui::PersonaPanel* persona_{};
        meowlive::desktop::ui::AgentObservabilityPanel* observations_{};
        meowlive::desktop::ui::LivePanel* live_{};
        meowlive::desktop::ui::AvatarPanel* avatar_{};
        meowlive::desktop::ui::CharacterPanel* characters_{};
        meowlive::desktop::ui::TrainingPanel* training_{};
        meowlive::desktop::ui::TrainingJobsPanel* training_jobs_{};
        meowlive::desktop::ui::VoiceModelsPanel* voice_models_{};
        meowlive::desktop::ui::ViewerPanel* viewers_{};
        meowlive::desktop::ui::MemoryPanel* memories_{};
        meowlive::desktop::ui::RelationshipPanel* relationships_{};
        ::QTimer refresh_;
        meowlive::desktop::services::AdminSession admin_status_{
            meowlive::desktop::services::AdminSession::Unavailable};
        bool health_pending_{};
        bool admin_pending_{};
        unsigned voice_models_session_{};
        bool voice_models_access_{};
        bool voice_models_pending_{};
    };
}
