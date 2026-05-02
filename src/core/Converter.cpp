#include "Converter.h"
#include "DbReader.h"
#include "DbWriter.h"
#include "NullDeobfuscator.h"
#include "PVZDeobfuscator.h"
#include "DADeobfuscator.h"
#include "BF3Deobfuscator.h"
#include "MEADeobfuscator.h"
#include "Logger.h"

#include <openssl/evp.h>
#include <openssl/sha.h>

#include <QFileDialog>
#include <QInputDialog>
#include <QMessageBox>
#include <QWidget>
#include <QRegularExpression>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>

#include <fstream>
#include <sstream>
#include <stdexcept>
#include <algorithm>
#include <filesystem>
#include <cstring>
#include <cstdio>
#include <ctime>
#include <cstdarg>
#include <mutex>

namespace fs = std::filesystem;

// ---- static member definitions ----
std::vector<uint8_t>  Converter::encryptionKey;

// ============================================================================
// Key management
// ============================================================================

std::vector<uint8_t> Converter::getKey()
{
    if (!encryptionKey.empty())
        return encryptionKey;

    return { 0x01,0x02,0x03,0x04, 0x05,0x06,0x07,0x08,
             0x09,0x0A,0x0B,0x0C, 0x0D,0x0E,0x0F,0x10 };
}

// ============================================================================
// Deobfuscator factory
// ============================================================================

std::shared_ptr<IDeobfuscator> Converter::createDeobfuscator(DeobfuscatorType type)
{
    switch (type)
    {
    case DeobfuscatorType::Null: return std::make_shared<NullDeobfuscator>();
    case DeobfuscatorType::MEA:  return std::make_shared<MEADeobfuscator>();
    case DeobfuscatorType::DA:   return std::make_shared<DADeobfuscator>();
    case DeobfuscatorType::BF3:  return std::make_shared<BF3Deobfuscator>();
    case DeobfuscatorType::PVZ:
    default:                     return std::make_shared<PVZDeobfuscator>();
    }
}

// ============================================================================
// Auto-detection
// ============================================================================

DeobfuscatorType Converter::autoDetectDeobfuscatorType(const std::string& filePath)
{
    Logger::log("[LoadInitfs] Inspecting: %s", filePath.c_str());

    try
    {
        std::ifstream fs(filePath, std::ios::binary);
        if (!fs) throw std::runtime_error("Cannot open file");

        uint8_t mb[4]; fs.read(reinterpret_cast<char*>(mb), 4);
        uint32_t magic = uint32_t(mb[0]) | uint32_t(mb[1]) << 8
            | uint32_t(mb[2]) << 16 | uint32_t(mb[3]) << 24;
        Logger::log("[LoadInitfs] Magic: 0x%X", magic);

        // BF3
        if (magic == 0x00CED100)
        {
            Logger::log("[LoadInitfs] Detected BF3 obfuscation");
            return DeobfuscatorType::BF3;
        }

        // PVZ / Null
        if (magic == 0x01CED100 || magic == 0x03CED100)
        {
            fs.seekg(0, std::ios::end);
            std::streamoff fileSize = fs.tellg();
            std::vector<uint8_t> raw(static_cast<size_t>(std::min<std::streamoff>(0x1000, fileSize)));
            fs.seekg(0);
            fs.read(reinterpret_cast<char*>(raw.data()), raw.size());

            // Search directly in the raw buffer   no std::string copy needed
            static const char kEnc[] = "encrypted";
            if (std::search(raw.begin(), raw.end(), kEnc, kEnc + 8) != raw.end())
            {
                Logger::log("[LoadInitfs] Found 'encrypted' buffer -> Detected Null deobfuscator, decrypting now...");
                return DeobfuscatorType::Null;
            }

            fs.clear();
            fs.seekg(0x128);
            std::vector<uint8_t> possibleKey(257);
            fs.read(reinterpret_cast<char*>(possibleKey.data()), 257);

            bool nonZero = std::any_of(possibleKey.begin(), possibleKey.end(),
                [](uint8_t b) { return b != 0; });
            if (nonZero)
            {
                try
                {
                    // Reuse fs seek back to start instead of opening a new handle
                    fs.clear();
                    fs.seekg(0);
                    DbReader testReader(fs, createDeobfuscator(DeobfuscatorType::PVZ));
                    DbObjectPtr obj = testReader.readDbObject();
                    if (obj && obj->count() > 0)
                    {
                        Logger::log("[LoadInitfs] Detected PVZ obfuscation");
                        return DeobfuscatorType::PVZ;
                    }
                }
                catch (const std::exception& ex)
                {
                    Logger::log("[LoadInitfs] PVZ failed: %s", ex.what());
                }
            }
        }

        // MEA reuse fs
        try
        {
            fs.clear();
            fs.seekg(-36, std::ios::end);
            char tail[36]; fs.read(tail, 36);
            // Compare directly   no std::string construction needed
            if (std::memcmp(tail + 4, "@e!adnXd$^!rfOsrDyIrI!xVgHeA!6Vc", 32) == 0)
            {
                Logger::log("[LoadInitfs] Detected MEA obfuscation");
                return DeobfuscatorType::MEA;
            }
        }
        catch (const std::exception& ex)
        {
            Logger::log("[LoadInitfs] MEA check failed: %s", ex.what());
        }

        // DA reuse fs
        try
        {
            fs.clear();
            fs.seekg(0);
            DbReader readerDA(fs, createDeobfuscator(DeobfuscatorType::DA));
            DbObjectPtr obj = readerDA.readDbObject();
            if (obj && obj->count() > 0)
            {
                Logger::log("[LoadInitfs] Detected DA obfuscation");
                return DeobfuscatorType::DA;
            }
        }
        catch (const std::exception& ex)
        {
            Logger::log("[LoadInitfs] DA failed: %s", ex.what());
        }
    }
    catch (const std::exception& ex)
    {
        Logger::log("[LoadInitfs] Unhandled exception: %s", ex.what());
    }

    Logger::log("[LoadInitfs] Detected Null obfuscation");
    return DeobfuscatorType::Null;
}

