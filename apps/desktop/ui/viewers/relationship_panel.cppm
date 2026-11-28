module;

#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QCheckBox>
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
#include <QString>
#include <QTextCursor>
#include <QUrlQuery>
#include <QUuid>
#include <QVBoxLayout>
#include <QWidget>

export module meowlive.desktop.ui.relationship_panel;

import meowlive.desktop.services.server_client;

export namespace meowlive::desktop::ui
{
    class RelationshipPanel final : public ::QWidget
    {
    public:
        RelationshipPanel(meowlive::desktop::services::ServerClient& server, ::QWidget* parent)
            : ::QWidget(parent), server_(server)
        {
            auto* layout = new ::QVBoxLayout(this);
            auto* rows = new ::QFormLayout;
            viewer_ = new ::QLineEdit(this);
            viewer_->setPlaceholderText(QStringLiteral("观众 UUID"));
            viewer_->setMaxLength(36);
            depth_ = new ::QComboBox(this);
            depth_->addItem(QStringLiteral("直接关系"), QStringLiteral("1"));
            depth_->addItem(QStringLiteral("两层关系"), QStringLiteral("2"));
            rows->addRow(QStringLiteral("观众标识"), viewer_);
            rows->addRow(QStringLiteral("展开范围"), depth_);
            layout->addLayout(rows);

            load_ = new ::QPushButton(QStringLiteral("查看关系"), this);
            layout->addWidget(load_, 0, ::Qt::AlignLeft);
            auto* edits = new ::QHBoxLayout;
            facts_ = new ::QComboBox(this);
            facts_->setAccessibleName(QStringLiteral("选择关系记录"));
            create_ = new ::QPushButton(QStringLiteral("新增关系"), this);
            confirm_ = new ::QPushButton(QStringLiteral("确认关系"), this);
            revoke_ = new ::QPushButton(QStringLiteral("撤销确认"), this);
            delete_ = new ::QPushButton(QStringLiteral("删除关系"), this);
            edits->addWidget(facts_, 1);
            for (auto* action : {create_, confirm_, revoke_, delete_})
            {
                edits->addWidget(action);
            }
            layout->addLayout(edits);
            content_ = new ::QPlainTextEdit(this);
            content_->setReadOnly(true);
            content_->setAccessibleName(QStringLiteral("观众关系和事件依据"));
            layout->addWidget(content_, 1);

            projection_ = this->label();
            layout->addWidget(projection_);
            auto* maintenance = new ::QHBoxLayout;
            status_ = new ::QPushButton(QStringLiteral("刷新关系图状态"), this);
            rebuild_ = new ::QPushButton(QStringLiteral("重建关系图"), this);
            reason_ = new ::QLineEdit(this);
            reason_->setPlaceholderText(QStringLiteral("填写关系操作原因"));
            reason_->setAccessibleName(QStringLiteral("关系操作原因"));
            maintenance->addWidget(status_);
            maintenance->addWidget(reason_, 1);
            maintenance->addWidget(rebuild_);
            layout->addLayout(maintenance);
            feedback_ = this->label();
            layout->addWidget(feedback_);

            this->connect(load_, &::QPushButton::clicked, this,
                          [this]
                          {
                              this->load();
                          });
            this->connect(viewer_, &::QLineEdit::returnPressed, load_, &::QPushButton::click);
            this->connect(viewer_, &::QLineEdit::textChanged, this,
                          [this]
                          {
                              facts_->clear();
                              content_->clear();
                              this->controls();
                          });
            this->connect(reason_, &::QLineEdit::textChanged, this, &RelationshipPanel::controls);
            this->connect(status_, &::QPushButton::clicked, this, &RelationshipPanel::refresh);
            this->connect(rebuild_, &::QPushButton::clicked, this, &RelationshipPanel::rebuild);
            this->connect(facts_, &::QComboBox::currentIndexChanged, this,
                          &RelationshipPanel::controls);
            this->connect(create_, &::QPushButton::clicked, this, &RelationshipPanel::create);
            this->connect(confirm_, &::QPushButton::clicked, this,
                          [this]
                          {
                              this->edit(QStringLiteral("confirm"));
                          });
            this->connect(revoke_, &::QPushButton::clicked, this,
                          [this]
                          {
                              this->edit(QStringLiteral("revoke"));
                          });
            this->connect(delete_, &::QPushButton::clicked, this,
                          [this]
                          {
                              this->edit(QStringLiteral("delete"));
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
            graph_enabled_ = false;
            if (!access)
            {
                viewer_->clear();
                reason_->clear();
                content_->clear();
                projection_->clear();
                feedback_->clear();
            }
            this->controls();
            if (this->isVisible())
            {
                this->refresh();
            }
        }

        void refresh()
        {
            if (!access_ || busy_)
            {
                return;
            }
            const auto revision = this->begin();
            server_.get_json(
                QStringLiteral("/api/admin/graph/status"), *this,
                [this, revision](const ::QJsonDocument& document, const ::QString& error)
                {
                    if (revision != revision_)
                    {
                        return;
                    }
                    busy_ = false;
                    const auto result = document.object();
                    const auto enabled = result.value(QStringLiteral("enabled"));
                    const auto connected = result.value(QStringLiteral("connected"));
                    graph_enabled_ = error.isEmpty() && enabled.toBool() && connected.isBool();
                    if (!error.isEmpty() || !enabled.isBool() || !connected.isBool())
                    {
                        projection_->setText(error.isEmpty() ? QStringLiteral("关系图状态无效")
                                                             : error);
                    }
                    else if (graph_enabled_)
                    {
                        projection_->setText(
                            QStringLiteral("关系图 %1　待处理 %2　处理中 %3　失败 %4")
                                .arg(connected.toBool() ? QStringLiteral("已连接")
                                                        : QStringLiteral("未连接"))
                                .arg(result.value(QStringLiteral("pending")).toInteger())
                                .arg(result.value(QStringLiteral("leased")).toInteger())
                                .arg(result.value(QStringLiteral("failed")).toInteger()));
                    }
                    else
                    {
                        projection_->setText(QStringLiteral("关系图未启用"));
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

        static bool valid_viewer(const ::QString& value)
        {
            static const ::QRegularExpression pattern(
                QStringLiteral(R"(\A[0-9a-fA-F]{8}(?:-[0-9a-fA-F]{4}){3}-[0-9a-fA-F]{12}\z)"));
            return pattern.match(value).hasMatch();
        }

        unsigned begin()
        {
            busy_ = true;
            this->controls();
            return ++revision_;
        }

        void controls()
        {
            const bool enabled = access_ && !busy_;
            const auto reason = reason_->text().trimmed().toUtf8();
            viewer_->setEnabled(enabled);
            depth_->setEnabled(enabled);
            const bool viewer_valid = valid_viewer(viewer_->text().trimmed());
            const bool justified = !reason.isEmpty() && reason.size() <= 1'000;
            const auto fact = facts_->currentData().toJsonObject();
            const bool selected = valid_viewer(fact.value(QStringLiteral("id")).toString()) &&
                                  fact.value(QStringLiteral("version")).toInteger() > 0 &&
                                  !fact.value(QStringLiteral("deleted")).toBool();
            load_->setEnabled(enabled && viewer_valid);
            status_->setEnabled(enabled);
            reason_->setEnabled(enabled);
            rebuild_->setEnabled(enabled && graph_enabled_ && justified);
            facts_->setEnabled(enabled);
            create_->setEnabled(enabled && viewer_valid && justified);
            confirm_->setEnabled(enabled && selected && justified);
            revoke_->setEnabled(enabled && selected && justified);
            delete_->setEnabled(enabled && selected && justified);
        }

        void load(const ::QString& success = {})
        {
            const auto revision = this->begin();
            const auto path = QStringLiteral("/api/admin/viewers/") + viewer_->text().trimmed() +
                              QStringLiteral("/relationships");
            ::QUrlQuery query;
            query.addQueryItem(QStringLiteral("depth"), depth_->currentData().toString());
            feedback_->setText(QStringLiteral("正在查询观众关系…"));
            server_.get_json(
                path, query, *this,
                [this, revision, success](const ::QJsonDocument& document, const ::QString& error)
                {
                    if (revision != revision_)
                    {
                        return;
                    }
                    busy_ = false;
                    facts_->clear();
                    content_->clear();
                    const auto result = document.object();
                    const auto relations = result.value(QStringLiteral("relationships"));
                    if (error.isEmpty() && relations.isArray())
                    {
                        this->render(relations.toArray());
                        feedback_->setText(result.value(QStringLiteral("degraded")).toBool()
                                               ? QStringLiteral("已返回数据库中的直接关系")
                                               : success);
                    }
                    else
                    {
                        feedback_->setText(error.isEmpty() ? QStringLiteral("关系查询结果无效")
                                                           : error);
                    }
                    this->controls();
                });
        }

        static ::QString endpoint(const ::QJsonObject& relation, const ::QString& field)
        {
            const auto object = relation.value(field).toObject();
            return QStringLiteral("%1 %2").arg(object.value(QStringLiteral("kind")).toString(),
                                               object.value(QStringLiteral("id")).toString());
        }

        void render(const ::QJsonArray& entries)
        {
            if (entries.isEmpty())
            {
                content_->setPlainText(QStringLiteral("该观众暂无关系"));
                return;
            }
            for (const auto& item : entries)
            {
                const auto relation = item.toObject();
                auto line = QStringLiteral("%1 → %2　%3　%4")
                                .arg(endpoint(relation, QStringLiteral("source")),
                                     endpoint(relation, QStringLiteral("target")),
                                     relation.value(QStringLiteral("kind")).toString(),
                                     relation.value(QStringLiteral("confirmation")).toString());
                facts_->addItem(line, relation);
                for (const auto& item : relation.value(QStringLiteral("evidence")).toArray())
                {
                    const auto event = item.toObject();
                    line += QStringLiteral("\n%1 · %2　%3")
                                .arg(event.value(QStringLiteral("source")).toString(),
                                     event.value(QStringLiteral("event_id")).toString(),
                                     event.value(QStringLiteral("quote")).toString());
                }
                content_->appendPlainText(line);
            }
            content_->moveCursor(::QTextCursor::Start);
        }

        ::QJsonObject request() const
        {
            return {{QStringLiteral("request_key"),
                     ::QUuid::createUuid().toString(::QUuid::WithoutBraces)},
                    {QStringLiteral("reason"), reason_->text().trimmed()}};
        }

        void edit(const ::QString& action)
        {
            const auto selected = facts_->currentData().toJsonObject();
            const auto revision = revision_;
            if (action == QStringLiteral("delete") &&
                (::QMessageBox::question(this, QStringLiteral("删除关系"),
                                         QStringLiteral("删除选中的关系记录？"),
                                         ::QMessageBox::Yes | ::QMessageBox::No,
                                         ::QMessageBox::No) != ::QMessageBox::Yes ||
                 !access_ || busy_ || revision != revision_))
            {
                return;
            }
            auto body = this->request();
            body.insert(QStringLiteral("action"), action);
            body.insert(QStringLiteral("expected_version"),
                        selected.value(QStringLiteral("version")));
            this->write(QStringLiteral("/api/admin/relationships/") +
                            selected.value(QStringLiteral("id")).toString(),
                        body);
        }

        void create()
        {
            const auto revision = revision_;
            ::QDialog dialog(this);
            dialog.setWindowTitle(QStringLiteral("新增关系"));
            auto* form = new ::QFormLayout(&dialog);
            auto* target = new ::QLineEdit(&dialog);
            auto* entity = new ::QComboBox(&dialog);
            auto* relationship = new ::QComboBox(&dialog);
            auto* confirmed = new ::QCheckBox(QStringLiteral("管理员确认"), &dialog);
            auto* evidence_source = new ::QLineEdit(&dialog);
            auto* evidence_event = new ::QLineEdit(&dialog);
            auto* evidence_quote = new ::QLineEdit(&dialog);
            entity->addItems({QStringLiteral("viewer"), QStringLiteral("topic"),
                              QStringLiteral("activity"), QStringLiteral("unresolved")});
            relationship->addItems({QStringLiteral("mention"), QStringLiteral("acquaintance"),
                                    QStringLiteral("participated"),
                                    QStringLiteral("shared_interest"), QStringLiteral("friend")});
            form->addRow(QStringLiteral("目标类型"), entity);
            form->addRow(QStringLiteral("目标标识"), target);
            form->addRow(QStringLiteral("关系类型"), relationship);
            form->addRow(QStringLiteral("依据来源"), evidence_source);
            form->addRow(QStringLiteral("事件标识"), evidence_event);
            form->addRow(QStringLiteral("引用内容"), evidence_quote);
            form->addRow(confirmed);
            auto* buttons = new ::QDialogButtonBox(
                ::QDialogButtonBox::Ok | ::QDialogButtonBox::Cancel, &dialog);
            form->addRow(buttons);
            dialog.connect(buttons, &::QDialogButtonBox::accepted, &dialog, &::QDialog::accept);
            dialog.connect(buttons, &::QDialogButtonBox::rejected, &dialog, &::QDialog::reject);
            if (dialog.exec() != ::QDialog::Accepted || !access_ || busy_ || revision != revision_)
            {
                return;
            }

            const auto target_id = target->text().trimmed();
            const auto source = evidence_source->text().trimmed();
            const auto event = evidence_event->text().trimmed();
            const auto quote = evidence_quote->text().trimmed();
            const bool has_evidence = !source.isEmpty() && !event.isEmpty() && !quote.isEmpty();
            if (target_id.isEmpty() ||
                (entity->currentText() == QStringLiteral("viewer") && !valid_viewer(target_id)) ||
                (!confirmed->isChecked() && !has_evidence))
            {
                feedback_->setText(QStringLiteral("请填写目标标识和完整依据，或明确管理员确认"));
                return;
            }
            ::QJsonArray evidence;
            if (has_evidence)
            {
                evidence.append(::QJsonObject{{QStringLiteral("source"), source},
                                              {QStringLiteral("event_id"), event},
                                              {QStringLiteral("quote"), quote}});
            }
            auto body = this->request();
            body.insert(QStringLiteral("source"),
                        ::QJsonObject{{QStringLiteral("kind"), QStringLiteral("viewer")},
                                      {QStringLiteral("id"), viewer_->text().trimmed()}});
            body.insert(QStringLiteral("target"),
                        ::QJsonObject{{QStringLiteral("kind"), entity->currentText()},
                                      {QStringLiteral("id"), target_id}});
            body.insert(QStringLiteral("kind"), relationship->currentText());
            body.insert(QStringLiteral("admin_confirmed"), confirmed->isChecked());
            body.insert(QStringLiteral("evidence"), evidence);
            body.insert(QStringLiteral("expires_at_ms"), ::QJsonValue::Null);
            this->write(QStringLiteral("/api/admin/relationships"), body);
        }

        void write(const ::QString& path, const ::QJsonObject& request)
        {
            const auto revision = this->begin();
            feedback_->setText(QStringLiteral("正在提交关系操作…"));
            server_.post_json(
                path, request, *this,
                [this, revision](const ::QJsonDocument& document, const ::QString& error)
                {
                    if (revision != revision_)
                    {
                        return;
                    }
                    busy_ = false;
                    if (!error.isEmpty() ||
                        !valid_viewer(document.object().value(QStringLiteral("id")).toString()))
                    {
                        feedback_->setText(error.isEmpty() ? QStringLiteral("主服务未确认关系操作")
                                                           : error);
                        this->controls();
                        return;
                    }
                    reason_->clear();
                    this->load(QStringLiteral("关系已更新"));
                });
        }

        void rebuild()
        {
            const auto revision = revision_;
            const auto choice =
                ::QMessageBox::question(this, QStringLiteral("重建关系图"),
                                        QStringLiteral("从 PostgreSQL 关系记录重新生成关系图？"),
                                        ::QMessageBox::Yes | ::QMessageBox::No, ::QMessageBox::No);
            if (choice != ::QMessageBox::Yes || !access_ || busy_ || revision != revision_)
            {
                return;
            }
            const auto pending = this->begin();
            const ::QJsonObject body{{QStringLiteral("request_key"),
                                      ::QUuid::createUuid().toString(::QUuid::WithoutBraces)},
                                     {QStringLiteral("reason"), reason_->text().trimmed()}};
            feedback_->setText(QStringLiteral("正在提交重建请求…"));
            server_.post_json(
                QStringLiteral("/api/admin/graph/rebuild"), body, *this,
                [this, pending](const ::QJsonDocument& document, const ::QString& error)
                {
                    if (pending != revision_)
                    {
                        return;
                    }
                    busy_ = false;
                    if (error.isEmpty() &&
                        document.object().value(QStringLiteral("queued")).toInteger(-1) >= 0)
                    {
                        feedback_->setText(QStringLiteral("关系图重建已安排"));
                        reason_->clear();
                        this->refresh();
                    }
                    else
                    {
                        feedback_->setText(error.isEmpty() ? QStringLiteral("主服务未确认重建请求")
                                                           : error);
                        this->controls();
                    }
                });
        }

        meowlive::desktop::services::ServerClient& server_;
        ::QLineEdit* viewer_{};
        ::QComboBox* depth_{};
        ::QLineEdit* reason_{};
        ::QPlainTextEdit* content_{};
        ::QLabel* projection_{};
        ::QLabel* feedback_{};
        ::QPushButton* load_{};
        ::QPushButton* status_{};
        ::QPushButton* rebuild_{};
        ::QComboBox* facts_{};
        ::QPushButton* create_{};
        ::QPushButton* confirm_{};
        ::QPushButton* revoke_{};
        ::QPushButton* delete_{};
        unsigned revision_{};
        bool access_{};
        bool busy_{};
        bool graph_enabled_{};
    };
}
