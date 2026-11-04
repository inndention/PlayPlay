#ifndef CGTEXT_H
#define CGTEXT_H

#include <QJsonObject>
#include <QJsonArray>
#include <QJsonValue>
#include <QSet>
#include <QString>
#include <QStringList>

/**
 * CG text convention: every template can be driven by three generic text inputs, text1 / text2 / text3.
 *
 * textN maps onto the template's N-th "text field". A template names them itself with a `textFields` array of param names in
 * web/cg/templates.json; without one, the first three text / textarea params that are not a logo, icon, image or similar asset
 * reference are used (lower thirds: name, title, ...; tickers: text, label; score bugs have none, so they keep their own fields).
 * An explicit param of the real name always wins over textN, so an old caller that sends `name` is never overridden.
 * The textN keys themselves stay in the params, so a template page may also read `text1` directly.
 *
 * Header only: shared by the overlay manager (deploy / update / presets), the legacy /api/v1/cg/html route and the manifest
 * (which adds `textFields` to each template so the native app and the web dashboard can label the three inputs).
 */
namespace cgtext {

inline QStringList textFieldNames(const QJsonObject& tpl) {
    QStringList out;
    for (const QJsonValue& v : tpl.value("textFields").toArray()) if (v.isString() && !v.toString().isEmpty()) out << v.toString();
    if (!out.isEmpty() || tpl.contains("textFields")) return out.mid(0, 3);
    if (tpl.value("id").toString() == "karaoke") return out;       // captions come from the transcript of the item on air
    static const QSet<QString> asset = {"logo", "icon", "avatar", "image", "background", "position", "align", "url", "src", "media", "demo", "autoplay", "interval", "target", "showtimer"};
    for (const QJsonValue& pv : tpl.value("params").toArray()) {
        const QJsonObject p = pv.toObject();
        const QString type = p.value("type").toString("text"), name = p.value("name").toString();
        if (name.isEmpty() || (type != "text" && type != "textarea")) continue;
        const QString low = name.toLower();
        if (asset.contains(low) || low.endsWith("color") || low.endsWith("logo") || low.endsWith("image") || low.endsWith("icon")) continue;
        out << name;
        if (out.size() == 3) break;
    }
    return out;
}

inline QString textKey(int n) { return QStringLiteral("text") + QString::number(n); }

/// Fills the template's own fields from text1..3 (only where the real field is not given). Returns `params` with the additions.
inline QJsonObject applyText(const QJsonObject& tpl, QJsonObject params) {
    const QStringList f = textFieldNames(tpl);
    for (int i = 0; i < f.size(); ++i) {
        const QString k = textKey(i + 1);
        if (!params.contains(k)) continue;
        const QJsonValue v = params.value(k);
        if (v.isString() && v.toString().isEmpty()) continue;       // a blank input leaves the template default alone
        const QString field = f.at(i);
        const QJsonValue cur = params.value(field);
        if (!params.contains(field) || (cur.isString() && cur.toString().isEmpty())) params[field] = v;
    }
    return params;
}

/// The manifest as served to UIs: every template gets `textFields` (explicit or derived) so a form can label Text 1/2/3.
inline QJsonObject annotateManifest(QJsonObject manifest) {
    QJsonArray arr;
    for (const QJsonValue& v : manifest.value("templates").toArray()) {
        QJsonObject t = v.toObject();
        QJsonArray tf; for (const QString& s : textFieldNames(t)) tf.append(s);
        t["textFields"] = tf;
        arr.append(t);
    }
    manifest["templates"] = arr;
    return manifest;
}

} // namespace cgtext

#endif // CGTEXT_H
