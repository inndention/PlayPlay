// Headline News helpers: feed hygiene, story pages, story picking, speech forms, scene timing and the ffmpeg graph.
#include <QtTest>
#include <QDateTime>
#include "../playout/supervisor/headlinenews.h"

using namespace headline;

class TestHeadlineNews : public QObject {
    Q_OBJECT
private slots:
    void placeFromStoryUrls() {
        QCOMPARE(placeFromUrl("https://getstarnews.com/local/winston-salem/story/2229/"), QString("Winston-Salem"));
        QCOMPARE(placeFromUrl("https://getstarnews.com/local/roanoke/story/2231/"), QString("Roanoke"));
        QCOMPARE(placeFromUrl("https://getstarnews.com/local/henry-county/story/12/"), QString("Henry County"));
        QCOMPARE(placeFromUrl("https://getstarnews.com/local/martinsville/story/2220/", {"Martinsville", "Facebook"}), QString("Martinsville"));
        QCOMPARE(placeFromUrl("https://getstarnews.com/local/winston-salem/story/2229/", {"Winstonsalem"}), QString("Winston-Salem"));   // a flattened tag loses to the slug
        QCOMPARE(placeFromUrl("https://example.com/2026/10/23/some-story"), QString());
    }

    void titlesAndJunk() {
        QCOMPARE(cleanTitle("UPDATE | A woman is facing a DUI charge"), QString("UPDATE: A woman is facing a DUI charge"));
        QCOMPARE(cleanTitle(QString::fromUtf8("\xF0\x9F\x9A\xA8 The Sheriff's Office thanks partners")), QString("The Sheriff's Office thanks partners"));
        QCOMPARE(cleanTitle("Roanoke lists closures - GetStarNews", "GetStarNews"), QString("Roanoke lists closures"));
        QCOMPARE(cleanTitle("Tom &amp; Jerry&#039;s <b>show</b>"), QString("Tom & Jerry's show"));
        QVERIFY(isJunk("10:38 p.m.", "10:38 p.m. 10/22/26 - FORGET THE EL-NINO HYPE FOR NOW."));
        QVERIFY(isJunk("Welcome Race Fans!", "Welcome Race Fans!"));
        QVERIFY(isJunk("Signs of racing everywhere!", "Signs of racing everywhere!"));
        QVERIFY(!isJunk("Danville farmers market returns Saturday at Crossing at the Dan", "Danville's official calendar lists a farmers market."));
        QVERIFY(!isJunk("Storm damage", "Crews worked overnight to clear trees from roads across Henry County after strong winds."));
        QVERIFY(isTruncated("federal law..."));
        QVERIFY(isTruncated(QString("federal law") + QChar(0x2026)));
        QVERIFY(!isTruncated("Oct. 24"));
        QVERIFY(isGenericImage("https://getstarnews.com/backdrop_system/hollywood_transsky.png"));
        QVERIFY(isGenericImage("https://getstarnews.com/branding/logo_2026_2.png"));
        QVERIFY(isGenericImage(""));
        QVERIFY(!isGenericImage("https://getstarnews.com/uploads/posts/upload-20261023-093348-076375afff.png"));
    }

    void sentencesKeepNewsAbbreviations() {
        const QStringList a = sentences("The closure runs from Friday, Oct. 23, through Saturday, Oct. 24, 2026. Drivers should allow extra time.");
        QCOMPARE(a.size(), 2);
        QVERIFY(a[0].endsWith("2026."));
        QCOMPARE(sentences("J. Smith spoke at 3 p.m. Saturday at the U.S. Marshals office. It went well.").size(), 2);
        QCOMPARE(sentences("The game starts at 7 p.m. Tickets are five dollars.").size(), 2);
        const QString longOne = "Crews from three counties worked through the night to clear fallen trees, restore power to thousands of homes, reopen the main roads into town, and check on elderly residents who live alone in the hills above the river valley.";
        const QString f = firstSentence(longOne, 20);
        QVERIFY(f.endsWith('.'));
        QVERIFY(f.split(' ', Qt::SkipEmptyParts).size() <= 20);
        QVERIFY(!f.contains(",."));
    }

