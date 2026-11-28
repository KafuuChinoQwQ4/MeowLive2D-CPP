module;

#include <QComboBox>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSignalBlocker>
#include <QString>
#include <QWidget>

#include <functional>
#include <utility>

export module meowlive.desktop.ui.profile_selector;

export namespace meowlive::desktop::ui
{
    class ProfileSelector final : public ::QWidget
    {
    public:
        using Action = std::function<void(const ::QString&, const ::QString&)>;

        ProfileSelector(const ::QString& label, Action action, ::QWidget* parent)
            : ::QWidget(parent), action_(std::move(action))
        {
            auto* layout = new ::QHBoxLayout(this);
            layout->setContentsMargins(0, 0, 0, 0);
            profiles_ = new ::QComboBox(this);
            profiles_->setAccessibleName(label);
            auto* caption = new ::QLabel(label, this);
            caption->setBuddy(profiles_);
            layout->addWidget(caption);
            layout->addWidget(profiles_, 1);
            for (const auto& [operation, text] :
                 {std::pair{QStringLiteral("select"), QStringLiteral("切换")},
                  std::pair{QStringLiteral("create"), QStringLiteral("新建")},
                  std::pair{QStringLiteral("rename"), QStringLiteral("重命名")},
                  std::pair{QStringLiteral("delete"), QStringLiteral("删除")}})
            {
                auto* button = new ::QPushButton(text, this);
                layout->addWidget(button);
                this->connect(button, &::QPushButton::clicked, this,
                              [this, operation, text]
                              {
                                  this->activate(operation, text);
                              });
            }
        }

        void update_profiles(const ::QJsonObject& snapshot)
        {
            const ::QSignalBlocker blocked(profiles_);
            profiles_->clear();
            const auto selected = snapshot.value(QStringLiteral("selected_profile_id")).toString();
            const auto profiles = snapshot.value(QStringLiteral("profiles")).toArray();
            for (const auto& profile : profiles)
            {
                const auto item = profile.toObject();
                const auto id = item.value(QStringLiteral("id")).toString();
                profiles_->addItem(item.value(QStringLiteral("name")).toString(), id);
            }
            profiles_->setCurrentIndex(profiles_->findData(selected));
        }

        ::QString id() const
        {
            return profiles_->currentData().toString();
        }

    private:
        void activate(const ::QString& operation, const ::QString& title)
        {
            if (operation == QStringLiteral("create") || operation == QStringLiteral("rename"))
            {
                if (operation == QStringLiteral("rename") && this->id().isEmpty())
                {
                    return;
                }
                bool accepted{};
                const auto name = ::QInputDialog::getText(this, title, QStringLiteral("名称"),
                                                          ::QLineEdit::Normal,
                                                          operation == QStringLiteral("rename")
                                                              ? profiles_->currentText()
                                                              : ::QString{},
                                                          &accepted)
                                      .trimmed();
                if (accepted && !name.isEmpty())
                {
                    action_(operation, name);
                }
                return;
            }
            if (this->id().isEmpty())
            {
                return;
            }
            if (operation == QStringLiteral("delete") &&
                ::QMessageBox::question(
                    this, title, QStringLiteral("删除「%1」？").arg(profiles_->currentText())) !=
                    ::QMessageBox::Yes)
            {
                return;
            }
            action_(operation, {});
        }

        Action action_;
        ::QComboBox* profiles_{};
    };
}
