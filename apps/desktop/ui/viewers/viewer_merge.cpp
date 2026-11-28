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
    void ViewerPanel::preview_merge()
    {
        const auto source = adjustment_viewer_;
        const auto revision = selection_revision_;
        bool accepted{};
        const auto target =
            ::QInputDialog::getText(this, QStringLiteral("合并观众"), QStringLiteral("目标观众 ID"),
                                    ::QLineEdit::Normal, {}, &accepted)
                .trimmed();
        if (!accepted || !access_ || revision != selection_revision_ || target.isEmpty() ||
            target == source)
        {
            return;
        }
        pending_ = true;
        this->update_controls();
        ::QJsonObject request;
        request.insert(QStringLiteral("source_viewer_id"), source);
        request.insert(QStringLiteral("target_viewer_id"), target);
        server_.post_json(
            QStringLiteral("/api/admin/viewers/merge/preview"), request, *this,
            [this, request, revision](const ::QJsonDocument& document, const ::QString& error)
            {
                if (revision != selection_revision_ || !access_)
                {
                    return;
                }
                pending_ = false;
                this->update_controls();
                if (!error.isEmpty())
                {
                    detail_->appendPlainText(error);
                    return;
                }
                this->confirm_merge(request, document.object(), revision);
            });
    }

    void ViewerPanel::confirm_merge(::QJsonObject request, const ::QJsonObject& preview,
                                    unsigned revision)
    {
        const auto source = preview.value(QStringLiteral("source")).toObject();
        const auto target = preview.value(QStringLiteral("target")).toObject();
        ::QStringList summary;
        summary << QStringLiteral("源档案 %1（%2）")
                       .arg(source.value(QStringLiteral("alias")).toString(),
                            source.value(QStringLiteral("viewer_id")).toString());
        summary << QStringLiteral("目标档案 %1（%2）")
                       .arg(target.value(QStringLiteral("alias")).toString(),
                            target.value(QStringLiteral("viewer_id")).toString());
        summary << QStringLiteral("合并后好感度 %1，熟悉度 %2")
                       .arg(preview.value(QStringLiteral("resulting_affinity_milli")).toInt() /
                            1000.0)
                       .arg(preview.value(QStringLiteral("resulting_familiarity_milli")).toInt() /
                            1000.0);
        for (const auto risk : preview.value(QStringLiteral("risks")).toArray())
        {
            summary << risk.toString();
        }
        ::QMessageBox confirmation(::QMessageBox::Warning, QStringLiteral("确认合并观众"),
                                   summary.join(QChar(u'\n')),
                                   ::QMessageBox::Yes | ::QMessageBox::Cancel, this);
        confirmation.setTextFormat(::Qt::PlainText);
        confirmation.setDefaultButton(::QMessageBox::Cancel);
        if (confirmation.exec() != ::QMessageBox::Yes || !access_ ||
            revision != selection_revision_)
        {
            return;
        }
        bool accepted{};
        const auto reason =
            ::QInputDialog::getText(this, QStringLiteral("记录合并依据"), QStringLiteral("原因"),
                                    ::QLineEdit::Normal, {}, &accepted);
        if (!accepted || reason.trimmed().isEmpty() || !access_ || revision != selection_revision_)
        {
            return;
        }
        request.insert(QStringLiteral("reason"), reason);
        request.insert(QStringLiteral("confirmed"), true);
        request.insert(QStringLiteral("request_key"),
                       ::QUuid::createUuid().toString(::QUuid::WithoutBraces));
        request.insert(QStringLiteral("expected_revision"),
                       preview.value(QStringLiteral("revision")));
        request.insert(QStringLiteral("fingerprint"), preview.value(QStringLiteral("fingerprint")));
        pending_ = true;
        this->update_controls();
        server_.post_json(QStringLiteral("/api/admin/viewers/merge/apply"), request, *this,
                          [this, revision](const ::QJsonDocument&, const ::QString& error)
                          {
                              if (revision != selection_revision_)
                              {
                                  return;
                              }
                              pending_ = false;
                              if (error.isEmpty())
                              {
                                  this->refresh();
                              }
                              else
                              {
                                  detail_->appendPlainText(error);
                                  this->update_controls();
                              }
                          });
    }
}