    void storyPage() {
        const QString html = R"(<html><head>
<meta property="og:title" content="Roanoke lists closures - GetStarNews">
<meta content="Two Roanoke street segments are scheduled to close all day Friday and Saturday." property="og:description">
<meta property="og:image" content="/uploads/posts/upload-1.png">
<meta property="article:published_time" content="2026-10-23T09:35:03+00:00">
<meta property="article:tag" content="Roanoke"><meta property="article:tag" content="Roadclosures">
</head><body><article class="story-article"><div class="story-content">
<p>The City of Roanoke&#039;s calendar lists a closure on Luck Avenue SW.</p>
<script>var x = "<p>not this one at all</p>";</script>
<p>Official sources: City of Roanoke notice and more.</p>
<p>Community</p>
</div></article><article class="story-related-card"><p>Another story entirely here today</p></article></body></html>)";
        const QJsonObject p = parseStoryPage(html, "https://getstarnews.com/local/roanoke/story/2231/");
        QCOMPARE(p.value("image").toString(), QString("https://getstarnews.com/uploads/posts/upload-1.png"));
        QCOMPARE(p.value("description").toString(), QString("Two Roanoke street segments are scheduled to close all day Friday and Saturday."));
        QCOMPARE(p.value("title").toString(), QString("Roanoke lists closures - GetStarNews"));
        QCOMPARE(p.value("tags").toArray().size(), 2);
        QCOMPARE(qint64(p.value("publishedMs").toDouble()), QDateTime::fromString("2026-10-23T09:35:03+00:00", Qt::ISODate).toMSecsSinceEpoch());
        const QJsonArray paras = p.value("paragraphs").toArray();
        QCOMPARE(paras.size(), 1);
        QCOMPARE(paras[0].toString(), QString("The City of Roanoke's calendar lists a closure on Luck Avenue SW."));
    }

    void mergeStories() {
        // a social post imported with its first ~100 characters as the title
        QJsonObject item; item["title"] = QString::fromUtf8("\xF0\x9F\x9A\xA8 The Rockingham County Sheriff's Office is highlighting the importance of local, state, and federal law...");
        item["link"] = "https://getstarnews.com/local/martinsville/story/2220/"; item["category"] = "Local"; item["pubMs"] = 1000.0; item["source"] = "getstarnews.com";
        QJsonObject page; page["image"] = "https://getstarnews.com/uploads/facebook/2026/10/x.jpg"; page["description"] = "The Rockingham County Sheriff's Office is highlighting the importance of local, state, and federal law enforcement partnerships following a major operation that res...";
        page["tags"] = QJsonArray{"Martinsville", "Facebook"};
        page["paragraphs"] = QJsonArray{"The Rockingham County Sheriff's Office is highlighting the importance of local, state, and federal law enforcement partnerships following a major four-month operation. Operation Piedmont Pipeline launched June 1, 2026, by the ATF."};
        const QJsonObject s = mergeStory(item, page, "GetStarNews");
        QCOMPARE(s.value("title").toString(), QString("The Rockingham County Sheriff's Office is highlighting the importance of local, state, and federal law enforcement partnerships following a major four-month operation."));
        QCOMPARE(s.value("summary").toString(), QString("Operation Piedmont Pipeline launched June 1, 2026, by the ATF."));
        QCOMPARE(s.value("place").toString(), QString("Martinsville"));
        QCOMPARE(s.value("category").toString(), QString("Local"));
        QCOMPARE(s.value("image").toString(), QString("https://getstarnews.com/uploads/facebook/2026/10/x.jpg"));
        // an editorial post: the complete og:description is the summary; a site-wide picture is no picture
        QJsonObject item2; item2["title"] = "Roanoke lists Luck and Norfolk Avenue closures for Oct. 23-24 events"; item2["link"] = "https://getstarnews.com/local/roanoke/story/2231/";
        QJsonObject page2; page2["description"] = "Two Roanoke street segments are scheduled to close all day Friday and Saturday for special events. Drivers should plan ahead.";
        page2["image"] = "https://getstarnews.com/backdrop_system/hollywood_transsky.png";
        const QJsonObject s2 = mergeStory(item2, page2);
        QCOMPARE(s2.value("summary").toString(), QString("Two Roanoke street segments are scheduled to close all day Friday and Saturday for special events."));
        QCOMPARE(s2.value("image").toString(), QString());
        QCOMPARE(s2.value("place").toString(), QString("Roanoke"));
    }

    void pickStoriesNewestWithPictures() {
        auto st = [](const char* t, const char* l, const char* img, double pub) { QJsonObject o; o["title"] = t; o["link"] = l; o["image"] = img; o["pubMs"] = pub; return o; };
        const QJsonArray all{st("A story one", "https://s/a/", "", 500), st("B story two", "https://s/b/", "x.png", 400), st("C story three", "https://s/c/", "y.png", 300),
                             st("B story two", "https://s/d/", "z.png", 200), st("E story five", "https://s/b", "w.png", 100)};
        auto titles = [](const QJsonArray& a) { QStringList t; for (const QJsonValue& v : a) t << v.toObject().value("title").toString(); return t; };
        QCOMPARE(titles(pickStories(all, 2, true)), (QStringList{"B story two", "C story three"}));
        QCOMPARE(titles(pickStories(all, 3, true)), (QStringList{"A story one", "B story two", "C story three"}));
        QCOMPARE(titles(pickStories(all, 10, false)), (QStringList{"A story one", "B story two", "C story three"}));
    }

    void speechForms() {
        QCOMPARE(speakable("Roanoke lists Luck Ave. SW closures for Oct. 23-24 events"), QString("Roanoke lists Luck Avenue Southwest closures for October 23 to 24 events"));
        QCOMPARE(speakable("Call 540-555-0100 today"), QString("Call 540-555-0100 today"));
        QCOMPARE(speakable("Painting runs from 2-3 p.m. Saturday"), QString("Painting runs from 2 to 3 p.m. Saturday"));
        QCOMPARE(speakable("Meet at Main Ave."), QString("Meet at Main Avenue."));
        QCOMPARE(speakable("The festival is at 231 W. First St., with closures"), QString("The festival is at 231 West First Street, with closures"));
        QCOMPARE(speakable("Parking on Church St."), QString("Parking on Church Street."));
        QCOMPARE(speakable("A trip to St. Louis"), QString("A trip to St. Louis"));
        QCOMPARE(spokenName("GetStarNews"), QString("Get Star News"));
        QCOMPARE(spokenName("WSLS"), QString("WSLS"));
        QCOMPARE(spokenSite("getstarnews.com", "GetStarNews"), QString("Get Star News dot com"));
        QCOMPARE(spokenSite("https://www.example.org/news", ""), QString("example dot org"));
    }

    void animatedBedsAreNotZoomed() {
        // weather scenes carry a looping frame sequence as their bed: scaled to 1080p and cut to length, but never moved by zoompan
        const SceneTiming t = sceneTimings({2.0, 5.0, 1.5}, {"story", "story", "outro"}, 30.0, 0.6, 0.35, 0.75, 4.0, 5.0);
        const QString still = storiesFilter(t, false), mixed = storiesFilter(t, false, QVector<bool>{false, true, false});
        QCOMPARE(still.count("zoompan"), 3);
        QCOMPARE(mixed.count("zoompan"), 2);
        QVERIFY(mixed.contains("[2:v]fps=30,scale=1920:1080:force_original_aspect_ratio=increase:out_range=tv,crop=1920:1080,format=yuv420p,setsar=1[bg1];"));
        QVERIFY(!mixed.contains("[2:v]scale=2880"));
        QVERIFY(mixed.contains("[0:v]scale=2880") && mixed.contains("[4:v]scale=2880"));                  // the other scenes are untouched
        QVERIFY(mixed.contains("[bg1][3:v]overlay=0:0") && mixed.contains("trim=end_frame="));            // furniture over the animated bed, cut to the scene length
        QCOMPARE(storiesFilter(t, false, QVector<bool>{false, false, false}), still);                     // an all-false list is the old graph
    }

    void sceneTimingAndGraph() {
        const SceneTiming t = sceneTimings({2.0, 5.0, 6.0, 1.5}, {"intro", "story", "story", "outro"}, 30.0, 0.6, 0.35, 0.75, 4.0, 5.0);
        QCOMPARE(t.slotFrames, (QVector<int>{120, 183, 213, 150}));
        QCOMPARE(t.videoFrames, (QVector<int>{138, 201, 231, 150}));
        QCOMPARE(t.xfadeFrames, 18);
        QCOMPARE(t.totalFrames, 666);
        QCOMPARE(t.leadMs, 350);
        const QString fc = storiesFilter(t, false);
        QCOMPARE(fc.count("xfade="), 3);
        QVERIFY(fc.contains("offset=4.000000"));       // story 1 arrives as slot 1 starts
        QVERIFY(fc.contains("offset=10.100000"));      // (120 + 183) frames
        QVERIFY(fc.contains("offset=17.200000"));      // (120 + 183 + 213) frames
        QVERIFY(fc.contains("trim=end_frame=138"));
        QVERIFY(fc.contains("[8:a]") && fc.contains("[11:a]") && !fc.contains("[12:a]"));
        QVERIFY(fc.contains("concat=n=4:v=0:a=1[narr]"));
        QVERIFY(fc.contains("fade=t=out:st=21.600000"));
        QVERIFY(fc.endsWith("[narr]anull[a]"));
        const QString withMusic = storiesFilter(t, true);
        QVERIFY(withMusic.contains("[12:a]") && withMusic.contains("sidechaincompress") && withMusic.endsWith("[a]"));
        // a scene never ends inside its own transition
        const SceneTiming tiny = sceneTimings({0.1, 0.1, 0.1}, {"intro", "story", "outro"}, 30.0, 0.6, 0.0, 0.0, 0.0, 0.0);
        for (int f : tiny.slotFrames) QVERIFY(f > tiny.xfadeFrames);
    }
};

QTEST_GUILESS_MAIN(TestHeadlineNews)
#include "test_headlinenews.moc"
