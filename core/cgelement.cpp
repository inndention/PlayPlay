#include "cgelement.h"
#include <QJsonDocument>

QString cgTypeToString(CGType type) {
    switch(type) {
        case CGType::LowerThird: return "lower_third";
        case CGType::Ticker: return "ticker";
        case CGType::Bug: return "bug";
        case CGType::Clock: return "clock";
        case CGType::Weather: return "weather";
        case CGType::Custom: return "custom";
    }
    return "lower_third";
}

CGType stringToCGType(const QString& str) {
    if (str == "lower_third") return CGType::LowerThird;
    if (str == "ticker") return CGType::Ticker;
    if (str == "bug") return CGType::Bug;
    if (str == "clock") return CGType::Clock;
    if (str == "weather") return CGType::Weather;
    if (str == "custom") return CGType::Custom;
    return CGType::LowerThird;
}

QString cgAnimationInToString(CGAnimationIn anim) {
    switch(anim) {
        case CGAnimationIn::None: return "none";
        case CGAnimationIn::Fade: return "fade";
        case CGAnimationIn::SlideLeft: return "slide_left";
        case CGAnimationIn::SlideRight: return "slide_right";
        case CGAnimationIn::SlideUp: return "slide_up";
        case CGAnimationIn::SlideDown: return "slide_down";
        case CGAnimationIn::Zoom: return "zoom";
        case CGAnimationIn::Bounce: return "bounce";
    }
    return "none";
}

CGAnimationIn stringToCGAnimationIn(const QString& str) {
    if (str == "none") return CGAnimationIn::None;
    if (str == "fade") return CGAnimationIn::Fade;
    if (str == "slide_left") return CGAnimationIn::SlideLeft;
    if (str == "slide_right") return CGAnimationIn::SlideRight;
    if (str == "slide_up") return CGAnimationIn::SlideUp;
    if (str == "slide_down") return CGAnimationIn::SlideDown;
    if (str == "zoom") return CGAnimationIn::Zoom;
    if (str == "bounce") return CGAnimationIn::Bounce;
    return CGAnimationIn::None;
}

QString cgAnimationOutToString(CGAnimationOut anim) {
    switch(anim) {
        case CGAnimationOut::None: return "none";
        case CGAnimationOut::Fade: return "fade";
        case CGAnimationOut::SlideLeft: return "slide_left";
        case CGAnimationOut::SlideRight: return "slide_right";
        case CGAnimationOut::SlideUp: return "slide_up";
        case CGAnimationOut::SlideDown: return "slide_down";
        case CGAnimationOut::Zoom: return "zoom";
    }
    return "none";
}

CGAnimationOut stringToCGAnimationOut(const QString& str) {
    if (str == "none") return CGAnimationOut::None;
    if (str == "fade") return CGAnimationOut::Fade;
    if (str == "slide_left") return CGAnimationOut::SlideLeft;
    if (str == "slide_right") return CGAnimationOut::SlideRight;
    if (str == "slide_up") return CGAnimationOut::SlideUp;
    if (str == "slide_down") return CGAnimationOut::SlideDown;
    if (str == "zoom") return CGAnimationOut::Zoom;
    return CGAnimationOut::None;
}

QString cgLoopToString(CGLoop loop) {
    switch(loop) {
        case CGLoop::None: return "none";
        case CGLoop::Pulse: return "pulse";
        case CGLoop::Sheen: return "sheen";
        case CGLoop::Shimmer: return "shimmer";
        case CGLoop::GlowPulse: return "glow_pulse";
        case CGLoop::TickerScroll: return "ticker_scroll";
        case CGLoop::Bounce: return "bounce";
    }
    return "none";
}

CGLoop stringToCGLoop(const QString& str) {
    if (str == "none") return CGLoop::None;
    if (str == "pulse") return CGLoop::Pulse;
    if (str == "sheen") return CGLoop::Sheen;
    if (str == "shimmer") return CGLoop::Shimmer;
    if (str == "glow_pulse") return CGLoop::GlowPulse;
    if (str == "ticker_scroll") return CGLoop::TickerScroll;
    if (str == "bounce") return CGLoop::Bounce;
    return CGLoop::None;
}

QString cgAnchorToString(CGAnchor anchor) {
    switch(anchor) {
        case CGAnchor::TopLeft: return "top-left";
        case CGAnchor::TopCenter: return "top-center";
        case CGAnchor::TopRight: return "top-right";
        case CGAnchor::CenterLeft: return "center-left";
        case CGAnchor::Center: return "center";
        case CGAnchor::CenterRight: return "center-right";
        case CGAnchor::BottomLeft: return "bottom-left";
        case CGAnchor::BottomCenter: return "bottom-center";
        case CGAnchor::BottomRight: return "bottom-right";
    }
    return "center";
}

CGAnchor stringToCGAnchor(const QString& str) {
    if (str == "top-left") return CGAnchor::TopLeft;
    if (str == "top-center") return CGAnchor::TopCenter;
    if (str == "top-right") return CGAnchor::TopRight;
    if (str == "center-left") return CGAnchor::CenterLeft;
    if (str == "center") return CGAnchor::Center;
    if (str == "center-right") return CGAnchor::CenterRight;
    if (str == "bottom-left") return CGAnchor::BottomLeft;
    if (str == "bottom-center") return CGAnchor::BottomCenter;
    if (str == "bottom-right") return CGAnchor::BottomRight;
    return CGAnchor::Center;
}

