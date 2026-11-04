#ifndef CGDESIGN_H
#define CGDESIGN_H

#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>

/**
 * cgdesign — the pure half of the CG Designer (docs/cg-designer.md): no store, no network, so a unit test drives it.
 *
 *   * ids:        "cgd_<slug>" from a name, unique against the ids already taken;
 *   * the model:  the system prompt (template contract + broadcast design rules + output format), the user message
 *                 (prompt, refine page, example templates / pasted HTML / reference images as image_url parts), and the
 *                 parser for its reply (```json metadata + ```html page, with fallbacks);
 *   * the page:   normalizeHtml (frame, runtime scripts, data-cg-root, nothing loaded off the machine, editor leftovers
 *                 stripped) and deriveParams (data-field elements in document order, then :root custom properties).
 */
namespace cgdesign {

QString slugify(const QString& name);                                       // "Breaking News Bar!" -> "breaking_news_bar"
QString makeId(const QString& name, const QStringList& taken);              // "cgd_breaking_news_bar" (_2, _3 ... when taken)
bool validId(const QString& id);                                            // ^cgd_[a-z0-9_]{1,44}$

struct Reply {
    QJsonObject meta;        // {name, description, category, role, params[]} (may be empty)
    QString html;            // the page as the model wrote it
    QString error;           // non-empty: unusable
    bool truncated = false;  // the page has no </html> and no </body> (max_tokens hit)
};
/// Reads the model's answer: one ```json block and one ```html block; else a bare page (<!DOCTYPE / <html ... </html>)
/// with the first {...} before it as the metadata.
Reply parseModelReply(const QString& content);

/// The page made playable and safe to air (see docs/cg-designer.md "Normalisation"). Each change that removes something
/// or that the author should know about is appended to `warnings`.
QString normalizeHtml(const QString& html, QStringList* warnings = nullptr);

/// Manifest params read from the page: every data-field (document order; type text | textarea | image | number; label
/// from data-label, else `metaParams`, else the name made readable; default = the element's text / src), then every
/// :root custom property (type color when the value is a colour). A metaParams entry with no element survives only when
/// the page reads it with CGFields.get('<name>' ...).
QJsonArray deriveParams(const QString& html, const QJsonArray& metaParams = QJsonArray());
/// The first three text / textarea params (the Text 1/2/3 inputs).
QJsonArray textFields(const QJsonArray& params);

QString humanize(const QString& name);                                      // "anchorName" -> "Anchor name"
QStringList categories();                                                   // design categories the model may pick
QStringList roles();                                                        // overlay roles (layer bands)

/// The contract + design rules + output format; a non-empty `houseStyle` (settings cgdesigner.config houseStyle, at most
/// 4000 characters) is appended under a "Station house style" heading.
QString systemPrompt(const QString& houseStyle = QString());
struct Example {
    QString kind;            // template | html | image
    QString name;            // template id / a label
    QString html;            // template or pasted HTML
    QString dataUrl;         // image: data:image/...;base64,...
    QString note;
};
struct Request {
    QString prompt;
    QString mode = "new";    // new | refine
    QString baseHtml;        // refine: the current page
    QString kind;            // optional hint: lower third, bug, ticker, full screen ...
    QJsonObject brand;       // optional {colors, font, notes}
    QList<Example> examples;
    QString houseStyle;      // station house style for the system prompt
    int variation = 1, variations = 1;
};
/// The user turn: one text part (the brief, the examples' HTML, the refine page) and one image_url part per image.
QJsonArray userContent(const Request& r);
/// {model, temperature, max_tokens, messages:[system, user]} for chat/completions.
QJsonObject chatBody(const Request& r, const QString& model, double temperature, int maxTokens);
/// `body` with every image_url data URL replaced by "<image N: K bytes>" (the dry run / "View full prompt" answer).
QJsonObject redactImages(const QJsonObject& body);

} // namespace cgdesign

#endif // CGDESIGN_H
