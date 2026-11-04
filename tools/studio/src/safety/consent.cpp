#include "safety/consent.h"

#include "core/datafiles.h"
#include "safety/realpeople.h"

#include <QDate>
#include <QDateTime>
#include <QRegularExpression>
#include <QTimeZone>

namespace studio::safety {
namespace {

// consent.ts patterns, compiled with their JS meaning. A pattern that fails to compile never matches, so each use below
// checks isValid() and refuses (fail closed) rather than passing a text it could not read.
struct Patterns {
    QRegularExpression titles, imitation, consentWords, voiceWords, proxyWords, date, selfWords;
    bool ok = true;
    Patterns() {
        auto c = [this](QRegularExpression& re, QStringView src, QStringView flags) {
            re = data::regex(src, flags);
            ok = ok && re.isValid();
        };
        c(titles, u"\\b(president|vice president|senator|congress(wo)?man|congressperson|representative|governor|mayor|prime minister|premier|"
                  u"chancellor|minister|secretary of state|king|queen|prince|princess|duke|duchess|emperor|pope|cardinal|ayatollah|sheikh|sultan|"
                  u"dictator|celebrity|celebrities|famous|movie star|film star|pop star|rock star|rapper|news anchor|anchorman|anchorwoman|"
                  u"talk show host|influencer)\\b", u"");
        c(imitation, u"\\b(sounds? like|impersonat\\w*|imitat\\w*|in the voice of|voice of (a|the) (famous|celebrity|president)|deepfake|parody of)\\b", u"");
        c(consentWords, u"\\b(consent\\w*|permission|agree\\w*|authori[sz]\\w*|allow\\w*|approve\\w*)\\b", u"i");
        c(voiceWords, u"\\b(voice|clon\\w*|synthe\\w*|speech|narrat\\w*|speak\\w*)\\b", u"i");
        c(proxyWords, u"\\b(on (his|her|their|my child'?s?) behalf|on behalf of|guardian|parent|legal(ly)?|authori[sz]ed|representative|"
                      u"power of attorney|estate|executor|agent)\\b", u"i");
        c(date, u"^(\\d{4})-(\\d{2})-(\\d{2})$", u"");
        c(selfWords, u"^(the speaker|speaker|myself|me|self)$", u"i");
    }
};
const Patterns& P() {
    static const Patterns p;
    return p;
}

ConsentRefusal refusal(const QString& code, const QString& message, QStringList missing = {}) {
    return ConsentRefusal{code, message, std::move(missing)};
}

QString str(const QJsonObject& o, QStringView key) {
    const QJsonValue v = o.value(key);
    return v.isString() ? v.toString() : QString();
}

} // namespace

QString consentRequirements() {
    return QStringLiteral("Voice cloning needs a consent record from the person whose voice it is: consent: { speaker_name (the person in the sample), "
                          "statement (their consent in their own words, e.g. \"I, Jane Doe, agree to StarMedia cloning my voice for narration\"), "
                          "given_by (who gave it: the speaker, or a legally authorised representative), date (YYYY-MM-DD) }. Ask the user for these; "
                          "never fill them in yourself.");
}

ConsentInput ConsentInput::fromJson(const QJsonObject& o) {
    return ConsentInput{str(o, u"speaker_name"), str(o, u"statement"), str(o, u"given_by"), str(o, u"date")};
}

QJsonObject ConsentRecord::toJson() const {
    QJsonObject o{{QStringLiteral("speaker_name"), speakerName}, {QStringLiteral("statement"), statement},
                  {QStringLiteral("given_by"), givenBy},         {QStringLiteral("date"), date},
                  {QStringLiteral("recorded_at"), recordedAt},   {QStringLiteral("confirmed_by_user"), confirmedByUser},
                  {QStringLiteral("session"), session},          {QStringLiteral("self_given"), selfGiven}};
    if (!confirmedAt.isEmpty()) o.insert(QStringLiteral("confirmed_at"), confirmedAt);
    return o;
}

ConsentRecord ConsentRecord::fromJson(const QJsonObject& o) {
    ConsentRecord r;
    r.speakerName = str(o, u"speaker_name");
    r.statement = str(o, u"statement");
    r.givenBy = str(o, u"given_by");
    r.date = str(o, u"date");
    r.recordedAt = str(o, u"recorded_at");
    r.confirmedByUser = o.value(u"confirmed_by_user").toBool(false);   // only a JSON true counts
    r.confirmedAt = str(o, u"confirmed_at");
    r.session = str(o, u"session");
    r.selfGiven = o.value(u"self_given").toBool(false);
    return r;
}

std::optional<ConsentRefusal> checkConsent(const std::optional<ConsentInput>& input, qint64 nowMs, ConsentRecord* out) {
    const Patterns& p = P();
    if (!p.ok) return refusal(QStringLiteral("incomplete_consent"), QStringLiteral("Refused: the consent rules could not be loaded; nothing is cloned."));
    if (!input) return refusal(QStringLiteral("no_consent"), QStringLiteral("Refused: no consent record. ") + consentRequirements());
    const QString speaker = input->speakerName.trimmed(), statement = input->statement.trimmed(), givenBy = input->givenBy.trimmed(),
                  date = input->date.trimmed();
    QStringList missing;
    if (speaker.isEmpty()) missing << QStringLiteral("speaker_name");
    if (statement.isEmpty()) missing << QStringLiteral("statement");
    if (givenBy.isEmpty()) missing << QStringLiteral("given_by");
    if (date.isEmpty()) missing << QStringLiteral("date");
    if (!missing.isEmpty())
        return refusal(QStringLiteral("incomplete_consent"),
                       QStringLiteral("Refused: the consent record is missing %1. ").arg(missing.join(QStringLiteral(", "))) + consentRequirements(), missing);
    if (normName(speaker).size() < 2)
        return refusal(QStringLiteral("incomplete_consent"), QStringLiteral("Refused: speaker_name must be the speaker's real name."));
    if (statement.size() < 20 || !p.consentWords.match(statement).hasMatch() || !p.voiceWords.match(statement).hasMatch())
        return refusal(QStringLiteral("weak_consent"),
                       QStringLiteral("Refused: the consent statement must say, in the speaker's words, that they agree to their voice being cloned / "
                                      "synthesised (at least a full sentence, e.g. \"I agree to my voice being cloned for StarMedia narration\")."));
    const auto m = p.date.match(date);
    const QDate d = m.hasMatch() ? QDate(m.captured(1).toInt(), m.captured(2).toInt(), m.captured(3).toInt()) : QDate();
    if (!d.isValid())
        return refusal(QStringLiteral("incomplete_consent"), QStringLiteral("Refused: consent.date must be the date the consent was given, as YYYY-MM-DD."));
    const qint64 dMs = QDateTime(d, QTime(0, 0), QTimeZone::UTC).toMSecsSinceEpoch();
    if (dMs > nowMs + 36LL * 3600 * 1000)
        return refusal(QStringLiteral("incomplete_consent"), QStringLiteral("Refused: consent.date is in the future."));
    if (d.year() < 2000)
        return refusal(QStringLiteral("incomplete_consent"), QStringLiteral("Refused: consent.date is implausibly old; record the consent again."));
    const bool selfGiven = normName(givenBy) == normName(speaker) || p.selfWords.match(givenBy).hasMatch();
    if (!selfGiven && !p.proxyWords.match(statement + QLatin1Char(' ') + givenBy).hasMatch())
        return refusal(QStringLiteral("consent_not_from_speaker"),
                       QStringLiteral("Refused: the consent was given by \"%1\", not by the speaker \"%2\". Consent must come from the speaker, or from "
                                      "someone legally authorised to act for them (say so in the statement or given_by, e.g. \"parent and legal "
                                      "guardian of …\").").arg(givenBy, speaker));
    if (out) {
        out->speakerName = speaker;
        out->statement = statement;
        out->givenBy = givenBy;
        out->date = date;
        out->selfGiven = selfGiven;
    }
    return std::nullopt;
}

QString publicFigureReason(const QStringList& names, const QStringList& freeText) {
    const Patterns& p = P();
    if (!p.ok) return QStringLiteral("the voice rules could not be loaded");
    for (const QString& t : names) {
        if (t.isEmpty()) continue;
        const QString n = QLatin1Char(' ') + normName(t) + QLatin1Char(' ');
        const QString who = publicFigureInName(t);
        if (!who.isEmpty()) return QStringLiteral("\"%1\" matches a public figure (%2)").arg(t.left(80), who);
        const auto title = p.titles.match(n);
        if (title.hasMatch()) return QStringLiteral("\"%1\" names an office or celebrity title (\"%2\")").arg(t.left(80), title.captured(1));
        const auto im = p.imitation.match(n);
        if (im.hasMatch()) return QStringLiteral("\"%1\" asks for an imitation (\"%2\")").arg(t.left(80), im.captured(1));
    }
    for (const QString& t : freeText) {
        if (t.isEmpty()) continue;
        const QString n = QLatin1Char(' ') + normName(t) + QLatin1Char(' ');
        const auto im = p.imitation.match(n);
        if (im.hasMatch()) return QStringLiteral("the text asks for an imitation (\"%1\")").arg(im.captured(1));
        const QString full = findPublicFigure(t, true);
        if (!full.isEmpty()) return QStringLiteral("the text names a public figure (%1)").arg(full);
    }
    return {};
}

std::optional<ConsentRefusal> checkCloneUse(const CloneUse& use, qint64 nowMs) {
    if (use.fromAgent)
        return refusal(QStringLiteral("agent"), QStringLiteral("Refused: an agent cannot clone a voice or speak in a cloned one. Voice cloning needs a "
                                                               "person to confirm the speaker's consent; ask the user to do it themselves."));
    std::optional<ConsentInput> in;
    if (!use.consent.isEmpty()) in = ConsentInput::fromJson(use.consent);
    if (auto r = checkConsent(in, nowMs)) return r;
    const ConsentRecord rec = ConsentRecord::fromJson(use.consent);
    if (!rec.confirmedByUser)
        return refusal(QStringLiteral("unconfirmed"),
                       QStringLiteral("Refused: the consent for voice \"%1\" (%2) was not confirmed by a person. Confirm it in the Studio (Voices) first.")
                           .arg(use.profileName.left(80), rec.speakerName.left(80)));
    const QString pf = publicFigureReason({use.profileName, rec.speakerName}, {rec.statement, use.transcript, use.text});
    if (!pf.isEmpty())
        return refusal(QStringLiteral("public_figure"), QStringLiteral("Refused: %1. Public figures' voices are never cloned.").arg(pf));
    return std::nullopt;
}

} // namespace studio::safety
