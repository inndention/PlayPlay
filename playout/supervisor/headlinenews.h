#ifndef HEADLINENEWS_H
#define HEADLINENEWS_H

#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVector>

/**
 * Headline News: the story-by-story render of the "headlines" spot recipe (SpotGenerator, render "stories").
 *
 * The newest posts of a news site's RSS feed become one scene each: the post's own photo (from its story page,
 * og:image) moved by ffmpeg, the furniture card (kicker, category, place, headline, summary, counter) over it,
 * and the narration for that story; an intro scene (rundown) opens and an outro (the site) closes. Scenes are
 * chained with crossfades; each scene's narration starts as its picture arrives.
 *
 * Everything here is pure (QtCore only) so tests can check it without a network, Python or ffmpeg:
 * feed hygiene, story page parsing, story picking, spoken-text normalisation, scene timing and the
 * ffmpeg filter graph.
 */
namespace headline {

/// Feed / page text: tags out, entities decoded, whitespace collapsed.
QString plainText(const QString& html);
/// Screen + speech hygiene for a title or a lead sentence: leading emoji / bullets go, "UPDATE | x" -> "UPDATE: x",
/// a trailing " - <siteName>" (og:title) goes.
QString cleanTitle(const QString& title, const QString& siteName = QString());
/// A title the site cut short ("... law..."): social posts imported with the first ~100 characters as the title.
bool isTruncated(const QString& text);
/// Feed items that are not headlines: a bare clock time ("10:38 p.m."), or a short caption with nothing behind it
/// ("Welcome Race Fans!").
bool isJunk(const QString& title, const QString& text);
/// Site-wide pictures (logo, default backdrop, placeholder) that a page reports when the post has none of its own.
bool isGenericImage(const QString& url);
/// "https://site/local/winston-salem/story/2229/" -> "Winston-Salem" (tags spell the place when they match the slug).
QString placeFromUrl(const QString& url, const QStringList& tags = QStringList());
/// Sentence split that knows news abbreviations: "from Friday, Oct. 23, through ..." stays one sentence, as do
/// "J. Smith", "U.S. Marshals" and "3 p.m. Saturday".
QStringList sentences(const QString& text);
/// The first sentence; one longer than maxWords ends at its last clause break inside the limit.
QString firstSentence(const QString& text, int maxWords = 32);

/// A story page: {image, title, description, tags[], section, publishedMs, paragraphs[]} from og:* / article:*
/// meta tags and the paragraphs of the first <article> (boilerplate such as "Official sources: ..." skipped).
QJsonObject parseStoryPage(const QString& html, const QString& pageUrl);
/// Feed item ({title, text, link, image, pubMs, source, category}) + its parsed page -> a story
/// {title, summary, place, category, image, link, pubMs, source}. A truncated title is rebuilt from the
/// post's first sentence; the summary is the first sentence that does not repeat the headline.
QJsonObject mergeStory(const QJsonObject& item, const QJsonObject& page, const QString& siteName = QString());
/// Up to n stories, newest first, duplicates (same link or same headline) dropped. With preferImages the
/// newest stories that have their own picture win; imageless ones fill up when there are not enough.
QJsonArray pickStories(const QJsonArray& stories, int n, bool preferImages = true);

/// Written forms a TTS voice misreads: "Oct." -> "October", "Ave." -> "Avenue", "Avenue SW" -> "Avenue Southwest",
/// "23-24" -> "23 to 24" (one- or two-digit ranges only, so phone numbers stay intact).
QString speakable(const QString& text);
/// "GetStarNews" -> "Get Star News" (camel case split for the voice).
QString spokenName(const QString& name);
/// "getstarnews.com" (+ "GetStarNews") -> "Get Star News dot com".
QString spokenSite(const QString& site, const QString& siteName = QString());

/// Scene timing in whole frames. slotFrames[i] is the part of the timeline that belongs to scene i (its
/// narration starts leadSec into it); every scene but the last runs xfadeFrames longer so the crossfade into the
/// next scene happens over the start of the next slot. totalFrames = sum(slotFrames).
struct SceneTiming {
    QVector<int> slotFrames, videoFrames;
    int xfadeFrames = 0, totalFrames = 0, leadMs = 0;
    double fps = 30.0;
};
SceneTiming sceneTimings(const QVector<double>& wavSec, const QStringList& kinds, double fps = 30.0,
                         double xfadeSec = 0.6, double leadSec = 0.35, double tailSec = 0.75,
                         double introMinSec = 4.0, double outroMinSec = 5.0);
/// filter_complex for K scenes. Inputs: scene i's bed (a still) at 2i, its furniture PNG (looped) at 2i+1,
/// its narration WAV at 2K+i, the music bed (optional) at 3K. Outputs [v] (1920x1080 yuv420p) and [a]
/// (48 kHz stereo, music ducked under the voice). A scene flagged in `animated` has a looping frame sequence as its bed
/// (scripts/weather_cards.py): scaled and cut to length like the others but NOT moved by zoompan - it moves itself.
QString storiesFilter(const SceneTiming& t, bool music, const QVector<bool>& animated = QVector<bool>());

} // namespace headline

#endif // HEADLINENEWS_H
