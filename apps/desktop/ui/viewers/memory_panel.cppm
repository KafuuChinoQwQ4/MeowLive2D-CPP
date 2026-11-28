module;

#include <QComboBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QSignalBlocker>
#include <QString>
#include <QUuid>
#include <QVBoxLayout>
#include <QWidget>

export module meowlive.desktop.ui.memory_panel;

import meowlive.desktop.services.server_client;

export namespace meowlive::desktop::ui
{
    class MemoryPanel final : public ::QWidget
    {
    public:
        MemoryPanel(meowlive::desktop::services::ServerClient& server, ::QWidget* parent)
            : ::QWidget(parent), server_(server)
        {
            auto* layout = new ::QVBoxLayout(this);
            auto* viewer_line = new ::QHBoxLayout;
            viewer_ = new ::QLineEdit(this);
            viewer_->setAccessibleName(QStringLiteral("观众标识"));
            viewer_->setPlaceholderText(QStringLiteral("观众 UUID"));
            viewer_->setMaxLength(36);
            load_ = new ::QPushButton(QStringLiteral("查看记忆"), this);
            viewer_line->addWidget(viewer_, 1);
            viewer_line->addWidget(load_);
            layout->addLayout(viewer_line);

            auto* fields = new ::QFormLayout;
            memories_ = new ::QComboBox(this);
            value_ = new ::QLineEdit(this);
            reason_ = new ::QLineEdit(this);
            fields->addRow(QStringLiteral("记忆"), memories_);
            fields->addRow(QStringLiteral("记忆内容"), value_);
            fields->addRow(QStringLiteral("操作原因"), reason_);
            layout->addLayout(fields);

            auto* actions = new ::QHBoxLayout;
            correct_ = new ::QPushButton(QStringLiteral("更正记忆"), this);
            freeze_ = new ::QPushButton(QStringLiteral("冻结记忆"), this);
            remove_ = new ::QPushButton(QStringLiteral("删除记忆"), this);
            actions->addWidget(correct_);
            actions->addWidget(freeze_);
            actions->addWidget(remove_);
            actions->addStretch();
            layout->addLayout(actions);

            evidence_ = new ::QPlainTextEdit(this);
            evidence_->setReadOnly(true);
            evidence_->setAccessibleName(QStringLiteral("记忆状态和事件依据"));
            layout->addWidget(evidence_, 1);
            job_status_ = this->label();
            layout->addWidget(job_status_);
            auto* maintenance = new ::QHBoxLayout;
            status_ = new ::QPushButton(QStringLiteral("刷新任务状态"), this);
            retry_ = new ::QPushButton(QStringLiteral("重试失败任务"), this);
            rebuild_ = new ::QPushButton(QStringLiteral("重建向量"), this);
            maintenance->addWidget(status_);
            maintenance->addWidget(retry_);
            maintenance->addWidget(rebuild_);
            maintenance->addStretch();
            layout->addLayout(maintenance);
            feedback_ = this->label();
            layout->addWidget(feedback_);

            this->connect(viewer_, &::QLineEdit::textChanged, this,
                          [this]
                          {
                              memories_->clear();
                              this->show_memory();
                          });
            this->connect(load_, &::QPushButton::clicked, this,
                          [this]
                          {
                              this->list({});
                          });
            this->connect(viewer_, &::QLineEdit::returnPressed, load_, &::QPushButton::click);
            this->connect(memories_, &::QComboBox::currentIndexChanged, this,
                          &MemoryPanel::show_memory);
            this->connect(reason_, &::QLineEdit::textChanged, this, &MemoryPanel::controls);
            this->connect(value_, &::QLineEdit::textChanged, this, &MemoryPanel::controls);
            this->connect(correct_, &::QPushButton::clicked, this,
                          [this]
                          {
                              this->mutate(QStringLiteral("correct"));
                          });
            this->connect(freeze_, &::QPushButton::clicked, this,
                          [this]
                          {
                              this->mutate(QStringLiteral("freeze"));
                          });
            this->connect(remove_, &::QPushButton::clicked, this,
                          [this]
                          {
                              this->mutate(QStringLiteral("delete"));
                          });
            this->connect(status_, &::QPushButton::clicked, this, &MemoryPanel::refresh_status);
            this->connect(retry_, &::QPushButton::clicked, this,
                          [this]
                          {
                              this->maintain(QStringLiteral("retry"));
                          });
            this->connect(rebuild_, &::QPushButton::clicked, this,
                          [this]
                          {
                              this->maintain(QStringLiteral("rebuild-vectors"));
                          });
            this->controls();
        }

        void set_access(bool access)
        {
            if (access_ == access)
            {
                return;
            }
            access_ = access;
            ++revision_;
            busy_ = false;
            status_ready_ = false;
            if (!access)
            {
                viewer_->clear();
                reason_->clear();
                feedback_->clear();
                job_status_->clear();
            }
            this->controls();
            if (this->isVisible())
            {
                this->refresh_status();
            }
        }

