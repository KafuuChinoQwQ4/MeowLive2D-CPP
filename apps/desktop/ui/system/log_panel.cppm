module;

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollBar>
#include <QString>
#include <QVBoxLayout>
#include <QWidget>

export module meowlive.desktop.ui.log_panel;

export namespace meowlive::desktop::ui
{
    class LogPanel final : public ::QWidget
    {
    public:
        LogPanel(const ::QString& data_directory, ::QWidget* parent) : ::QWidget(parent)
        {
            if (!data_directory.isEmpty())
            {
                path_ = ::QDir(data_directory).absoluteFilePath(QStringLiteral("logs"));
            }
            auto* layout = new ::QVBoxLayout(this);
            auto* reload = new ::QPushButton(QStringLiteral("刷新日志"), this);
            status_ = new ::QLabel(this);
            status_->setTextFormat(::Qt::PlainText);
            text_ = new ::QPlainTextEdit(this);
            text_->setReadOnly(true);
            text_->setLineWrapMode(::QPlainTextEdit::NoWrap);
            text_->setAccessibleName(QStringLiteral("本机主服务日志"));
            layout->addWidget(reload, 0, ::Qt::AlignLeft);
            layout->addWidget(status_);
            layout->addWidget(text_, 1);
            this->connect(reload, &::QPushButton::clicked, this, &LogPanel::refresh);
        }

        void refresh()
        {
            if (path_.isEmpty() || ::QFileInfo(path_).isSymLink())
            {
                this->show_log(QStringLiteral("本机主服务日志目录不可用"), {});
                return;
            }
            const ::QDir directory(path_);
            auto files = directory.entryInfoList({QStringLiteral("meowlive-server.*.log")},
                                                 ::QDir::Files | ::QDir::NoSymLinks, ::QDir::Time);
            files.removeIf(
                [](const ::QFileInfo& file)
                {
                    return file.size() == 0;
                });
            if (files.isEmpty())
            {
                this->show_log(QStringLiteral("暂无本机主服务日志"), {});
                return;
            }

            ::QFile file(files.front().absoluteFilePath());
            if (!file.open(::QIODevice::ReadOnly))
            {
                this->show_log(QStringLiteral("无法读取本机主服务日志"), {});
                return;
            }

            constexpr ::qint64 tail_bytes = 64 * 1024;
            const auto size = file.size();
            const bool truncated = size > tail_bytes;
            if (truncated && !file.seek(size - tail_bytes))
            {
                this->show_log(QStringLiteral("无法读取本机主服务日志"), {});
                return;
            }
            auto bytes = file.read(tail_bytes);
            if (file.error() != ::QFileDevice::NoError)
            {
                this->show_log(QStringLiteral("无法读取本机主服务日志"), {});
                return;
            }
            if (truncated)
            {
                const auto first_line = bytes.indexOf('\n');
                if (first_line >= 0)
                {
                    bytes.remove(0, first_line + 1);
                }
                else
                {
                    while (!bytes.isEmpty() &&
                           (static_cast<unsigned char>(bytes.front()) & 0xc0) == 0x80)
                    {
                        bytes.remove(0, 1);
                    }
                }
            }
            this->show_log(files.front().fileName(), ::QString::fromUtf8(bytes));
        }

    private:
        void show_log(const ::QString& status, const ::QString& content)
        {
            status_->setText(status);
            if (content != text_->toPlainText())
            {
                text_->setPlainText(content);
                auto* scrollbar = text_->verticalScrollBar();
                scrollbar->setValue(scrollbar->maximum());
            }
        }

        ::QString path_;
        ::QLabel* status_{};
        ::QPlainTextEdit* text_{};
    };
}
