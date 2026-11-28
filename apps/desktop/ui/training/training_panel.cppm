module;

#include <QComboBox>
#include <QDateTime>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QString>
#include <QWidget>

#include <functional>
#include <utility>

export module meowlive.desktop.ui.training_panel;

import meowlive.desktop.services.server_client;
import meowlive.desktop.ui.transcription_choices;

export namespace meowlive::desktop::ui
{
    class TrainingPanel final : public ::QWidget
    {
    public:
        TrainingPanel(meowlive::desktop::services::ServerClient& server,
                      std::function<void()> open_models, ::QWidget* parent)
            : ::QWidget(parent), server_(server)
        {
            auto* layout = new ::QFormLayout(this);
            choose_ = new ::QPushButton(QStringLiteral("选择参考 WAV"), this);
            filename_ = new ::QLabel(this);
            model_ = new meowlive::desktop::ui::TranscriptionChoices(
                QStringLiteral("reference_model"), this);
            models_page_ = new ::QPushButton(QStringLiteral("前往可选模型"), this);
            language_ = new ::QComboBox(this);
            model_->update_languages(*language_);
            convert_ = new ::QPushButton(QStringLiteral("转写"), this);
            reference_ = new ::QLineEdit(this);
            reference_->setMaxLength(500);
            name_ = new ::QLineEdit(this);
            import_ = new ::QPushButton(QStringLiteral("导入音色"), this);
            feedback_ = new ::QLabel(this);
            feedback_->setWordWrap(true);

            layout->addRow(choose_, filename_);
            layout->addRow(QStringLiteral("转写模型"), model_);
            layout->addRow(models_page_);
            layout->addRow(QStringLiteral("语言"), language_);
            layout->addRow(convert_);
            layout->addRow(QStringLiteral("参考文本"), reference_);
            layout->addRow(QStringLiteral("音色名称"), name_);
            layout->addRow(import_);
            layout->addRow(feedback_);

            this->connect(choose_, &::QPushButton::clicked, this,
                          [this]
                          {
                              const auto selected = ::QFileDialog::getOpenFileName(
                                  this, QStringLiteral("选择参考录音"), {},
                                  QStringLiteral("WAV (*.wav)"));
                              if (!selected.isEmpty() && access_)
                              {
                                  file_ = selected;
                                  const ::QFileInfo info(file_);
                                  size_ = info.size();
                                  modified_ = info.lastModified();
                                  filename_->setText(info.fileName());
                                  this->invalidate();
                              }
                          });
            this->connect(models_page_, &::QPushButton::clicked, this, std::move(open_models));
            this->connect(model_, &::QComboBox::currentIndexChanged, this,
                          [this](int)
                          {
                              model_->update_languages(*language_);
                          });
            this->connect(reference_, &::QLineEdit::textChanged, this,
                          [this](const ::QString&)
                          {
                              this->controls();
                          });
            this->connect(name_, &::QLineEdit::textChanged, this,
                          [this](const ::QString&)
                          {
                              this->controls();
                          });
            this->connect(convert_, &::QPushButton::clicked, this,
                          [this]
                          {
                              this->transcribe();
                          });
            this->connect(import_, &::QPushButton::clicked, this,
                          [this]
                          {
                              this->import_voice();
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
            if (!access_)
            {
                file_.clear();
                filename_->clear();
                name_->clear();
            }
            this->invalidate();
        }

        void update_voice_models(const ::QJsonObject& catalog)
        {
            if (model_->update_catalog(catalog))
            {
                model_->update_languages(*language_);
            }
            this->controls();
        }

    private:
        bool check_file()
        {
            const ::QFileInfo info(file_);
            if (!info.isFile() || info.size() != size_ || info.lastModified() != modified_)
            {
                this->invalidate();
                feedback_->setText(QStringLiteral("参考录音已更换，请重新选择"));
                return false;
            }
            return true;
        }

        void invalidate()
        {
            ++revision_;
            busy_ = false;
            reference_->clear();
            feedback_->clear();
            this->controls();
        }

        void controls()
        {
            choose_->setEnabled(access_ && !busy_);
            model_->setEnabled(access_ && !busy_ && model_->count() > 0);
            models_page_->setVisible(access_ && model_->count() == 0);
            language_->setEnabled(access_ && !busy_);
            reference_->setEnabled(access_ && !busy_);
            name_->setEnabled(access_);
            convert_->setEnabled(access_ && !busy_ && model_->currentIndex() >= 0 &&
                                 !file_.isEmpty());
            import_->setEnabled(access_ && !busy_ && !file_.isEmpty() &&
                                !reference_->text().trimmed().isEmpty() &&
                                !name_->text().trimmed().isEmpty());
        }

        void transcribe()
        {
            if (!this->check_file())
            {
                return;
            }
            const auto metadata = model_->request(*language_);
            const auto revision = ++revision_;
            busy_ = true;
            feedback_->setText(QStringLiteral("正在转写…"));
            this->controls();
            server_.transcribe(
                metadata, file_, *this,
                [this, revision, metadata](const ::QJsonDocument& document, const ::QString& error)
                {
                    if (revision != revision_ || !this->check_file())
                    {
                        return;
                    }
                    busy_ = false;
                    const auto data = document.object();
                    const auto text = data.value(QStringLiteral("text")).toString();
                    if (!error.isEmpty())
                    {
                        feedback_->setText(error);
                    }
                    else if (text.isEmpty() ||
                             !meowlive::desktop::ui::TranscriptionChoices::matches(metadata, data))
                    {
                        feedback_->setText(QStringLiteral("转写结果无效"));
                    }
                    else
                    {
                        reference_->setText(text);
                        feedback_->setText(QStringLiteral("请校对参考文本，确认后导入音色"));
                    }
                    this->controls();
                });
        }

        void import_voice()
        {
            if (!this->check_file())
            {
                return;
            }
            const auto revision = ++revision_;
            busy_ = true;
            feedback_->setText(QStringLiteral("正在导入音色…"));
            this->controls();
            server_.import_voice({{QStringLiteral("name"), name_->text().trimmed()},
                                  {QStringLiteral("language"), language_->currentData().toString()},
                                  {QStringLiteral("reference_text"), reference_->text().trimmed()}},
                                 file_, *this,
                                 [this, revision](const ::QJsonDocument&, const ::QString& error)
                                 {
                                     if (revision != revision_)
                                     {
                                         return;
                                     }
                                     busy_ = false;
                                     feedback_->setText(
                                         error.isEmpty() ? QStringLiteral("已导入音色") : error);
                                     this->controls();
                                 });
        }

        meowlive::desktop::services::ServerClient& server_;
        ::QPushButton* choose_{};
        ::QPushButton* convert_{};
        ::QPushButton* import_{};
        ::QPushButton* models_page_{};
        meowlive::desktop::ui::TranscriptionChoices* model_{};
        ::QComboBox* language_{};
        ::QLabel* filename_{};
        ::QLabel* feedback_{};
        ::QLineEdit* reference_{};
        ::QLineEdit* name_{};
        ::QString file_;
        ::QDateTime modified_;
        ::qint64 size_{};
        unsigned revision_{};
        bool access_{};
        bool busy_{};
    };
}