// ============================================================================
// Read
// ============================================================================

bool Converter::tryDecryptInitfs(const std::vector<uint8_t>& encryptedBuffer,
    const std::vector<uint8_t>& key,
    std::shared_ptr<IDeobfuscator> deobfuscator,
    DbObjectPtr& result,
    bool silent)
{
    result = nullptr;
    try
    {
        auto decrypted = decryptBuffer(encryptedBuffer, key, silent);
        std::string buf(reinterpret_cast<const char*>(decrypted.data()), decrypted.size());
        std::istringstream ms(std::move(buf), std::ios::binary);
        DbReader reader(ms, deobfuscator);
        result = reader.readDbObject();
        return result != nullptr;
    }
    catch (const std::exception& ex)
    {
        if (!silent)
            Logger::log("[LoadInitfs] Decryption failed: %s", ex.what());
        return false;
    }
}

DbObjectPtr Converter::readPlainFileDbObject(
    const std::string& plainFile,
    const std::vector<uint8_t>& fallbackKey,
    DeobfuscatorType type,
    bool& hadEncrypted,
    KeyPromptCallback keyPrompt)
{
    hadEncrypted = false;

    std::ifstream fs(plainFile, std::ios::binary);
    if (!fs) throw std::runtime_error("Cannot open file: " + plainFile);

    auto deobfuscator = createDeobfuscator(type);
    DbReader reader(fs, deobfuscator);
    DbObjectPtr obj = reader.readDbObject();
    if (!obj) throw std::runtime_error("Failed to read initfs object.");

    auto encryptedBuffer = obj->getValue<std::vector<uint8_t>>("encrypted");
    if (!encryptedBuffer.empty())
    {
        hadEncrypted = true;
        // --- Try fallback key first ---
        DbObjectPtr decryptedObj;
        bool success = tryDecryptInitfs(encryptedBuffer, fallbackKey,
            createDeobfuscator(type), decryptedObj, true);
        if (success)
        {
            if (encryptionKey.empty())
                encryptionKey = fallbackKey;
        }
        else
        {
            // --- Fallback key failed: prompt user in a retry loop ---
            if (!keyPrompt)
                throw std::runtime_error("AES decryption failed with provided key.");
            while (!success)
            {
                std::vector<uint8_t> userKey = keyPrompt();
                // Empty vector = user cancelled
                if (userKey.empty())
                    throw std::runtime_error("No AES key provided.");
                success = tryDecryptInitfs(encryptedBuffer, userKey,
                    createDeobfuscator(type), decryptedObj);
                if (success)
                {
                    encryptionKey = userKey;
                    Logger::log("[LoadInitfs] Key accepted.");
                    // Key saving is handled by the caller (MainWindow)
                }
                else
                {
                    Logger::log("[LoadInitfs] Key failed, prompting again...");
                }
            }
        }
        obj = decryptedObj;
        Logger::log("[LoadInitfs] Decrypted successfully");
    }

    Logger::log("[LoadInitfs] Done");
    return obj;
}