QJsonObject CGElement::toJson() const {
    QJsonObject obj;
    obj["id"] = id;
    obj["name"] = name;
    obj["type"] = cgTypeToString(type);
    obj["title"] = title;
    if (!subtitle.isEmpty()) obj["subtitle"] = subtitle;
    if (!tickerText.isEmpty()) obj["tickerText"] = tickerText;
    if (!imageSrc.isEmpty()) obj["imageSrc"] = imageSrc;
    
    QJsonObject colorsObj;
    colorsObj["primary"] = colors.primary;
    colorsObj["secondary"] = colors.secondary;
    colorsObj["text"] = colors.text;
    colorsObj["gradient"] = colors.gradient;
    if (!colors.gradientDirection.isEmpty()) colorsObj["gradientDirection"] = colors.gradientDirection;
    obj["colors"] = colorsObj;
    
    QJsonObject effectsObj;
    effectsObj["shadow"] = effects.shadow;
    if (!effects.shadowColor.isEmpty()) effectsObj["shadowColor"] = effects.shadowColor;
    effectsObj["shadowBlur"] = effects.shadowBlur;
    effectsObj["glow"] = effects.glow;
    if (!effects.glowColor.isEmpty()) effectsObj["glowColor"] = effects.glowColor;
    effectsObj["glowIntensity"] = effects.glowIntensity;
    effectsObj["borderRadius"] = effects.borderRadius;
    effectsObj["opacity"] = effects.opacity;
    effectsObj["glassMorphism"] = effects.glassMorphism;
    obj["effects"] = effectsObj;
    
    QJsonObject posObj;
    posObj["x"] = position.x;
    posObj["y"] = position.y;
    posObj["width"] = position.width;
    posObj["height"] = position.height;
    posObj["anchor"] = cgAnchorToString(position.anchor);
    obj["position"] = posObj;
    
    obj["fontSize"] = fontSize;
    if (!fontFamily.isEmpty()) obj["fontFamily"] = fontFamily;
    obj["animationIn"] = cgAnimationInToString(animationIn);
    obj["animationOut"] = cgAnimationOutToString(animationOut);
    obj["animationDuration"] = animationDuration;
    obj["loop"] = cgLoopToString(loop);
    if (loopDuration > 0) obj["loopDuration"] = loopDuration;
    obj["isVisible"] = isVisible;
    if (autoHideAfter > 0) obj["autoHideAfter"] = autoHideAfter;
    obj["isPreset"] = isPreset;
    if (!presetCategory.isEmpty()) obj["presetCategory"] = presetCategory;
    
    return obj;
}

CGElement CGElement::fromJson(const QJsonObject& json) {
    CGElement element;
    element.id = json["id"].toString();
    element.name = json["name"].toString();
    element.type = stringToCGType(json["type"].toString());
    element.title = json["title"].toString();
    element.subtitle = json["subtitle"].toString();
    element.tickerText = json["tickerText"].toString();
    element.imageSrc = json["imageSrc"].toString();
    
    QJsonObject colorsObj = json["colors"].toObject();
    element.colors.primary = colorsObj["primary"].toString();
    element.colors.secondary = colorsObj["secondary"].toString();
    element.colors.text = colorsObj["text"].toString();
    element.colors.gradient = colorsObj["gradient"].toBool();
    element.colors.gradientDirection = colorsObj["gradientDirection"].toString();
    
    QJsonObject effectsObj = json["effects"].toObject();
    element.effects.shadow = effectsObj["shadow"].toBool();
    element.effects.shadowColor = effectsObj["shadowColor"].toString();
    element.effects.shadowBlur = effectsObj["shadowBlur"].toDouble();
    element.effects.glow = effectsObj["glow"].toBool();
    element.effects.glowColor = effectsObj["glowColor"].toString();
    element.effects.glowIntensity = effectsObj["glowIntensity"].toDouble();
    element.effects.borderRadius = effectsObj["borderRadius"].toDouble();
    element.effects.opacity = effectsObj["opacity"].toDouble();
    element.effects.glassMorphism = effectsObj["glassMorphism"].toBool();
    
    QJsonObject posObj = json["position"].toObject();
    element.position.x = posObj["x"].toDouble();
    element.position.y = posObj["y"].toDouble();
    element.position.width = posObj["width"].toDouble();
    element.position.height = posObj["height"].toDouble();
    element.position.anchor = stringToCGAnchor(posObj["anchor"].toString());
    
    element.fontSize = json["fontSize"].toDouble();
    element.fontFamily = json["fontFamily"].toString();
    element.animationIn = stringToCGAnimationIn(json["animationIn"].toString());
    element.animationOut = stringToCGAnimationOut(json["animationOut"].toString());
    element.animationDuration = json["animationDuration"].toInt();
    element.loop = stringToCGLoop(json["loop"].toString());
    element.loopDuration = json["loopDuration"].toInt();
    element.isVisible = json["isVisible"].toBool();
    element.autoHideAfter = json["autoHideAfter"].toInt();
    element.isPreset = json["isPreset"].toBool();
    element.presetCategory = json["presetCategory"].toString();
    
    return element;
}

QJsonObject CGPreset::toJson() const {
    QJsonObject obj;
    obj["id"] = id;
    obj["name"] = name;
    obj["category"] = category;
    obj["element"] = element.toJson();
    if (!thumbnail.isEmpty()) obj["thumbnail"] = thumbnail;
    return obj;
}

CGPreset CGPreset::fromJson(const QJsonObject& json) {
    CGPreset preset;
    preset.id = json["id"].toString();
    preset.name = json["name"].toString();
    preset.category = json["category"].toString();
    preset.element = CGElement::fromJson(json["element"].toObject());
    preset.thumbnail = json["thumbnail"].toString();
    return preset;
}

