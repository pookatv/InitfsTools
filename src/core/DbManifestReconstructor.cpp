#include "DbManifestReconstructor.h"
#include <QMap>
#include <QSet>
#include <QVector>
#include <QString>
#include <QByteArray>
#include <openssl/evp.h>
#include <cctype>
#include <cstring>
#include <cstdio>
#include <climits>

// ============================================================
//  Frostbite type name hash
//
//  Algorithm:
//    SHA256( lowercase(shortTypeName) + lowercase(str(seed)) )
//    -> last 4 bytes of 32-byte digest as big-endian uint32
//
//  "short name" = type name after the last '.' (namespace stripped)
// ============================================================
uint32_t DbManifestReconstructor::fbTypeHash(const char* shortName, uint32_t seed)
{
    char seedStr[16];
    snprintf(seedStr, sizeof(seedStr), "%u", seed);

    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr);

    {
        int len = (int)strlen(shortName);
        if (len > 511) len = 511;
        char lower[512];
        for (int i = 0; i < len; i++)
            lower[i] = (char)tolower((unsigned char)shortName[i]);
        EVP_DigestUpdate(ctx, lower, (size_t)len);
    }
    {
        int len = (int)strlen(seedStr);
        char lower[16];
        for (int i = 0; i < len; i++)
            lower[i] = (char)tolower((unsigned char)seedStr[i]);
        EVP_DigestUpdate(ctx, lower, (size_t)len);
    }

    uint8_t digest[32];
    unsigned int dlen = 32;
    EVP_DigestFinal_ex(ctx, digest, &dlen);
    EVP_MD_CTX_free(ctx);

    return ((uint32_t)digest[28] << 24) |
        ((uint32_t)digest[29] << 16) |
        ((uint32_t)digest[30] << 8) |
        ((uint32_t)digest[31]);
}

// ============================================================
// Known short type names
// ============================================================
static const char* k_knownTypeNames[] = {
    "FBScript",
    "EditorMaterialGridData",
    "EffectParameterList",
    "EmitterTagList",
    "EmitterGraphSystemSettings",
    "CutsceneImportSettings",
    "CinematicSettings",
    "BasicCinematicSettings",
    "BlueprintRefWellKnownNamedClass",
    "TextureBaseAsset",
    "TextureAsset",
    "RenderTextureAsset",
    "ShaderGraph",
    "WorldPartData",
    "SubWorldData",
    "LevelData",
    "SoundWaveAsset",
    "SoundBank",
    "SoundPatchAsset",
    "MeshVariationDatabase",
    "MeshAsset",
    "SkinnedMeshAsset",
    "VehicleBlueprint",
    "SoldierBlueprint",
    "WeaponBlueprint",
    "ObjectBlueprint",
    "SpawnPointBlueprint",
    "UIPackageAsset",
    "FBScriptAsset",
    "SyntaxHighlightingData",
    "PhysicsCollisionCategoryPresets",
    "RagdollAsset",
    "AllBundledWeapons",
    "WeaponUnlockAsset",
    "SoldierBodyData",
    "NavMeshAsset",
    "CoverAsset",
    "UIAsset",
    "UIRoot",
    "UIChunkData",
    "LocalizationAsset",
    "SettingsAsset",
    "PipelineSettings",
    "QualityScalabilitySettings",
    "PlayerSettings",
    "GameSettings",
    "NetworkSettings",
    "AntiCheatSettings",
    "DebugMenuGroup",
    "UnlockAssetBase",
    "StatsCategoryBase",
    "AnimationTree",
    "SkeletonAsset",
    "AnimationSetAsset",
    "VegetationAsset",
    "TerrainAsset",
    "RoadAsset",
    "WaterAsset",
    "SpeedTreeAsset",
    "CompositeAnimationGraph",
    "AntPackageAsset",
    "ImpactEffectData",
    "MaterialContainerAsset",
    "MaterialParameterGroup",
    "SurfaceShadingMaterial",
    "ParticleSystemAsset",
    "EmitterData",
    "PostProcessAsset",
    "PostProcessBinding",
    "LensFlareEntityData",
    "DestructionMeshAsset",
    "DestructionMaterialPair",
    "LodGroupData",
    "StaticModelGroupEntityData",
    "ProfileOptionData",
    "PersistenceStatDefinition",
};
static const int k_knownTypeNamesCount =
(int)(sizeof(k_knownTypeNames) / sizeof(k_knownTypeNames[0]));

