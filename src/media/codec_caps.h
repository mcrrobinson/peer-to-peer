#pragma once

#include <QJsonObject>
#include <QList>
#include <QString>

#include <optional>

namespace p2p::media {

enum class Codec { HEVC, AV1 };

QString codecName(Codec c);
std::optional<Codec> codecFromName(const QString& name);

// One encoder or decoder implementation available on a host.
struct CodecCap {
    Codec codec = Codec::HEVC;
    bool hw = false;
    int maxWidth = 0;
    int maxHeight = 0;

    QJsonObject toJson() const;
    static std::optional<CodecCap> fromJson(const QJsonObject& o);
    bool operator==(const CodecCap&) const = default;
};

struct MediaCaps {
    QList<CodecCap> encoders;
    QList<CodecCap> decoders;
    // Opus DRED bitstream revision this peer sends and parses (0 = none).
    // DRED is only used when both peers report the same non-zero value.
    int audioDredRev = 0;

    QJsonObject toJson() const;
    static MediaCaps fromJson(const QJsonObject& o);
    QString summary() const;
    bool canDecode(Codec c) const;
};

// Whether two peers can exchange DRED.
inline bool dredCompatible(const MediaCaps& a, const MediaCaps& b)
{
    return a.audioDredRev > 0 && a.audioDredRev == b.audioDredRev;
}

struct NegotiationPolicy {
    // When set, any AV1 pairing (even software) beats any HEVC pairing.
    bool preferAv1 = false;
};

// The codec one sender will use for one stream, and where it runs.
struct StreamSelection {
    Codec codec = Codec::HEVC;
    bool encoderHw = false;
    bool decoderHw = false;
    int maxWidth = 0;
    int maxHeight = 0;

    QJsonObject toJson() const;
    static std::optional<StreamSelection> fromJson(const QJsonObject& o);
    QString summary() const;
    bool operator==(const StreamSelection&) const = default;
};

// Picks the best (sender encoder, receiver decoder) pair for one direction.
// Default ordering: AV1-hw/hw > HEVC-hw/hw > ... > software. Returns nullopt
// if the two sides share no codec.
std::optional<StreamSelection> negotiate(const QList<CodecCap>& senderEncoders,
                                         const QList<CodecCap>& receiverDecoders,
                                         const NegotiationPolicy& policy);

} // namespace p2p::media
