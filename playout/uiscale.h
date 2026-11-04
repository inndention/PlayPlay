#ifndef PLAYPLAY_UISCALE_H
#define PLAYPLAY_UISCALE_H

#include <QObject>
#include <QPointer>
#include <QQuickWindow>
#include <QQuickItem>

/**
 * UiScale: whole-interface zoom for the operator app ("much larger text").
 *
 * Scaling the window's root item (QQuickWindow::contentItem) with its size divided by the zoom keeps
 * layouts, fixed-height rows, text, popups, menus, combo-box dropdowns, tooltips and dialogs scaling together
 * (the popup overlay is a child of that root), which a font-only scale cannot do: 170 hard-coded pixel sizes and
 * many fixed row heights would clip. The requested zoom is clamped so the logical area never falls below
 * 1280 x 720 (otherwise the layouts overflow), and persists in <AppData>/ui.ini.
 *
 * QML: context property `uiScale` with `zoom` (requested), `effectiveZoom` (after the clamp), `step(+1/-1)`, `reset()`.
 */
class UiScale : public QObject {
    Q_OBJECT
    Q_PROPERTY(qreal zoom READ zoom WRITE setZoom NOTIFY zoomChanged)
    Q_PROPERTY(qreal effectiveZoom READ effectiveZoom NOTIFY zoomChanged)
    Q_PROPERTY(QVariantList steps READ steps CONSTANT)
public:
    static constexpr qreal kDefaultZoom = 1.5;
    static constexpr qreal kMinLogicalW = 1280.0;   // the operator layouts are designed for at least 1280 x 720
    static constexpr qreal kMinLogicalH = 720.0;

    explicit UiScale(const QString& settingsPath, QObject* parent = nullptr);

    qreal zoom() const { return m_zoom; }
    qreal effectiveZoom() const { return m_effective; }
    QVariantList steps() const;

    void setZoom(qreal z);
    Q_INVOKABLE void step(int direction);        // +1 / -1 along steps()
    Q_INVOKABLE void reset();                    // back to the default (150 %)
    void setPersist(bool on) { m_persist = on; } // false for one-off overrides such as the screenshot harness
    /// Attach to the window whose root item is scaled; safe to call once the QML window exists.
    void attach(QQuickWindow* window);

signals:
    void zoomChanged();

private:
    void apply();
    QQuickItem* appContent() const;
    qreal clampFor(qreal requested) const;
    void load();
    void save() const;

    QString m_path;
    qreal m_zoom = kDefaultZoom;
    qreal m_effective = kDefaultZoom;
    QPointer<QQuickWindow> m_window;
    bool m_applying = false;
    bool m_persist = true;
};

#endif // PLAYPLAY_UISCALE_H
