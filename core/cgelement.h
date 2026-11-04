#ifndef CGELEMENT_H
#define CGELEMENT_H

#include <QString>
#include <QJsonObject>
#include <QDateTime>

// CG Types
enum class CGType {
    LowerThird,
    Ticker,
    Bug,
    Clock,
    Weather,
    Custom
};

enum class CGAnimationIn {
    None,
    Fade,
    SlideLeft,
    SlideRight,
    SlideUp,
    SlideDown,
    Zoom,
    Bounce
};

enum class CGAnimationOut {
    None,
    Fade,
    SlideLeft,
    SlideRight,
    SlideUp,
    SlideDown,
    Zoom
};

enum class CGLoop {
    None,
    Pulse,
    Sheen,
    Shimmer,
    GlowPulse,
    TickerScroll,
    Bounce
};

enum class CGAnchor {
    TopLeft,
    TopCenter,
    TopRight,
    CenterLeft,
    Center,
    CenterRight,
    BottomLeft,
    BottomCenter,
    BottomRight
};

struct CGColors {
    QString primary;      // Hex
    QString secondary;    // Hex
    QString text;         // Hex
    bool gradient;
    QString gradientDirection; // "horizontal" | "vertical" | "diagonal"
    
    CGColors() : primary("#ffffff"), secondary("#000000"), text("#ffffff"), gradient(false) {}
};

struct CGEffects {
    bool shadow;
    QString shadowColor;
    double shadowBlur;
    bool glow;
    QString glowColor;
    double glowIntensity;
    double borderRadius;
    double opacity;
    bool glassMorphism;
    
    CGEffects() : shadow(false), shadowBlur(0), glow(false), glowIntensity(0), 
                  borderRadius(0), opacity(100), glassMorphism(false) {}
};

struct CGPosition {
    double x;              // Percentage from left (0-100)
    double y;              // Percentage from top (0-100)
    double width;          // Percentage of screen width
    double height;         // Percentage of screen height
    CGAnchor anchor;
    
    CGPosition() : x(50), y(50), width(30), height(10), anchor(CGAnchor::Center) {}
};

struct CGElement {
    QString id;
    QString name;
    CGType type;
    
    // Content
    QString title;
    QString subtitle;
    QString tickerText;    // For ticker type
    QString imageSrc;      // For bug type
    
    // Styling
    CGColors colors;
    CGEffects effects;
    CGPosition position;
    double fontSize;
    QString fontFamily;
    
    // Animation
    CGAnimationIn animationIn;
    CGAnimationOut animationOut;
    int animationDuration; // ms
    CGLoop loop;
    int loopDuration;      // ms per loop cycle
    
    // State
    bool isVisible;
    int autoHideAfter;    // ms, 0 = manual hide
    
    // Metadata
    bool isPreset;
    QString presetCategory;
    
    CGElement() : type(CGType::LowerThird), fontSize(48), fontFamily("Arial"),
                  animationIn(CGAnimationIn::Fade), animationOut(CGAnimationOut::Fade),
                  animationDuration(500), loop(CGLoop::None), loopDuration(0),
                  isVisible(false), autoHideAfter(0), isPreset(false) {}
    
    QJsonObject toJson() const;
    static CGElement fromJson(const QJsonObject& json);
};

struct CGPreset {
    QString id;
    QString name;
    QString category;
    CGElement element;
    QString thumbnail;
    
    QJsonObject toJson() const;
    static CGPreset fromJson(const QJsonObject& json);
};

// Helper functions
QString cgTypeToString(CGType type);
CGType stringToCGType(const QString& str);
QString cgAnimationInToString(CGAnimationIn anim);
CGAnimationIn stringToCGAnimationIn(const QString& str);
QString cgAnimationOutToString(CGAnimationOut anim);
CGAnimationOut stringToCGAnimationOut(const QString& str);
QString cgLoopToString(CGLoop loop);
CGLoop stringToCGLoop(const QString& str);
QString cgAnchorToString(CGAnchor anchor);
CGAnchor stringToCGAnchor(const QString& str);

#endif // CGELEMENT_H