// ============================================================================
// Write / serialise
// ============================================================================

std::vector<uint8_t> Converter::writePlainFileData(DbObjectPtr obj)
{
    Logger::log("[LoadInitfs] Serializing DbObject...");
    std::ostringstream ms(std::ios::binary);
    DbWriter writer(ms);
    writer.write(obj);
    std::string s = ms.str();
    Logger::log("[LoadInitfs] Serialized %zu bytes", s.size());
    const auto* p = reinterpret_cast<const uint8_t*>(s.data());
    return std::vector<uint8_t>(p, p + s.size());
}

// ============================================================================
// AES-128-CBC
// ============================================================================

std::vector<uint8_t> Converter::decryptBuffer(
    const std::vector<uint8_t>& buffer,
    const std::vector<uint8_t>& key,
    bool silent)
{
    if (!silent)
        Logger::log("[LoadInitfs] Decrypting %zu bytes", buffer.size());

    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) throw std::runtime_error("EVP_CIPHER_CTX_new failed");

    if (EVP_DecryptInit_ex(ctx, EVP_aes_128_cbc(), nullptr,
        key.data(), key.data()) != 1)
    {
        EVP_CIPHER_CTX_free(ctx);
        throw std::runtime_error("EVP_DecryptInit_ex failed");
    }

    std::vector<uint8_t> out(buffer.size() + EVP_MAX_BLOCK_LENGTH);
    int outLen1 = 0, outLen2 = 0;

    if (EVP_DecryptUpdate(ctx, out.data(), &outLen1,
        buffer.data(), static_cast<int>(buffer.size())) != 1)
    {
        EVP_CIPHER_CTX_free(ctx);
        throw std::runtime_error("EVP_DecryptUpdate failed");
    }
    if (EVP_DecryptFinal_ex(ctx, out.data() + outLen1, &outLen2) != 1)
    {
        EVP_CIPHER_CTX_free(ctx);
        throw std::runtime_error("EVP_DecryptFinal_ex failed (bad key or padding)");
    }

    EVP_CIPHER_CTX_free(ctx);
    out.resize(static_cast<size_t>(outLen1 + outLen2));
    return out;
}

std::vector<uint8_t> Converter::encryptInitfsPayloadAes(
    const std::vector<uint8_t>& plainData,
    const std::vector<uint8_t>& key)
{
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) throw std::runtime_error("EVP_CIPHER_CTX_new failed");

    if (EVP_EncryptInit_ex(ctx, EVP_aes_128_cbc(), nullptr,
        key.data(), key.data()) != 1)
    {
        EVP_CIPHER_CTX_free(ctx);
        throw std::runtime_error("EVP_EncryptInit_ex failed");
    }

    std::vector<uint8_t> out(plainData.size() + EVP_MAX_BLOCK_LENGTH);
    int outLen1 = 0, outLen2 = 0;

    EVP_EncryptUpdate(ctx, out.data(), &outLen1,
        plainData.data(), static_cast<int>(plainData.size()));
    EVP_EncryptFinal_ex(ctx, out.data() + outLen1, &outLen2);
    EVP_CIPHER_CTX_free(ctx);

    out.resize(static_cast<size_t>(outLen1 + outLen2));
    return out;
}

// ============================================================================
// Obfuscated writers
// ============================================================================

void Converter::obfuscateInitfsFromPlainData(
    const std::string& originalEncryptedFile,
    const std::vector<uint8_t>& plainData,
    const std::string& outputFile,
    const std::vector<uint8_t>& key)
{
    Logger::log("[ObfuscateInitfsFromPlainData] %s -> %s",
        originalEncryptedFile.c_str(), outputFile.c_str());

    if (key.size() != 16)
        throw std::runtime_error("AES key must be exactly 16 bytes");

    DeobfuscatorType dt = autoDetectDeobfuscatorType(originalEncryptedFile);
    auto deobfuscator = createDeobfuscator(dt);

    std::ifstream fs(originalEncryptedFile, std::ios::binary);
    if (!fs) throw std::runtime_error("Cannot open: " + originalEncryptedFile);
    DbReader hdrReader(fs, deobfuscator);
    int headerSize = static_cast<int>(hdrReader.position());
    hdrReader.setPosition(0);
    auto obfuscationHeader = hdrReader.readBytes(headerSize);

    auto encrypted = encryptInitfsPayloadAes(plainData, key);

    auto root = DbObject::createObject();
    root->addValue("encrypted", DbValue(encrypted));

    std::ofstream outFs(outputFile, std::ios::binary);
    if (!outFs) throw std::runtime_error("Cannot write: " + outputFile);
    DbWriter writer(outFs);
    writer.write(obfuscationHeader);
    writer.write(root);

    Logger::log("[ObfuscateInitfsFromPlainData] Done -> %s", outputFile.c_str());
}

