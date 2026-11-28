module;

#include <QComboBox>
#include <QFormLayout>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSignalBlocker>
#include <QString>
#include <QTableWidget>
#include <QVBoxLayout>
#include <QWidget>

export module meowlive.desktop.ui.character_panel;

import meowlive.desktop.services.server_client;

export namespace meowlive::desktop::ui
{
    class CharacterPanel final : public ::QWidget
    {
    public:
        CharacterPanel(meowlive::desktop::services::ServerClient& server, ::QWidget* parent)
            : ::QWidget(parent), server_(server)
        {
            auto* layout = new ::QVBoxLayout(this);
            profiles_ = new ::QComboBox(this);
            profiles_->setAccessibleName(QStringLiteral("角色档案"));
            layout->addWidget(profiles_);
            auto* fields = new ::QFormLayout;
            name_ = new ::QLineEdit(this);
            model_ = new ::QComboBox(this);
            model_->setEditable(true);
            model_->setInsertPolicy(::QComboBox::NoInsert);
            voice_ = new ::QComboBox(this);
            mouth_ = new ::QLineEdit(this);
            fields->addRow(QStringLiteral("名称"), name_);
            fields->addRow(QStringLiteral("VTube Studio 模型"), model_);
            fields->addRow(QStringLiteral("音色"), voice_);
            fields->addRow(QStringLiteral("口型参数"), mouth_);
            layout->addLayout(fields);

            auto* tools = new ::QHBoxLayout;
            auto* models = new ::QPushButton(QStringLiteral("获取模型"), this);
            auto* hotkeys = new ::QPushButton(QStringLiteral("获取快捷键"), this);
            keys_ = new ::QComboBox(this);
            keys_->setAccessibleName(QStringLiteral("模型快捷键标识"));
            keys_->setEditable(true);
            keys_->lineEdit()->setReadOnly(true);
            keys_->setInsertPolicy(::QComboBox::NoInsert);
            tools->addWidget(models);
            tools->addWidget(hotkeys);
            tools->addWidget(keys_, 1);
            layout->addLayout(tools);

            mappings_ = new ::QTableWidget(0, 3, this);
            mappings_->setHorizontalHeaderLabels({QStringLiteral("动作名"),
                                                  QStringLiteral("快捷键标识"),
                                                  QStringLiteral("备用快捷键标识")});
            mappings_->horizontalHeader()->setSectionResizeMode(::QHeaderView::Stretch);
            layout->addWidget(mappings_, 1);

            auto* rows = new ::QHBoxLayout;
            auto* add = new ::QPushButton(QStringLiteral("添加动作"), this);
            auto* remove = new ::QPushButton(QStringLiteral("移除动作"), this);
            preview_ = new ::QPushButton(QStringLiteral("预览选中动作"), this);
            rows->addWidget(add);
            rows->addWidget(remove);
            rows->addWidget(preview_);
            rows->addStretch();
            layout->addLayout(rows);

            auto* buttons = new ::QHBoxLayout;
            auto* reload = new ::QPushButton(QStringLiteral("刷新角色"), this);
            auto* save = new ::QPushButton(QStringLiteral("保存角色"), this);
            select_ = new ::QPushButton(QStringLiteral("切换角色"), this);
            delete_ = new ::QPushButton(QStringLiteral("删除角色"), this);
            for (auto* button : {reload, save, select_, delete_})
            {
                buttons->addWidget(button);
            }
            buttons->addStretch();
            layout->addLayout(buttons);
            feedback_ = new ::QLabel(this);
            feedback_->setTextFormat(::Qt::PlainText);
            feedback_->setWordWrap(true);
            layout->addWidget(feedback_);

            this->connect(profiles_, &::QComboBox::currentIndexChanged, this,
                          &CharacterPanel::show_profile);
            this->connect(reload, &::QPushButton::clicked, this, &CharacterPanel::reload);
            this->connect(save, &::QPushButton::clicked, this,
                          [this]
                          {
                              this->post(QStringLiteral("save"), this->draft());
                          });
            this->connect(select_, &::QPushButton::clicked, this,
                          [this]
                          {
                              this->post(QStringLiteral("select"),
                                         {{QStringLiteral("id"), this->profile_id()}});
                          });
            this->connect(delete_, &::QPushButton::clicked, this, &CharacterPanel::delete_profile);
            this->connect(preview_, &::QPushButton::clicked, this, &CharacterPanel::preview);
            this->connect(add, &::QPushButton::clicked, this,
                          [this]
                          {
                              if (mappings_->rowCount() < 32)
                              {
                                  mappings_->insertRow(mappings_->rowCount());
                              }
                          });
            this->connect(remove, &::QPushButton::clicked, this,
                          [this]
                          {
                              if (mappings_->currentRow() >= 0)
                              {
                                  mappings_->removeRow(mappings_->currentRow());
                              }
                          });
            this->connect(models, &::QPushButton::clicked, this,
                          [this]
                          {
                              this->query_models();
                          });
            this->connect(hotkeys, &::QPushButton::clicked, this,
                          [this]
                          {
                              this->query_hotkeys();
                          });
            this->connect(model_, &::QComboBox::currentTextChanged, this,
                          [this]
                          {
                              keys_->clear();
                          });
            this->setEnabled(false);
        }

