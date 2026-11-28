module;

#include <QComboBox>
#include <QDateTime>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QString>
#include <QStringList>
#include <QUrlQuery>
#include <QUuid>
#include <QVBoxLayout>
#include <QWidget>
#include <utility>

module meowlive.desktop.ui.viewer_panel;

import meowlive.desktop.services.server_client;

namespace meowlive::desktop::ui
{
    void ViewerPanel::read_page(unsigned revision)
    {
        const bool event_page = this->events();
        ::QUrlQuery query;
        query.addQueryItem(QStringLiteral("limit"), ::QString::number(page_size));
        query.addQueryItem(QStringLiteral("offset"), ::QString::number(offset_));
        server_.get_json(
            event_page ? QStringLiteral("/api/admin/events") : QStringLiteral("/api/admin/viewers"),
            query, *this,
            [this, revision, event_page](const ::QJsonDocument& document, const ::QString& error)
            {
                if (revision != revision_)
                {
                    return;
                }
                pending_ = false;
                if (!error.isEmpty())
                {
                    status_->setText(error);
                    this->update_controls();
                    return;
                }
                const auto page = document.object();
                const auto key = event_page ? QStringLiteral("events") : QStringLiteral("viewers");
                rows_ = page.value(key).toArray();
                const auto scope = page.value(QStringLiteral("scope_id")).toString();
                status_->setText(
                    event_page
                        ? QStringLiteral("作用域 %1　未确认事件 %2")
                              .arg(scope)
                              .arg(page.value(QStringLiteral("unconfirmed_events")).toInteger())
                        : QStringLiteral("作用域 %1").arg(scope));
                for (const auto row : rows_)
                {
                    const auto item = row.toObject();
                    if (event_page)
                    {
                        const auto kind = item.value(QStringLiteral("kind")).toObject();
                        list_->addItem(QStringLiteral("%1　%2").arg(
                            item.value(QStringLiteral("viewer")).toString(),
                            event_name(kind.value(QStringLiteral("type")).toString())));
                    }
                    else
                    {
                        auto label = item.value(QStringLiteral("current_alias")).toString();
                        if (label.isEmpty())
                        {
                            label = item.value(QStringLiteral("viewer_id")).toString();
                        }
                        list_->addItem(label);
                    }
                }
                this->update_controls();
            });
    }

    void ViewerPanel::selected(int index)
    {
        ++selection_revision_;
        ledger_ = {};
        adjustment_viewer_.clear();
        this->update_controls();
        detail_->clear();
        if (index < 0 || index >= rows_.size())
        {
            return;
        }
        const auto item = rows_.at(index).toObject();
        if (this->events())
        {
            this->show_detail(item);
            return;
        }
        const auto id = item.value(QStringLiteral("viewer_id")).toString();
        ::QUrlQuery query;
        query.addQueryItem(QStringLiteral("viewer_id"), id);
        const auto revision = selection_revision_;
        server_.get_json(
            QStringLiteral("/api/admin/viewers/detail"), query, *this,
            [this, revision, id](const ::QJsonDocument& document, const ::QString& error)
            {
                const auto row = list_->currentRow();
                if (revision != selection_revision_ || row < 0 || row >= rows_.size() ||
                    rows_.at(row).toObject().value(QStringLiteral("viewer_id")).toString() != id)
                {
                    return;
                }
                if (error.isEmpty())
                {
                    this->show_detail(document.object());
                    this->load_affinity(id, revision);
                }
                else
                {
                    detail_->setPlainText(error);
                }
            });
    }

    void ViewerPanel::show_detail(const ::QJsonObject& item)
    {
        ::QStringList rows;
        if (this->events())
        {
            const auto kind = item.value(QStringLiteral("kind")).toObject();
            rows << QStringLiteral("%1　%2　%3")
                        .arg(local_time(item.value(QStringLiteral("occurred_at_ms"))),
                             item.value(QStringLiteral("viewer")).toString(),
                             event_name(kind.value(QStringLiteral("type")).toString()));
            if (kind.contains(QStringLiteral("text")))
            {
                rows << kind.value(QStringLiteral("text")).toString();
            }
            if (kind.contains(QStringLiteral("name")))
            {
                rows << QStringLiteral("%1 × %2")
                            .arg(kind.value(QStringLiteral("name")).toString())
                            .arg(kind.value(QStringLiteral("count")).toInt());
            }
            rows << QStringLiteral("事件 ID %1")
                        .arg(item.value(QStringLiteral("event_id")).toString());
        }
        else
        {
            rows << QStringLiteral("观众 ID %1")
                        .arg(item.value(QStringLiteral("viewer_id")).toString());
            for (const auto identity : item.value(QStringLiteral("identities")).toArray())
            {
                const auto fields = identity.toObject();
                rows << QStringLiteral("%1　%2　%3")
                            .arg(fields.value(QStringLiteral("platform")).toString(),
                                 fields.value(QStringLiteral("id_kind")).toString(),
                                 fields.value(QStringLiteral("external_id")).toString());
            }
            for (const auto alias : item.value(QStringLiteral("aliases")).toArray())
            {
                const auto fields = alias.toObject();
                rows << QStringLiteral("别名 %1　最近出现 %2")
                            .arg(fields.value(QStringLiteral("alias")).toString(),
                                 local_time(fields.value(QStringLiteral("last_seen_at_ms"))));
            }
        }
        detail_->setPlainText(rows.join(QChar(u'\n')));
    }
}