        void refresh_status()
        {
            if (!access_ || busy_)
            {
                return;
            }
            const auto revision = this->begin();
            server_.get_json(
                QStringLiteral("/api/admin/memories/status"), *this,
                [this, revision](const ::QJsonDocument& document, const ::QString& error)
                {
                    if (revision != revision_)
                    {
                        return;
                    }
                    busy_ = false;
                    const auto result = document.object();
                    status_ready_ = error.isEmpty() &&
                                    result.value(QStringLiteral("pending")).toInteger(-1) >= 0;
                    if (status_ready_)
                    {
                        job_status_->setText(
                            QStringLiteral("记忆任务　待处理 %1　运行中 %2　失败 %3　"
                                           "向量任务　待处理 %4　失败 %5")
                                .arg(result.value(QStringLiteral("pending")).toInteger())
                                .arg(result.value(QStringLiteral("running")).toInteger())
                                .arg(result.value(QStringLiteral("failed")).toInteger())
                                .arg(result.value(QStringLiteral("embedding_pending")).toInteger())
                                .arg(result.value(QStringLiteral("embedding_failed")).toInteger()));
                    }
                    else
                    {
                        job_status_->setText(error.isEmpty() ? QStringLiteral("记忆任务状态无效")
                                                             : error);
                    }
                    this->controls();
                });
        }

    private:
        ::QLabel* label()
        {
            auto* label = new ::QLabel(this);
            label->setTextFormat(::Qt::PlainText);
            label->setWordWrap(true);
            return label;
        }

        static bool uuid(const ::QString& value)
        {
            static const ::QRegularExpression pattern(
                QStringLiteral(R"(\A[0-9a-fA-F]{8}(?:-[0-9a-fA-F]{4}){3}-[0-9a-fA-F]{12}\z)"));
            return pattern.match(value).hasMatch();
        }

        ::QJsonObject memory() const
        {
            return memories_->currentData().toJsonObject();
        }

        unsigned begin()
        {
            busy_ = true;
            this->controls();
            return ++revision_;
        }

        void controls()
        {
            const auto selected = this->memory();
            const bool enabled = access_ && !busy_;
            const bool editable = enabled &&
                                  uuid(selected.value(QStringLiteral("id")).toString()) &&
                                  selected.value(QStringLiteral("version")).toInteger() > 0 &&
                                  !selected.value(QStringLiteral("deleted")).toBool();
            const auto reason = reason_->text().trimmed();
            const bool justified = !reason.isEmpty() && reason.toUtf8().size() <= 1'000;
            const auto value = value_->text().trimmed();
            viewer_->setEnabled(enabled);
            load_->setEnabled(enabled && uuid(viewer_->text().trimmed()));
            memories_->setEnabled(enabled);
            value_->setEnabled(editable);
            reason_->setEnabled(enabled);
            correct_->setEnabled(editable && justified && !value.isEmpty() &&
                                 value.toUtf8().size() <= 512 &&
                                 value != selected.value(QStringLiteral("value")).toString());
            freeze_->setEnabled(editable && justified);
            freeze_->setText(selected.value(QStringLiteral("locked")).toBool()
                                 ? QStringLiteral("解冻记忆")
                                 : QStringLiteral("冻结记忆"));
            remove_->setEnabled(editable && justified);
            status_->setEnabled(enabled);
            retry_->setEnabled(enabled && status_ready_ && justified);
            rebuild_->setEnabled(enabled && status_ready_ && justified);
        }

        ::QString viewer_path() const
        {
            return QStringLiteral("/api/admin/viewers/") + viewer_->text().trimmed() +
                   QStringLiteral("/memories");
        }

        void list(const ::QString& preserve, const ::QString& success = {})
        {
            const auto revision = this->begin();
            feedback_->setText(QStringLiteral("正在读取记忆…"));
            server_.get_json(
                this->viewer_path(), *this,
                [this, revision, preserve, success](const ::QJsonDocument& document,
                                                    const ::QString& error)
                {
                    if (revision != revision_)
                    {
                        return;
                    }
                    busy_ = false;
                    const ::QSignalBlocker blocked(memories_);
                    memories_->clear();
                    feedback_->setText(error);
                    const auto rows = document.object().value(QStringLiteral("memories"));
                    if (error.isEmpty() && rows.isArray())
                    {
                        int selected = 0;
                        for (const auto& item : rows.toArray())
                        {
                            const auto row = item.toObject();
                            auto text = row.value(QStringLiteral("key")).toString();
                            if (row.value(QStringLiteral("deleted")).toBool())
                            {
                                text += QStringLiteral("（已删除）");
                            }
                            memories_->addItem(text, row);
                            if (row.value(QStringLiteral("id")).toString() == preserve)
                            {
                                selected = memories_->count() - 1;
                            }
                        }
                        memories_->setCurrentIndex(memories_->count() ? selected : -1);
                        feedback_->setText(rows.toArray().isEmpty() && success.isEmpty()
                                               ? QStringLiteral("该观众暂无记忆")
                                               : success);
                    }
                    else if (error.isEmpty())
                    {
                        feedback_->setText(QStringLiteral("服务返回了无效的记忆列表"));
                    }
                    this->show_memory();
                });
        }

