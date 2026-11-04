// iface/knowledge.h - the editorial context the knowledge module may share (mod-knowledge.md PublishContext).
// FROZEN (ANFS-F0). Implemented by knowledge; published with svcSet<IKnowledge>; used by w3-ai (StarNews generate).
// Only the allow-listed editorial items (raw.knowledge.editorial; default skills/getstarnews-editorial/SKILL.md)
// leave the plant; infrastructure notes never do. Content is already redacted.
#pragma once

#include "core/json.h"

namespace anfs {

class IKnowledge {
public:
    virtual ~IKnowledge() = default;
    // {text, items:[{relPath, sha256, bytes}]}: the concatenated allow-listed items.
    virtual JVal publishContext() const = 0;
};

} // namespace anfs
