#pragma once
#include <string>
#include <vector>
#include <map>
#include <unordered_map>
#include <memory>
#include <functional>
#include "DbObject.h"
#include "IDeobfuscator.h"

enum class DeobfuscatorType
{
    PVZ,
    MEA,
    DA,
    BF3,
    Null
};

class Converter
{
public:
    // ---- Key management ----
    static std::vector<uint8_t> encryptionKey;

    static std::vector<uint8_t> getKey();

    // ---- Deobfuscator factory ----
    static std::shared_ptr<IDeobfuscator> createDeobfuscator(
        DeobfuscatorType type = DeobfuscatorType::PVZ);

    static DeobfuscatorType autoDetectDeobfuscatorType(const std::string& filePath);

    // ---- Read / write ----
    using KeyPromptCallback = std::function<std::vector<uint8_t>()>;

    static DbObjectPtr readPlainFileDbObject(
        const std::string& plainFile,
        const std::vector<uint8_t>& fallbackKey,
        DeobfuscatorType type,
        bool& hadEncrypted,
        KeyPromptCallback keyPrompt = nullptr);

    static std::vector<uint8_t> writePlainFileData(DbObjectPtr obj);

    // ---- Crypto ----
    static std::vector<uint8_t> decryptBuffer(
        const std::vector<uint8_t>& buffer,
        const std::vector<uint8_t>& key,
        bool silent = false);

    static std::vector<uint8_t> encryptInitfsPayloadAes(
        const std::vector<uint8_t>& plainData,
        const std::vector<uint8_t>& key);

    // ---- Obfuscated writers ----
    static void obfuscateInitfsFromPlainData(
        const std::string& originalEncryptedFile,
        const std::vector<uint8_t>& plainData,
        const std::string& outputFile,
        const std::vector<uint8_t>& key);

    static void writeDeobfuscatedInitfsFromDbObject(
        const std::string& originalFilePath,
        DbObjectPtr editedListRoot,
        const std::string& outputFile,
        DeobfuscatorType type);

    static void writeBF3ObfuscatedInitfs(
        const std::string& sourcePath,
        DbObjectPtr obj,
        const std::string& outputPath);

    static void writePvzObfuscatedInitfs(
        const std::string& sourcePath,
        DbObjectPtr obj,
        const std::string& outputPath);

    // ---- Utilities ----
    static std::vector<uint8_t> hexToBinary(const std::string& hex);
    static std::string          binaryToHex(const std::vector<uint8_t>& data);

    static bool isWin32Initfs(const std::string& path);
    static void updatePayload(DbObjectPtr dbObj, int payloadIndex,
        const std::vector<uint8_t>& newPayload);

private:

    // ---- Internal helpers ----
    static bool tryDecryptInitfs(
        const std::vector<uint8_t>& encryptedBuffer,
        const std::vector<uint8_t>& key,
        std::shared_ptr<IDeobfuscator> deobfuscator,
        DbObjectPtr& result,
        bool silent = false);

    static int  parseBF3BodyStart(const std::vector<uint8_t>& src);
    static std::vector<uint8_t> getBF3Key(const std::vector<uint8_t>& src);
    static void bf3XorInPlace(std::vector<uint8_t>& data,
        const std::vector<uint8_t>& key,
        int startOffsetInBody = 0);

    static int  getPvzBodyStart(const std::vector<uint8_t>& src);
    static std::vector<uint8_t> getPvzKey(const std::vector<uint8_t>& src);
    static void pvzXorInPlace(std::vector<uint8_t>& data,
        const std::vector<uint8_t>& key,
        int startOffsetInBody = 0);

    static bool sha256Equal(const std::vector<uint8_t>& a,
        const std::vector<uint8_t>& b);
    static void tryUnblock(const std::string& path);
};