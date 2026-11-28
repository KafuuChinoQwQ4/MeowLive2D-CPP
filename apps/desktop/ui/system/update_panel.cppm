module;

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QLockFile>
#include <QMessageBox>
#include <QProcess>
#include <QPushButton>
#include <QRegularExpression>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include <QVBoxLayout>
#include <QWidget>

export module meowlive.desktop.ui.update_panel;

import meowlive.desktop.services.managed_server;
import meowlive.desktop.services.server_client;
import meowlive.desktop.services.update_readiness;

export namespace meowlive::desktop::ui
{
    class UpdatePanel final : public ::QWidget
    {
    public:
        UpdatePanel(meowlive::desktop::services::ServerClient& server,
                    meowlive::desktop::services::ManagedServer& managed,
                    const ::QString& data_directory, ::QWidget* parent)
            : ::QWidget(parent), server_(server), managed_(managed),
              cache_(data_directory.isEmpty()
                         ? ::QString{}
                         : ::QDir(data_directory).absoluteFilePath(QStringLiteral("updates"))),
              lock_(::QDir(cache_).filePath(QStringLiteral("update.lock")))
        {
            auto* layout = new ::QVBoxLayout(this);
            tag_ = new ::QLineEdit(this);
            tag_->setPlaceholderText(QStringLiteral("发布标签，例如 v1.2.3"));
            tag_->setAccessibleName(QStringLiteral("发布标签"));
            layout->addWidget(tag_);
            auto* actions = new ::QHBoxLayout;
            download_ = new ::QPushButton(QStringLiteral("下载并验证"), this);
            install_ = new ::QPushButton(QStringLiteral("退出并安装"), this);
            actions->addWidget(download_);
            actions->addWidget(install_);
            actions->addStretch();
            layout->addLayout(actions);
            status_ = new ::QLabel(this);
            status_->setWordWrap(true);
            status_->setTextFormat(::Qt::PlainText);
            layout->addWidget(status_);
            layout->addStretch();

            this->connect(tag_, &::QLineEdit::textEdited, this,
                          [this]
                          {
                              prepared_tag_.clear();
                              this->controls();
                          });
            this->connect(download_, &::QPushButton::clicked, this, &UpdatePanel::download);
            this->connect(install_, &::QPushButton::clicked, this, &UpdatePanel::install);
            this->connect(&helper_, &::QProcess::finished, this,
                          [this](int code, ::QProcess::ExitStatus result)
                          {
                              this->download_finished(code, result);
                          });
            this->connect(&helper_, &::QProcess::errorOccurred, this,
                          [this](::QProcess::ProcessError error)
                          {
                              if (error == ::QProcess::FailedToStart)
                              {
                                  this->finish(QStringLiteral("无法启动随包更新程序"));
                              }
                          });
            lock_.setStaleLockTime(0);
            if (!cache_.isEmpty())
            {
                ::QFile notice(this->notice_path());
                if (notice.open(::QIODevice::ReadOnly))
                {
                    status_->setText(::QString::fromUtf8(notice.read(2'048)));
                    notice.close();
                    notice.remove();
                }
            }
            this->controls();
        }

        ~UpdatePanel() override
        {
            if (helper_.state() != ::QProcess::NotRunning)
            {
                helper_.kill();
                helper_.waitForFinished(2'000);
            }
        }

    private:
        ::QString notice_path() const
        {
            return ::QDir(cache_).filePath(QStringLiteral("install-error.txt"));
        }

        ::QDir app_directory() const
        {
            return ::QDir(::QCoreApplication::applicationDirPath());
        }

        void controls()
        {
            tag_->setEnabled(!busy_);
            download_->setEnabled(!busy_ && !cache_.isEmpty() && !tag_->text().trimmed().isEmpty());
            install_->setEnabled(!busy_ && !prepared_tag_.isEmpty());
        }

        void finish(const ::QString& message)
        {
            if (lock_.isLocked())
            {
                lock_.unlock();
            }
            busy_ = false;
            status_->setText(message);
            this->controls();
        }

        void download()
        {
            static const ::QRegularExpression version(
                QStringLiteral(R"(\Av?[0-9]+\.[0-9]+\.[0-9]+(?:-windows-preview\.[0-9]{8})?\z)"));
            const auto tag = tag_->text().trimmed();
            if (tag.size() > 64 || !version.match(tag).hasMatch())
            {
                status_->setText(QStringLiteral("请输入有效的发布标签"));
                return;
            }
            if (!::QDir().mkpath(cache_) || !lock_.tryLock())
            {
                status_->setText(QStringLiteral("更新目录不可用，或已有更新任务在运行"));
                return;
            }
            prepared_tag_.clear();
            requested_tag_ = tag;
            busy_ = true;
            this->controls();
            status_->setText(QStringLiteral("正在下载并验证签名更新…"));
            helper_.setProcessChannelMode(::QProcess::SeparateChannels);
            helper_.start(this->app_directory().filePath(QStringLiteral("meowlive-update.exe")),
                          {requested_tag_, cache_});
        }

        void download_finished(int code, ::QProcess::ExitStatus result)
        {
            if (code == 0 && result == ::QProcess::NormalExit)
            {
                prepared_tag_ = requested_tag_;
                this->finish(QStringLiteral("安装包已验证，可在空闲时安装"));
                return;
            }
            const auto message =
                ::QString::fromUtf8(helper_.readAllStandardError().left(2'048)).trimmed();
            this->finish(message.isEmpty() ? QStringLiteral("下载或校验更新失败") : message);
        }

        void install()
        {
            if (::QMessageBox::question(
                    this, QStringLiteral("安装更新"),
                    QStringLiteral("关闭桌面端和受管主服务，然后打开安装程序？")) !=
                ::QMessageBox::Yes)
            {
                return;
            }
            if (!lock_.tryLock())
            {
                status_->setText(QStringLiteral("已有更新任务在运行"));
                return;
            }
            busy_ = true;
            this->controls();
            status_->setText(QStringLiteral("正在检查直播、训练和播报状态…"));
            meowlive::desktop::services::check_update_readiness(server_, *this,
                                                                [this](const ::QString& problem)
                                                                {
                                                                    if (problem.isEmpty())
                                                                    {
                                                                        this->handoff();
                                                                    }
                                                                    else
                                                                    {
                                                                        this->finish(problem);
                                                                    }
                                                                });
        }

        void handoff()
        {
            ::QTemporaryDir staging(::QDir(cache_).filePath(QStringLiteral("handoff-XXXXXX")));
            const auto app = this->app_directory();
            const ::QDir helper_dir(staging.path());
            const auto executable = QStringLiteral("meowlive-update.exe");
            if (!staging.isValid() ||
                !::QFile::copy(app.filePath(executable), helper_dir.filePath(executable)))
            {
                this->finish(QStringLiteral("无法准备更新程序"));
                return;
            }
            const auto dependencies =
                app.entryList({QStringLiteral("*.dll")}, ::QDir::Files | ::QDir::NoSymLinks);
            for (const auto& name : dependencies)
            {
                if (!::QFile::copy(app.filePath(name), helper_dir.filePath(name)))
                {
                    this->finish(QStringLiteral("无法准备更新运行库"));
                    return;
                }
            }
            using StopResult = meowlive::desktop::services::UpdateStopResult;
            const auto stopped = managed_.stop_for_update();
            if (stopped != StopResult::Stopped)
            {
                this->finish(stopped == StopResult::Busy
                                 ? QStringLiteral("主服务正在处理直播、播报或语音任务，请稍后重试")
                                 : QStringLiteral("无法正常停止受管主服务，请检查运行状态"));
                return;
            }
            const ::QStringList args{QStringLiteral("--install"),
                                     prepared_tag_,
                                     cache_,
                                     app.absolutePath(),
                                     ::QString::number(::QCoreApplication::applicationPid()),
                                     this->notice_path()};
            if (!::QProcess::startDetached(helper_dir.filePath(executable), args,
                                           helper_dir.absolutePath()))
            {
                this->finish(QStringLiteral("无法交接安装更新，已尝试恢复主服务"));
                managed_.start();
                return;
            }
            staging.setAutoRemove(false);
            ::QCoreApplication::quit();
        }

        meowlive::desktop::services::ServerClient& server_;
        meowlive::desktop::services::ManagedServer& managed_;
        ::QString cache_;
        ::QLockFile lock_;
        ::QProcess helper_;
        ::QLineEdit* tag_{};
        ::QLabel* status_{};
        ::QPushButton* download_{};
        ::QPushButton* install_{};
        ::QString requested_tag_;
        ::QString prepared_tag_;
        bool busy_{};
    };
}