// ============================================================
// Build hash -> name map for one seed
// ============================================================
static const QMap<uint32_t, QString>& buildTypeMap(uint32_t seed)
{
    static uint32_t             s_cachedSeed = UINT32_MAX - 2;
    static QMap<uint32_t, QString> s_cachedMap;

    if (seed != s_cachedSeed)
    {
        s_cachedMap.clear();
        for (int i = 0; i < k_knownTypeNamesCount; i++)
        {
            uint32_t h = DbManifestReconstructor::fbTypeHash(k_knownTypeNames[i], seed);
            if (!s_cachedMap.contains(h))
                s_cachedMap[h] = QString::fromLatin1(k_knownTypeNames[i]);
        }
        s_cachedSeed = seed;
    }
    return s_cachedMap;
}

// ============================================================
//  Brute-force seed discovery (0..9999)
//  Returns UINT32_MAX if no seed matched any known name hash
// ============================================================
static uint32_t bruteForceSeed(const QVector<uint32_t>& binaryHashes)
{
    if (binaryHashes.isEmpty())
        return UINT32_MAX;

    QSet<uint32_t> hashSet;
    for (uint32_t h : binaryHashes)
        hashSet.insert(h);

    for (uint32_t seed = 0; seed <= 9999; seed++)
    {
        for (int ni = 0; ni < k_knownTypeNamesCount; ni++)
        {
            uint32_t h = DbManifestReconstructor::fbTypeHash(k_knownTypeNames[ni], seed);
            if (hashSet.contains(h))
                return seed;
        }
    }
    return UINT32_MAX;
}

// ============================================================
// Reader helpers
// ============================================================
namespace {

    struct Reader {
        const uint8_t* data;
        int            size;
        int            pos;

        bool canRead(int n) const { return pos + n <= size; }

        bool readString(QString& out) {
            if (!canRead(1)) return false;
            int slen = data[pos++];
            if (slen == 0) { out.clear(); return true; }
            int contentLen = slen - 1;
            if (!canRead(contentLen + 1)) return false;
            out = QString::fromUtf8(
                reinterpret_cast<const char*>(data + pos), contentLen);
            pos += contentLen;
            if (data[pos] != 0x00) return false;
            pos++;
            return true;
        }

        bool readU8(uint8_t& out) {
            if (!canRead(1)) return false;
            out = data[pos++];
            return true;
        }

        bool readU16(uint16_t& out) {
            if (!canRead(2)) return false;
            out = uint16_t(data[pos]) | uint16_t(data[pos + 1]) << 8;
            pos += 2;
            return true;
        }

        bool readU32(uint32_t& out) {
            if (!canRead(4)) return false;
            out = uint32_t(data[pos])
                | uint32_t(data[pos + 1]) << 8
                | uint32_t(data[pos + 2]) << 16
                | uint32_t(data[pos + 3]) << 24;
            pos += 4;
            return true;
        }

        bool probeFailedReplPresent(int savedPos) const {
            if (savedPos + 2 > size) return false;
            uint16_t count = uint16_t(data[savedPos]) | uint16_t(data[savedPos + 1]) << 8;
            if (count > 64) return false;
            int p = savedPos + 2;
            for (int i = 0; i < count; i++) {
                if (p + 5 > size) return false;
                p += 4;
                int slen = data[p++];
                if (slen == 0 || p + slen > size) return false;
                p += slen;
            }
            return p <= size;
        }
    };

    QString xmlEscape(const QString& s) {
        QString out;
        out.reserve(s.size() + 8);
        for (QChar c : s) {
            switch (c.unicode()) {
            case '&': out += QLatin1String("&amp;");  break;
            case '<': out += QLatin1String("&lt;");   break;
            case '>': out += QLatin1String("&gt;");   break;
            case '"': out += QLatin1String("&quot;"); break;
            default:  out += c;                       break;
            }
        }
        return out;
    }

    QString resolveTypeName(uint32_t hash, const QMap<uint32_t, QString>& typeMap)
    {
        auto it = typeMap.find(hash);
        if (it != typeMap.end())
            return it.value();
        return QString("UnknownType_0x%1").arg(hash, 8, 16, QChar('0')).toUpper();
    }

}

