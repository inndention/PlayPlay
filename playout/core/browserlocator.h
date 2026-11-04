#ifndef PLAYPLAY_BROWSERLOCATOR_H
#define PLAYPLAY_BROWSERLOCATOR_H

#include <QString>
#include <QStringList>
#include <QJsonObject>

/**
 * cgbrowser: finds the Chromium-family browser that renders HTML overlays (the channel engine drives it headless
 * over the DevTools protocol; no Qt WebEngine is needed). Shared by the engine (to launch it) and the Supervisor
 * (to report availability and to pick HTML versus the native fallback before anything is sent to air).
 *
 * Search order: `preferred` (settings cg.browserPath), $PLAYPLAY_BROWSER, Chrome (Program Files, Program Files (x86),
 * %LOCALAPPDATA%), Edge, Chromium/Brave in the usual places, then PATH (chrome, msedge, chromium).
 */
namespace cgbrowser {

/// Candidate executables in search order (existing or not), for diagnostics.
QStringList candidates(const QString& preferred = QString());

/// First candidate that exists as a file; empty when none.
QString find(const QString& preferred = QString());

/// Browser build number read from the versioned folder next to the executable ("141.0.7390.77"), or "unknown".
QString version(const QString& exe);

/// {available, path, version, candidates[], preferred, env} for REST/UI.
QJsonObject describe(const QString& preferred = QString());

} // namespace cgbrowser

#endif // PLAYPLAY_BROWSERLOCATOR_H