        void open()
        {
            if (!loaded_)
            {
                this->reload();
            }
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
            loaded_ = false;
            snapshot_ = {};
            feedback_->clear();
            model_->clear();
            keys_->clear();
            this->render({});
            if (this->isVisible())
            {
                this->open();
            }
        }

    private:
        void reload()
        {
            if (!access_ || busy_)
            {
                return;
            }
            const auto revision = this->begin();
            const auto preferred = this->profile_id();
            server_.get_json(
                QStringLiteral("/api/resources"), *this,
                [this, revision, preferred](const ::QJsonDocument& document, const ::QString& error)
                {
                    this->finish(revision, document, error, preferred, {});
                });
        }

        unsigned begin()
        {
            busy_ = true;
            feedback_->setText(QStringLiteral("正在处理角色操作…"));
            this->controls();
            return ++revision_;
        }

        void controls()
        {
            this->setEnabled(access_ && !busy_);
            const bool existing = !this->profile_id().isEmpty();
            select_->setEnabled(existing);
            delete_->setEnabled(existing);
            preview_->setEnabled(existing);
        }

        ::QString profile_id() const
        {
            return profiles_->currentData().toJsonObject().value(QStringLiteral("id")).toString();
        }

        ::QString cell(int row, int column) const
        {
            const auto* item = mappings_->item(row, column);
            return item ? item->text().trimmed() : ::QString{};
        }

        ::QString model_id() const
        {
            const auto index = model_->currentIndex();
            return index >= 0 && model_->itemText(index) == model_->currentText()
                       ? model_->itemData(index).toString()
                       : model_->currentText().trimmed();
        }

        ::QJsonObject draft() const
        {
            ::QJsonArray mappings;
            for (int row = 0; row < mappings_->rowCount(); ++row)
            {
                const auto intent = this->cell(row, 0);
                const auto id = this->cell(row, 1);
                const auto fallback = this->cell(row, 2);
                if (!intent.isEmpty() || !id.isEmpty() || !fallback.isEmpty())
                {
                    mappings.append(::QJsonObject{
                        {QStringLiteral("intent"), intent},
                        {QStringLiteral("hotkey_id"), id},
                        {QStringLiteral("fallback_hotkey_id"),
                         fallback.isEmpty() ? ::QJsonValue::Null : ::QJsonValue(fallback)}});
                }
            }
            const auto id = this->profile_id();
            return {{QStringLiteral("id"), id.isEmpty() ? ::QJsonValue::Null : ::QJsonValue(id)},
                    {QStringLiteral("name"), name_->text().trimmed()},
                    {QStringLiteral("model_id"), this->model_id()},
                    {QStringLiteral("voice_id"), voice_->currentData().toString()},
                    {QStringLiteral("mouth_parameter"), mouth_->text().trimmed()},
                    {QStringLiteral("mappings"), mappings}};
        }