// ============================================================
// detectFormat
// ============================================================
DbManifestReconstructor::Format
DbManifestReconstructor::detectFormat(const QByteArray& binary)
{
    if (binary.isEmpty())
        return Format::Unknown;

    // Skip any leading UTF-8 BOM (EF BB BF)
    int start = 0;
    if (binary.size() >= 3 &&
        (uint8_t)binary[0] == 0xEF &&
        (uint8_t)binary[1] == 0xBB &&
        (uint8_t)binary[2] == 0xBF)
        start = 3;

    // Skip leading whitespace to find the first meaningful byte
    while (start < binary.size() && (uint8_t)binary[start] <= 0x20)
        start++;

    if (start >= binary.size())
        return Format::Unknown;

    uint8_t first = (uint8_t)binary[start];

    // XML: starts with '<'
    if (first == '<')
        return Format::AlreadyXml;

    // JSON: starts with '{'
    if (first == '{')
        return Format::AlreadyJson;

    // Frostbite stripped binary: magic byte 0x93
    if (first == 0x93 && binary.size() >= 3) {
        uint8_t ver = (uint8_t)binary[start + 2 < binary.size() ? start + 2 : 2];
        if (ver >= 1 && ver <= 30)
            return Format::StripedBinary;
    }

    return Format::Unknown;
}

// ============================================================
// looksLikeManifest
// ============================================================
bool DbManifestReconstructor::looksLikeManifest(const QByteArray& binary)
{
    return detectFormat(binary) == Format::StripedBinary;
}

//  Collect all type hashes from the binary structure
static QVector<uint32_t> collectBinaryHashes(const uint8_t* data, int size)
{
    QVector<uint32_t> hashes;

    Reader r;
    r.data = data;
    r.size = size;
    r.pos = 0;

    // Skip magic (3 bytes) + 5 header strings
    uint8_t b;
    if (!r.readU8(b) || !r.readU8(b) || !r.readU8(b)) return hashes;
    for (int i = 0; i < 5; i++) {
        QString tmp;
        if (!r.readString(tmp)) return hashes;
    }

    // Optional format-variant byte (same heuristic as reconstruct())
    if (r.canRead(3)) {
        uint8_t  candidate = r.data[r.pos];
        uint16_t afterSkip = uint16_t(r.data[r.pos + 1]) | uint16_t(r.data[r.pos + 2]) << 8;
        uint16_t noSkip = uint16_t(r.data[r.pos]) | uint16_t(r.data[r.pos + 1]) << 8;
        bool skipValid = (afterSkip >= 1 && afterSkip <= 64);
        bool noSkipValid = (noSkip >= 1 && noSkip <= 64);
        if (skipValid && (!noSkipValid || candidate <= 0x01))
            r.pos++;
    }

    uint16_t numDomains = 0;
    if (!r.readU16(numDomains) || numDomains == 0 || numDomains > 64) return hashes;

    // Probe for FailedReplacement presence
    bool hasFailedRepl = false;
    {
        Reader probe = r;
        QString tmp; uint8_t pb; uint16_t n;
        if (probe.readString(tmp) && probe.readString(tmp) &&
            probe.readU8(pb) && probe.readU8(pb) && probe.readU16(n))
        {
            bool ok = true;
            for (int i = 0; i < (int)n && ok; i++) {
                uint32_t h; QString t;
                ok = probe.readU32(h) && probe.readString(t);
            }
            if (ok)
                hasFailedRepl = probe.probeFailedReplPresent(probe.pos);
        }
    }

    for (int di = 0; di < (int)numDomains; di++) {
        QString domName, domRoot;
        uint8_t isReadOnly = 0, isTargetDomain = 0;
        uint16_t wknaCount = 0;

        if (!r.readString(domName) || !r.readString(domRoot) ||
            !r.readU8(isReadOnly) || !r.readU8(isTargetDomain) ||
            !r.readU16(wknaCount))
            return hashes;

        for (int ai = 0; ai < (int)wknaCount; ai++) {
            uint32_t hash; QString target;
            if (!r.readU32(hash) || !r.readString(target)) return hashes;
            hashes.append(hash);
        }

        if (hasFailedRepl) {
            uint16_t failCount = 0;
            if (!r.readU16(failCount)) return hashes;
            for (int ai = 0; ai < (int)failCount; ai++) {
                uint32_t hash; QString target;
                if (!r.readU32(hash) || !r.readString(target)) return hashes;
                hashes.append(hash);
            }
        }
    }

    return hashes;
}

