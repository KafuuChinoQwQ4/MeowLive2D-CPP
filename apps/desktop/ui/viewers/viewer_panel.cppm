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

export module meowlive.desktop.ui.viewer_panel;

import meowlive.desktop.services.server_client;

export namespace meowlive::desktop::ui
{
    class ViewerPanel final : public ::QWidget
    {
    public:
        ViewerPanel(meowlive::desktop::services::ServerClient& server, ::QWidget* parent)
            : ::QWidget(parent), server_(server)
        {
            auto* layout = new ::QVBoxLayout(this);
            status_ = new ::QLabel(this);
            status_->setTextFormat(::Qt::PlainText);
            layout->addWidget(status_);
            auto* toolbar = new ::QHBoxLayout;
            view_ = new ::QComboBox(this);
            view_->addItems({QStringLiteral("观众档案"), QStringLiteral("直播事件")});
            reload_ = new ::QPushButton(QStringLiteral("刷新"), this);
            previous_ = new ::QPushButton(QStringLiteral("上一页"), this);
            next_ = new ::QPushButton(QStringLiteral("下一页"), this);
            toolbar->addWidget(view_);
            toolbar->addWidget(reload_);
            toolbar->addStretch();
            toolbar->addWidget(previous_);
            toolbar->addWidget(next_);
            layout->addLayout(toolbar);
            list_ = new ::QListWidget(this);
            layout->addWidget(list_, 2);
            detail_ = new ::QPlainTextEdit(this);
            detail_->setReadOnly(true);
            layout->addWidget(detail_, 1);
            auto* affinity = new ::QHBoxLayout;
            adjust_ = new ::QPushButton(QStringLiteral("调整好感度"), this);
            reverse_ = new ::QPushButton(QStringLiteral("撤销流水"), this);
            merge_ = new ::QPushButton(QStringLiteral("合并观众"), this);
            affinity->addWidget(adjust_);
            affinity->addWidget(reverse_);
            affinity->addWidget(merge_);
            affinity->addStretch();
            layout->addLayout(affinity);
            this->connect(adjust_, &::QPushButton::clicked, this,
                          [this]
                          {
                              this->change_affinity(false);
                          });
            this->connect(reverse_, &::QPushButton::clicked, this,
                          [this]
                          {
                              this->change_affinity(true);
                          });
            this->connect(merge_, &::QPushButton::clicked, this, &ViewerPanel::preview_merge);

            this->connect(reload_, &::QPushButton::clicked, this, &ViewerPanel::refresh);
            this->connect(previous_, &::QPushButton::clicked, this,
                          [this]
                          {
                              offset_ -= page_size;
                              this->refresh();
                          });
            this->connect(next_, &::QPushButton::clicked, this,
                          [this]
                          {
                              offset_ += page_size;
                              this->refresh();
                          });
            this->connect(view_, &::QComboBox::currentIndexChanged, this,
                          [this]
                          {
                              offset_ = 0;
                              this->refresh();
                          });
            this->connect(list_, &::QListWidget::currentRowChanged, this, &ViewerPanel::selected);
            this->update_controls();
        }

        void set_access(bool access)
        {
            if (access_ == access)
            {
                return;
            }
            access_ = access;
            ++revision_;
            available_ = false;
            pending_ = false;
            offset_ = 0;
            this->clear();
            this->update_controls();
            if (access_ && this->isVisible())
            {
                this->refresh();
            }
        }

        void refresh()
        {
            if (!access_)
            {
                return;
            }
            const auto revision = ++revision_;
            pending_ = true;
            this->clear();
            this->update_controls();
            status_->setText(QStringLiteral("正在读取观众数据库…"));
            server_.get_json(
                QStringLiteral("/api/admin/viewers/status"), *this,
                [this, revision](const ::QJsonDocument& document, const ::QString& error)
                {
                    if (revision != revision_)
                    {
                        return;
                    }
                    const auto status = document.object();
                    available_ =
                        error.isEmpty() && status.value(QStringLiteral("available")).toBool();
                    if (available_)
                    {
                        this->read_page(revision);
                        return;
                    }
                    pending_ = false;
                    const bool enabled = status.value(QStringLiteral("enabled")).toBool();
                    status_->setText(!error.isEmpty() ? error
                                     : enabled        ? QStringLiteral("观众数据库不可用")
                                                      : QStringLiteral("观众存储未启用"));
                    this->update_controls();
                });
        }

    private:
        static constexpr int page_size = 50;

        bool events() const
        {
            return view_->currentIndex() == 1;
        }

        static ::QString event_name(const ::QString& type)
        {
            if (type == QStringLiteral("chat"))
            {
                return QStringLiteral("弹幕");
            }
            if (type == QStringLiteral("gift"))
            {
                return QStringLiteral("礼物");
            }
            if (type == QStringLiteral("super_chat"))
            {
                return QStringLiteral("SC");
            }
            return QStringLiteral("进房");
        }

        static ::QString local_time(const ::QJsonValue& timestamp)
        {
            return ::QDateTime::fromMSecsSinceEpoch(timestamp.toInteger())
                .toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"));
        }

        void read_page(unsigned revision);

        void selected(int index);

        void load_affinity(const ::QString& id, unsigned revision);

        void change_affinity(bool reverse);

        void preview_merge();

        void confirm_merge(::QJsonObject request, const ::QJsonObject& preview, unsigned revision);

        void show_detail(const ::QJsonObject& item);

        void clear()
        {
            rows_ = {};
            list_->clear();
            ++selection_revision_;
            ledger_ = {};
            adjustment_viewer_.clear();
            detail_->clear();
            status_->clear();
        }

        void update_controls()
        {
            const bool idle = access_ && !pending_;
            view_->setEnabled(idle);
            reload_->setEnabled(idle);
            list_->setEnabled(idle);
            adjust_->setEnabled(idle && !adjustment_viewer_.isEmpty());
            merge_->setEnabled(idle && !adjustment_viewer_.isEmpty());
            bool reversible{};
            for (const auto entry : ledger_)
            {
                reversible |= entry.toObject().value(QStringLiteral("reversible")).toBool();
            }
            reverse_->setEnabled(idle && !adjustment_viewer_.isEmpty() && reversible);
            previous_->setEnabled(idle && available_ && offset_ >= page_size);
            next_->setEnabled(idle && available_ && rows_.size() == page_size &&
                              offset_ < 1'000'000);
        }

        meowlive::desktop::services::ServerClient& server_;
        ::QComboBox* view_{};
        ::QLabel* status_{};
        ::QPushButton* reload_{};
        ::QPushButton* previous_{};
        ::QPushButton* next_{};
        ::QPushButton* adjust_{};
        ::QPushButton* reverse_{};
        ::QPushButton* merge_{};
        ::QListWidget* list_{};
        ::QPlainTextEdit* detail_{};
        ::QJsonArray rows_;
        ::QJsonArray ledger_;
        ::QString adjustment_viewer_;
        int offset_{};
        unsigned revision_{};
        unsigned selection_revision_{};
        bool access_{};
        bool available_{};
        bool pending_{};
    };
}
