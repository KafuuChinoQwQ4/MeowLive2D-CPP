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
    void ViewerPanel::load_affinity(const ::QString& id, unsigned revision)
    {
        server_.get_json(
            QStringLiteral("/api/admin/viewers/") + id, *this,
            [this, id, revision](const ::QJsonDocument& document, const ::QString& error)
            {
                if (revision != selection_revision_ || !access_)
                {
                    return;
                }
                if (!error.isEmpty())
                {
                    detail_->appendPlainText(error);
                    return;
                }
                const auto details = document.object();
                adjustment_viewer_ = id;
                ledger_ = details.value(QStringLiteral("ledger")).toArray();
                detail_->appendPlainText(
                    QStringLiteral("好感度 %1　熟悉度 %2")
                        .arg(details.value(QStringLiteral("affinity_milli")).toInt() / 1000.0)
                        .arg(details.value(QStringLiteral("familiarity_milli")).toInt() / 1000.0));
                for (const auto entry : ledger_)
                {
                    const auto item = entry.toObject();
                    detail_->appendPlainText(
                        QStringLiteral("%1　%2 毫分　%3")
                            .arg(local_time(item.value(QStringLiteral("created_at_ms"))))
                            .arg(item.value(QStringLiteral("applied_delta_milli")).toInt())
                            .arg(item.value(QStringLiteral("reason")).toString()));
                }
                this->update_controls();
            });
    }

    void ViewerPanel::change_affinity(bool reverse)
    {
        const auto id = adjustment_viewer_;
        const auto revision = selection_revision_;
        bool accepted{};
        int delta{};
        ::QString ledger_id;
        if (reverse)
        {
            ::QStringList choices;
            for (const auto entry : ledger_)
            {
                const auto row = entry.toObject();
                if (row.value(QStringLiteral("reversible")).toBool())
                {
                    choices << QStringLiteral("%1　%2　%3")
                                   .arg(row.value(QStringLiteral("ledger_id")).toString(),
                                        local_time(row.value(QStringLiteral("created_at_ms"))),
                                        row.value(QStringLiteral("reason")).toString());
                }
            }
            ledger_id =
                ::QInputDialog::getItem(this, QStringLiteral("撤销流水"),
                                        QStringLiteral("选择原流水"), choices, 0, false, &accepted)
                    .left(36);
        }
        else
        {
            delta = ::QInputDialog::getInt(this, QStringLiteral("调整好感度"),
                                           QStringLiteral("调整毫分（1000 毫分 = 1 分）"), 1000,
                                           -100000, 100000, 100, &accepted);
            accepted = accepted && delta != 0;
        }
        if (!accepted || !access_ || revision != selection_revision_)
        {
            return;
        }
        const auto reason =
            ::QInputDialog::getText(this, QStringLiteral("记录操作依据"), QStringLiteral("原因"),
                                    ::QLineEdit::Normal, {}, &accepted);
        if (!accepted || reason.trimmed().isEmpty() || !access_ || revision != selection_revision_)
        {
            return;
        }
        ::QJsonObject request;
        request.insert(QStringLiteral("request_key"),
                       ::QUuid::createUuid().toString(::QUuid::WithoutBraces));
        request.insert(QStringLiteral("reason"), reason);
        if (reverse)
        {
            request.insert(QStringLiteral("ledger_id"), ledger_id);
        }
        else
        {
            request.insert(QStringLiteral("delta_milli"), delta);
        }
        pending_ = true;
        this->update_controls();
        server_.post_json(QStringLiteral("/api/admin/viewers/") + id +
                              (reverse ? QStringLiteral("/reverse") : QStringLiteral("/adjust")),
                          request, *this,
                          [this, revision](const ::QJsonDocument&, const ::QString& error)
                          {
                              if (revision != selection_revision_)
                              {
                                  return;
                              }
                              pending_ = false;
                              if (error.isEmpty())
                              {
                                  this->selected(list_->currentRow());
                              }
                              else
                              {
                                  detail_->appendPlainText(error);
                                  this->update_controls();
                              }
                          });
    }
}