void Converter::writeDeobfuscatedInitfsFromDbObject(
    const std::string& originalFilePath,
    DbObjectPtr editedListRoot,
    const std::string& outputFile,
    DeobfuscatorType type)
{
    if (!fs::exists(originalFilePath))
        throw std::runtime_error("Original file not found: " + originalFilePath);
    if (!editedListRoot)
        throw std::invalid_argument("Root cannot be null");

    Logger::log("[SaveInitfs] Rebuilding initfs...");

    DeobfuscatorType dt = (type == DeobfuscatorType::PVZ) ? DeobfuscatorType::DA : type;
    auto deobfuscator = createDeobfuscator(dt);

    std::ifstream fs(originalFilePath, std::ios::binary);
    if (!fs) throw std::runtime_error("Cannot open: " + originalFilePath);
    DbReader hdrReader(fs, deobfuscator);
    int headerSize = static_cast<int>(hdrReader.position());
    hdrReader.setPosition(0);
    auto headerBytes = hdrReader.readBytes(headerSize);

    std::ofstream outFs(outputFile, std::ios::binary);
    if (!outFs) throw std::runtime_error("Cannot write: " + outputFile);
    DbWriter writer(outFs);
    writer.write(headerBytes);
    writer.write(editedListRoot);

    Logger::log("[SaveInitfs] Wrote to %s", outputFile.c_str());
}

// ---- BF3 helpers ----

int Converter::parseBF3BodyStart(const std::vector<uint8_t>& src)
{
    constexpr int bodyStart = 0x22C;
    if (src.size() < bodyStart)
        throw std::runtime_error("Source too small for BF3 header");
    return bodyStart;
}

std::vector<uint8_t> Converter::getBF3Key(const std::vector<uint8_t>& src)
{
    constexpr int tableOffset = 0x128;
    constexpr int keyLen = 0x101;
    if (src.size() < tableOffset + keyLen)
        throw std::runtime_error("Source too small for BF3 key table");
    return std::vector<uint8_t>(src.begin() + tableOffset,
        src.begin() + tableOffset + keyLen);
}

void Converter::bf3XorInPlace(std::vector<uint8_t>& data,
    const std::vector<uint8_t>& key,
    int startOffsetInBody)
{
    if (key.size() != 0x101)
        throw std::runtime_error("Invalid BF3 key length");
    for (size_t i = 0; i < data.size(); i++)
    {
        int keyIndex = (startOffsetInBody + static_cast<int>(i)) % 0x101;
        data[i] ^= key[keyIndex] ^ 0x7B;
    }
}

void Converter::writeBF3ObfuscatedInitfs(
    const std::string& sourcePath,
    DbObjectPtr obj,
    const std::string& outputPath)
{
    Logger::log("[WriteBF3ObfuscatedInitfs] %s -> %s",
        sourcePath.c_str(), outputPath.c_str());

    std::ifstream in(sourcePath, std::ios::binary);
    if (!in) throw std::runtime_error("Cannot open: " + sourcePath);
    std::vector<uint8_t> src((std::istreambuf_iterator<char>(in)),
        std::istreambuf_iterator<char>());

    int bodyStart = parseBF3BodyStart(src);
    auto key = getBF3Key(src);
    auto plain = writePlainFileData(obj);
    bf3XorInPlace(plain, key, 0);

    std::ofstream out(outputPath, std::ios::binary);
    if (!out) throw std::runtime_error("Cannot write: " + outputPath);
    out.write(reinterpret_cast<const char*>(src.data()), bodyStart);
    out.write(reinterpret_cast<const char*>(plain.data()), plain.size());

    Logger::log("[WriteBF3ObfuscatedInitfs] header=%d body=%zu",
        bodyStart, plain.size());
}

// ---- PVZ helpers ----

int Converter::getPvzBodyStart(const std::vector<uint8_t>& src)
{
    constexpr int bodyStart = 0x22C;
    if (src.size() < bodyStart)
        throw std::runtime_error("Source too small for PVZ header");
    return bodyStart;
}

std::vector<uint8_t> Converter::getPvzKey(const std::vector<uint8_t>& src)
{
    constexpr int tableOffset = 0x128;
    constexpr int keyLen = 0x101;
    if (src.size() < tableOffset + keyLen)
        throw std::runtime_error("Source too small for PVZ key table");
    std::vector<uint8_t> key(src.begin() + tableOffset,
        src.begin() + tableOffset + keyLen);
    for (auto& b : key) b ^= 123;
    return key;
}