// ============================================================
// reconstruct
// ============================================================
QString DbManifestReconstructor::reconstruct(const QByteArray& binary,
    QString& errorOut,
    uint32_t* discoveredSeedOut)
{
    errorOut.clear();
    if (discoveredSeedOut)
        *discoveredSeedOut = UINT32_MAX;

    //  Guard 1: already readable - skip reconstruction
    Format fmt = detectFormat(binary);
    if (fmt == Format::AlreadyXml || fmt == Format::AlreadyJson) {
        // Return the payload as-is; it's already displayable text
        return QString::fromUtf8(binary);
    }
    if (fmt != Format::StripedBinary) {
        errorOut = "Unrecognised manifest format (not XML, JSON, or Frostbite binary)";
        return {};
    }

    if (binary.size() < 8) {
        errorOut = "Binary too short to be a stripped_database.dbmanifest";
        return {};
    }

    const uint8_t* rawData = reinterpret_cast<const uint8_t*>(binary.constData());
    const int      rawSize = binary.size();

    //  Pass 1: collect all type hashes present in the binary
    QVector<uint32_t> binaryHashes = collectBinaryHashes(rawData, rawSize);

    //  Guard 2: no type entries (WKNA) at all   skip seed brute-force
    // UINT32_MAX      = brute-force ran but no seed matched
    // UINT32_MAX - 1  = brute-force was skipped (no WKNA type entries present)
    uint32_t seed = UINT32_MAX;
    bool hasTypes = !binaryHashes.isEmpty();

    if (hasTypes) {
        seed = bruteForceSeed(binaryHashes);
    }
    else {
        seed = UINT32_MAX - 1;  // sentinel: seed logic skipped entirely
    }

    // Build type map   use discovered seed, or seed=0 as neutral fallback
    const QMap<uint32_t, QString>& typeMap = buildTypeMap(
        (seed == UINT32_MAX || seed == UINT32_MAX - 1) ? 0 : seed);

    if (discoveredSeedOut)
        *discoveredSeedOut = seed;

    //  Pass 2: full structural parse ? emit XML
    Reader r;
    r.data = rawData;
    r.size = rawSize;
    r.pos = 0;

    // Header
    uint8_t magic0, magic1, version;
    r.readU8(magic0);
    r.readU8(magic1);
    r.readU8(version);

    // 5 header strings
    QString databaseId, databaseFamily, displayName, pipelineTag, licenseeTag;
    if (!r.readString(databaseFamily) ||
        !r.readString(databaseId) ||
        !r.readString(displayName) ||
        !r.readString(licenseeTag) ||
        !r.readString(pipelineTag))
    {
        errorOut = "Failed to read header strings from manifest binary";
        return {};
    }

    // Optional format-variant byte (value 0x00 or 0x01 seen in the wild)
    if (r.canRead(3)) {
        uint8_t  candidate = r.data[r.pos];
        uint16_t afterSkip = uint16_t(r.data[r.pos + 1]) | uint16_t(r.data[r.pos + 2]) << 8;
        uint16_t noSkip = uint16_t(r.data[r.pos]) | uint16_t(r.data[r.pos + 1]) << 8;
        bool skipValid = (afterSkip >= 1 && afterSkip <= 64);
        bool noSkipValid = (noSkip >= 1 && noSkip <= 64);
        // Skip the variant byte when:
        //   - skipping gives a valid count AND not-skipping doesn't, OR
        //   - both are valid but the candidate byte looks like a variant byte (<=0x01)
        if (skipValid && (!noSkipValid || candidate <= 0x01))
            r.pos++;
    }

    // Domain count
    uint16_t numDomains = 0;
    if (!r.readU16(numDomains) || numDomains == 0 || numDomains > 64) {
        errorOut = QString("Invalid domain count: %1").arg(numDomains);
        return {};
    }

    // Probe for FailedReplacement presence
    bool hasFailedRepl = false;
    {
        Reader probe = r;
        QString tmp; uint8_t b2; uint16_t n;
        if (probe.readString(tmp) && probe.readString(tmp) &&
            probe.readU8(b2) && probe.readU8(b2) && probe.readU16(n))
        {
            bool ok = true;
            for (int i = 0; i < (int)n && ok; i++) {
                uint32_t h; QString t;
                ok = probe.readU32(h) && probe.readString(t);
            }
            if (ok)
                hasFailedRepl = probe.probeFailedReplPresent(probe.pos);
        }
    }

    // Build XML   reserve upfront to avoid repeated reallocation
    QString xml;
    xml.reserve(1024 + numDomains * 512);
    xml += "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    xml += QString("<Database id=\"%1\"")
        .arg(xmlEscape(databaseId));
    if (version >= 5)
        xml += QString(" family=\"%1\"").arg(xmlEscape(databaseFamily));
    xml += QString(" displayName=\"%1\" pipelineTag=\"%2\" licensee=\"%3\"")
        .arg(xmlEscape(displayName))
        .arg(xmlEscape(licenseeTag))
        .arg(xmlEscape(pipelineTag));
    xml += ">\n";
    xml += "    <Settings>\n";
    xml += "        <Setting name='UseSparseSerialization' value='false'/>\n";
    xml += "    </Settings>\n\n";
    xml += "    <!-- Domain configuration\n\t  -->\n\n";

    for (int di = 0; di < (int)numDomains; di++) {
        QString domName, domRoot;
        uint8_t isReadOnly = 0, isTargetDomain = 0;
        uint16_t wknaCount = 0;

        if (!r.readString(domName) || !r.readString(domRoot) ||
            !r.readU8(isReadOnly) || !r.readU8(isTargetDomain) ||
            !r.readU16(wknaCount))
        {
            errorOut = QString("Failed to read domain[%1] header").arg(di);
            return {};
        }

        xml += QString("    <Domain name=\"%1\" root=\"%2\"")
            .arg(xmlEscape(domName))
            .arg(xmlEscape(domRoot));
        if (isReadOnly)
            xml += " isReadOnly=\"true\"";
        if (isTargetDomain)
            xml += " isTargetDomain='true' isEphemeral='true'";
        else if (domName.compare("Temp", Qt::CaseInsensitive) == 0)
            xml += " isEphemeral='true'";

        bool hasChildren = (wknaCount > 0) ||
            (domName.compare("Temp", Qt::CaseInsensitive) == 0);

        if (!hasChildren) {
            xml += "/>\n";
        }
        else {
            xml += ">\n";

            if (domName.compare("Source", Qt::CaseInsensitive) == 0) {
                xml += "        <EmergencyBindings>\n";
                xml += "            <Type name=\"TextureBaseAsset\" target=\"Textures/white\"/>\n";
                xml += "            <Type name=\"ShaderGraph\" target=\"Shaders/debug/black\"/>\n";
                xml += "        </EmergencyBindings>\n";
            }

            if (wknaCount > 0) {
                xml += "        <WellKnownNamedAssets>\n";
                for (int ai = 0; ai < (int)wknaCount; ai++) {
                    uint32_t hash; QString target;
                    if (!r.readU32(hash) || !r.readString(target)) {
                        errorOut = QString("Failed to read WKNA[%1] in domain[%2]").arg(ai).arg(di);
                        return {};
                    }
                    QString typeName = resolveTypeName(hash, typeMap);
                    xml += QString("            <Type name=\"%1\" target=\"%2\"/>\n")
                        .arg(xmlEscape(typeName))
                        .arg(xmlEscape(target));
                }
                xml += "        </WellKnownNamedAssets>\n";
            }

            if (domName.compare("Temp", Qt::CaseInsensitive) == 0)
                xml += "        <Import domain=\"Source\" readOnly='true'/>\n";

            xml += "    </Domain>\n";
        }

        if (hasFailedRepl) {
            uint16_t failCount = 0;
            if (!r.readU16(failCount)) {
                errorOut = QString("Failed to read FailedReplacement count for domain[%1]").arg(di);
                return {};
            }
            if (failCount > 0) {
                xml += QString("    <!-- FailedAssetReplacementMappings in domain '%1': -->\n")
                    .arg(domName);
                for (int ai = 0; ai < (int)failCount; ai++) {
                    uint32_t hash; QString target;
                    if (!r.readU32(hash) || !r.readString(target)) {
                        errorOut = QString("Failed to read FailedRepl[%1] in domain[%2]").arg(ai).arg(di);
                        return {};
                    }
                    QString typeName = resolveTypeName(hash, typeMap);
                    xml += QString("    <!--   <Type name=\"%1\" target=\"%2\"/> -->\n")
                        .arg(xmlEscape(typeName))
                        .arg(xmlEscape(target));
                }
            }
        }
    }

    xml += "</Database>\n";
    return xml;
}