        void post(const ::QString& action, const ::QJsonObject& body)
        {
            const auto revision = this->begin();
            const auto preferred = this->profile_id();
            server_.post_json(
                QStringLiteral("/api/characters/") + action, body, *this,
                [this, revision, preferred](const ::QJsonDocument& document, const ::QString& error)
                {
                    this->finish(revision, document, error, preferred,
                                 QStringLiteral("角色操作成功"));
                });
        }

        void preview()
        {
            const auto intent = this->cell(mappings_->currentRow(), 0);
            if (intent.isEmpty())
            {
                feedback_->setText(QStringLiteral("请选择已保存的动作"));
                return;
            }
            this->post(QStringLiteral("preview"),
                       {{QStringLiteral("character_id"), this->profile_id()},
                        {QStringLiteral("intent"), intent}});
        }

        void delete_profile()
        {
            const auto revision = revision_;
            if (::QMessageBox::question(this, QStringLiteral("删除角色"),
                                        profiles_->currentText()) == ::QMessageBox::Yes &&
                access_ && !busy_ && revision == revision_)
            {
                this->post(QStringLiteral("delete"), {{QStringLiteral("id"), this->profile_id()}});
            }
        }

        void finish(unsigned revision, const ::QJsonDocument& document, const ::QString& error,
                    const ::QString& preferred, const ::QString& success)
        {
            if (revision != revision_)
            {
                return;
            }
            busy_ = false;
            auto result = document.object();
            if (!error.isEmpty())
            {
                feedback_->setText(error);
            }
            else if (!result.value(QStringLiteral("characters")).isArray())
            {
                feedback_->setText(QStringLiteral("服务返回了无效的角色列表"));
            }
            else
            {
                loaded_ = true;
                snapshot_ = result;
                this->render(preferred);
                feedback_->setText(success);
            }
            this->controls();
        }

        void render(const ::QString& preferred)
        {
            const ::QSignalBlocker blocked(profiles_);
            profiles_->clear();
            profiles_->addItem(QStringLiteral("新建角色"));
            auto selected = 0;
            const auto active = snapshot_.value(QStringLiteral("active_character_id")).toString();
            const auto profiles = snapshot_.value(QStringLiteral("characters")).toArray();
            for (const auto& item : profiles)
            {
                const auto profile = item.toObject();
                const auto id = profile.value(QStringLiteral("id")).toString();
                auto label = profile.value(QStringLiteral("name")).toString();
                if (id == active)
                {
                    label += QStringLiteral("（当前）");
                }
                profiles_->addItem(label, profile);
                if (id == preferred || (preferred.isEmpty() && id == active))
                {
                    selected = profiles_->count() - 1;
                }
            }
            profiles_->setCurrentIndex(selected);
            voice_->clear();
            voice_->addItem(QStringLiteral("未选择"), ::QString{});
            if (snapshot_.value(QStringLiteral("default_voice_available")).toBool())
            {
                voice_->addItem(QStringLiteral("默认音色"), QStringLiteral("default"));
            }
            for (const auto& item : snapshot_.value(QStringLiteral("voices")).toArray())
            {
                const auto voice = item.toObject();
                voice_->addItem(voice.value(QStringLiteral("name")).toString(),
                                voice.value(QStringLiteral("id")).toString());
            }
            this->show_profile();
        }

