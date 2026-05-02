#pragma once
#include <QByteArray>
#include <QString>
#include <cstdint>

// Reconstructs a human-readable XML database.dbmanifest from the binary
//
// Format:
//   [u8 magic0][u8 magic1][u8 version]
//   5x length-prefixed strings: databaseId, databaseFamily, displayName, pipelineTag, licenseeTag
//     string encoding: [u8 slen_incl_null][slen-1 chars][0x00]
//   optional u8 format-variant byte (=0x00, present when next u16 > 0xFF domains — detect by heuristic)
//   [u16 LE] domain count
//   per domain:
//     name (string), root (string), isReadOnly (u8 bool), isTargetDomain (u8 bool)
//     [u16 LE] WKNA count
//       per WKNA: [u32 LE typeHash][string target]
//     [u16 LE] FailedReplacement count
//       per FailedRepl: [u32 LE typeHash][string target]

class DbManifestReconstructor
{
public:
    enum class Format {
        AlreadyXml,    // Starts with '<' — original unstripped XML, pass through as-is
        AlreadyJson,   // Starts with '{' — JSON format (Anthem/newer titles), pass through as-is
        StripedBinary, // 0x93 magic — needs reconstruction
        Unknown        // Unrecognised — fall back to ascii extraction
    };

    // Detect the format of the payload without attempting reconstruction.
    static Format detectFormat(const QByteArray& binary);

    // Returns reconstructed XML string, or empty string + sets errorOut on failure.
    // Should only be called when detectFormat() returns StripedBinary.
    // discoveredSeedOut (optional): set to the brute-forced seed, or UINT32_MAX if not found
    static QString reconstruct(const QByteArray& binary, QString& errorOut,
        uint32_t* discoveredSeedOut = nullptr);

    // Returns true if the binary looks like a stripped binary manifest
    // Equivalent to detectFormat() == StripedBinary
    static bool looksLikeManifest(const QByteArray& binary);

    // Public so MainWindow / Test Hash dialog can use it directly
    static uint32_t fbTypeHash(const char* shortName, uint32_t seed);
};