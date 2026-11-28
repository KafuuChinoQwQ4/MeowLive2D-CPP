module;

#include <QComboBox>
#include <QJsonArray>
#include <QJsonObject>
#include <QSettings>
#include <QSignalBlocker>
#include <QString>
#include <QWidget>

export module meowlive.desktop.ui.transcription_choices;

export namespace meowlive::desktop::ui
{
    class TranscriptionChoices final : public ::QComboBox
    {
    public:
        TranscriptionChoices(const ::QString& preference, ::QWidget* parent)
            : ::QComboBox(parent), preference_(QStringLiteral("voice/") + preference),
              preferred_(::QSettings(QStringLiteral("MeowLive2D"), QStringLiteral("MeowLive2D"))
                             .value(preference_)
                             .toString())
        {
            this->connect(this, &::QComboBox::activated, this,
                          [this](int)
                          {
                              preferred_ = this->current_id();
                              ::QSettings(QStringLiteral("MeowLive2D"),
                                          QStringLiteral("MeowLive2D"))
                                  .setValue(preference_, preferred_);
                          });
        }

        bool update_catalog(const ::QJsonObject& catalog)
        {
            ::QJsonArray available;
            for (const auto& item : catalog.value(QStringLiteral("models")).toArray())
            {
                const auto model = item.toObject();
                if (model.value(QStringLiteral("purpose")) == QStringLiteral("transcription") &&
                    model.value(QStringLiteral("ready")).toBool())
                {
                    available.append(::QJsonObject{
                        {QStringLiteral("id"), model.value(QStringLiteral("id"))},
                        {QStringLiteral("revision"), model.value(QStringLiteral("revision"))},
                        {QStringLiteral("name"), model.value(QStringLiteral("name"))},
                        {QStringLiteral("languages"), model.value(QStringLiteral("languages"))}});
                }
            }
            if (catalog.value(QStringLiteral("legacy_transcription_configured")).toBool())
            {
                available.append(
                    ::QJsonObject{{QStringLiteral("id"), QStringLiteral("legacy")},
                                  {QStringLiteral("name"), QStringLiteral("已有 Whisper 环境")}});
            }
            if (available == available_)
            {
                return false;
            }
            available_ = available;
            const auto current = this->current_id();
            const ::QSignalBlocker blocked(this);
            this->clear();
            int remembered = -1;
            for (const auto& item : available)
            {
                const auto model = item.toObject();
                if (model.value(QStringLiteral("id")) ==
                    (preferred_.isEmpty() ? current : preferred_))
                {
                    remembered = this->count();
                }
                this->addItem(model.value(QStringLiteral("name")).toString(), model);
            }
            this->setCurrentIndex(remembered >= 0 ? remembered : (this->count() > 0 ? 0 : -1));
            return true;
        }

        void update_languages(::QComboBox& language, bool allow_manual = false) const
        {
            const auto selected = language.currentData().toString();
            const auto allowed =
                this->currentData().toJsonObject().value(QStringLiteral("languages")).toArray();
            const ::QSignalBlocker blocked(&language);
            language.clear();
            for (const auto* code : {"zh", "en", "ja", "ko", "yue"})
            {
                const auto id = ::QString::fromLatin1(code);
                if (allow_manual || allowed.isEmpty() || allowed.contains(id))
                {
                    language.addItem(meowlive::desktop::ui::TranscriptionChoices::language_name(id),
                                     id);
                }
            }
            const auto index = language.findData(selected);
            language.setCurrentIndex(index >= 0 ? index : 0);
        }

        bool supports(const ::QComboBox& language) const
        {
            const auto allowed =
                this->currentData().toJsonObject().value(QStringLiteral("languages")).toArray();
            return this->currentIndex() >= 0 &&
                   (allowed.isEmpty() || allowed.contains(language.currentData().toString()));
        }

        ::QJsonObject request(const ::QComboBox& language) const
        {
            ::QJsonObject result{{QStringLiteral("language"), language.currentData().toString()}};
            const auto revision =
                this->currentData().toJsonObject().value(QStringLiteral("revision"));
            if (revision.isString())
            {
                result.insert(QStringLiteral("asr_model_id"), this->current_id());
                result.insert(QStringLiteral("asr_model_revision"), revision);
            }
            return result;
        }

        static bool matches(const ::QJsonObject& request, const ::QJsonObject& transcript)
        {
            return transcript.value(QStringLiteral("language")) ==
                       request.value(QStringLiteral("language")) &&
                   transcript.value(QStringLiteral("asr_model_id")) ==
                       request.value(QStringLiteral("asr_model_id")) &&
                   transcript.value(QStringLiteral("asr_model_revision")) ==
                       request.value(QStringLiteral("asr_model_revision"));
        }

        static ::QString language_name(const ::QString& code)
        {
            if (code == QStringLiteral("zh"))
                return QStringLiteral("普通话");
            if (code == QStringLiteral("en"))
                return QStringLiteral("英语");
            if (code == QStringLiteral("ja"))
                return QStringLiteral("日语");
            if (code == QStringLiteral("ko"))
                return QStringLiteral("韩语");
            if (code == QStringLiteral("yue"))
                return QStringLiteral("粤语");
            return code;
        }

    private:
        ::QString current_id() const
        {
            return this->currentData().toJsonObject().value(QStringLiteral("id")).toString();
        }

        const ::QString preference_;
        ::QString preferred_;
        ::QJsonArray available_;
    };
}
