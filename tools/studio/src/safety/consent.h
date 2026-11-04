#pragma once
// Consent and public-figure rules for voice cloning (ST-15). Port of StarMedia mcp/voice/consent.ts, plus the use-time
// re-check of mcp/tools/voice.ts (assertUsable) that the Studio runs for EVERY cloned-speech request, immediately before
// it is sent (guards.h checkOutgoing calls checkCloneUse).
//
// A voice is cloned only with a CONSENT RECORD: who the speaker is (speaker_name), what they agreed to in their own words
// (statement), who gave the consent (given_by: the speaker, or someone legally authorised for them) and the date. The
// Studio stores it with the profile (voices.consent_json) and checks it again before every use: a hand-edited row does
// not get through.
//
// PUBLIC FIGURES are never cloned. The check is a deliberately conservative heuristic (no lookup service): names are
// refused when they contain a name of the shared lists (realpeople.h; any case), an office or celebrity title, or ask
// for an imitation; free text (consent statement, transcript, the text to speak) is refused for imitation requests and
// full famous names. False positives (a private person who shares a famous name) are refused too: the intended
// trade-off. The lists unavailable = refused (fail closed).
//
// Stricter than the TS where the Studio's rules ask for it: a consent can be recorded only by a person through the API /
// UI with an explicit confirmation, so a record without confirmed_by_user is refused at use (the TS asks the person at
// that point; the Studio has no one to ask), and a call from an agent is always refused. A date is valid only as a real
// calendar date (the TS let "2026-13-01" roll over to January).
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <optional>

namespace studio::safety {

/// What a person typed: any field may be missing.
struct ConsentInput {
    QString speakerName, statement, givenBy, date;   // date: YYYY-MM-DD
    static ConsentInput fromJson(const QJsonObject& o);   // speaker_name, statement, given_by, date
};

/// The record stored with a voice profile (voices.consent_json). Fields as in the TS ConsentRecord.
struct ConsentRecord {
    QString speakerName, statement, givenBy, date;
    QString recordedAt;                    // ISO, when the Studio stored it
    bool confirmedByUser = false;          // a person confirmed it (API / UI, explicit confirmation)
    QString confirmedAt;
    QString session;                       // who / which session recorded it
    bool selfGiven = false;                // the speaker gave it themselves (else an authorised representative)
    QJsonObject toJson() const;            // snake_case keys like the TS
    static ConsentRecord fromJson(const QJsonObject& o);
};

/// A refusal: `code` is the TS `refused` value (no_consent | incomplete_consent | weak_consent |
/// consent_not_from_speaker | public_figure | unconfirmed | agent), `message` is said to the caller.
struct ConsentRefusal {
    QString code;
    QString message;
    QStringList missing;                   // incomplete_consent: the missing fields
};

/// Validates a consent input (consent.ts checkConsent). On success fills `out` (recordedAt / confirmed / session left
/// empty) and returns nullopt; else the refusal. `nowMs` is the epoch ms "now" (a date over 36 h ahead is refused).
std::optional<ConsentRefusal> checkConsent(const std::optional<ConsentInput>& input, qint64 nowMs, ConsentRecord* out = nullptr);

/// The public-figure reason for refusing, or empty when the texts pass the heuristic (consent.ts publicFigureReason).
/// `names` (speaker name, profile name) are checked against the lists, titles and imitation requests; `freeText`
/// (consent statement, transcript, text to speak) only for imitation requests and full famous names.
QString publicFigureReason(const QStringList& names, const QStringList& freeText = {});

/// One cloned-speech request about to be sent: the profile as stored and what is about to be spoken.
struct CloneUse {
    QString profileName;
    QJsonObject consent;                   // voices.consent_json as stored (may be {} or hand-edited)
    QString transcript;                    // the sample's transcript, if any
    QString text;                          // the text to speak
    bool fromAgent = false;                // the request comes from an agent (never allowed)
};

/// The send-time re-check (voice.ts assertUsable + speak_cloned): agent → refused; the stored record re-validated
/// (checkConsent); not confirmed by a person → refused; public figure / title / imitation in the profile name, speaker,
/// statement, transcript or the text → refused. nullopt = may be sent.
std::optional<ConsentRefusal> checkCloneUse(const CloneUse& use, qint64 nowMs);

/// consent.ts REQUIRED: what a complete consent record needs (said with every no / incomplete consent refusal).
QString consentRequirements();

} // namespace studio::safety
