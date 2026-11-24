module;

#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMainWindow>
#include <QMessageBox>
#include <QPushButton>
#include <QString>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

export module meowlive.desktop.ui.main_window;

import meowlive.desktop.services.server_client;

export namespace meowlive::desktop::ui
{
    class MainWindow final : public ::QMainWindow
    {
    public:
        explicit MainWindow(meowlive::desktop::services::ServerClient& server) : server_(server)
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
            layout->addWidget(bridge_);
            layout->addWidget(admin_);
            layout->addWidget(login_, 0, ::Qt::AlignLeft);
            layout->addStretch();
            this->setCentralWidget(content);

            this->set_admin(meowlive::desktop::services::AdminSession::Unavailable);
            this->connect(login_, &::QPushButton::clicked, this, &MainWindow::change_admin);
            this->connect(&refresh_, &::QTimer::timeout, this, &MainWindow::refresh);
            refresh_.start(2'000);
            this->refresh();
        }

        void set_bridge(const ::QString& status)
        {
            bridge_->setText(status);
        }

    private:
        void refresh()
        {
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

        void set_admin(meowlive::desktop::services::AdminSession status)
        {
            admin_status_ = status;
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
        ::QLabel* bridge_{};
        ::QLabel* admin_{};
        ::QPushButton* login_{};
        ::QTimer refresh_;
        meowlive::desktop::services::AdminSession admin_status_{
            meowlive::desktop::services::AdminSession::Unavailable};
        bool health_pending_{};
        bool admin_pending_{};
    };
}
