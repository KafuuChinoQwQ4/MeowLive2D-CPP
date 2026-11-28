module;

#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QString>
#include <QVBoxLayout>
#include <QWidget>

export module meowlive.desktop.ui.speech_panel;

import meowlive.desktop.services.server_client;

namespace
{
    ::QString speech_status(const ::QString& value)
    {
        if (value == QStringLiteral("queued"))
        {
            return QStringLiteral("排队中");
        }
        if (value == QStringLiteral("synthesizing"))
        {
            return QStringLiteral("合成中");
        }
        if (value == QStringLiteral("ready"))
        {
            return QStringLiteral("等待播放");
        }
        if (value == QStringLiteral("playing"))
        {
            return QStringLiteral("播放中");
        }
        if (value == QStringLiteral("completed"))
        {
            return QStringLiteral("已播毕");
        }
        if (value == QStringLiteral("cancelled"))
        {
            return QStringLiteral("已取消");
        }
        if (value == QStringLiteral("failed"))
        {
            return QStringLiteral("失败");
        }
        if (value == QStringLiteral("unknown"))
        {
            return QStringLiteral("播报结果未知");
        }
        return value;
    }
}

export namespace meowlive::desktop::ui
{
    class SpeechPanel final : public ::QWidget
    {
    public:
        SpeechPanel(meowlive::desktop::services::ServerClient& server, ::QWidget* parent)
            : ::QWidget(parent), server_(server)
        {
            auto* layout = new ::QVBoxLayout(this);
            layout->setContentsMargins(0, 8, 0, 0);
            auto* label = new ::QLabel(QStringLiteral("播报文本"), this);
            text_ = new ::QPlainTextEdit(this);
            text_->setAccessibleName(QStringLiteral("播报文本"));
            text_->setPlaceholderText(QStringLiteral("输入要播报的文字"));
            text_->setMaximumHeight(120);
            label->setBuddy(text_);
            layout->addWidget(label);
            layout->addWidget(text_);
            auto* buttons = new ::QHBoxLayout;
            preview_ = new ::QPushButton(QStringLiteral("试音"), this);
            enqueue_ = new ::QPushButton(QStringLiteral("入队"), this);
            stop_ = new ::QPushButton(QStringLiteral("立即停止"), this);
            buttons->addWidget(preview_);
            buttons->addWidget(enqueue_);
            buttons->addWidget(stop_);
            buttons->addStretch();
            layout->addLayout(buttons);

            feedback_ = new ::QLabel(this);
            feedback_->setTextFormat(::Qt::PlainText);
            feedback_->setWordWrap(true);
            layout->addWidget(feedback_);
            history_ = new ::QListWidget(this);
            history_->setAccessibleName(QStringLiteral("播报任务"));
            history_->setSelectionMode(::QAbstractItemView::NoSelection);
            layout->addWidget(history_, 1);

            this->connect(text_, &::QPlainTextEdit::textChanged, this,
                          &SpeechPanel::update_buttons);
            this->connect(preview_, &::QPushButton::clicked, this,
                          [this]
                          {
                              submit_pending_ = true;
                              this->update_buttons();
                              server_.preview_speech(
                                  [panel = ::QPointer<SpeechPanel>(this)](const ::QString& error)
                                  {
                                      if (panel)
                                      {
                                          panel->submitted(QStringLiteral("试音已入队"), error);
                                      }
                                  });
                          });
            this->connect(
                enqueue_, &::QPushButton::clicked, this,
                [this]
                {
                    const auto text = text_->toPlainText().trimmed();
                    submit_pending_ = true;
                    this->update_buttons();
                    server_.enqueue_speech(
                        text,
                        [panel = ::QPointer<SpeechPanel>(this), text](const ::QString& error)
                        {
                            if (panel)
                            {
                                if (error.isEmpty() &&
                                    panel->text_->toPlainText().trimmed() == text)
                                {
                                    panel->text_->clear();
                                }
                                panel->submitted(QStringLiteral("播报已入队"), error);
                            }
                        });
                });
            this->connect(stop_, &::QPushButton::clicked, this,
                          [this]
                          {
                              stop_pending_ = true;
                              this->update_buttons();
                              server_.stop_speech(
                                  [panel = ::QPointer<SpeechPanel>(this)](const ::QString& error)
                                  {
                                      if (panel)
                                      {
                                          panel->stop_pending_ = false;
                                          panel->feedback_->setText(
                                              error.isEmpty() ? QStringLiteral("已停止播报")
                                                              : error);
                                          panel->update_buttons();
                                          panel->refresh();
                                      }
                                  });
                          });
            this->update_buttons();
        }

        void set_access(bool enabled)
        {
            accessible_ = enabled;
            if (!enabled)
            {
                history_->clear();
            }
            this->update_buttons();
        }

        void refresh()
        {
            if (!accessible_ || refresh_pending_)
            {
                return;
            }
            refresh_pending_ = true;
            server_.get_speeches(
                [panel = ::QPointer<SpeechPanel>(this)](const ::QJsonArray& speeches)
                {
                    if (panel)
                    {
                        panel->refresh_pending_ = false;
                        if (panel->accessible_)
                        {
                            panel->update_history(speeches);
                        }
                    }
                });
        }

    private:
        void submitted(const ::QString& success, const ::QString& error)
        {
            submit_pending_ = false;
            feedback_->setText(error.isEmpty() ? success : error);
            this->update_buttons();
            this->refresh();
        }

        void update_buttons()
        {
            const bool can_submit = accessible_ && !submit_pending_ && !stop_pending_;
            preview_->setEnabled(can_submit);
            enqueue_->setEnabled(can_submit && !text_->toPlainText().trimmed().isEmpty());
            stop_->setEnabled(accessible_ && !stop_pending_);
        }

        void update_history(const ::QJsonArray& speeches)
        {
            history_->clear();
            constexpr ::qsizetype history_rows = 20;
            for (::qsizetype index = speeches.size();
                 index > 0 && speeches.size() - index < history_rows; --index)
            {
                const auto row = speeches.at(index - 1).toObject();
                const auto status = speech_status(row.value(QStringLiteral("status")).toString());
                const auto text =
                    row.value(QStringLiteral("voice_id")).toString() == QStringLiteral("preview")
                        ? QStringLiteral("试音")
                        : row.value(QStringLiteral("text")).toString().simplified();
                const auto error = row.value(QStringLiteral("error")).toString();
                const auto summary = QStringLiteral("%1　%2").arg(status, text);
                history_->addItem(error.isEmpty() ? summary
                                                  : QStringLiteral("%1　%2").arg(summary, error));
            }
        }

        meowlive::desktop::services::ServerClient& server_;
        ::QPlainTextEdit* text_{};
        ::QPushButton* preview_{};
        ::QPushButton* enqueue_{};
        ::QPushButton* stop_{};
        ::QLabel* feedback_{};
        ::QListWidget* history_{};
        bool accessible_{};
        bool submit_pending_{};
        bool stop_pending_{};
        bool refresh_pending_{};
    };
}
