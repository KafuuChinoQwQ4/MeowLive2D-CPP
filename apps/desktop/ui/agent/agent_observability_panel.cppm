module;

#include <QComboBox>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QString>
#include <QUrl>
#include <QVBoxLayout>
#include <QWidget>

export module meowlive.desktop.ui.agent_observability_panel;

import meowlive.desktop.services.server_client;

export namespace meowlive::desktop::ui
{
    class AgentObservabilityPanel final : public ::QWidget
    {
    public:
        AgentObservabilityPanel(meowlive::desktop::services::ServerClient& server,
                                ::QWidget* parent)
            : ::QWidget(parent), server_(server)
        {
            auto* layout = new ::QVBoxLayout(this);
            auto* controls = new ::QHBoxLayout;
            view_ = new ::QComboBox(this);
            view_->setAccessibleName(QStringLiteral("观测类别"));
            view_->addItem(QStringLiteral("运行状态"), QStringLiteral("/api/agent"));
            view_->addItem(QStringLiteral("活动"), QStringLiteral("/api/agent/activity"));
            view_->addItem(QStringLiteral("调度"), QStringLiteral("/api/agent/scheduler"));
            view_->addItem(QStringLiteral("决策轨迹"), QStringLiteral("/api/agent/traces"));
            view_->addItem(QStringLiteral("模型用量"), QStringLiteral("/api/llm/usage"));
            auto* refresh = new ::QPushButton(QStringLiteral("刷新"), this);
            trace_ = new ::QLineEdit(this);
            trace_->setPlaceholderText(QStringLiteral("决策轨迹 ID"));
            trace_->setAccessibleName(QStringLiteral("决策轨迹 ID"));
            detail_ = new ::QPushButton(QStringLiteral("查询轨迹"), this);
            controls->addWidget(view_);
            controls->addWidget(refresh);
            controls->addWidget(trace_, 1);
            controls->addWidget(detail_);
            layout->addLayout(controls);
            status_ = new ::QLabel(this);
            status_->setTextFormat(::Qt::PlainText);
            status_->setWordWrap(true);
            layout->addWidget(status_);
            result_ = new ::QPlainTextEdit(this);
            result_->setReadOnly(true);
            result_->setAccessibleName(QStringLiteral("观测结果"));
            layout->addWidget(result_, 1);

            this->connect(refresh, &::QPushButton::clicked, this,
                          &AgentObservabilityPanel::refresh);
            this->connect(view_, &::QComboBox::currentIndexChanged, this,
                          [this]
                          {
                              this->update_trace_controls();
                              this->refresh();
                          });
            this->connect(detail_, &::QPushButton::clicked, this,
                          &AgentObservabilityPanel::query_trace);
            this->connect(trace_, &::QLineEdit::returnPressed, this,
                          &AgentObservabilityPanel::query_trace);
            this->update_trace_controls();
            this->setEnabled(false);
        }

        void set_access(bool access)
        {
            if (access == access_)
            {
                return;
            }
            access_ = access;
            ++revision_;
            this->setEnabled(access);
            status_->clear();
            result_->clear();
            trace_->clear();
            if (this->isVisible())
            {
                this->refresh();
            }
        }

        void refresh()
        {
            this->query(view_->currentData().toString());
        }

    private:
        void update_trace_controls()
        {
            const bool trace =
                view_->currentData().toString() == QStringLiteral("/api/agent/traces");
            trace_->setVisible(trace);
            detail_->setVisible(trace);
        }

        void query_trace()
        {
            const auto id = trace_->text().trimmed();
            if (id.isEmpty())
            {
                status_->setText(QStringLiteral("请输入决策轨迹 ID"));
                return;
            }
            for (const auto character : id)
            {
                if (!character.isLetterOrNumber() && character != u'-' && character != u'_')
                {
                    status_->setText(QStringLiteral("决策轨迹 ID 无效"));
                    return;
                }
            }
            this->query(QStringLiteral("/api/agent/traces/") + id);
        }

        void query(const ::QString& path)
        {
            if (!access_)
            {
                return;
            }
            const auto revision = ++revision_;
            status_->setText(QStringLiteral("读取中…"));
            result_->clear();
            server_.get_json(
                path, *this,
                [this, revision](const ::QJsonDocument& document, const ::QString& error)
                {
                    if (revision != revision_)
                    {
                        return;
                    }
                    status_->setText(error);
                    if (error.isEmpty())
                    {
                        result_->setPlainText(
                            ::QString::fromUtf8(document.toJson(::QJsonDocument::Indented)));
                    }
                });
        }

        meowlive::desktop::services::ServerClient& server_;
        ::QComboBox* view_{};
        ::QLineEdit* trace_{};
        ::QPushButton* detail_{};
        ::QLabel* status_{};
        ::QPlainTextEdit* result_{};
        unsigned revision_{};
        bool access_{};
    };
}
