#include "uiscale.h"
#include <QSettings>
#include <QQuickItem>
#include <QVariantList>
#include <QtMath>
#include <algorithm>

namespace {
const QList<qreal>& stepList() {
    static const QList<qreal> s = {0.8, 0.9, 1.0, 1.15, 1.3, 1.5, 1.75, 2.0, 2.5, 3.0};
    return s;
}
} // namespace

UiScale::UiScale(const QString& settingsPath, QObject* parent) : QObject(parent), m_path(settingsPath) { load(); m_effective = m_zoom; }

QVariantList UiScale::steps() const { QVariantList l; for (qreal v : stepList()) l << v; return l; }

void UiScale::load() {
    QSettings s(m_path, QSettings::IniFormat);
    const qreal z = s.value("ui/zoom", kDefaultZoom).toReal();
    m_zoom = (z >= 0.6 && z <= 4.0) ? z : kDefaultZoom;
}

void UiScale::save() const {
    if (!m_persist) return;
    QSettings s(m_path, QSettings::IniFormat);
    s.setValue("ui/zoom", m_zoom);
}

qreal UiScale::clampFor(qreal requested) const {
    qreal z = qBound(0.6, requested, 4.0);
    if (m_window) {
        // Keep the logical area usable: never smaller than 900 x 540 at the current window size (but never below 1.0 forced).
        const qreal maxByW = m_window->width() > 0 ? m_window->width() / kMinLogicalW : z;
        const qreal maxByH = m_window->height() > 0 ? m_window->height() / kMinLogicalH : z;
        z = qMin(z, qMax(1.0, qMin(maxByW, maxByH)));
    }
    return z;
}

void UiScale::setZoom(qreal z) {
    z = qBound(0.6, z, 4.0);
    if (qFuzzyCompare(z, m_zoom)) return;
    m_zoom = z;
    save();
    apply();
    emit zoomChanged();      // the requested value changed even when the clamp keeps the effective one
}

void UiScale::step(int direction) {
    const QList<qreal>& l = stepList();
    int idx = 0; qreal best = 1e9;
    for (int i = 0; i < l.size(); ++i) { const qreal d = qAbs(l[i] - m_zoom); if (d < best) { best = d; idx = i; } }
    // From a value between steps, +1 goes to the next larger step and -1 to the next smaller one.
    if (direction > 0 && l[idx] <= m_zoom + 1e-6) idx = qMin(l.size() - 1, idx + 1);
    else if (direction < 0 && l[idx] >= m_zoom - 1e-6) idx = qMax(0, idx - 1);
    setZoom(l[idx]);
}

void UiScale::reset() { setZoom(kDefaultZoom); }

void UiScale::attach(QQuickWindow* window) {
    if (!window || m_window == window) return;
    m_window = window;
    QQuickItem* root = window->contentItem();
    // Qt resets the root item's size on every window resize and the logical size depends on the zoom:
    // re-apply whenever either the window or the root item changes size.
    connect(window, &QQuickWindow::widthChanged, this, [this](int) { apply(); });
    connect(window, &QQuickWindow::heightChanged, this, [this](int) { apply(); });
    connect(root, &QQuickItem::widthChanged, this, [this]() { apply(); });
    connect(root, &QQuickItem::heightChanged, this, [this]() { apply(); });
    // ApplicationWindow lays its own content item out to the WINDOW size on every resize; follow it too.
    if (QQuickItem* app = appContent()) {
        connect(app, &QQuickItem::widthChanged, this, [this]() { apply(); });
        connect(app, &QQuickItem::heightChanged, this, [this]() { apply(); });
    }
    apply();
}

QQuickItem* UiScale::appContent() const {
    if (!m_window) return nullptr;
    QQuickItem* app = m_window->property("contentItem").value<QQuickItem*>();   // ApplicationWindow's content area
    return app && app != m_window->contentItem() ? app : nullptr;
}

static void fitTo(QQuickItem* it, const QSizeF& sz) {
    if (!it) return;
    if (!qFuzzyCompare(it->width() + 1.0, sz.width() + 1.0) || !qFuzzyCompare(it->height() + 1.0, sz.height() + 1.0)) it->setSize(sz);
}

void UiScale::apply() {
    if (m_applying || !m_window) return;
    m_applying = true;
    const qreal z = clampFor(m_zoom);
    QQuickItem* root = m_window->contentItem();
    root->setTransformOrigin(QQuickItem::TopLeft);
    root->setScale(z);
    const QSizeF logical(m_window->width() / z, m_window->height() / z);
    fitTo(root, logical);
    // Everything that ApplicationWindow sizes to the window must be sized to the logical area instead, or the scaled
    // content lays out at window size and spills off the right and bottom edges: the content item, and the popup
    // overlay (menus, dialogs, dropdowns), both children of the root.
    if (QQuickItem* app = appContent()) { app->setPosition(QPointF(0, 0)); fitTo(app, logical); }
    for (QQuickItem* c : root->childItems())
        if (QByteArray(c->metaObject()->className()).contains("Overlay")) { c->setPosition(QPointF(0, 0)); fitTo(c, logical); }
    const bool changed = !qFuzzyCompare(z, m_effective);
    m_effective = z;
    m_applying = false;
    if (changed) emit zoomChanged();
}