void Converter::pvzXorInPlace(std::vector<uint8_t>& data,
    const std::vector<uint8_t>& key,
    int startOffsetInBody)
{
    if (key.size() != 0x101)
        throw std::runtime_error("Invalid PVZ key length");
    for (size_t i = 0; i < data.size(); i++)
        data[i] ^= key[(startOffsetInBody + static_cast<int>(i)) % 0x101];
}

void Converter::writePvzObfuscatedInitfs(
    const std::string& sourcePath,
    DbObjectPtr obj,
    const std::string& outputPath)
{
    Logger::log("[WritePvzObfuscatedInitfs] %s -> %s",
        sourcePath.c_str(), outputPath.c_str());

    std::ifstream in(sourcePath, std::ios::binary);
    if (!in) throw std::runtime_error("Cannot open: " + sourcePath);
    std::vector<uint8_t> src((std::istreambuf_iterator<char>(in)),
        std::istreambuf_iterator<char>());

    int bodyStart = getPvzBodyStart(src);
    auto key = getPvzKey(src);
    auto plain = writePlainFileData(obj);
    pvzXorInPlace(plain, key, 0);

    std::ofstream out(outputPath, std::ios::binary);
    if (!out) throw std::runtime_error("Cannot write: " + outputPath);
    out.write(reinterpret_cast<const char*>(src.data()), bodyStart);
    out.write(reinterpret_cast<const char*>(plain.data()), plain.size());

    Logger::log("[WritePvzObfuscatedInitfs] header=%d body=%zu",
        bodyStart, plain.size());
}

// ============================================================================
// Utilities
// ============================================================================

std::vector<uint8_t> Converter::hexToBinary(const std::string& hex)
{
    if (hex.empty()) return {};
    if (hex.size() % 2 != 0)
        throw std::runtime_error("Hex string length must be even");
    std::vector<uint8_t> bytes(hex.size() / 2);
    for (size_t i = 0; i < bytes.size(); i++)
    {
        unsigned int v;
        if (std::sscanf(hex.c_str() + i * 2, "%02x", &v) != 1)
            throw std::runtime_error("Invalid hex character");
        bytes[i] = static_cast<uint8_t>(v);
    }
    return bytes;
}

std::string Converter::binaryToHex(const std::vector<uint8_t>& data)
{
    static const char* h = "0123456789ABCDEF";
    std::string s;
    s.reserve(data.size() * 2);
    for (uint8_t b : data) { s += h[b >> 4]; s += h[b & 0xF]; }
    return s;
}

bool Converter::isWin32Initfs(const std::string& path)
{
    if (path.empty()) return false;
    std::string name = fs::path(path).filename().string();
    std::string lower = name;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    return lower.find("initfs_win32") != std::string::npos;
}

bool Converter::sha256Equal(const std::vector<uint8_t>& a,
    const std::vector<uint8_t>& b)
{
    uint8_t ha[SHA256_DIGEST_LENGTH], hb[SHA256_DIGEST_LENGTH];
    SHA256(a.data(), a.size(), ha);
    SHA256(b.data(), b.size(), hb);
    return std::memcmp(ha, hb, SHA256_DIGEST_LENGTH) == 0;
}

void Converter::tryUnblock(const std::string& path)
{
#ifdef _WIN32
    std::string ads = path + ":Zone.Identifier";
    std::remove(ads.c_str());
#else
    (void)path;
#endif
}

void Converter::updatePayload(DbObjectPtr dbObj, int payloadIndex,
    const std::vector<uint8_t>& newPayload)
{
    int index = 0;
    for (size_t i = 0; i < static_cast<size_t>(dbObj->count()); i++)
    {
        DbValue& item = (*dbObj)[i];
        if (auto* childPtr = std::get_if<DbObjectPtr>(&item))
        {
            DbObjectPtr child = *childPtr;
            if (child->hasValue("$file"))
            {
                if (index == payloadIndex)
                {
                    DbObjectPtr fileObj =
                        child->getValue<DbObjectPtr>("$file");
                    fileObj->setValue("payload", DbValue(newPayload));
                    if (fileObj->hasValue("length"))
                        fileObj->setValue("length",
                            DbValue(static_cast<int32_t>(newPayload.size())));
                    return;
                }
                index++;
            }
        }
    }
    throw std::runtime_error("Payload index not found in DbObject");
}