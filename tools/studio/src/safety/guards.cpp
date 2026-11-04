#include "safety/guards.h"

#include "core/clock.h"
#include "core/datafiles.h"
#include "core/log.h"
#include "safety/realpeople.h"

#include <QJsonArray>
#include <QRegularExpression>
#include <QUrl>

namespace studio::safety {
namespace {

// spot/safety.ts patterns, compiled with their JS meaning (fail closed when one does not compile).
struct SpotPatterns {
    QRegularExpression deepfake, official, realPerson, celebPhoto;
    QRegularExpression negatedPeople, avoidClause, peoplePhrase;   // the hook's negation neutraliser (not in the TS)
    bool ok = true;
    SpotPatterns() {
        auto c = [this](QRegularExpression& re, QStringView src) {
            re = data::regex(src, u"i");
            ok = ok && re.isValid();
        };
        c(deepfake, u"\\b(deep\\s?fakes?|face\\s?swap|look-?alikes?|lookalikes?|impersonat\\w*|doppelg[aä]nger)\\b");
        // depicting a real office-holder by title ("photo of the president", "the governor shaking hands")
        c(official, u"\\b(photo|picture|image|portrait|footage|video|render|depict\\w*|showing)\\s+(of\\s+)?(the\\s+)?(current\\s+|former\\s+|real\\s+)?"
                    u"(us\\s+|u\\.s\\.\\s+)?(president|vice president|prime minister|chancellor|senator|congress(wo)?man|governor|pope|first lady)\\b");
        c(realPerson, u"\\b(real|actual|identifiable|recogni[sz]able)\\s+(person|people|celebrit\\w+|politicians?|athletes?|face)\\b");
        c(celebPhoto, u"\\b(photo-?real\\w*|realistic|lifelike|hyper-?real\\w*)\\b[^.]{0,60}\\b(celebrit\\w+|famous (person|people|actor|actress|singer|athlete|politician))\\b"
                      u"|\\b(celebrit\\w+|famous (person|people|actor|actress|singer|athlete|politician))\\b[^.]{0,60}\\b(photo-?real\\w*|realistic|lifelike)\\b");
        // "identifiable real people" / "real or identifiable faces": the adjectives the wording rules key on, then the noun ($1)
#define STUDIO_PEOPLE_PHRASE u"(?:(?:real(?:-life)?|actual|identifiable|recogni[sz]able)\\s+(?:(?:or|and)\\s+)?){1,3}" \
                             u"(persons?|people|faces?|celebrit\\w+|politicians?|athletes?|individuals?)\\b"
        c(peoplePhrase, u"\\b" STUDIO_PEOPLE_PHRASE);
        // a negation right before it, at most two plain words between and no punctuation ("no identifiable people",
        // "do not draw identifiable real people"); "not only", "but" etc. do not negate
        c(negatedPeople, u"\\b(no|not|never|without|avoid(?:s|ing)?)\\b((?:\\s+(?!only\\b|just\\b|but\\b|merely\\b|also\\b)[\\w'-]+){0,2}?)\\s+"
                         STUDIO_PEOPLE_PHRASE);
#undef STUDIO_PEOPLE_PHRASE
        // the composed shot prompt's "Avoid: a, b, c." list (plan.ts imagePrompt, tools/image.ts, tools/video.ts), to the
        // end of that sentence or line
        c(avoidClause, u"\\bavoid\\s*:([^.\\n]*)");
    }
};
const SpotPatterns& SP() {
    static const SpotPatterns p;
    return p;
}

/// safety.ts plain(): NFKC, invisible format characters removed.
QString plain(QStringView s) {
    const QString n = s.toString().normalized(QString::NormalizationForm_KC);
    QString out;
    out.reserve(n.size());
    for (char32_t c : n.toUcs4())
        if (QChar::category(c) != QChar::Other_Format) out += QString::fromUcs4(&c, 1);
    return out;
}

std::optional<SafetyHit> wordingProblem(const QString& t) {
    const SpotPatterns& p = SP();
    if (auto m = p.deepfake.match(t); m.hasMatch())
        return SafetyHit{QStringLiteral("asks for a deepfake / look-alike (\"%1\")").arg(m.captured(1)), m.captured(1)};
    if (auto m = p.official.match(t); m.hasMatch()) {
        const QString s = m.captured(0).trimmed();
        return SafetyHit{QStringLiteral("asks to depict a real office-holder (\"%1\")").arg(s), s};
    }
    if (auto m = p.celebPhoto.match(t); m.hasMatch()) {
        const QString s = m.captured(0).trimmed();
        return SafetyHit{QStringLiteral("asks for a realistic image of a celebrity (\"%1\")").arg(s.left(60)), s};
    }
    if (auto m = p.realPerson.match(t); m.hasMatch()) {
        const QString s = m.captured(0).trimmed();
        return SafetyHit{QStringLiteral("asks for a depiction of a real, identifiable person (\"%1\")").arg(s), s};
    }
    return std::nullopt;
}

bool wantsOutput(const QJsonObject& body, QStringView kind) {
    for (const QJsonValue& m : body.value(u"modalities").toArray())
        if (m.toString().compare(kind, Qt::CaseInsensitive) == 0) return true;
    return false;
}

bool hasAudioReference(const QJsonObject& body) {
    for (const QJsonValue& r : body.value(u"input_references").toArray()) {
        const QJsonObject o = r.toObject();
        if (o.value(u"type").toString() == QLatin1String("input_audio") || o.contains(u"input_audio") || o.contains(u"audio_url")) return true;
    }
    return false;
}

bool asksImageTool(const QJsonObject& body) {
    for (const QJsonValue& t : body.value(u"tools").toArray())
        if (t.toObject().value(u"type").toString().compare(u"image_generation", Qt::CaseInsensitive) == 0) return true;
    return false;
}

/// Chat content (a string or {text} parts) and /responses input items ({role, content: [{type: "input_text", text}]}).
void addTextParts(const QJsonValue& content, QStringList& out, int depth = 0) {
    if (content.isString()) { out << content.toString(); return; }
    if (depth > 4) return;
    for (const QJsonValue& part : content.toArray()) {
        if (part.isString()) { out << part.toString(); continue; }
        const QJsonObject o = part.toObject();
        if (o.value(u"text").isString()) out << o.value(u"text").toString();
        if (o.contains(u"content")) addTextParts(o.value(u"content"), out, depth + 1);
    }
}

Verdict unavailable(const QString& why) {
    Verdict v;
    v.refuse = true;
    v.code = QStringLiteral("safety_data_unavailable");
    v.reason = QStringLiteral("Refused: the real-people safety lists are unavailable (%1); no picture, video or cloned voice can be made until "
                              "the Studio installation is repaired. Nothing was sent.").arg(why);
    v.names = {kUnavailable.toString()};
    return v;
}

} // namespace

std::optional<SafetyHit> realPeopleProblem(QStringView text) {
    const QString raw = text.toString();
    if (!SP().ok) return SafetyHit{QStringLiteral("cannot be checked (the spot safety rules did not load)"), QString()};
    QString who = findPublicFigure(raw);
    if (who.isEmpty()) who = findPublicFigure(plain(raw));
    if (!who.isEmpty()) return SafetyHit{QStringLiteral("names a real public figure (\"%1\")").arg(who), who};
    const QString t = plain(raw);
    if (auto h = wordingProblem(t)) return h;
    // look-alike letters ("dееpfake" with Cyrillic е) read as the word they imitate
    const QString f = foldText(t);
    if (f != t) return wordingProblem(f);
    return std::nullopt;
}

QString realPeoplePolicy() {
    return QStringLiteral("StarMedia does not generate AI images or video of real, identifiable people (public figures, officials, celebrities, "
                          "named private individuals), photoreal or otherwise, and does not make look-alikes or deepfakes.");
}

QString refusal(const SafetyHit& hit, QStringView what) {
    return QStringLiteral("Refused: %1 %2. %3 Rewrite the brief with invented characters (e.g. \"a friendly barista in her 30s\"), or use your own "
                          "licensed photos of the person with images: \"provided\" (and make sure you have their consent and the rights).")
        .arg(what.toString(), hit.reason, realPeoplePolicy());
}

QString inventedPeopleClause() {
    return QStringLiteral("Any people shown are invented, non-identifiable characters, not real or famous persons.");
}

QString withInventedPeople(const QString& prompt) {
    const QString c = inventedPeopleClause();
    if (prompt.contains(c)) return prompt;
    const QString p = prompt.trimmed();
    return p.isEmpty() ? c : p + QLatin1Char(' ') + c;
}

Verdict checkVisual(QStringView prompt, QStringView style, VisualRule rule) {
    QString why;
    if (!listsReady(&why)) return unavailable(why);
    Verdict v;
    if (rule == VisualRule::Spot) {
        const QString both = prompt.toString() + QLatin1Char(' ') + style.toString();
        if (const auto hit = realPeopleProblem(both)) {
            v.refuse = true;
            v.code = QStringLiteral("real_person");
            v.reason = refusal(*hit, u"the shot");
            v.names = {hit->match};
            return v;
        }
    }
    const RealPersonVerdict r = realPersonCheck(prompt, style);
    if (r.refuse) {
        v.refuse = true;
        v.code = QStringLiteral("real_person");
        v.reason = r.reason;
        v.names = r.names;
    }
    return v;
}

RequestKind classify(QStringView path, const QJsonObject& body) {
    // "/images", "/api/v1/videos", "https://openrouter.ai/api/v1/images?x=1": compare the path's tail only
    QString p = QUrl(path.toString()).path().toLower();
    if (p.isEmpty()) p = path.toString().toLower();
    while (p.endsWith(QLatin1Char('/'))) p.chop(1);
    if (p.endsWith(u"/images") || p.endsWith(u"/images/generations") || p.endsWith(u"/images/edits") || p.endsWith(u"/images/variations"))
        return RequestKind::Image;
    if (p.endsWith(u"/videos")) return RequestKind::Video;   // the submit; polls (/videos/{id}) and content carry no prompt
    if (p.endsWith(u"/chat/completions") || p.endsWith(u"/responses")) {
        if (wantsOutput(body, u"video")) return RequestKind::Video;
        if (wantsOutput(body, u"image") || asksImageTool(body)) return RequestKind::Image;
        return RequestKind::Other;
    }
    if (p.contains(u"/audio/") && hasAudioReference(body)) return RequestKind::CloneSpeech;
    return RequestKind::Other;
}

QString promptText(const QJsonObject& body) {
    QStringList parts;
    // "negative_prompt" is left out on purpose: it lists what NOT to draw (model-prompts.json moves the style's Avoid
    // list there for some families), so "identifiable real people" in it is a ban, not a request.
    if (body.value(u"prompt").isString()) parts << body.value(u"prompt").toString();
    if (body.value(u"instructions").isString()) parts << body.value(u"instructions").toString();   // /responses
    for (const QJsonValue& m : body.value(u"messages").toArray()) addTextParts(m.toObject().value(u"content"), parts);
    addTextParts(body.value(u"input"), parts);   // speech "input" string, /responses input arrays
    return parts.join(QLatin1Char('\n'));
}

QString neutraliseNegatedPeople(QStringView text) {
    const SpotPatterns& p = SP();
    QString t = text.toString();
    if (!p.ok) return t;   // the wording rules refuse everything then anyway
    // inside "Avoid: …" every listed item is a ban: drop the real/identifiable adjectives from its people phrases
    QString out;
    qsizetype at = 0;
    for (auto it = p.avoidClause.globalMatch(t); it.hasNext();) {
        const QRegularExpressionMatch m = it.next();
        out += QStringView(t).mid(at, m.capturedStart(1) - at);
        out += m.captured(1).replace(p.peoplePhrase, QStringLiteral("\\1"));
        at = m.capturedEnd(1);
    }
    out += QStringView(t).mid(at);
    // "no identifiable people", "do not draw identifiable real people"
    return out.replace(p.negatedPeople, QStringLiteral("\\1\\2 \\3"));
}

Verdict checkOutgoing(QStringView path, const QJsonObject& body, const SendContext& ctx) {
    const RequestKind kind = classify(path, body);
    Verdict v;
    switch (kind) {
    case RequestKind::Other: return v;
    case RequestKind::Image:
    case RequestKind::Video:
        // the composed prompt carries the style's own bans ("Style: … no identifiable people", "Avoid: identifiable real
        // people"), which the TS never checked (it checks the brief and scene); neutralise those before the wording rules.
        // Names are untouched, so a public figure is still refused wherever it appears outside negative_prompt.
        v = checkVisual(neutraliseNegatedPeople(promptText(body)), neutraliseNegatedPeople(ctx.style), ctx.rule);
        break;
    case RequestKind::CloneSpeech: {
        QString why;
        if (!listsReady(&why)) { v = unavailable(why); break; }
        if (!ctx.voice) {
            v.refuse = true;
            v.code = QStringLiteral("voice_consent:no_consent");
            v.reason = QStringLiteral("Refused: a cloned-voice request without its voice profile; the consent cannot be re-checked. ") + consentRequirements();
            break;
        }
        CloneUse use = *ctx.voice;
        // the body's text is what the model speaks: always read it, also when the caller passed its own copy
        const QString sent = promptText(body);
        if (!sent.isEmpty()) use.text = use.text.isEmpty() ? sent : use.text + QLatin1Char('\n') + sent;
        // the transcript sent with the sample is read too (it can name the speaker)
        for (const QJsonValue& r : body.value(u"input_references").toArray())
            if (r.toObject().value(u"type").toString() == QLatin1String("text")) use.transcript += QLatin1Char(' ') + r.toObject().value(u"text").toString();
        const qint64 now = ctx.nowMs >= 0 ? ctx.nowMs : Clock::system().nowMs();
        if (const auto r = checkCloneUse(use, now)) {
            v.refuse = true;
            v.code = QStringLiteral("voice_consent:") + r->code;
            v.reason = r->message;
        }
        break;
    }
    }
    if (v.refuse) {
        const char* k = kind == RequestKind::Image ? "image" : kind == RequestKind::Video ? "video" : "cloned speech";
        log::warn(u"safety", QStringLiteral("blocked before send: %1 %2 (%3, rule %4): %5")
                                 .arg(QLatin1String(k), log::urlForLog(QUrl(path.toString())), v.code,
                                      ctx.rule == VisualRule::Spot ? QStringLiteral("spot") : QStringLiteral("picture"), v.reason.left(300)));
    }
    return v;
}

} // namespace studio::safety