        void show_profile()
        {
            const auto profile = profiles_->currentData().toJsonObject();
            name_->setText(profile.value(QStringLiteral("name")).toString());
            this->set_model(profile.value(QStringLiteral("model_id")).toString());
            mouth_->setText(profile.value(QStringLiteral("mouth_parameter"))
                                .toString(QStringLiteral("MeowMouthOpen")));
            const auto voice_id = profile.value(QStringLiteral("voice_id")).toString();
            int voice_index = voice_->findData(voice_id);
            if (voice_index < 0 && !voice_id.isEmpty())
            {
                voice_->addItem(voice_id, voice_id);
                voice_index = voice_->count() - 1;
            }
            voice_->setCurrentIndex(voice_index);
            mappings_->setRowCount(0);
            for (const auto& item : profile.value(QStringLiteral("mappings")).toArray())
            {
                const auto mapping = item.toObject();
                const auto row = mappings_->rowCount();
                mappings_->insertRow(row);
                mappings_->setItem(
                    row, 0,
                    new ::QTableWidgetItem(mapping.value(QStringLiteral("intent")).toString()));
                mappings_->setItem(
                    row, 1,
                    new ::QTableWidgetItem(mapping.value(QStringLiteral("hotkey_id")).toString()));
                mappings_->setItem(
                    row, 2,
                    new ::QTableWidgetItem(
                        mapping.value(QStringLiteral("fallback_hotkey_id")).toString()));
            }
            this->controls();
        }

        void set_model(const ::QString& id)
        {
            const auto index = model_->findData(id);
            if (index < 0)
            {
                model_->setEditText(id);
            }
            else
            {
                model_->setCurrentIndex(index);
            }
        }

        void query_models()
        {
            this->query(QStringLiteral("list_models"), {}, model_);
        }

        void query_hotkeys()
        {
            const auto id = this->model_id();
            if (id.isEmpty())
            {
                feedback_->setText(QStringLiteral("请先选择模型"));
                return;
            }
            this->query(QStringLiteral("list_hotkeys"), id, keys_);
        }

        void query(const ::QString& type, const ::QString& model_id, ::QComboBox* list)
        {
            const auto revision = this->begin();
            const bool models = list == model_;
            const auto selected = this->model_id();
            ::QJsonObject body{{QStringLiteral("type"), type}};
            if (!models)
            {
                body.insert(QStringLiteral("model_id"), model_id);
            }
            server_.post_json(
                QStringLiteral("/api/desktop/resources"), body, *this,
                [this, revision, models, selected, list](const ::QJsonDocument& document,
                                                         const ::QString& error)
                {
                    if (revision != revision_)
                    {
                        return;
                    }
                    busy_ = false;
                    const auto value = document.object();
                    auto message =
                        error.isEmpty() ? value.value(QStringLiteral("message")).toString() : error;
                    const auto entries =
                        value.value(models ? QStringLiteral("models") : QStringLiteral("hotkeys"));
                    if (message.isEmpty() && entries.isArray())
                    {
                        list->clear();
                        for (const auto& item : entries.toArray())
                        {
                            const auto entry = item.toObject();
                            const auto id = entry.value(QStringLiteral("id")).toString();
                            const auto name =
                                models ? entry.value(QStringLiteral("name")).toString() : id;
                            list->addItem(name, id);
                        }
                        if (models)
                        {
                            this->set_model(selected);
                        }
                    }
                    else if (message.isEmpty())
                    {
                        message = QStringLiteral("执行端返回了无效的资源列表");
                    }
                    feedback_->setText(message);
                    this->controls();
                });
        }

        meowlive::desktop::services::ServerClient& server_;
        ::QComboBox* profiles_{};
        ::QComboBox* model_{};
        ::QComboBox* voice_{};
        ::QComboBox* keys_{};
        ::QLineEdit* name_{};
        ::QLineEdit* mouth_{};
        ::QTableWidget* mappings_{};
        ::QPushButton* select_{};
        ::QPushButton* delete_{};
        ::QPushButton* preview_{};
        ::QLabel* feedback_{};
        ::QJsonObject snapshot_;
        unsigned revision_{};
        bool access_{};
        bool busy_{};
        bool loaded_{};
    };
}