        void show_memory()
        {
            const auto row = this->memory();
            value_->setText(row.value(QStringLiteral("value")).toString());
            ::QString display;
            if (!row.isEmpty())
            {
                display = QStringLiteral("类型 %1　状态 %2　版本 %3")
                              .arg(row.value(QStringLiteral("kind")).toString(),
                                   row.value(QStringLiteral("status")).toString())
                              .arg(row.value(QStringLiteral("version")).toInteger());
                for (const auto& item : row.value(QStringLiteral("evidence")).toArray())
                {
                    const auto evidence = item.toObject();
                    display += QStringLiteral("\n%1 · %2\n%3")
                                   .arg(evidence.value(QStringLiteral("source")).toString(),
                                        evidence.value(QStringLiteral("event_id")).toString(),
                                        evidence.value(QStringLiteral("quote")).toString());
                }
            }
            evidence_->setPlainText(display);
            this->controls();
        }

        ::QJsonObject request() const
        {
            return {{QStringLiteral("request_key"),
                     ::QUuid::createUuid().toString(::QUuid::WithoutBraces)},
                    {QStringLiteral("reason"), reason_->text().trimmed()}};
        }

        bool confirm(const ::QString& title, const ::QString& prompt)
        {
            const auto revision = revision_;
            return ::QMessageBox::question(this, title, prompt,
                                           ::QMessageBox::Yes | ::QMessageBox::No,
                                           ::QMessageBox::No) == ::QMessageBox::Yes &&
                   access_ && !busy_ && revision == revision_;
        }

        void mutate(const ::QString& operation)
        {
            const auto selected = this->memory();
            const auto id = selected.value(QStringLiteral("id")).toString();
            if (operation == QStringLiteral("delete") &&
                !this->confirm(QStringLiteral("删除记忆"),
                               QStringLiteral("删除选中的记忆及其检索向量？")))
            {
                return;
            }
            auto body = this->request();
            body.insert(QStringLiteral("operation"), operation);
            body.insert(QStringLiteral("expected_version"),
                        selected.value(QStringLiteral("version")));
            if (operation == QStringLiteral("correct"))
            {
                body.insert(QStringLiteral("value"), value_->text().trimmed());
            }
            else if (operation == QStringLiteral("freeze"))
            {
                body.insert(QStringLiteral("frozen"),
                            !selected.value(QStringLiteral("locked")).toBool());
            }
            this->post(this->viewer_path() + QStringLiteral("/") + id, body, id);
        }

        void maintain(const ::QString& operation)
        {
            if (operation == QStringLiteral("rebuild-vectors") &&
                !this->confirm(QStringLiteral("重建向量"),
                               QStringLiteral("重新生成当前范围的记忆向量？")))
            {
                return;
            }
            this->post(QStringLiteral("/api/admin/memories/") + operation, this->request(), {});
        }

        void post(const ::QString& path, const ::QJsonObject& body, const ::QString& memory_id)
        {
            const auto revision = this->begin();
            feedback_->setText(QStringLiteral("正在提交记忆操作…"));
            server_.post_json(
                path, body, *this,
                [this, revision, memory_id](const ::QJsonDocument& document, const ::QString& error)
                {
                    if (revision != revision_)
                    {
                        return;
                    }
                    busy_ = false;
                    if (!error.isEmpty() ||
                        document.object().value(QStringLiteral("updated")) != true)
                    {
                        feedback_->setText(error.isEmpty() ? QStringLiteral("主服务未确认操作")
                                                           : error);
                        this->controls();
                        return;
                    }
                    reason_->clear();
                    const auto success = QStringLiteral("记忆操作已提交");
                    if (memory_id.isEmpty())
                    {
                        feedback_->setText(success);
                        this->refresh_status();
                    }
                    else
                    {
                        this->list(memory_id, success);
                    }
                });
        }

        meowlive::desktop::services::ServerClient& server_;
        ::QLineEdit* viewer_{};
        ::QComboBox* memories_{};
        ::QLineEdit* value_{};
        ::QLineEdit* reason_{};
        ::QPushButton* load_{};
        ::QPushButton* correct_{};
        ::QPushButton* freeze_{};
        ::QPushButton* remove_{};
        ::QPushButton* status_{};
        ::QPushButton* retry_{};
        ::QPushButton* rebuild_{};
        ::QPlainTextEdit* evidence_{};
        ::QLabel* job_status_{};
        ::QLabel* feedback_{};
        unsigned revision_{};
        bool access_{};
        bool busy_{};
        bool status_ready_{};
    };
}
