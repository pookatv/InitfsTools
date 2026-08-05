#include "BuildInfoWindow.h"
#include "MainWindow.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QTableWidget>
#include <QHeaderView>
#include <QPushButton>
#include <QFileDialog>
#include <QFileInfo>
#include <QDir>
#include <QFileIconProvider>
#include <QMessageBox>
#include <QSettings>
#include <QStyle>
#include <QApplication>
#include <QScrollArea>

#ifdef Q_OS_WIN
#  include <windows.h>
#  include <winver.h>
#  pragma comment(lib, "Version.lib")
#endif

#include <cstring>
#include <cstdint>
#include <ctime>
#include <vector>
#include <QFile>
#include <QCryptographicHash>
#include <QDirIterator>
#include <QPixmap>
#include <QXmlStreamReader>
#include <QRegularExpression>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>

// ============================================================
// SEH-guarded raw memory helpers — free functions only, no local
// C++ objects with destructors, so __try/__except stays legal
// alongside /EHsc in the rest of the project
// ============================================================
#ifdef Q_OS_WIN
namespace {

    bool SafeCallFactory(void* fn, void** outObj)
    {
        __try
        {
            using FactoryFn = void* (*)();
            *outObj = reinterpret_cast<FactoryFn>(fn)();
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    bool SafeReadBytes(const void* addr, void* dst, size_t n)
    {
        __try
        {
            memcpy(dst, addr, n);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    bool SafeReadPtr(const void* addr, void** out)
    {
        return SafeReadBytes(addr, out, sizeof(void*));
    }

    // Reads a bounded, NUL-terminated ASCII string starting at addr
    // Returns false if the memory couldn't be read at all
    bool SafeReadCString(const void* addr, QString& out, int maxLen = 4096)
    {
        std::vector<char> buf(maxLen + 1, 0);
        // Read in growing chunks guarded individually, since a string
        // may sit right at the end of a mapped region
        int chunk = 256;
        int total = 0;
        while (total < maxLen)
        {
            int want = qMin(chunk, maxLen - total);
            if (!SafeReadBytes(reinterpret_cast<const char*>(addr) + total,
                buf.data() + total, (size_t)want))
            {
                if (total == 0) return false;
                break; // keep what we already safely read
            }
            // Stop early if we already hit a NUL
            bool foundNul = false;
            for (int i = total; i < total + want; i++)
                if (buf[i] == '\0') { foundNul = true; break; }
            total += want;
            if (foundNul) break;
        }
        buf[maxLen] = '\0';
        out = QString::fromUtf8(buf.data());
        return true;
    }

    template<typename T>
    T* RVA(HMODULE hMod, DWORD rva)
    {
        return reinterpret_cast<T*>(reinterpret_cast<BYTE*>(hMod) + rva);
    }

    // Reads the FILE version explicitly from the numeric VS_FIXEDFILEINFO
    // block rather than the free-text StringFileInfo "FileVersion" entry
    bool GetFixedFileVersion(const QString& path, QString& out)
    {
        std::wstring wpath = path.toStdWString();
        DWORD handle = 0;
        DWORD size = GetFileVersionInfoSizeW(wpath.c_str(), &handle);
        if (size == 0) return false;

        std::vector<BYTE> buffer(size);
        if (!GetFileVersionInfoW(wpath.c_str(), handle, size, buffer.data()))
            return false;

        VS_FIXEDFILEINFO* fixedInfo = nullptr;
        UINT fixedInfoLen = 0;
        if (!VerQueryValueW(buffer.data(), L"\\", reinterpret_cast<LPVOID*>(&fixedInfo), &fixedInfoLen)
            || !fixedInfo || fixedInfoLen < sizeof(VS_FIXEDFILEINFO))
            return false;

        out = QString("%1.%2.%3.%4")
            .arg(HIWORD(fixedInfo->dwFileVersionMS))
            .arg(LOWORD(fixedInfo->dwFileVersionMS))
            .arg(HIWORD(fixedInfo->dwFileVersionLS))
            .arg(LOWORD(fixedInfo->dwFileVersionLS));
        return true;
    }

    // ============================================================
    // Static (non-executing) PE image reader. Used only when the target
    // DLL's architecture doesn't match this process — LoadLibrary would
    // simply refuse such a file, so instead we read section data
    // straight out of the bytes on disk and never run a single
    // instruction of the target
    // ============================================================
    struct StaticPeImage
    {
        QByteArray                bytes;
        bool                      is64 = false;
        ULONGLONG                 imageBase = 0;
        const IMAGE_SECTION_HEADER* sections = nullptr;
        WORD                      sectionCount = 0;
    };

    // RVA -> pointer into the on-disk file buffer, honoring each
    // section's VirtualAddress/PointerToRawData/SizeOfRawData. Returns
    // nullptr if the RVA isn't backed by any section's raw data
    const uint8_t* RvaToPtr(const StaticPeImage& img, DWORD rva, size_t neededBytes = 1)
    {
        for (WORD i = 0; i < img.sectionCount; i++)
        {
            const IMAGE_SECTION_HEADER& s = img.sections[i];
            DWORD vSize = s.Misc.VirtualSize ? s.Misc.VirtualSize : s.SizeOfRawData;
            if (rva >= s.VirtualAddress && rva < s.VirtualAddress + vSize)
            {
                DWORD fileOffset = s.PointerToRawData + (rva - s.VirtualAddress);
                if ((size_t)fileOffset + neededBytes > (size_t)img.bytes.size())
                    return nullptr;
                return reinterpret_cast<const uint8_t*>(img.bytes.constData()) + fileOffset;
            }
        }
        return nullptr;
    }

    // Absolute VA (as embedded directly in x86 direct-addressing
    // instructions) -> RVA, using the image's preferred load address
    bool VaToRva(const StaticPeImage& img, ULONGLONG va, DWORD& outRva)
    {
        if (va < img.imageBase) return false;
        ULONGLONG rva64 = va - img.imageBase;
        if (rva64 > 0xFFFFFFFFull) return false;
        outRva = (DWORD)rva64;
        return true;
    }

    // Follows a direct "jmp rel32" (opcode E9) thunk to its resolved
    // target RVA. Incremental-linker builds commonly route both the
    // export table AND individual vtable getters through such stubs, so
    // this is applied uniformly wherever we resolve a function address,
    // rather than only at the vtable-slot level. Returns the RVA
    // unchanged if it isn't a thunk. Bounded hop count guards against
    // malformed or cyclic input
    DWORD ResolveJmpThunk(const StaticPeImage& img, DWORD rva, int maxHops = 4)
    {
        DWORD cur = rva;
        for (int hop = 0; hop < maxHops; hop++)
        {
            const uint8_t* code = RvaToPtr(img, cur, 5);
            if (!code || code[0] != 0xE9) break;
            int32_t rel;
            memcpy(&rel, code + 1, 4);
            ULONGLONG funcVA = img.imageBase + cur;
            ULONGLONG targetVA = funcVA + 5 + (int64_t)rel;
            DWORD targetRVA = 0;
            if (!VaToRva(img, targetVA, targetRVA)) break;
            cur = targetRVA;
        }
        return cur;
    }

    // Reads a printable, NUL-terminated ASCII string at an RVA. Returns
    // false (rather than garbage) if the bytes there don't look like a
    // real string — this is also how we distinguish "string field" from
    // "raw int field" for 32-bit thunks, which use the same opcode for
    // both (see extractDynamicFieldsStatic)
    bool ReadStaticCString(const StaticPeImage& img, DWORD rva, QString& out, int maxLen = 4096)
    {
        const uint8_t* p = RvaToPtr(img, rva, 1);
        if (!p) return false;
        const uint8_t* bufEnd = reinterpret_cast<const uint8_t*>(img.bytes.constData()) + img.bytes.size();
        int len = 0;
        while (p + len < bufEnd && len < maxLen && p[len] != 0)
        {
            if (p[len] < 0x20 || p[len] > 0x7E) return false; // not printable -> not a real string
            len++;
        }
        if (p + len >= bufEnd) return false;
        out = QString::fromLatin1(reinterpret_cast<const char*>(p), len);
        return true;
    }

    // Parses the common PE32/PE32+ structures (DOS/NT/optional headers,
    // sections) shared by every static reader — this used to be
    // duplicated inline in extractDynamicFieldsStatic; factored out here
    // so extractDebugInfoStatic can reuse it rather than re-parsing the
    // same headers a second time
    bool ParseStaticPeImage(const QByteArray& bytes, StaticPeImage& img,
        const IMAGE_FILE_HEADER** outFileHeader, const uint8_t** outOptHeaderPtr, WORD* outMagic)
    {
        if (bytes.size() < (int)sizeof(IMAGE_DOS_HEADER)) return false;
        const uint8_t* base = reinterpret_cast<const uint8_t*>(bytes.constData());
        auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
        if (dos->e_lfanew <= 0
            || (size_t)dos->e_lfanew + sizeof(DWORD) + sizeof(IMAGE_FILE_HEADER) > (size_t)bytes.size())
            return false;

        DWORD ntSig = *reinterpret_cast<const DWORD*>(base + dos->e_lfanew);
        if (ntSig != IMAGE_NT_SIGNATURE) return false;

        auto* fileHeader = reinterpret_cast<const IMAGE_FILE_HEADER*>(base + dos->e_lfanew + sizeof(DWORD));
        const uint8_t* optHeaderPtr = base + dos->e_lfanew + sizeof(DWORD) + sizeof(IMAGE_FILE_HEADER);
        WORD magic = *reinterpret_cast<const WORD*>(optHeaderPtr);

        img.bytes = bytes;

        if (magic == IMAGE_NT_OPTIONAL_HDR32_MAGIC)
        {
            img.is64 = false;
            auto* opt = reinterpret_cast<const IMAGE_OPTIONAL_HEADER32*>(optHeaderPtr);
            img.imageBase = opt->ImageBase;
            img.sections = reinterpret_cast<const IMAGE_SECTION_HEADER*>(optHeaderPtr + fileHeader->SizeOfOptionalHeader);
        }
        else if (magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC)
        {
            img.is64 = true;
            auto* opt = reinterpret_cast<const IMAGE_OPTIONAL_HEADER64*>(optHeaderPtr);
            img.imageBase = opt->ImageBase;
            img.sections = reinterpret_cast<const IMAGE_SECTION_HEADER*>(optHeaderPtr + fileHeader->SizeOfOptionalHeader);
        }
        else
        {
            return false; // neither PE32 nor PE32+
        }
        img.sectionCount = fileHeader->NumberOfSections;

        if (outFileHeader) *outFileHeader = fileHeader;
        if (outOptHeaderPtr) *outOptHeaderPtr = optHeaderPtr;
        if (outMagic) *outMagic = magic;
        return true;
    }

    // Looks up a data directory entry (export table, debug directory,
    // etc.) from an already-parsed optional header, regardless of
    // whether the image is PE32 or PE32+
    bool GetDataDirectory(const uint8_t* optHeaderPtr, WORD magic, int dirIndex, DWORD& outRva, DWORD& outSize)
    {
        if (magic == IMAGE_NT_OPTIONAL_HDR32_MAGIC)
        {
            auto* opt = reinterpret_cast<const IMAGE_OPTIONAL_HEADER32*>(optHeaderPtr);
            if ((DWORD)dirIndex >= opt->NumberOfRvaAndSizes) return false;
            outRva = opt->DataDirectory[dirIndex].VirtualAddress;
            outSize = opt->DataDirectory[dirIndex].Size;
            return true;
        }
        if (magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC)
        {
            auto* opt = reinterpret_cast<const IMAGE_OPTIONAL_HEADER64*>(optHeaderPtr);
            if ((DWORD)dirIndex >= opt->NumberOfRvaAndSizes) return false;
            outRva = opt->DataDirectory[dirIndex].VirtualAddress;
            outSize = opt->DataDirectory[dirIndex].Size;
            return true;
        }
        return false;
    }

    // Durango PE files are otherwise ordinary PE32+/AMD64
    // images — identical DOS/NT header shape to any x64 Windows DLL — so
    // machine type alone can't tell them apart from something we can
    // actually LoadLibrary. What gives them away is their import table:
    // Durango titles link against kernelx.dll (the platform's kernel32
    // equivalent) rather than kernel32.dll, a name that simply doesn't
    // resolve off-console — that mismatch is what produces "Win32 error
    // 126" when LoadLibraryW is attempted. This scans the import
    // directory purely statically (no LoadLibrary, no execution) looking
    // for that signature DLL name
    bool IsXboxOnePe(const QByteArray& bytes)
    {
        StaticPeImage img;
        const uint8_t* optHeaderPtr = nullptr;
        WORD magic = 0;
        if (!ParseStaticPeImage(bytes, img, nullptr, &optHeaderPtr, &magic))
            return false;

        DWORD importRVA = 0, importSize = 0;
        if (!GetDataDirectory(optHeaderPtr, magic, IMAGE_DIRECTORY_ENTRY_IMPORT, importRVA, importSize)
            || importRVA == 0)
            return false;

        constexpr int kMaxDescriptors = 256; // guard against a corrupt/cyclic table
        for (int i = 0; i < kMaxDescriptors; i++)
        {
            const auto* desc = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(
                RvaToPtr(img, importRVA + (DWORD)(i * sizeof(IMAGE_IMPORT_DESCRIPTOR)),
                    sizeof(IMAGE_IMPORT_DESCRIPTOR)));
            if (!desc || desc->Name == 0) break; // null descriptor terminates the array

            QString dllName;
            if (ReadStaticCString(img, desc->Name, dllName, 260)
                && dllName.compare("kernelx.dll", Qt::CaseInsensitive) == 0)
            {
                return true;
            }
        }
        return false;
    }

    // ============================================================
    // Static (non-executing) PS4 .prx (ELF64 + SCE dynamic-linking
    // extensions) reader. Structurally unrelated to the PE reader above
    // — different container format entirely — so it gets its own
    // parallel set of types/helpers rather than trying to force-fit PE
    // concepts (RVAs, sections-as-VA-ranges) onto ELF program headers
    // and SCE relocation tables
    // ============================================================
#pragma pack(push, 1)
    struct Elf64_Ehdr
    {
        uint8_t  e_ident[16];
        uint16_t e_type;
        uint16_t e_machine;
        uint32_t e_version;
        uint64_t e_entry;
        uint64_t e_phoff;
        uint64_t e_shoff;
        uint32_t e_flags;
        uint16_t e_ehsize;
        uint16_t e_phentsize;
        uint16_t e_phnum;
        uint16_t e_shentsize;
        uint16_t e_shnum;
        uint16_t e_shstrndx;
    };
    struct Elf64_Phdr
    {
        uint32_t p_type;
        uint32_t p_flags;
        uint64_t p_offset;
        uint64_t p_vaddr;
        uint64_t p_paddr;
        uint64_t p_filesz;
        uint64_t p_memsz;
        uint64_t p_align;
    };
    struct Elf64_Dyn
    {
        int64_t  d_tag;
        uint64_t d_val;
    };
    struct Elf64_Rela
    {
        uint64_t r_offset;
        uint64_t r_info;
        int64_t  r_addend;
    };
    struct Elf64_Sym
    {
        uint32_t st_name;
        uint8_t  st_info;
        uint8_t  st_other;
        uint16_t st_shndx;
        uint64_t st_value;
        uint64_t st_size;
    };
    struct Elf64_Shdr
    {
        uint32_t sh_name;
        uint32_t sh_type;
        uint64_t sh_flags;
        uint64_t sh_addr;
        uint64_t sh_offset;
        uint64_t sh_size;
        uint32_t sh_link;
        uint32_t sh_info;
        uint64_t sh_addralign;
        uint64_t sh_entsize;
    };
#pragma pack(pop)

    constexpr uint32_t kPT_LOAD = 1;
    constexpr uint32_t kPT_DYNAMIC = 2;
    // PS4-specific: DT_SCE_SYMTAB/STRTAB/RELA below are byte offsets
    // relative to this segment's start in the file — NOT ordinary VAs
    // resolvable through PT_LOAD, unlike standard ELF's DT_SYMTAB etc
    constexpr uint32_t kPT_SCE_DYNLIBDATA = 0x61000000;
    constexpr int64_t  kDT_SCE_ORIGINAL_FILENAME = 0x61000009;
    constexpr int64_t  kDT_SCE_STRTAB = 0x61000035;
    constexpr int64_t  kDT_SCE_STRSZ = 0x61000037;
    constexpr int64_t  kDT_SCE_SYMTAB = 0x61000039;
    constexpr int64_t  kDT_SCE_SYMTABSZ = 0x6100003F;
    constexpr int64_t  kDT_SCE_RELA = 0x6100002F;
    constexpr int64_t  kDT_SCE_RELASZ = 0x61000031;
    constexpr uint32_t kR_X86_64_RELATIVE = 8;
    constexpr uint8_t  kSTT_FUNC = 2;
    constexpr uint8_t  kSTB_GLOBAL = 1;

    // Switch/.nrs: a completely ordinary ELF64/AArch64 shared object —
    // no SCE dynamic-tag extensions, no SELF wrapping — so it gets
    // standard-ELF constants rather than PS4's kDT_SCE_* ones
    constexpr uint32_t kEM_AARCH64 = 183;
    constexpr uint32_t kR_AARCH64_RELATIVE = 1027;
    constexpr uint8_t  kSTT_OBJECT = 1;

    // The NID for getBuildInfo, confirmed identical across every
    // Engine.BuildInfo_gen4b_retail.prx sample seen so far (PS4's NID
    // is a deterministic hash of symbol name + library, so this is
    // expected to be stable for this specific library/function pair
    // across titles built against the same SDK generation).
    const char* kGetBuildInfoNid = "UCIrgrj61F0#C#A";

    struct ElfImage
    {
        QByteArray             bytes;
        QVector<Elf64_Phdr>    loadSegments;      // PT_LOAD only
        QHash<uint64_t, uint64_t> relocMap;        // R_X86_64_RELATIVE / R_AARCH64_RELATIVE: offset(VA) -> resolved target(VA)
        uint64_t dynstrVA = 0, dynstrSize = 0;
        uint64_t dynsymVA = 0, dynsymSize = 0;
        uint64_t relaVA = 0, relaSize = 0;
        uint64_t dynlibDataOffset = 0;
        bool     haveDynlibData = false;
        // Set by ParseElfImage from the ORIGINAL on-disk bytes, before any
        // unwrapping — true if this .prx was SCE SELF-wrapped (the normal
        // format for retail/patch files on disk) and had to be
        // reconstructed into a flat ELF; false if it was already bare
        bool     wasSelfWrapped = false;

        // Switch/.nrs only: file offsets of .comment (compiler/linker
        // version strings) and .note.gnu.build-id, populated by
        // ParseNrsElfImage. Left at 0 (and simply unused) for PS4/.prx
        uint64_t commentOff = 0, commentSize = 0;
        uint64_t buildIdOff = 0, buildIdSize = 0;
    };

    // VA -> file offset via PT_LOAD program headers (ELF's equivalent
    // of the PE reader's section-based RvaToPtr)
    bool ElfVaToFileOffset(const ElfImage& img, uint64_t va, uint64_t& outOffset)
    {
        for (const auto& ph : img.loadSegments)
        {
            if (va >= ph.p_vaddr && va < ph.p_vaddr + ph.p_filesz)
            {
                outOffset = ph.p_offset + (va - ph.p_vaddr);
                return true;
            }
        }
        return false;
    }

    const uint8_t* ElfPtr(const ElfImage& img, uint64_t va, size_t neededBytes = 1)
    {
        uint64_t off = 0;
        if (!ElfVaToFileOffset(img, va, off)) return nullptr;
        if (off + neededBytes > (uint64_t)img.bytes.size()) return nullptr;
        return reinterpret_cast<const uint8_t*>(img.bytes.constData()) + off;
    }

    bool ReadElfCString(const ElfImage& img, uint64_t va, QString& out, int maxLen = 4096)
    {
        const uint8_t* p = ElfPtr(img, va, 1);
        if (!p) return false;
        uint64_t off = 0;
        ElfVaToFileOffset(img, va, off);
        const uint8_t* bufEnd = reinterpret_cast<const uint8_t*>(img.bytes.constData()) + img.bytes.size();
        int len = 0;
        while (p + len < bufEnd && len < maxLen && p[len] != 0)
        {
            if (p[len] < 0x20 || p[len] > 0x7E) return false; // not printable -> not a real string
            len++;
        }
        if (p + len >= bufEnd) return false;
        out = QString::fromLatin1(reinterpret_cast<const char*>(p), len);
        return true;
    }

    // Same as ReadElfCString, but takes a raw absolute FILE OFFSET
    // directly instead of a VA — for reading out of the dynlibdata
    // symtab/strtab region, which isn't part of the normal PT_LOAD
    // VA-mapped address space on PS4
    bool ReadRawCString(const QByteArray& bytes, uint64_t fileOffset, QString& out, int maxLen = 4096)
    {
        if (fileOffset >= (uint64_t)bytes.size()) return false;
        const uint8_t* p = reinterpret_cast<const uint8_t*>(bytes.constData()) + fileOffset;
        const uint8_t* bufEnd = reinterpret_cast<const uint8_t*>(bytes.constData()) + bytes.size();
        int len = 0;
        while (p + len < bufEnd && len < maxLen && p[len] != 0)
        {
            if (p[len] < 0x20 || p[len] > 0x7E) return false;
            len++;
        }
        if (p + len >= bufEnd) return false;
        out = QString::fromLatin1(reinterpret_cast<const char*>(p), len);
        return true;
    }

    QString ElfSegmentTypeName(uint32_t type)
    {
        switch (type)
        {
        case 0:  return "PT_NULL";
        case 1:  return "PT_LOAD";
        case 2:  return "PT_DYNAMIC";
        case 3:  return "PT_INTERP";
        case 4:  return "PT_NOTE";
        case 6:  return "PT_PHDR";
        case 7:  return "PT_TLS";
        case 0x6474E550: return "PT_GNU_EH_FRAME";
        case 0x6474E551: return "PT_GNU_STACK";
        case 0x6474E552: return "PT_GNU_RELRO";
        case 0x61000000: return "PT_SCE_DYNLIBDATA";
        case 0x61000001: return "PT_SCE_PROCPARAM";
        case 0x61000002: return "PT_SCE_MODULE_PARAM";
        case 0x61000010: return "PT_SCE_RELRO";
        case 0x6FFFFF00: return "PT_SCE_COMMENT";
        case 0x6FFFFF01: return "PT_SCE_LIBVERSION";
        default: return QString("0x%1").arg(type, 0, 16);
        }
    }

#pragma pack(push, 1)
    // Fixed-size SELF container header — signature/segment metadata that
    // Sony prepends to the real ELF for retail .prx packaging
    struct SceSelfHeader
    {
        uint32_t magic;
        uint8_t  version;
        uint8_t  mode;
        uint8_t  endian;
        uint8_t  attribs;
        uint32_t key_type;
        uint16_t header_size;
        uint16_t meta_size;
        uint64_t file_size;
        uint16_t num_entries;
        uint16_t flags;
        uint32_t reserved;
    };
    // One entry per stored segment block. Entries come in pairs: a fixed
    // 0x20-byte per-block info record (filesz always == sizeof(SceSelfEntry)),
    // followed by the segment's real content. `offset`/`filesz` here are
    // the block's PHYSICAL position in the .prx file — unrelated to the
    // matching program header's own p_offset, which instead describes
    // where those same bytes belong in the reconstructed flat ELF
    struct SceSelfEntry
    {
        uint64_t props;
        uint64_t offset;
        uint64_t filesz;
        uint64_t memsz;
    };
#pragma pack(pop)

    // PS4 retail .prx files are normally wrapped in an SCE SELF container
    // The embedded ELF found right after the segment-info table is only a
    // skeleton — its own ELF header + program header table, with NO
    // segment data inline. Each segment's real bytes live in a separate
    // self_entry_t-referenced block elsewhere in the file, at a physical
    // offset that has nothing to do with that segment's own p_offset
    QByteArray UnwrapSelfIfPresent(const QByteArray& bytes)
    {
        constexpr uint32_t kSelfMagic = 0x1D3D154F;
        if (bytes.size() < (int)sizeof(SceSelfHeader)) return bytes;

        auto* sh = reinterpret_cast<const SceSelfHeader*>(bytes.constData());
        if (sh->magic != kSelfMagic) return bytes; // already a bare ELF, or unrecognized

        uint64_t entryTableOff = sizeof(SceSelfHeader);
        uint64_t elfOff = entryTableOff + (uint64_t)sh->num_entries * sizeof(SceSelfEntry);
        if (elfOff + sizeof(Elf64_Ehdr) > (uint64_t)bytes.size()) return bytes; // malformed

        const uint8_t* base = reinterpret_cast<const uint8_t*>(bytes.constData());
        auto* miniEh = reinterpret_cast<const Elf64_Ehdr*>(base + elfOff);
        if (miniEh->e_ident[0] != 0x7F || miniEh->e_ident[1] != 'E' || miniEh->e_ident[2] != 'L' || miniEh->e_ident[3] != 'F')
            return bytes; // no embedded ELF skeleton where we expected one

        // Read every program header from the skeleton (offsets below are
        // relative to elfOff, matching the skeleton's own internal e_phoff)
        struct PhdrInfo { uint64_t offset, filesz; };
        QList<PhdrInfo> phdrs;
        uint64_t maxReach = 0;
        for (int i = 0; i < miniEh->e_phnum; i++)
        {
            uint64_t phAddr = elfOff + miniEh->e_phoff + (uint64_t)i * miniEh->e_phentsize;
            if (phAddr + sizeof(Elf64_Phdr) > (uint64_t)bytes.size()) break;
            auto* ph = reinterpret_cast<const Elf64_Phdr*>(base + phAddr);
            phdrs.append({ ph->p_offset, ph->p_filesz });
            if (ph->p_filesz > 0)
                maxReach = qMax(maxReach, ph->p_offset + ph->p_filesz);
        }
        if (phdrs.isEmpty() || maxReach == 0) return bytes;

        // Map each segment's real content size to its target offset in the
        // reconstructed flat ELF. Sizes are only ambiguous if two segments
        // happen to share an identical byte length — every sample this was
        // verified against had unique sizes, but if it happens, both are
        // skipped rather than guessed at
        QHash<uint64_t, uint64_t> sizeToOffset;
        QSet<uint64_t> ambiguousSizes;
        for (const auto& ph : phdrs)
        {
            if (ph.filesz == 0) continue;
            if (sizeToOffset.contains(ph.filesz)) ambiguousSizes.insert(ph.filesz);
            else sizeToOffset.insert(ph.filesz, ph.offset);
        }
        for (uint64_t s : ambiguousSizes) sizeToOffset.remove(s);

        QByteArray recon(static_cast<int>(maxReach), '\0');

        // The skeleton's own header + program header table already sit at
        // virtual offset 0 (its e_phoff is self-relative, exactly like a
        // standalone flat ELF's would be) — copy that region verbatim
        uint64_t headerRegionLen = miniEh->e_phoff + (uint64_t)miniEh->e_phnum * miniEh->e_phentsize;
        headerRegionLen = qMin<uint64_t>(headerRegionLen, (uint64_t)recon.size());
        memcpy(recon.data(), base + elfOff, (size_t)headerRegionLen);

        // Skip the 0x20-byte per-block info records — they're metadata,
        // not segment data — and place every real data block at its
        // matching phdr's own offset
        int matched = 0;
        for (int i = 0; i < sh->num_entries; i++)
        {
            uint64_t eOff = entryTableOff + (uint64_t)i * sizeof(SceSelfEntry);
            if (eOff + sizeof(SceSelfEntry) > (uint64_t)bytes.size()) break;
            auto* entry = reinterpret_cast<const SceSelfEntry*>(base + eOff);
            if (entry->filesz == sizeof(SceSelfEntry)) continue; // per-block info record

            auto it = sizeToOffset.constFind(entry->filesz);
            if (it == sizeToOffset.constEnd()) continue; // no unique matching segment

            uint64_t destOff = it.value();
            if (entry->offset + entry->filesz > (uint64_t)bytes.size()) continue;
            if (destOff + entry->filesz > (uint64_t)recon.size()) continue;
            memcpy(recon.data() + destOff, base + entry->offset, (size_t)entry->filesz);
            matched++;
        }

        return matched > 0 ? recon : bytes;
    }

    bool ParseElfImage(const QByteArray& rawBytes, ElfImage& img)
    {
        constexpr uint32_t kSelfMagicCheck = 0x1D3D154F;
        bool selfWrapped = rawBytes.size() >= 4
            && *reinterpret_cast<const uint32_t*>(rawBytes.constData()) == kSelfMagicCheck;

        const QByteArray bytes = UnwrapSelfIfPresent(rawBytes);

        if (bytes.size() < (int)sizeof(Elf64_Ehdr))
            return false;

        const uint8_t* base = reinterpret_cast<const uint8_t*>(bytes.constData());
        auto* eh = reinterpret_cast<const Elf64_Ehdr*>(base);
        if (eh->e_ident[0] != 0x7F || eh->e_ident[1] != 'E' || eh->e_ident[2] != 'L' || eh->e_ident[3] != 'F')
            return false;
        if (eh->e_ident[4] != 2 /* ELFCLASS64 */) return false;

        img.bytes = bytes;
        img.wasSelfWrapped = selfWrapped;

        if (eh->e_phoff == 0 || eh->e_phnum == 0)
            return false;

        uint64_t dynOffset = 0, dynFilesz = 0;
        bool haveDynamic = false;

        for (int i = 0; i < eh->e_phnum; i++)
        {
            uint64_t phAddr = eh->e_phoff + (uint64_t)i * eh->e_phentsize;
            if (phAddr + sizeof(Elf64_Phdr) > (uint64_t)bytes.size()) break;
            auto* ph = reinterpret_cast<const Elf64_Phdr*>(base + phAddr);

            if (ph->p_type == kPT_LOAD)
                img.loadSegments.append(*ph);
            else if (ph->p_type == kPT_DYNAMIC)
            {
                dynOffset = ph->p_offset;
                dynFilesz = ph->p_filesz;
                haveDynamic = true;
            }
            else if (ph->p_type == kPT_SCE_DYNLIBDATA)
            {
                // DT_SCE_SYMTAB/STRTAB/RELA below are offsets relative
                // to THIS segment's file position — capture it now so
                // the dynamic-tag loop can resolve them correctly
                img.dynlibDataOffset = ph->p_offset;
                img.haveDynlibData = true;
            }
        }
        if (!haveDynamic || img.loadSegments.isEmpty() || !img.haveDynlibData) return false;

        uint64_t relaVA = 0, relaSz = 0;
        int dynCount = (int)(dynFilesz / sizeof(Elf64_Dyn));
        for (int i = 0; i < dynCount; i++)
        {
            uint64_t entryOff = dynOffset + (uint64_t)i * sizeof(Elf64_Dyn);
            if (entryOff + sizeof(Elf64_Dyn) > (uint64_t)bytes.size()) break;
            auto* dyn = reinterpret_cast<const Elf64_Dyn*>(base + entryOff);
            if (dyn->d_tag == 0 /* DT_NULL */) break;

            // DT_SCE_SYMTAB/STRTAB/RELA values are offsets relative to
            // PT_SCE_DYNLIBDATA's file position, not standalone VAs —
            // add that base now so every later access is a plain,
            // already-resolved file offset
            if (dyn->d_tag == kDT_SCE_RELA)        relaVA = img.dynlibDataOffset + dyn->d_val;
            else if (dyn->d_tag == kDT_SCE_RELASZ)  relaSz = dyn->d_val;
            else if (dyn->d_tag == kDT_SCE_SYMTAB)  img.dynsymVA = img.dynlibDataOffset + dyn->d_val;
            else if (dyn->d_tag == kDT_SCE_SYMTABSZ) img.dynsymSize = dyn->d_val;
            else if (dyn->d_tag == kDT_SCE_STRTAB)  img.dynstrVA = img.dynlibDataOffset + dyn->d_val;
            else if (dyn->d_tag == kDT_SCE_STRSZ)   img.dynstrSize = dyn->d_val;
        }

        if (relaVA && relaSz)
        {
            // relaVA is already an absolute file offset at this point
            // (dynlibDataOffset + the raw tag value was applied above) —
            // no PT_LOAD VA translation needed or correct here
            uint64_t relaOff = relaVA;
            int relaCount = (int)(relaSz / sizeof(Elf64_Rela));
            for (int i = 0; i < relaCount; i++)
            {
                uint64_t entryOff = relaOff + (uint64_t)i * sizeof(Elf64_Rela);
                if (entryOff + sizeof(Elf64_Rela) > (uint64_t)bytes.size()) break;
                auto* rel = reinterpret_cast<const Elf64_Rela*>(base + entryOff);
                uint32_t type = (uint32_t)(rel->r_info & 0xFFFFFFFFu);
                if (type == kR_X86_64_RELATIVE)
                    img.relocMap.insert(rel->r_offset, (uint64_t)rel->r_addend);
            }
        }
        img.relaVA = relaVA;
        img.relaSize = relaSz;

        return true;
    }

    // Looks up getBuildInfo's virtual address in the dynamic symbol table
    bool FindPrxFactoryVA(const ElfImage& img, uint64_t& outFactoryVA)
    {
        if (!img.dynsymVA || !img.dynstrVA)
            return false;

        int symCount = (int)(img.dynsymSize / sizeof(Elf64_Sym));

        static const QSet<QString> kReserved = {
            "module_start", "module_stop", "_init", "_fini", "__cxa_finalize"
        };

        uint64_t fallbackVA = 0;
        int fallbackCount = 0;

        for (int i = 0; i < symCount; i++)
        {
            // img.dynsymVA is already an absolute file offset (computed
            // as dynlibDataOffset + the raw DT_SCE_SYMTAB tag value in
            // ParseElfImage) — index straight into the file bytes here
            uint64_t symOff = img.dynsymVA + (uint64_t)i * sizeof(Elf64_Sym);
            if (symOff + sizeof(Elf64_Sym) > (uint64_t)img.bytes.size()) break;
            const uint8_t* p = reinterpret_cast<const uint8_t*>(img.bytes.constData()) + symOff;
            auto* sym = reinterpret_cast<const Elf64_Sym*>(p);

            uint8_t type = sym->st_info & 0xF;
            uint8_t bind = sym->st_info >> 4;
            if (type != kSTT_FUNC || bind != kSTB_GLOBAL || sym->st_shndx == 0)
                continue; // not a defined global function

            QString name;
            ReadRawCString(img.bytes, img.dynstrVA + sym->st_name, name);

            if (name == kGetBuildInfoNid)
            {
                outFactoryVA = sym->st_value;
                return true;
            }
            if (!kReserved.contains(name))
            {
                fallbackVA = sym->st_value;
                fallbackCount++;
            }
        }

        if (fallbackCount == 1)
        {
            outFactoryVA = fallbackVA;
            return true;
        }
        return false;
    }

    bool ParseNrsElfImage(const QByteArray& rawBytes, ElfImage& img)
    {
        img.bytes = rawBytes;
        const QByteArray& bytes = img.bytes;
        const uint8_t* base = reinterpret_cast<const uint8_t*>(bytes.constData());

        if (bytes.size() < (int)sizeof(Elf64_Ehdr)) return false;
        auto* eh = reinterpret_cast<const Elf64_Ehdr*>(base);
        if (memcmp(eh->e_ident, "\x7f""ELF", 4) != 0) return false;
        if (eh->e_ident[4] != 2 /* ELFCLASS64 */) return false;
        if (eh->e_machine != kEM_AARCH64) return false;
        if (eh->e_shoff == 0 || eh->e_shnum == 0) return false; // needs section headers

        for (int i = 0; i < eh->e_phnum; i++)
        {
            uint64_t phAddr = eh->e_phoff + (uint64_t)i * sizeof(Elf64_Phdr);
            if (phAddr + sizeof(Elf64_Phdr) > (uint64_t)bytes.size()) break;
            auto* ph = reinterpret_cast<const Elf64_Phdr*>(base + phAddr);
            if (ph->p_type == kPT_LOAD)
                img.loadSegments.append(*ph);
        }

        // Section header string table, to find sections by name
        if (eh->e_shstrndx >= eh->e_shnum) return false;
        uint64_t shstrHdrOff = eh->e_shoff + (uint64_t)eh->e_shstrndx * sizeof(Elf64_Shdr);
        if (shstrHdrOff + sizeof(Elf64_Shdr) > (uint64_t)bytes.size()) return false;
        auto* shstrHdr = reinterpret_cast<const Elf64_Shdr*>(base + shstrHdrOff);
        if (shstrHdr->sh_offset + shstrHdr->sh_size > (uint64_t)bytes.size()) return false;
        const char* shstrtab = reinterpret_cast<const char*>(base + shstrHdr->sh_offset);
        uint64_t shstrSize = shstrHdr->sh_size;

        auto sectionName = [&](uint32_t nameOff) -> QString
            {
                if (nameOff >= shstrSize) return QString();
                uint64_t end = nameOff;
                while (end < shstrSize && shstrtab[end] != '\0') end++;
                return QString::fromLatin1(shstrtab + nameOff, (int)(end - nameOff));
            };

        uint64_t relaDynOff = 0, relaDynSize = 0;
        for (int i = 0; i < eh->e_shnum; i++)
        {
            uint64_t shAddr = eh->e_shoff + (uint64_t)i * sizeof(Elf64_Shdr);
            if (shAddr + sizeof(Elf64_Shdr) > (uint64_t)bytes.size()) break;
            auto* sh = reinterpret_cast<const Elf64_Shdr*>(base + shAddr);
            QString name = sectionName(sh->sh_name);

            if (name == ".dynsym") { img.dynsymVA = sh->sh_offset; img.dynsymSize = sh->sh_size; }
            else if (name == ".dynstr") { img.dynstrVA = sh->sh_offset; img.dynstrSize = sh->sh_size; }
            else if (name == ".rela.dyn") { relaDynOff = sh->sh_offset; relaDynSize = sh->sh_size; }
            else if (name == ".comment") { img.commentOff = sh->sh_offset; img.commentSize = sh->sh_size; }
            else if (name == ".note.gnu.build-id") { img.buildIdOff = sh->sh_offset; img.buildIdSize = sh->sh_size; }
        }

        if (img.dynsymVA == 0 || img.dynstrVA == 0) return false;

        int relaCount = (int)(relaDynSize / sizeof(Elf64_Rela));
        for (int i = 0; i < relaCount; i++)
        {
            uint64_t entryOff = relaDynOff + (uint64_t)i * sizeof(Elf64_Rela);
            if (entryOff + sizeof(Elf64_Rela) > (uint64_t)bytes.size()) break;
            auto* rel = reinterpret_cast<const Elf64_Rela*>(base + entryOff);
            uint32_t type = (uint32_t)(rel->r_info & 0xFFFFFFFF);
            if (type == kR_AARCH64_RELATIVE)
                img.relocMap.insert(rel->r_offset, (uint64_t)rel->r_addend);
        }

        return true;
    }

    // "branchName" -> "Branch Name". A couple of common initialisms
    // ("Id", "Iso") are special-cased to read naturally, matching the
    // capitalization already used for the equivalent PS4/PE fields
    QString HumanizeFieldIdentifier(const QString& ident)
    {
        if (ident.isEmpty()) return ident;

        QString spaced;
        spaced += ident.at(0).toUpper();
        for (int i = 1; i < ident.size(); i++)
        {
            QChar c = ident.at(i);
            if (c.isUpper()) spaced += ' ';
            spaced += c;
        }

        QStringList words = spaced.split(' ', Qt::SkipEmptyParts);
        for (QString& w : words)
        {
            if (w.compare("Id", Qt::CaseInsensitive) == 0) w = "ID";
            else if (w.compare("Iso", Qt::CaseInsensitive) == 0) w = "ISO";
        }
        return words.join(' ');
    }

    // Searches startDir and every subdirectory beneath it (not parent
    // directories) for the first file matching fileName
    QString FindFileInSubtree(const QDir& startDir, const QString& fileName)
    {
        QDirIterator it(startDir.absolutePath(), QStringList() << fileName,
            QDir::Files, QDirIterator::Subdirectories);
        if (it.hasNext())
        {
            it.next();
            return it.filePath();
        }
        return QString();
    }

    // .BuildSettings-only fallback
    QString DetectPlatformFromFileName(const QString& path)
    {
        QString name = QFileInfo(path).fileName().toLower();
        if (name.contains("win32"))           return "Windows PC (Win32)";
        if (name.contains("ps3"))             return "PlayStation 3 (Ps3)";
        if (name.contains("xenon"))           return "Xbox 360 (Xenon)";
        if (name.contains("gen4a"))           return "Xbox One (Gen4a)";
        if (name.contains("gen4b"))           return "PlayStation 4 (Gen4b)";
        if (name.contains("xbsx"))            return "Xbox Series X|S (Xbsx)";
        if (name.contains("ps5"))             return "PlayStation 5 (Ps5)";
        if (name.contains("nx"))              return "Nintendo Switch (Nx)";
        if (name.contains("sprout"))          return "Nintendo Switch 2 (Sprout)";
        if (name.contains("osx"))             return "Mac OS (Osx)";
        if (name.contains("ios"))             return "iPhone OS (iOS)";
        if (name.contains("android"))         return "Android OS (Android)";
        if (name.contains("linux"))           return "Linux PC (Linux)";
        if (name.contains("stadia"))          return "Google Stadia (Stadia)";
        if (name.contains("dedicatedserver")) return "Dedicated Server";
        if (name.contains("editor"))          return "Internal Editor";
        return QString(); // empty = no match, unlike MainWindow's "Unknown"
    }

    QString DetectBuildTypeFromFileName(const QString& path)
    {
        QString name = QFileInfo(path).fileName().toLower();
        if (name.contains("retail")) return "Retail";
        if (name.contains("final"))  return "Final";
        return QString();
    }

    // ============================================================
    // Minimal param.sfo (PS4 "PSF" key/value store) reader. Only pulls
    // out the handful of string fields we care about (TITLE, TITLE_01,
    // VERSION) — not a general-purpose SFO editor. Struct layout and
    // magic bytes (00 50 53 46) verified directly against a real
    // param.sfo's raw hex, not assumed
    // ============================================================
#pragma pack(push, 1)
    struct SfoHeader
    {
        uint32_t magic;           // 0x46535000, i.e. bytes "\0PSF"
        uint32_t version;
        uint32_t keyTableOffset;
        uint32_t dataTableOffset;
        uint32_t entriesCount;
    };
    struct SfoIndexEntry
    {
        uint16_t keyOffset;
        uint16_t format;
        uint32_t length;
        uint32_t maxLength;
        uint32_t dataOffset;
    };
#pragma pack(pop)

    constexpr uint32_t kSfoMagic = 0x46535000;
    constexpr uint16_t kSfoFormatInt32 = 0x0404;

    bool ParseParamSfo(const QString& path, QHash<QString, QString>& outStrings)
    {
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly)) return false;
        QByteArray bytes = f.readAll();
        f.close();

        if (bytes.size() < (int)sizeof(SfoHeader)) return false;
        const uint8_t* base = reinterpret_cast<const uint8_t*>(bytes.constData());
        auto* hdr = reinterpret_cast<const SfoHeader*>(base);
        if (hdr->magic != kSfoMagic) return false;

        for (uint32_t i = 0; i < hdr->entriesCount; i++)
        {
            uint32_t entryOff = (uint32_t)sizeof(SfoHeader) + i * (uint32_t)sizeof(SfoIndexEntry);
            if (entryOff + sizeof(SfoIndexEntry) > (uint32_t)bytes.size()) break;
            auto* idx = reinterpret_cast<const SfoIndexEntry*>(base + entryOff);

            uint32_t keyAddr = hdr->keyTableOffset + idx->keyOffset;
            if (keyAddr >= (uint32_t)bytes.size()) continue;
            int keyLen = 0;
            while (keyAddr + keyLen < (uint32_t)bytes.size() && base[keyAddr + keyLen] != 0 && keyLen < 128)
                keyLen++;
            QString key = QString::fromLatin1(reinterpret_cast<const char*>(base + keyAddr), keyLen);

            uint32_t valAddr = hdr->dataTableOffset + idx->dataOffset;
            if (valAddr + idx->length > (uint32_t)bytes.size()) continue;

            if (idx->format == kSfoFormatInt32)
            {
                if (idx->length >= 4)
                {
                    int32_t v;
                    memcpy(&v, base + valAddr, 4);
                    outStrings.insert(key.toUpper(), QString::number(v));
                }
            }
            else
            {
                int strLen = 0;
                while (strLen < (int)idx->length && base[valAddr + strLen] != 0)
                    strLen++;
                outStrings.insert(key.toUpper(),
                    QString::fromUtf8(reinterpret_cast<const char*>(base + valAddr), strLen));
            }
        }
        return true;
    }

    QString FormatSfoVersion(const QString& raw)
    {
        QStringList parts = raw.split('.');
        if (parts.size() == 2)
        {
            bool ok = false;
            int major = parts[0].toInt(&ok);
            if (ok) return QString("%1.%2").arg(major).arg(parts[1]);
        }
        return raw;
    }

    // ============================================================
    // Minimal AppxManifest.xml reader (Durango / UWP package manifest)
    // Only pulls the two fields we care about — Properties/DisplayName
    // and Identity/@Version — not a general-purpose manifest parser
    // ============================================================
    bool ParseAppxManifest(const QString& path, QString& outDisplayName, QString& outVersion)
    {
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
            return false;

        QXmlStreamReader xml(&f);
        bool foundName = false, foundVersion = false;
        while (!xml.atEnd() && !xml.hasError())
        {
            if (xml.readNext() != QXmlStreamReader::StartElement)
                continue;

            if (xml.name().compare(QLatin1String("Identity"), Qt::CaseInsensitive) == 0)
            {
                QString v = xml.attributes().value("Version").toString();
                if (!v.isEmpty()) { outVersion = v; foundVersion = true; }
            }
            else if (!foundName
                && xml.name().compare(QLatin1String("DisplayName"), Qt::CaseInsensitive) == 0)
            {
                // Properties/DisplayName is the real title. There's also a
                // VisualElements/@DisplayName *attribute* elsewhere in the
                // same file (commonly "ms-resource:AppName" — a resource
                // key, not real text) — only reading the element form
                // here, and skipping anything that looks like a resource
                // reference, keeps the two from being confused
                QString text = xml.readElementText();
                if (!text.isEmpty() && !text.startsWith("ms-resource:", Qt::CaseInsensitive))
                {
                    outDisplayName = text;
                    foundName = true;
                }
            }
        }
        f.close();
        return foundName || foundVersion;
    }

} // namespace
#endif

// ============================================================
// Construction / UI
// ============================================================
BuildInfoWindow::BuildInfoWindow(MainWindow* mainWindow, QWidget* parent)
    : QWidget(parent), m_mainWindow(mainWindow)
{
    setWindowFlags(Qt::Window);
    setWindowTitle("Build Info Reader");
    resize(760, 480);
    setAttribute(Qt::WA_DeleteOnClose, false);
    setWindowModality(Qt::ApplicationModal);

    buildUi();
    resetDisplay();
}

BuildInfoWindow::~BuildInfoWindow()
{
#ifdef Q_OS_WIN
    freeCurrentModule();
#endif
}

void BuildInfoWindow::buildUi()
{
    QVBoxLayout* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);

    QScrollArea* scrollArea = new QScrollArea(this);
    scrollArea->setWidgetResizable(true);
    scrollArea->setFrameShape(QFrame::NoFrame);
    scrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    outer->addWidget(scrollArea);

    QWidget* scrollContent = new QWidget;
    scrollArea->setWidget(scrollContent);

    QVBoxLayout* root = new QVBoxLayout(scrollContent);
    root->setContentsMargins(10, 10, 10, 10);
    root->setSpacing(8);

    // ---- Top bar: status only — file selection now lives in MainWindow ----
    m_statusLabel = new QLabel(this);
    m_statusLabel->setWordWrap(true);
    root->addWidget(m_statusLabel);

    // ---- Body: left info card + right metadata table ----
    QHBoxLayout* body = new QHBoxLayout;
    body->setSpacing(12);

    QWidget* leftCard = new QWidget(this);
    leftCard->setFixedWidth(200);
    QVBoxLayout* leftVBox = new QVBoxLayout(leftCard);
    leftVBox->setContentsMargins(0, 0, 0, 0);
    leftVBox->setSpacing(6);

    m_iconLabel = new QLabel(leftCard);
    m_iconLabel->setFixedSize(96, 96);
    m_iconLabel->setAlignment(Qt::AlignCenter);
    m_iconLabel->setScaledContents(false);

    // Frostbite engine badge — square icon (source art is 256x256,
    // downscaled here same as the main icon) plus a caption directly
    // below it. Both start hidden; updateFrostbiteEngineBadge() shows
    // them once a DLL with a recognizable "Frostbite Release" field
    // has actually been loaded
    m_engineIconLabel = new QLabel(leftCard);
    m_engineIconLabel->setFixedSize(80, 80);
    m_engineIconLabel->setAlignment(Qt::AlignCenter);
    m_engineIconLabel->setScaledContents(false);
    m_engineIconLabel->setVisible(false);

    m_engineTextLabel = new QLabel(leftCard);
    m_engineTextLabel->setWordWrap(true);
    m_engineTextLabel->setAlignment(Qt::AlignHCenter);
    m_engineTextLabel->setVisible(false);

    m_titleLabel = new QLabel(leftCard);
    m_titleLabel->setWordWrap(true);
    m_titleLabel->setAlignment(Qt::AlignHCenter);
    QFont tf = m_titleLabel->font(); tf.setBold(true); tf.setPointSize(tf.pointSize() + 1);
    m_titleLabel->setFont(tf);

    m_subtitleLabel = new QLabel(leftCard);
    m_subtitleLabel->setWordWrap(true);
    m_subtitleLabel->setAlignment(Qt::AlignHCenter);

    m_pathLabel = new QLabel(leftCard);
    m_pathLabel->setWordWrap(true);
    m_pathLabel->setAlignment(Qt::AlignHCenter);
    QFont pf = m_pathLabel->font(); pf.setPointSize(qMax(7, pf.pointSize() - 1));
    m_pathLabel->setFont(pf);

    leftVBox->addWidget(m_iconLabel, 0, Qt::AlignHCenter);
    leftVBox->addWidget(m_titleLabel);

    leftVBox->addWidget(m_pathLabel);
    leftVBox->addWidget(m_subtitleLabel);

    // Frostbite engine badge sits below the entire game info block
    // (icon, title, path, subtitle) — not interleaved with it
    leftVBox->addWidget(m_engineIconLabel, 0, Qt::AlignHCenter);
    leftVBox->addWidget(m_engineTextLabel);

    leftVBox->addStretch(1);

    body->addWidget(leftCard);

    auto makeTable = [&](int cols, QStringList headers) -> QTableWidget* {
        auto* t = new QTableWidget(this);
        t->setColumnCount(cols);
        t->setHorizontalHeaderLabels(headers);
        t->horizontalHeader()->setStretchLastSection(true);
        for (int i = 0; i < cols - 1; ++i)
            t->horizontalHeader()->setSectionResizeMode(i, QHeaderView::ResizeToContents);
        t->verticalHeader()->setVisible(false);
        t->setEditTriggers(QAbstractItemView::NoEditTriggers);
        t->setSelectionBehavior(QAbstractItemView::SelectRows);
        t->setAlternatingRowColors(true);
        t->setWordWrap(false);
        t->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        t->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        t->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

        QPalette pal = t->palette();
        const QColor selBg(0, 120, 215);
        pal.setColor(QPalette::Active, QPalette::Highlight, selBg);
        pal.setColor(QPalette::Active, QPalette::HighlightedText, Qt::white);
        pal.setColor(QPalette::Inactive, QPalette::Highlight, selBg);
        pal.setColor(QPalette::Inactive, QPalette::HighlightedText, Qt::white);
        t->setPalette(pal);

        return t;
        };

    QWidget* rightPane = new QWidget(this);
    QVBoxLayout* rightVBox = new QVBoxLayout(rightPane);
    rightVBox->setContentsMargins(0, 0, 0, 0);
    rightVBox->setSpacing(8);

    // Initial Information — 2 columns, no type
    auto* labelInitial = new QLabel("— Initial Information —", rightPane);
    QFont lf = labelInitial->font(); lf.setBold(true); labelInitial->setFont(lf);
    rightVBox->addWidget(labelInitial);
    m_tableInitial = makeTable(2, { "PARAM", "VALUE" });
    rightVBox->addWidget(m_tableInitial);

    // Build Info Fields — 3 columns with type
    auto* labelBuild = new QLabel("— Build Info Fields —", rightPane);
    labelBuild->setFont(lf);
    rightVBox->addWidget(labelBuild);
    m_tableBuild = makeTable(3, { "PARAM", "TYPE", "VALUE" });
    rightVBox->addWidget(m_tableBuild);

    // PE / Debug Info — 2 columns, no type
    m_labelPe = new QLabel("— PE / Debug Info —", rightPane);
    m_labelPe->setFont(lf);
    rightVBox->addWidget(m_labelPe);
    m_tablePe = makeTable(2, { "PARAM", "VALUE" });
    rightVBox->addWidget(m_tablePe);

    body->addWidget(rightPane, 1);
    root->addLayout(body);
    root->addStretch(1);
}

void BuildInfoWindow::resetDisplay()
{
    m_iconLabel->setPixmap(style()->standardIcon(QStyle::SP_FileIcon).pixmap(64, 64));
    m_engineIconLabel->clear();
    m_engineIconLabel->setVisible(false);
    m_engineTextLabel->clear();
    m_engineTextLabel->setVisible(false);
    m_titleLabel->setText("No DLL loaded");
    m_subtitleLabel->clear();
    m_pathLabel->clear();
    m_statusLabel->setText("Click \"Load DLL...\" to pick a BuildInfo DLL to inspect.");
    populateTable({}); // clears and shrinks all three tables to zero rows
}

void BuildInfoWindow::applyTheme(bool dark)
{
    m_darkMode = dark;

    if (dark)
    {
        setStyleSheet(
            "QWidget { background: #1c1c1c; color: #f0f0f0; }"
            "QPushButton { background: #2a2a2a; border: 1px solid #444; padding: 4px 10px; }"
            "QPushButton:hover { background: #333333; }"
            "QTableWidget { background: #121212; color: #f0f0f0; gridline-color: #333; alternate-background-color: #1a1a1a; outline: none; }"
            "QHeaderView::section { background: #222222; color: #f0f0f0; padding: 4px; border: 1px solid #333; }"
            "QTableWidget::item:selected { background: #0078d7; color: white; border: none; outline: none; }"
            "QTableWidget::item:selected:!active { background: #0078d7; color: white; border: none; outline: none; }"
            "QTableWidget::item:focus { border: none; outline: none; }"
            "QScrollBar:vertical { background: #121212; width: 12px; border: none; margin: 0; }"
            "QScrollBar::handle:vertical { background: #555555; border-radius: 3px; min-height: 20px; margin: 2px; }"
            "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0px; background: none; border: none; }"
            "QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical { background: none; }"
            "QScrollBar:horizontal { background: #121212; height: 12px; border: none; margin: 0; }"
            "QScrollBar::handle:horizontal { background: #555555; border-radius: 3px; min-width: 20px; margin: 2px; }"
            "QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal { width: 0px; background: none; border: none; }"
            "QScrollBar::add-page:horizontal, QScrollBar::sub-page:horizontal { background: none; }");
    }
    else
    {
        setStyleSheet(
            "QTableWidget { alternate-background-color: #f2f2f2; outline: none; }"
            "QTableWidget::item:selected { background: #0078d7; color: white; border: none; outline: none; }"
            "QTableWidget::item:selected:!active { background: #0078d7; color: white; border: none; outline: none; }"
            "QTableWidget::item:focus { border: none; outline: none; }"
            "QScrollBar:vertical { background: palette(base); width: 12px; border: none; margin: 0; }"
            "QScrollBar::handle:vertical { background: palette(mid); border-radius: 3px; min-height: 20px; margin: 2px; }"
            "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0px; background: none; border: none; }"
            "QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical { background: none; }"
            "QScrollBar:horizontal { background: palette(base); height: 12px; border: none; margin: 0; }"
            "QScrollBar::handle:horizontal { background: palette(mid); border-radius: 3px; min-width: 20px; margin: 2px; }"
            "QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal { width: 0px; background: none; border: none; }"
            "QScrollBar::add-page:horizontal, QScrollBar::sub-page:horizontal { background: none; }");
    }
}

void BuildInfoWindow::showError(const QString& msg)
{
    m_statusLabel->setText(msg);
    QMessageBox::warning(this, "Build Info Reader", msg);
}

QIcon BuildInfoWindow::findAssociatedIcon(const QString& dllPath) const
{
    QFileInfo dllInfo(dllPath);
    QDir dir = dllInfo.absoluteDir();

    // Look in the DLL's own directory, then up to 3 parent levels,
    // for the first .exe — this is usually the game/tool executable
    // that ships alongside the build info DLL
    for (int depth = 0; depth < 4; depth++)
    {
        QStringList exes = dir.entryList({ "*.exe" }, QDir::Files);
        for (const QString& exe : exes)
        {
            if (exe.contains("unins", Qt::CaseInsensitive)) continue;
            QIcon ic = QFileIconProvider().icon(QFileInfo(dir.filePath(exe)));
            if (!ic.isNull()) return ic;
        }
        if (dir.isRoot() || !dir.cdUp()) break;
    }

    // Fall back to the DLL's own icon resource, if it has one
    QIcon dllIcon = QFileIconProvider().icon(dllInfo);
    if (!dllIcon.isNull()) return dllIcon;

    return QIcon();
}

// Extracts "year" and "minor" out of a Frostbite Release string like
// "Release 2013.2" or a bare "2013.2" / "2011"
bool BuildInfoWindow::parseFrostbiteVersion(const QString& raw, int& year, int& minor) const
{
    static QRegularExpression re(R"((\d{4})(?:\.(\d+))?)");
    QRegularExpressionMatch m = re.match(raw);
    if (!m.hasMatch())
        return false;

    year = m.captured(1).toInt();
    minor = m.captured(2).isEmpty() ? 0 : m.captured(2).toInt();
    return true;
}

// Reads "Frostbite Release" out of the already-collected Build Info
// fields and shows the matching engine-generation icon/caption beneath
// the main game icon. Ranges (inclusive):
//   2011      .. 2013.0  -> Frost2.png, "Frostbite Engine 2.0"
//   2013.1    .. 2024.0  -> Frost3.png, "Frostbite Engine 3.0"
//   2024.1 and higher    -> Frost4.png, "Frostbite Engine 4.0"
// Hidden entirely if no Frostbite Release field was found or it doesn't
// parse — e.g. .BuildSettings files or DLLs where that getter wasn't
// present in the vtable
void BuildInfoWindow::updateFrostbiteEngineBadge(const QList<MetaRow>& buildRows)
{
    // No build info table at all (nothing loaded, or extraction found no
    // fields whatsoever) — hide the badge entirely rather than guessing
    if (buildRows.isEmpty())
    {
        m_engineIconLabel->clear();
        m_engineIconLabel->setVisible(false);
        m_engineTextLabel->clear();
        m_engineTextLabel->setVisible(false);
        return;
    }

    QString releaseRaw;
    for (const MetaRow& r : buildRows)
    {
        if (r.param == "Frostbite Release")
        {
            releaseRaw = r.value;
            break;
        }
    }

    // A real build info table exists, but it has no "Frostbite Release"
    // field at all — that getter wasn't introduced until Frostbite 3, so
    // its absence here is itself the signal that this is a Frostbite 2
    // build, rather than a case to hide the badge for
    if (releaseRaw.isEmpty())
    {
        QPixmap pix(QStringLiteral(":/buildinfo/Frost2.png"));
        if (!pix.isNull())
            m_engineIconLabel->setPixmap(
                pix.scaled(80, 80, Qt::KeepAspectRatio, Qt::SmoothTransformation));
        else
            m_engineIconLabel->clear();

        m_engineTextLabel->setText(QStringLiteral("Frostbite Engine 2.0"));
        m_engineIconLabel->setVisible(true);
        m_engineTextLabel->setVisible(true);
        return;
    }

    // releaseRaw still carries its raw "(String) "..."" wrapper here —
    // populateTable() only strips that for display, this function reads
    // straight from the untouched MetaRow list — so pull the bare text
    // out from between the quotes first
    int a = releaseRaw.indexOf('"');
    int b = releaseRaw.lastIndexOf('"');
    QString bare = (a >= 0 && b > a) ? releaseRaw.mid(a + 1, b - a - 1) : releaseRaw;

    int year = 0, minor = 0;
    if (!parseFrostbiteVersion(bare, year, minor))
    {
        m_engineIconLabel->clear();
        m_engineIconLabel->setVisible(false);
        m_engineTextLabel->clear();
        m_engineTextLabel->setVisible(false);
        return;
    }

    // Combine into one sortable value (year*10 + minor) so the three
    // range checks below are simple comparisons rather than compound
    // year/minor conditionals
    const int v = year * 10 + minor;

    QString iconPath, labelText;
    if (v <= 20130) // 2011 .. 2013.0
    {
        iconPath = QStringLiteral(":/buildinfo/Frost2.png");
        labelText = QStringLiteral("Frostbite Engine 2.0");
    }
    else if (v <= 20240) // 2013.1 .. 2024.0
    {
        iconPath = QStringLiteral(":/buildinfo/Frost3.png");
        labelText = QStringLiteral("Frostbite Engine 3.0");
    }
    else // 2024.1 and higher
    {
        iconPath = QStringLiteral(":/buildinfo/Frost4.png");
        labelText = QStringLiteral("Frostbite Engine 4.0");
    }

    QPixmap pix(iconPath);
    if (!pix.isNull())
        m_engineIconLabel->setPixmap(
            pix.scaled(80, 80, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    else
        m_engineIconLabel->clear();

    m_engineTextLabel->setText(labelText);
    m_engineIconLabel->setVisible(true);
    m_engineTextLabel->setVisible(true);
}

QList<BuildInfoWindow::MetaRow> BuildInfoWindow::extractBuildSettingsFile(const QString& path) const
{
    QList<MetaRow> out;

    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return out;
    QByteArray raw = f.readAll();
    f.close();

    // Maps the file's internal g_* key to the same display label used by
    // assignFieldNames() for the vtable-walk path, so a row named e.g.
    // "Branch Name" means the same thing regardless of which source it
    // came from. Order here also controls display order below
    static const QList<QPair<QString, QString>> kKeyMap = {
        { "g_branchName",           "Branch Name" },
        { "g_fullBranchName",       "Full Branch Name" },
        { "g_branchId",             "Branch ID" },
        { "g_licenseeId",           "Licensee ID" },
        { "g_studioName",           "Studio Name" },
        { "g_changelist",           "Changelist" },
        { "g_sourceChangelist",     "Source Changelist" },
        { "g_frostbiteChangelist",  "Frostbite Changelist" },
        { "g_frostbiteRelease",     "Frostbite Release" },
        { "g_frostbiteReleaseName", "Frostbite Release Name" },
        { "g_username",             "Username" },
        { "g_usergroup",            "User Group" },
        { "g_hostName",             "Host Name" },
        { "g_isAutoBuild",          "Is Auto Build" },
        { "g_buildTime",            "Build Time" },
        { "g_buildDate",            "Build Date" },
        { "g_buildIso",             "Build ISO Date" },
        { "g_depotStream",          "Depot Stream" },
        { "g_depotPort",            "Depot Port" },
    };

    QHash<QString, QString> values;

    // Newer engine builds (e.g. CreateBuildInfo.cs) emit a plain JSON
    // object instead of C declarations — try that first since a match is
    // unambiguous. QJsonDocument::fromJson tolerates the trailing \r\n
    // seen in real files without any pre-trimming
    QJsonParseError jsonErr;
    QJsonDocument doc = QJsonDocument::fromJson(raw, &jsonErr);
    if (jsonErr.error == QJsonParseError::NoError && doc.isObject())
    {
        QJsonObject obj = doc.object();
        for (auto it = obj.constBegin(); it != obj.constEnd(); ++it)
        {
            if (it.key().compare("Warning", Qt::CaseInsensitive) == 0)
                continue; // descriptive comment, not a build field

            const QJsonValue v = it.value();
            QString typed;
            if (v.isBool())
                typed = QString("(Bool) %1").arg(v.toBool() ? "true" : "false");
            else if (v.isDouble())
                typed = QString("(Int32) %1").arg(v.toVariant().toString());
            else
                typed = QString("(String) \"%1\"").arg(v.toVariant().toString().trimmed());

            values.insert(it.key(), typed);
        }
    }
    else
    {
        // Legacy format: one declaration per line
        QRegularExpression reStr(R"re(g_(\w+)\s*=\s*"([^"]*)")re");
        QRegularExpression reBool(R"re(g_(\w+)\s*=\s*(true|false)\s*;)re",
            QRegularExpression::CaseInsensitiveOption);
        QRegularExpression reInt(R"re(g_(\w+)\s*=\s*(-?\d+)\s*;)re");
        for (const QString& line : QString::fromUtf8(raw).split('\n'))
        {
            QRegularExpressionMatch m = reStr.match(line);
            if (m.hasMatch())
            {
                values.insert("g_" + m.captured(1),
                    QString("(String) \"%1\"").arg(m.captured(2).trimmed()));
                continue;
            }

            m = reBool.match(line);
            if (m.hasMatch())
            {
                values.insert("g_" + m.captured(1),
                    QString("(Bool) %1").arg(m.captured(2).toLower()));
                continue;
            }

            m = reInt.match(line);
            if (m.hasMatch())
                values.insert("g_" + m.captured(1),
                    QString("(Int32) %1").arg(m.captured(2).trimmed()));
        }
    }

    // Fallback: fix newer .BuildSettings files quote every value
    static const QHash<QString, QString> kDefaultFieldTypes = {
        { "g_branchName",           "String" },
        { "g_fullBranchName",       "String" },
        { "g_branchId",             "String" },
        { "g_licenseeId",           "String" },
        { "g_studioName",           "String" },
        { "g_changelist",           "Int32"  },
        { "g_sourceChangelist",     "Int32"  },
        { "g_frostbiteChangelist",  "Int32"  },
        { "g_frostbiteRelease",     "String" },
        { "g_frostbiteReleaseName", "String" },
        { "g_isAutoBuild",          "Bool"   },
        { "g_username",             "String" },
        { "g_usergroup",            "Int32"  },
        { "g_hostName",             "String" },
        { "g_buildTime",            "String" },
        { "g_buildDate",            "String" },
        { "g_buildIso",             "String" },
        { "g_depotStream",          "String" },
        { "g_depotPort",            "String" },
    };

    for (auto it = values.begin(); it != values.end(); ++it)
    {
        if (!it.value().startsWith("(String)")) continue; // already typed correctly

        QString wantType = kDefaultFieldTypes.value(it.key());
        if (wantType.isEmpty() || wantType == "String") continue; // unknown key, or genuinely a string

        // Extract the bare value out of `(String) "..."`.
        QString v = it.value();
        int a = v.indexOf('"'), b = v.lastIndexOf('"');
        QString bare = (a >= 0 && b > a) ? v.mid(a + 1, b - a - 1).trimmed() : v.mid(8).trimmed();

        if (wantType == "Int32")
        {
            bool ok = false;
            bare.toLongLong(&ok);
            if (ok)
                it.value() = QString("(Int32) %1").arg(bare);
        }
        else if (wantType == "Bool")
        {
            QString lower = bare.toLower();
            if (lower == "true" || lower == "false")
            {
                it.value() = QString("(Bool) %1").arg(lower);
            }
            else
            {
                // A few samples encode bools as "0"/"1" instead of true/false.
                bool ok = false;
                qint64 n = bare.toLongLong(&ok);
                if (ok)
                    it.value() = QString("(Bool) %1").arg(n != 0 ? "true" : "false");
            }
        }
    }

    if (values.isEmpty())
        return out;

    // Emit in kKeyMap's fixed order first so display order matches the
    // vtable-walk path, then append anything unrecognized at the end so a
    // future new key never silently disappears
    QSet<QString> seen;
    for (const auto& kv : kKeyMap)
    {
        if (values.contains(kv.first))
        {
            out.append({ kv.second, values.value(kv.first), RowKind::Normal, TableKind::BuildInfo });
            seen.insert(kv.first);
        }
    }
    for (auto it = values.constBegin(); it != values.constEnd(); ++it)
    {
        if (!seen.contains(it.key()))
            out.append({ it.key(), it.value(), RowKind::Normal, TableKind::BuildInfo });
    }

    return out;
}

// ============================================================
// Loading & analysis
// ============================================================
#ifdef Q_OS_WIN

void BuildInfoWindow::freeCurrentModule()
{
    if (m_hMod)
    {
        FreeLibrary(m_hMod);
        m_hMod = nullptr;
    }
}

bool BuildInfoWindow::loadDll(const QString& dllPath)
{
    freeCurrentModule();
    m_tableInitial->setRowCount(0);
    m_tablePe->setRowCount(0);
    m_tableBuild->setRowCount(0);

    QFileInfo fi(dllPath);
    if (!fi.exists())
    {
        showError("File does not exist:\n" + dllPath);
        return false;
    }

    if (fi.suffix().compare("BuildSettings", Qt::CaseInsensitive) == 0)
    {
        QList<MetaRow> fields = extractBuildSettingsFile(dllPath);
        if (fields.isEmpty())
        {
            showError("Could not parse any fields from:\n" + dllPath);
            return false;
        }

        QDir dir = fi.absoluteDir();

        QString architecture = "Unknown (no platform siblings found)";
        QString gameName, gameVersion;
        QPixmap iconPixmap;

        QString sfoPath = FindFileInSubtree(dir, "param.sfo");
        QStringList nrsCandidates = dir.entryList({ "Engine.BuildInfo*.nrs" }, QDir::Files);
        QString manifestPath = dir.filePath("AppxManifest.xml");

        if (!sfoPath.isEmpty())
        {
            architecture = "x64 (PS4)";

            QHash<QString, QString> sfo;
            if (ParseParamSfo(sfoPath, sfo))
            {
                if (sfo.contains("TITLE_01"))
                    gameName = sfo.value("TITLE_01");
                else if (sfo.contains("TITLE"))
                    gameName = sfo.value("TITLE");

                if (sfo.contains("APP_VER"))
                    gameVersion = FormatSfoVersion(sfo.value("APP_VER"));
                else if (sfo.contains("VERSION"))
                    gameVersion = FormatSfoVersion(sfo.value("VERSION"));
            }

            QString iconPath = FindFileInSubtree(dir, "icon0.png");
            if (!iconPath.isEmpty())
            {
                QPixmap p(iconPath);
                if (!p.isNull())
                    iconPixmap = p.scaled(96, 96, Qt::KeepAspectRatio, Qt::SmoothTransformation);
            }
        }
        else if (!nrsCandidates.isEmpty())
        {
            architecture = "AArch64 (Switch)";
            // No confirmed sibling name/version metadata file/format yet
            // for Switch — same caveat as the isNrs branch below, so this
            // is left blank rather than guessed at
        }
        else if (QFile::exists(manifestPath))
        {
            architecture = "x64 (XB1)";

            QString displayName, version;
            if (ParseAppxManifest(manifestPath, displayName, version))
            {
                if (!displayName.isEmpty()) gameName = displayName;
                if (!version.isEmpty())     gameVersion = version;
            }

            QString logoPath = dir.filePath("Logo.png");
            if (QFile::exists(logoPath))
            {
                QPixmap p(logoPath);
                if (!p.isNull())
                    iconPixmap = p.scaled(96, 96, Qt::KeepAspectRatio, Qt::SmoothTransformation);
            }
        }
        else
        {
            // PC: same sibling .exe lookup
            for (const QString& exe : dir.entryList({ "*.exe" }, QDir::Files))
            {
                if (exe.contains("unins", Qt::CaseInsensitive)) continue;

                QString exePath = dir.filePath(exe);
                QFile f(exePath);
                if (!f.open(QIODevice::ReadOnly)) continue;
                QByteArray head = f.read(1024);
                f.close();

                if (head.size() < (int)sizeof(IMAGE_DOS_HEADER)) continue;
                auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(head.constData());
                if (dos->e_magic != IMAGE_DOS_SIGNATURE
                    || dos->e_lfanew <= 0
                    || (size_t)dos->e_lfanew + sizeof(DWORD) + sizeof(IMAGE_FILE_HEADER) > (size_t)head.size())
                    continue;

                const uint8_t* base = reinterpret_cast<const uint8_t*>(head.constData());
                DWORD ntSig = *reinterpret_cast<const DWORD*>(base + dos->e_lfanew);
                if (ntSig != IMAGE_NT_SIGNATURE) continue;

                auto* fh = reinterpret_cast<const IMAGE_FILE_HEADER*>(base + dos->e_lfanew + sizeof(DWORD));
                if (fh->Machine == IMAGE_FILE_MACHINE_AMD64)
                    architecture = "x64 (64-bit)";
                else if (fh->Machine == IMAGE_FILE_MACHINE_I386)
                    architecture = "x86 (32-bit)";
                else
                    continue;

                QList<MetaRow> exeVerInfo = extractVersionInfo(exePath);
                for (const auto& r : exeVerInfo)
                    if (r.param == "ProductName") { gameName = r.value; break; }

                GetFixedFileVersion(exePath, gameVersion);
                break;
            }
        }

        // Fallback
        if (architecture == "Unknown (no platform siblings found)")
        {
            QString fnPlatform = DetectPlatformFromFileName(dllPath);
            if (!fnPlatform.isEmpty())
                architecture = fnPlatform;
        }

        // Build type has no dedicated detection at all for .BuildSettings
        // (unlike detectBuildMode(), which relies on vtable slot ordering
        // that a text file doesn't have), so this is filename-only,
        // .BuildSettings-specific, by design rather than as a last resort
        QString buildType = DetectBuildTypeFromFileName(dllPath);

        m_pathLabel->setText(dllPath);
        m_titleLabel->setText(gameName.isEmpty() ? fi.fileName() : gameName);

        if (iconPixmap.isNull())
        {
            QIcon icon = findAssociatedIcon(dllPath);
            m_iconLabel->setPixmap(icon.isNull()
                ? style()->standardIcon(QStyle::SP_FileIcon).pixmap(64, 64)
                : icon.pixmap(96, 96));
        }
        else
        {
            m_iconLabel->setPixmap(iconPixmap);
        }

        // Same Initial Information ordering as the rest of loadDll():
        // Game Name, then Game Version, then Build Type, then Architecture
        QList<MetaRow> rows;
        if (!gameName.isEmpty())
            rows.append({ "Game Name", gameName, RowKind::Normal, TableKind::Initial });
        if (!gameVersion.isEmpty())
            rows.append({ "Game Version", gameVersion, RowKind::Normal, TableKind::Initial });
        if (!buildType.isEmpty())
            rows.append({ "Build Type", buildType, RowKind::Normal, TableKind::Initial });
        rows.append({ "Architecture", architecture, RowKind::Normal, TableKind::Initial });

        for (auto& r : fields)
            rows.append(r);

        m_statusLabel->setText(QString("Loaded successfully - found %1 build params.")
            .arg(fields.size()));

        populateTable(rows);
        return true;
    }

    if (!fi.fileName().startsWith("Engine.BuildInfo", Qt::CaseInsensitive))
    {
        showError("This tool only accepts DLLs named \"Engine.BuildInfo*.dll\".\n"
            "Selected file: " + fi.fileName());
        return false;
    }

    // Peek at the first few bytes to determine container format
    bool isElf = false;
    bool isNrs = false;
    WORD machine = 0;
    {
        QFile probe(dllPath);
        if (!probe.open(QIODevice::ReadOnly))
        {
            showError("Could not open file for reading:\n" + dllPath);
            return false;
        }
        QByteArray head = probe.read(1024);
        probe.close();

        // Retail .prx files are normally SELF-wrapped (SCE signature +
        // segment-info table prepended to the real ELF) rather than a bare
        // ELF — recognize that magic here too, so the file still takes the
        // ELF/PRX path below instead of falling through to the PE sniff
        constexpr uint32_t kSelfMagic = 0x1D3D154F;
        uint32_t maybeSelfMagic = 0;
        if (head.size() >= 4)
            memcpy(&maybeSelfMagic, head.constData(), 4);

        if (head.size() >= 4
            && (uint8_t)head[0] == 0x7F && head[1] == 'E' && head[2] == 'L' && head[3] == 'F')
        {
            isElf = true;
            // PS4 .prx and Switch .nrs both start with plain ELF magic,
            // so container format alone can't tell them apart — check
            // e_machine instead (offset 0x12 in a standard ELF header)
            if (head.size() >= 0x14)
            {
                uint16_t elfMachine = 0;
                memcpy(&elfMachine, head.constData() + 0x12, 2);
                if (elfMachine == kEM_AARCH64)
                    isNrs = true;
            }
        }
        else if (maybeSelfMagic == kSelfMagic)
        {
            isElf = true; // SCE SELF-wrapped — always PS4/.prx
        }
        else
        {
            if (head.size() < (int)sizeof(IMAGE_DOS_HEADER))
            {
                showError("Not a valid PE or ELF file (too small).");
                return false;
            }
            auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(head.constData());
            if (dos->e_magic != IMAGE_DOS_SIGNATURE
                || dos->e_lfanew <= 0
                || (size_t)dos->e_lfanew + sizeof(DWORD) + sizeof(IMAGE_FILE_HEADER) > (size_t)head.size())
            {
                showError("Not a valid PE file (bad DOS header).");
                return false;
            }
            const uint8_t* base = reinterpret_cast<const uint8_t*>(head.constData());
            DWORD ntSig = *reinterpret_cast<const DWORD*>(base + dos->e_lfanew);
            if (ntSig != IMAGE_NT_SIGNATURE)
            {
                showError("Not a valid PE file (bad NT header).");
                return false;
            }
            auto* fh = reinterpret_cast<const IMAGE_FILE_HEADER*>(base + dos->e_lfanew + sizeof(DWORD));
            machine = fh->Machine;
        }
    }

#ifdef _WIN64
    const WORD hostMachine = IMAGE_FILE_MACHINE_AMD64;
#else
    const WORD hostMachine = IMAGE_FILE_MACHINE_I386;
#endif

    if (!isElf && machine != IMAGE_FILE_MACHINE_AMD64 && machine != IMAGE_FILE_MACHINE_I386)
    {
        showError("Unsupported architecture for this DLL. Only x86 (32-bit), "
            "x64 (64-bit), PS4 (.prx), and Xbox One targets are currently supported.");
        return false;
    }

    // machine == AMD64 alone doesn't mean LoadLibraryW can actually load
    // this file — Durango PEs report the same COFF machine type as a
    // normal x64 Windows DLL. Sniff the import table now, before we ever
    // decide whether to call LoadLibrary, so Durango builds get routed to
    // the static reader below instead of failing with error 126
    bool isXboxOne = false;
    if (!isElf)
    {
        QFile archProbe(dllPath);
        if (archProbe.open(QIODevice::ReadOnly))
        {
            QByteArray fullBytes = archProbe.readAll();
            archProbe.close();
            isXboxOne = IsXboxOnePe(fullBytes);
        }
    }

    // ---- UI: icon + title ----
    QPixmap iconPixmap;

    if (isNrs)
    {
        // No confirmed sibling icon file/format for Switch dumps yet
        // (likely control.nacp, analogous to PS4's param.sfo) — leave
        // blank rather than guess at a binary layout without a sample
    }
    else if (isElf)
    {
        // PS4 dumps carry their icon as icon0.png somewhere beneath the
        // .prx's own directory tree (typically under sce_sys/), rather
        // than as an embedded PE resource or a sibling .exe's icon
        QString iconPath = FindFileInSubtree(fi.absoluteDir(), "icon0.png");
        if (!iconPath.isEmpty())
        {
            QPixmap p(iconPath);
            if (!p.isNull())
                iconPixmap = p.scaled(96, 96, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        }
    }
    else if (isXboxOne)
    {
        // Durango / UWP packages carry their tile art as Logo.png sitting
        // directly alongside the BuildInfo DLL (it's what AppxManifest.xml
        // declares as VisualElements/@Logo), rather than as an embedded PE
        // resource the way a normal PC .exe/.dll would
        QString logoPath = fi.absoluteDir().filePath("Logo.png");
        if (QFile::exists(logoPath))
        {
            QPixmap p(logoPath);
            if (!p.isNull())
                iconPixmap = p.scaled(96, 96, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        }
    }
    else
    {
        QIcon icon = findAssociatedIcon(dllPath);
        if (!icon.isNull())
        {
            // Pick the largest embedded size, then explicitly scale it to the
            // label's target size in EITHER direction
            QList<QSize> sizes = icon.availableSizes();
            QSize best = sizes.isEmpty() ? QSize(64, 64) : sizes.first();
            for (const QSize& sz : sizes)
                if (sz.width() * sz.height() > best.width() * best.height())
                    best = sz;

            iconPixmap = icon.pixmap(best);
            iconPixmap = iconPixmap.scaled(96, 96, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        }
    }

    if (iconPixmap.isNull())
        iconPixmap = style()->standardIcon(QStyle::SP_FileIcon).pixmap(64, 64);

    m_iconLabel->setPixmap(iconPixmap);
    m_pathLabel->setText(dllPath);

    // ---- Gather all metadata sections ----
    QList<MetaRow> rows;

    // --- PE / Debug Info rows ---
    QList<MetaRow> dynFields;

    MetaRow archRow{ "Architecture",
                isNrs ? "AArch64 (Switch)"
                : isElf ? "x64 (PS4)"
                : isXboxOne ? "x64 (XB1)"
                : (machine == IMAGE_FILE_MACHINE_I386 ? "x86 (32-bit)" : "x64 (64-bit)"),
                RowKind::Normal, TableKind::Initial };
    rows.append(archRow);

    if (isNrs)
    {
        QList<MetaRow> dbgInfo = extractDebugInfoNrs(dllPath);
        rows.append(dbgInfo);

        dynFields = extractDynamicFieldsNrs(dllPath);
    }
    else if (isElf)
    {
        QList<MetaRow> dbgInfo = extractDebugInfoPrx(dllPath);
        rows.append(dbgInfo);

        dynFields = extractDynamicFieldsPrx(dllPath);
    }
    else if (machine == hostMachine && !isXboxOne)
    {
        std::wstring wpath = dllPath.toStdWString();
        HMODULE hMod = LoadLibraryW(wpath.c_str());
        if (!hMod)
        {
            DWORD err = GetLastError();
            showError(QString("Failed to load DLL (Win32 error %1).\n"
                "This usually means the DLL has missing dependencies.").arg(err));
            return false;
        }
        m_hMod = hMod;

        QList<MetaRow> dbgInfo = extractDebugInfo(hMod);
        for (auto& r : dbgInfo)
            r.tableKind = TableKind::PeDebug;
        rows.append(dbgInfo);

        dynFields = extractDynamicFields(hMod);
    }
    else
    {
        QList<MetaRow> dbgInfo = extractDebugInfoStatic(dllPath);
        rows.append(dbgInfo);

        dynFields = extractDynamicFieldsStatic(dllPath);
    }

    // --- Game name/version ---
    QString gameName, gameVersion;

    if (isNrs)
    {
        // Same story as the icon above — no confirmed sibling metadata
        // file/format for Switch yet, so this is left blank rather than
        // guessed at
    }
    else if (isElf)
    {
        // PS4 dumps carry title/version metadata in param.sfo somewhere
        // beneath the .prx's own directory tree (typically under
        // sce_sys/) — there's no Win32-style version resource to read
        // here at all, so this replaces the PE lookup path entirely
        // rather than falling back to it
        QString sfoPath = FindFileInSubtree(fi.absoluteDir(), "param.sfo");
        if (!sfoPath.isEmpty())
        {
            QHash<QString, QString> sfo;
            if (ParseParamSfo(sfoPath, sfo))
            {
                // param.sfo can carry two title entries — a base "TITLE"
                // and a locale-specific "TITLE_01" sitting right next to
                // TITLE_ID. Prefer TITLE_01 (the one adjacent to the CUSA
                // ID) whenever both are present
                if (sfo.contains("TITLE_01"))
                    gameName = sfo.value("TITLE_01");
                else if (sfo.contains("TITLE"))
                    gameName = sfo.value("TITLE");

                // Prefer "APP_VER" over "VERSION"
                if (sfo.contains("APP_VER"))
                    gameVersion = FormatSfoVersion(sfo.value("APP_VER"));
                else if (sfo.contains("VERSION"))
                    gameVersion = FormatSfoVersion(sfo.value("VERSION"));
            }
        }
    }
    else if (isXboxOne)
    {
        // Durango packages carry title/version metadata in
        // AppxManifest.xml sitting directly alongside the BuildInfo DLL
        QString manifestPath = fi.absoluteDir().filePath("AppxManifest.xml");
        if (QFile::exists(manifestPath))
        {
            QString displayName, version;
            if (ParseAppxManifest(manifestPath, displayName, version))
            {
                if (!displayName.isEmpty()) gameName = displayName;
                if (!version.isEmpty())     gameVersion = version;
            }
        }
    }
    else
    {
        // Read the BuildInfo DLL's own version resource first, then the
        // sibling .exe's
        QList<MetaRow> verInfo = extractVersionInfo(dllPath);
        for (const auto& r : verInfo)
            if (r.param == "ProductName") gameName = r.value;

        bool nameIsGeneric = gameName.isEmpty()
            || gameName.compare("Frostbite", Qt::CaseInsensitive) == 0;

        if (nameIsGeneric)
        {
            QDir dir = fi.absoluteDir();
            QStringList exes = dir.entryList({ "*.exe" }, QDir::Files);
            for (const QString& exe : exes)
            {
                if (exe.contains("unins", Qt::CaseInsensitive)) continue;

                QList<MetaRow> exeVerInfo = extractVersionInfo(dir.filePath(exe));
                QString exeName;
                for (const auto& r : exeVerInfo)
                    if (r.param == "ProductName") { exeName = r.value; break; }

                if (!exeName.isEmpty())
                {
                    gameName = exeName;
                    GetFixedFileVersion(dir.filePath(exe), gameVersion);
                    break;
                }
            }
        }

        // Version always comes from the numeric fixed-info block
        if (gameVersion.isEmpty())
            GetFixedFileVersion(dllPath, gameVersion);

        if (gameVersion.isEmpty())
        {
            QDir dir = fi.absoluteDir();
            QStringList exes = dir.entryList({ "*.exe" }, QDir::Files);
            for (const QString& exe : exes)
            {
                if (exe.contains("unins", Qt::CaseInsensitive)) continue;
                if (GetFixedFileVersion(dir.filePath(exe), gameVersion) && !gameVersion.isEmpty())
                    break;
            }
        }
    }

    m_titleLabel->setText(gameName.isEmpty() ? fi.fileName() : gameName);

    // --- Initial Information table ---
    int insertPos = 0;
    if (!gameName.isEmpty())
        rows.insert(insertPos++, { "Game Name", gameName, RowKind::Normal, TableKind::Initial });
    if (!gameVersion.isEmpty())
        rows.insert(insertPos++, { "Game Version", gameVersion, RowKind::Normal, TableKind::Initial });
    if (!m_buildMode.isEmpty())
        rows.insert(insertPos++, { "Build Type", m_buildMode, RowKind::Normal, TableKind::Initial });

    // --- Build Info rows ---
    if (!dynFields.isEmpty())
    {
        for (auto& r : dynFields)
        {
            if (r.param == "Factory Export") continue; // skip it
            r.tableKind = TableKind::BuildInfo;
            rows.append(r);
        }
        m_statusLabel->setText(QString("Loaded successfully - found %1 build info field(s).")
            .arg(dynFields.size() - 1));
    }
    else
    {
        m_statusLabel->setText(
            "Loaded, but no recognizable BuildInfo-style factory export was found. "
            "Showing file/version metadata only.");
    }

    populateTable(rows);
    return true;
}

QList<BuildInfoWindow::MetaRow> BuildInfoWindow::extractVersionInfo(const QString& path) const
{
    QList<MetaRow> out;

    std::wstring wpath = path.toStdWString();
    DWORD handle = 0;
    DWORD size = GetFileVersionInfoSizeW(wpath.c_str(), &handle);
    if (size == 0) return out;

    std::vector<BYTE> buffer(size);
    if (!GetFileVersionInfoW(wpath.c_str(), handle, size, buffer.data()))
        return out;

    struct LangCodePage { WORD wLanguage; WORD wCodePage; };
    LangCodePage* translations = nullptr;
    UINT translationBytes = 0;
    if (!VerQueryValueW(buffer.data(), L"\\VarFileInfo\\Translation",
        reinterpret_cast<LPVOID*>(&translations), &translationBytes)
        || translationBytes < sizeof(LangCodePage))
        return out;

    auto queryField = [&](const wchar_t* fieldName) -> QString
        {
            wchar_t subBlock[256];
            swprintf_s(subBlock, L"\\StringFileInfo\\%04x%04x\\%s",
                translations[0].wLanguage, translations[0].wCodePage, fieldName);
            LPVOID valuePtr = nullptr;
            UINT valueLen = 0;
            if (VerQueryValueW(buffer.data(), subBlock, &valuePtr, &valueLen)
                && valuePtr && valueLen > 0)
                return QString::fromWCharArray(reinterpret_cast<const wchar_t*>(valuePtr));
            return QString();
        };

    static const wchar_t* kFields[] = {
        L"CompanyName", L"FileDescription", L"FileVersion", L"InternalName",
        L"LegalCopyright", L"OriginalFilename", L"ProductName", L"ProductVersion"
    };
    for (const wchar_t* f : kFields)
    {
        QString val = queryField(f);
        if (!val.isEmpty())
            out.append({ QString::fromWCharArray(f), val, RowKind::Normal, TableKind::BuildInfo });
    }
    return out;
}

QList<BuildInfoWindow::MetaRow> BuildInfoWindow::extractDebugInfo(HMODULE hMod) const
{
    QList<MetaRow> out;

    IMAGE_DOS_HEADER* dos = RVA<IMAGE_DOS_HEADER>(hMod, 0);
    IMAGE_NT_HEADERS64* nt = RVA<IMAGE_NT_HEADERS64>(hMod, dos->e_lfanew);

    time_t t = static_cast<time_t>(nt->FileHeader.TimeDateStamp);
    if (t > 0)
    {
        tm tmStruct{};
        if (gmtime_s(&tmStruct, &t) == 0)
        {
            char buf[64] = {};
            strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S UTC", &tmStruct);
            out.append({ "Compile Timestamp", QString::fromLatin1(buf), RowKind::Normal, TableKind::PeDebug });
        }
    }

    auto& debugDataDir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG];
    if (debugDataDir.VirtualAddress == 0 || debugDataDir.Size == 0)
        return out;

    int count = (int)(debugDataDir.Size / sizeof(IMAGE_DEBUG_DIRECTORY));
    IMAGE_DEBUG_DIRECTORY* entries = RVA<IMAGE_DEBUG_DIRECTORY>(hMod, debugDataDir.VirtualAddress);

    for (int i = 0; i < count; i++)
    {
        if (entries[i].Type != IMAGE_DEBUG_TYPE_CODEVIEW) continue;
        if (entries[i].AddressOfRawData == 0) continue;

        BYTE* cv = RVA<BYTE>(hMod, entries[i].AddressOfRawData);
        if (memcmp(cv, "RSDS", 4) != 0) continue;

#pragma pack(push, 1)
        struct CvInfoPdb70
        {
            DWORD signature;
            GUID  guid;
            DWORD age;
            char  pdbName[1];
        };
#pragma pack(pop)
        auto* info = reinterpret_cast<CvInfoPdb70*>(cv);

        QString guidStr = QString("%1-%2-%3-%4%5-%6%7%8%9%10%11")
            .arg(info->guid.Data1, 8, 16, QChar('0'))
            .arg(info->guid.Data2, 4, 16, QChar('0'))
            .arg(info->guid.Data3, 4, 16, QChar('0'))
            .arg(info->guid.Data4[0], 2, 16, QChar('0'))
            .arg(info->guid.Data4[1], 2, 16, QChar('0'))
            .arg(info->guid.Data4[2], 2, 16, QChar('0'))
            .arg(info->guid.Data4[3], 2, 16, QChar('0'))
            .arg(info->guid.Data4[4], 2, 16, QChar('0'))
            .arg(info->guid.Data4[5], 2, 16, QChar('0'))
            .arg(info->guid.Data4[6], 2, 16, QChar('0'))
            .arg(info->guid.Data4[7], 2, 16, QChar('0'))
            .toUpper();

        out.append({ "PDB GUID", guidStr, RowKind::Normal, TableKind::PeDebug });
        out.append({ "PDB Age", QString::number(info->age), RowKind::Normal, TableKind::PeDebug });

        // Don't trust a bare null-terminator scan here
        const BYTE* nameBytes = reinterpret_cast<const BYTE*>(info->pdbName);
        int nameByteCount = (int)entries[i].SizeOfData - (int)offsetof(CvInfoPdb70, pdbName);
        QString pdbPath;
        if (nameByteCount > 1)
        {
            // Heuristic: UTF-16 ASCII text has a 0x00 byte at every other
            // position; 8-bit text does not. Sample several characters in
            // rather than just the first, since one byte pair alone can
            // coincide either way
            bool looksWide = true;
            int sampleChars = qMin(nameByteCount / 2, 8);
            for (int c = 0; c < sampleChars; ++c)
            {
                if (nameBytes[c * 2 + 1] != 0x00) { looksWide = false; break; }
            }

            if (looksWide)
                pdbPath = QString::fromUtf16(reinterpret_cast<const char16_t*>(nameBytes),
                    nameByteCount / 2);
            else
                pdbPath = QString::fromUtf8(reinterpret_cast<const char*>(nameBytes), nameByteCount);

            int nul = pdbPath.indexOf(QChar(0));
            if (nul >= 0) pdbPath.truncate(nul);
        }

        out.append({ "PDB Path", pdbPath, RowKind::Normal, TableKind::PeDebug });
    }
    return out;
}

QList<BuildInfoWindow::MetaRow> BuildInfoWindow::extractDebugInfoStatic(const QString& dllPath) const
{
    QList<MetaRow> out;

    QFile f(dllPath);
    if (!f.open(QIODevice::ReadOnly))
        return out;
    QByteArray bytes = f.readAll();
    f.close();

    StaticPeImage img;
    const IMAGE_FILE_HEADER* fileHeader = nullptr;
    const uint8_t* optHeaderPtr = nullptr;
    WORD magic = 0;
    if (!ParseStaticPeImage(bytes, img, &fileHeader, &optHeaderPtr, &magic))
        return out;

    time_t t = static_cast<time_t>(fileHeader->TimeDateStamp);
    if (t > 0)
    {
        tm tmStruct{};
        if (gmtime_s(&tmStruct, &t) == 0)
        {
            char buf[64] = {};
            strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S UTC", &tmStruct);
            out.append({ "Compile Timestamp", QString::fromLatin1(buf), RowKind::Normal, TableKind::PeDebug });
        }
    }

    DWORD debugRVA = 0, debugSize = 0;
    if (!GetDataDirectory(optHeaderPtr, magic, IMAGE_DIRECTORY_ENTRY_DEBUG, debugRVA, debugSize)
        || debugRVA == 0 || debugSize == 0)
        return out;

    int count = (int)(debugSize / sizeof(IMAGE_DEBUG_DIRECTORY));
    auto* entries = reinterpret_cast<const IMAGE_DEBUG_DIRECTORY*>(
        RvaToPtr(img, debugRVA, (size_t)count * sizeof(IMAGE_DEBUG_DIRECTORY)));
    if (!entries) return out;

#pragma pack(push, 1)
    struct CvInfoPdb70
    {
        DWORD signature;
        GUID  guid;
        DWORD age;
        char  pdbName[1];
    };
#pragma pack(pop)

    for (int i = 0; i < count; i++)
    {
        if (entries[i].Type != IMAGE_DEBUG_TYPE_CODEVIEW) continue;
        if (entries[i].AddressOfRawData == 0) continue;

        const uint8_t* cv = RvaToPtr(img, entries[i].AddressOfRawData, sizeof(CvInfoPdb70));
        if (!cv || memcmp(cv, "RSDS", 4) != 0) continue;

        auto* info = reinterpret_cast<const CvInfoPdb70*>(cv);

        QString guidStr = QString("%1-%2-%3-%4%5-%6%7%8%9%10%11")
            .arg(info->guid.Data1, 8, 16, QChar('0'))
            .arg(info->guid.Data2, 4, 16, QChar('0'))
            .arg(info->guid.Data3, 4, 16, QChar('0'))
            .arg(info->guid.Data4[0], 2, 16, QChar('0'))
            .arg(info->guid.Data4[1], 2, 16, QChar('0'))
            .arg(info->guid.Data4[2], 2, 16, QChar('0'))
            .arg(info->guid.Data4[3], 2, 16, QChar('0'))
            .arg(info->guid.Data4[4], 2, 16, QChar('0'))
            .arg(info->guid.Data4[5], 2, 16, QChar('0'))
            .arg(info->guid.Data4[6], 2, 16, QChar('0'))
            .arg(info->guid.Data4[7], 2, 16, QChar('0'))
            .toUpper();

        out.append({ "PDB GUID", guidStr, RowKind::Normal, TableKind::PeDebug });
        out.append({ "PDB Age", QString::number(info->age), RowKind::Normal, TableKind::PeDebug });

        // Same UTF-16-vs-8-bit heuristic as the live-execution path
        // (extractDebugInfo) — some linkers write this name as UTF-16
        // rather than 8-bit, and SizeOfData is the only reliable way to
        // know the real byte length rather than guessing from one NUL
        int nameByteCount = (int)entries[i].SizeOfData - (int)offsetof(CvInfoPdb70, pdbName);
        QString pdbPath;
        if (nameByteCount > 1)
        {
            const uint8_t* nameBytes = RvaToPtr(img,
                entries[i].AddressOfRawData + (DWORD)offsetof(CvInfoPdb70, pdbName),
                (size_t)nameByteCount);
            if (nameBytes)
            {
                bool looksWide = true;
                int sampleChars = qMin(nameByteCount / 2, 8);
                for (int c = 0; c < sampleChars; ++c)
                {
                    if (nameBytes[c * 2 + 1] != 0x00) { looksWide = false; break; }
                }

                if (looksWide)
                    pdbPath = QString::fromUtf16(reinterpret_cast<const char16_t*>(nameBytes), nameByteCount / 2);
                else
                    pdbPath = QString::fromUtf8(reinterpret_cast<const char*>(nameBytes), nameByteCount);

                int nul = pdbPath.indexOf(QChar(0));
                if (nul >= 0) pdbPath.truncate(nul);
            }
        }

        out.append({ "PDB Path", pdbPath, RowKind::Normal, TableKind::PeDebug });
    }
    return out;
}

void BuildInfoWindow::assignFieldNames(QList<MetaRow>& fields) const
{
    QRegularExpression reBuildIso(R"(^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}Z\s*$)");
    QRegularExpression reBuildDate(R"(^\d{4}-\d{2}-\d{2}\s*$)");
    QRegularExpression reBuildTime(R"(^\d{2}:\d{2}(:\d{2})?Z?\s*$)");
    QRegularExpression reBuildDateSlash(R"(^\d{1,2}/\d{1,2}/\d{4}\s*$)");
    QRegularExpression reBuildTimeAmPm(R"(^\d{1,2}:\d{2}\s*[AaPp][Mm]\s*$)");
    QRegularExpression reDepotPort(R"(^(ssl:)?[\w.\-]+:\d+$)");

    // Separate slots by type, preserving their original index so we can
    // write names back in a second pass
    QList<int> stringSlots, intSlots, boolSlots;
    for (int i = 0; i < fields.size(); ++i)
    {
        const QString& v = fields[i].value;
        if (v.startsWith("(String)"))      stringSlots.append(i);
        else if (v.startsWith("(Int32)"))  intSlots.append(i);
        else if (v.startsWith("(Bool)"))   boolSlots.append(i);
    }

    // --- Booleans: only one exists in this interface ---
    for (int i : boolSlots)
        fields[i].param = "Is Auto Build";

    // --- Strings: match by value pattern first, remainder by declaration order ---
    // Extract the bare string value from "(String) \"foo\"" for matching
    auto bareString = [](const QString& v) -> QString {
        int a = v.indexOf('"');
        int b = v.lastIndexOf('"');
        if (a >= 0 && b > a) return v.mid(a + 1, b - a - 1).trimmed();
        return v.trimmed();
        };

    QList<int> unnamedStrings;
    for (int i : stringSlots)
    {
        QString s = bareString(fields[i].value);
        if (reBuildIso.match(s).hasMatch())
            fields[i].param = "Build ISO Date";
        else if (reBuildDate.match(s).hasMatch() || reBuildDateSlash.match(s).hasMatch())
            fields[i].param = "Build Date";
        else if (reBuildTime.match(s).hasMatch() || reBuildTimeAmPm.match(s).hasMatch())
            fields[i].param = "Build Time";
        else if (reDepotPort.match(s).hasMatch())
            fields[i].param = "Depot Port";
        else
            unnamedStrings.append(i);
    }

    // Remaining strings follow header declaration order:
    // getBranchName, getLicenseeId, getStudioName, getFrostbiteRelease,
    // getUsername, getHostName (non-retail only)
    QRegularExpression reFbRelease(
        R"(^(Release\s+\S|\d{4}-[A-Za-z]{2}\d*|\d{4}-\d+\.\d+|\d{4}-\d+-[A-Za-z]+\d*|\d+\.\d+))",
        QRegularExpression::CaseInsensitiveOption);

    int fbReleaseIdx = -1;
    QString fbReleaseValue;
    for (int i : unnamedStrings)
    {
        if (reFbRelease.match(bareString(fields[i].value)).hasMatch())
        {
            fbReleaseIdx = i;
            fbReleaseValue = bareString(fields[i].value);
            fields[i].param = "Frostbite Release";
            break;
        }
    }

    // Some builds return the identical string from both getFrostbiteRelease
    // and getFrostbiteReleaseName (e.g. "2023.1.PR2" twice in a row) rather
    // than a distinct human-readable name for the second getter. Without
    // this second pass, that duplicate falls through unnamed into the
    // generic postSlots sequence below and silently consumes the
    // "Username" slot — which is what was shifting the real username
    // ("svc_do013") down into "Host Name" instead
    int fbReleaseNameIdx = -1;
    if (fbReleaseIdx != -1)
    {
        for (int i : unnamedStrings)
        {
            if (i == fbReleaseIdx) continue;
            if (bareString(fields[i].value) == fbReleaseValue)
            {
                fbReleaseNameIdx = i;
                fields[i].param = "Frostbite Release Name";
                break;
            }
        }
    }

    QList<int> remainingStrings;
    for (int i : unnamedStrings)
        if (i != fbReleaseIdx && i != fbReleaseNameIdx)
            remainingStrings.append(i);

    // Detect streamName: appears as the second unnamed string, is purely
    // lowercase alphanumeric with underscores/hyphens but no dots, spaces,
    // digits-only segments, or capital letters
    QRegularExpression reStreamName(R"(^[a-z][a-z0-9_\-]*$)");

    int streamNameIdx = -1;
    // Only check slot index 1 of remainingStrings (second unnamed string)
    if (remainingStrings.size() >= 2)
    {
        QString s = bareString(fields[remainingStrings[1]].value);
        if (reStreamName.match(s).hasMatch())
        {
            streamNameIdx = remainingStrings[1];
            fields[streamNameIdx].param = "Stream Name";
        }
    }

    // Build the ordered pre-release list excluding streamName
    QList<int> preSlots;
    for (int i : remainingStrings)
        if (i != streamNameIdx) preSlots.append(i);

    static const char* kPreRelease[] = { "Branch Name", "Licensee ID" };
    for (int j = 0; j < qMin(2, preSlots.size()); ++j)
        fields[preSlots[j]].param = kPreRelease[j];

    static const char* kPostRelease[] = { "Studio Name", "Username", "Host Name" };
    QList<int> postSlots;
    for (int j = 2; j < preSlots.size(); ++j)
        postSlots.append(preSlots[j]);

    if (postSlots.size() == 1)
        fields[postSlots[0]].param = "Username";
    else
    {
        int pi = 0;
        for (int i : postSlots)
            fields[i].param = (pi < 3) ? kPostRelease[pi++] : QString("String Field %1").arg(i);
    }

    // --- Integers follow header declaration order:
    // getChangelist, getSourceChangelist, getFrostbiteChangelist,
    // getUsergroup (non-retail only) ---
    static const char* kIntNames3[] = {
        "Changelist",
        "Source Changelist",
        "Frostbite Changelist",
    };
    static const char* kIntNames2[] = {
        "Changelist",
        "Frostbite Changelist",
    };

    int ii = 0;
    int intCount = intSlots.size();

    // Check if last int looks like a usergroup (small value 0-10)
    bool hasUserGroup = false;
    if (intCount >= 3)
    {
        int32_t lastIntVal = 0;
        // Parse the raw value from "(Int32) N"
        QString lastValStr = fields[intSlots.last()].value;
        lastValStr.remove("(Int32)").remove("(Int32) ");
        lastIntVal = lastValStr.trimmed().toInt();
        if (lastIntVal >= 0 && lastIntVal <= 10)
            hasUserGroup = true;
    }

    int namedIntCount = hasUserGroup ? intCount - 1 : intCount;
    const char** kNames = (namedIntCount <= 2) ? kIntNames2 : kIntNames3;
    int nameCount = (namedIntCount <= 2) ? 2 : 3;

    for (int i : intSlots)
    {
        if (hasUserGroup && i == intSlots.last())
            fields[i].param = "User Group";
        else if (ii < nameCount)
            fields[i].param = kNames[ii++];
        else
            fields[i].param = QString("Int Field %1").arg(i);
    }
}

void BuildInfoWindow::detectBuildMode(const QList<MetaRow>& fields) const
{
    // Fields are in raw vtable walk order here (before reorderFields)
    // Retail: usergroup int slot comes AFTER the first build time/date string slot
    // Final:  usergroup int slot comes BEFORE the first build time/date string slot
    QRegularExpression reTime(R"(^\d{2}:\d{2}|^\d{4}-\d{2}-\d{2})");

    int lastIntIdx = -1;
    int firstTimeIdx = -1;
    for (int i = 0; i < fields.size(); ++i)
    {
        const QString& v = fields[i].value;
        if (v.startsWith("(Int32)"))
            lastIntIdx = i;
        if (firstTimeIdx == -1 && v.startsWith("(String)"))
        {
            int a = v.indexOf('"'), b = v.lastIndexOf('"');
            QString s = (a >= 0 && b > a) ? v.mid(a + 1, b - a - 1).trimmed() : QString();
            if (reTime.match(s).hasMatch())
                firstTimeIdx = i;
        }
    }

    if (lastIntIdx != -1 && firstTimeIdx != -1)
        m_buildMode = (lastIntIdx > firstTimeIdx) ? "Retail" : "Final";
    else
        m_buildMode.clear();
}

QList<BuildInfoWindow::MetaRow> BuildInfoWindow::extractDynamicFields(HMODULE hMod) const
{
    QList<MetaRow> out;

    IMAGE_DOS_HEADER* dos = RVA<IMAGE_DOS_HEADER>(hMod, 0);
    IMAGE_NT_HEADERS64* nt = RVA<IMAGE_NT_HEADERS64>(hMod, dos->e_lfanew);

    uintptr_t moduleBase = reinterpret_cast<uintptr_t>(hMod);
    uintptr_t moduleLimit = moduleBase + nt->OptionalHeader.SizeOfImage;

    auto& exportDataDir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (exportDataDir.VirtualAddress == 0)
        return out; // no exports at all

    IMAGE_EXPORT_DIRECTORY* exportDir = RVA<IMAGE_EXPORT_DIRECTORY>(hMod, exportDataDir.VirtualAddress);
    DWORD* nameRVAs = RVA<DWORD>(hMod, exportDir->AddressOfNames);
    WORD* nameOrdinals = RVA<WORD>(hMod, exportDir->AddressOfNameOrdinals);
    DWORD* funcRVAs = RVA<DWORD>(hMod, exportDir->AddressOfFunctions);

    // ---- Tier 1: exports whose name contains "buildinfo" ----
    QStringList candidateNames;
    for (DWORD i = 0; i < exportDir->NumberOfNames; i++)
    {
        const char* namePtr = RVA<const char>(hMod, nameRVAs[i]);
        QString name = QString::fromUtf8(namePtr);
        if (name.contains("buildinfo", Qt::CaseInsensitive))
            candidateNames.append(name);
    }
    // If nothing matched by name, fall back to trying every named export
    if (candidateNames.isEmpty())
    {
        for (DWORD i = 0; i < exportDir->NumberOfNames; i++)
        {
            const char* namePtr = RVA<const char>(hMod, nameRVAs[i]);
            candidateNames.append(QString::fromUtf8(namePtr));
        }
    }

    auto tryFactory = [&](void* funcPtr) -> QList<MetaRow>
        {
            QList<MetaRow> fields;

            void* obj = nullptr;
            if (!SafeCallFactory(funcPtr, &obj) || !obj)
                return fields;

            void* vtablePtr = nullptr;
            if (!SafeReadPtr(obj, &vtablePtr) || !vtablePtr)
                return fields;

            uintptr_t vtableAddr = reinterpret_cast<uintptr_t>(vtablePtr);
            if (vtableAddr < moduleBase || vtableAddr >= moduleLimit)
                return fields; // not our module's vtable — reject candidate

            const int kMaxSlots = 64;
            for (int slot = 0; slot < kMaxSlots; slot++)
            {
                void* slotAddr = reinterpret_cast<void*>(vtableAddr + (size_t)slot * sizeof(void*));
                void* funcAt = nullptr;
                if (!SafeReadPtr(slotAddr, &funcAt) || !funcAt)
                    break;

                uintptr_t funcAddr = reinterpret_cast<uintptr_t>(funcAt);
                if (funcAddr < moduleBase || funcAddr >= moduleLimit)
                    break;

                uint8_t code[16] = {};
                if (!SafeReadBytes(funcAt, code, sizeof(code)))
                    break;

                MetaRow field;
                field.param = QString(); // filled in after all slots are collected
                bool matched = false;

                // Pattern A: 48 8B 05 dd dd dd dd  -> mov rax,[rip+disp32] (pointer/string indirect)
                if (code[0] == 0x48 && code[1] == 0x8B && code[2] == 0x05)
                {
                    int32_t disp;
                    memcpy(&disp, &code[3], 4);
                    uintptr_t target = funcAddr + 7 + (intptr_t)disp;
                    void* strPtr = nullptr;
                    if (SafeReadPtr(reinterpret_cast<void*>(target), &strPtr) && strPtr)
                    {
                        QString s;
                        if (SafeReadCString(strPtr, s))
                        {
                            field.value = QString("(String) \"%1\"").arg(s);
                            matched = true;
                        }
                    }
                }
                // Pattern A2: 48 8D 05 dd dd dd dd -> lea rax,[rip+disp32] (string direct)
                else if (code[0] == 0x48 && code[1] == 0x8D && code[2] == 0x05)
                {
                    int32_t disp;
                    memcpy(&disp, &code[3], 4);
                    uintptr_t strAddr = funcAddr + 7 + (intptr_t)disp;
                    QString s;
                    if (SafeReadCString(reinterpret_cast<void*>(strAddr), s))
                    {
                        field.value = QString("(String) \"%1\"").arg(s);
                        matched = true;
                    }
                }
                // Pattern B: 0F B6 05 dd dd dd dd -> movzx eax, byte [rip+disp32] (bool)
                else if (code[0] == 0x0F && code[1] == 0xB6 && code[2] == 0x05)
                {
                    int32_t disp;
                    memcpy(&disp, &code[3], 4);
                    uintptr_t target = funcAddr + 7 + (intptr_t)disp;
                    uint8_t b = 0;
                    if (SafeReadBytes(reinterpret_cast<void*>(target), &b, 1))
                    {
                        field.value = QString("(Bool) %1").arg(b ? "true" : "false");
                        matched = true;
                    }
                }
                // Pattern C: 8B 05 dd dd dd dd -> mov eax,[rip+disp32] (int32 indirect)
                else if (code[0] == 0x8B && code[1] == 0x05)
                {
                    int32_t disp;
                    memcpy(&disp, &code[2], 4);
                    uintptr_t target = funcAddr + 6 + (intptr_t)disp;
                    int32_t v = 0;
                    if (SafeReadBytes(reinterpret_cast<void*>(target), &v, 4))
                    {
                        field.value = QString("(Int32) %1").arg(v);
                        matched = true;
                    }
                }
                // Pattern C2: B8 dd dd dd dd -> mov eax, imm32 (int32 direct)
                else if (code[0] == 0xB8)
                {
                    int32_t v;
                    memcpy(&v, &code[1], 4);
                    field.value = QString("(Int32) %1").arg(v);
                    matched = true;
                }
                // Pattern D: 33 C0 -> xor eax,eax (constant-folded int 0)
                else if (code[0] == 0x33 && code[1] == 0xC0)
                {
                    field.value = "(Int32) 0";
                    matched = true;
                }
                // Pattern E: B0 xx -> mov al, imm8 (constant-folded bool)
                else if (code[0] == 0xB0)
                {
                    field.value = QString("(Bool) %1").arg(code[1] != 0 ? "true" : "false");
                    matched = true;
                }

                if (!matched) break; // unrecognized thunk shape = end of vtable
                fields.append(field);
            }
            return fields;
        };

    // Try tier-1/tier-2 named candidates first
    for (const QString& name : candidateNames)
    {
        QByteArray nameUtf8 = name.toUtf8();
        FARPROC proc = GetProcAddress(hMod, nameUtf8.constData());
        if (!proc) continue;

        QList<MetaRow> fields = tryFactory(reinterpret_cast<void*>(proc));
        if (!fields.isEmpty())
        {
            out.append({ "Factory Export", name, RowKind::Normal, TableKind::BuildInfo });
            detectBuildMode(fields); // detect BEFORE reorder
            assignFieldNames(fields);
            reorderFields(fields);
            out.append(fields);
            return out;
        }
    }

    // ---- Tier 3: try every exported function RVA directly (covers
    // ordinal-only exports with no name) ----
    for (DWORD i = 0; i < exportDir->NumberOfFunctions; i++)
    {
        DWORD rva = funcRVAs[i];
        if (rva == 0) continue;
        void* funcPtr = RVA<void>(hMod, rva);

        QList<MetaRow> fields = tryFactory(funcPtr);
        if (!fields.isEmpty())
        {
            out.append({ "Factory Export", QString("(ordinal ") + QString::number(exportDir->Base + i) + ")", RowKind::Normal, TableKind::BuildInfo });
            detectBuildMode(fields); // detect BEFORE reorder
            assignFieldNames(fields);
            reorderFields(fields);
            out.append(fields);
            return out;
        }
    }

    return out; // nothing found
}

QList<BuildInfoWindow::MetaRow> BuildInfoWindow::extractDynamicFieldsStatic(const QString& dllPath) const
{
    QList<MetaRow> out;

    QFile f(dllPath);
    if (!f.open(QIODevice::ReadOnly))
        return out;
    QByteArray bytes = f.readAll();
    f.close();

    if (bytes.size() < (int)sizeof(IMAGE_DOS_HEADER))
        return out;

    StaticPeImage img;
    WORD magic = 0;
    const uint8_t* optHeaderPtr = nullptr;
    if (!ParseStaticPeImage(bytes, img, nullptr, &optHeaderPtr, &magic))
        return out;

    DWORD exportRVA = 0, exportSize = 0;
    if (!GetDataDirectory(optHeaderPtr, magic, IMAGE_DIRECTORY_ENTRY_EXPORT, exportRVA, exportSize)
        || exportRVA == 0 || exportSize == 0)
        return out;

    auto* exportDir = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(RvaToPtr(img, exportRVA));
    if (!exportDir) return out;

    auto* nameRVAs = reinterpret_cast<const DWORD*>(
        RvaToPtr(img, exportDir->AddressOfNames, exportDir->NumberOfNames * sizeof(DWORD)));
    auto* funcRVAs = reinterpret_cast<const DWORD*>(
        RvaToPtr(img, exportDir->AddressOfFunctions, exportDir->NumberOfFunctions * sizeof(DWORD)));
    auto* ordinals = reinterpret_cast<const WORD*>(
        RvaToPtr(img, exportDir->AddressOfNameOrdinals, exportDir->NumberOfNames * sizeof(WORD)));
    if (!nameRVAs || !funcRVAs || !ordinals) return out;

    DWORD factoryRVA = 0;
    for (DWORD i = 0; i < exportDir->NumberOfNames; i++)
    {
        const uint8_t* namePtr = RvaToPtr(img, nameRVAs[i]);
        if (!namePtr) continue;
        if (qstrcmp(reinterpret_cast<const char*>(namePtr), "getBuildInfo") == 0)
        {
            factoryRVA = funcRVAs[ordinals[i]];
            break;
        }
    }
    if (factoryRVA == 0) return out;

    // The export table itself may point at a bare "jmp" thunk rather
    // than the real factory implementation — resolve that first, since
    // scanning thunk bytes for the flag-store pattern below would just
    // read garbage
    factoryRVA = ResolveJmpThunk(img, factoryRVA);

    // Step 1: locate the vtable array's address
    const uint8_t* fn = RvaToPtr(img, factoryRVA, 16);
    if (!fn) return out;
    ULONGLONG vtableArrayVA = 0;
    const int kScanWindow = 96;

    if (!img.is64)
    {
        for (int i = 0; i + 10 <= kScanWindow; i++)
        {
            const uint8_t* p = RvaToPtr(img, factoryRVA + i, 10);
            if (!p) break;
            if (p[0] == 0xC7 && p[1] == 0x05)
            {
                DWORD imm = 0;
                memcpy(&imm, p + 6, 4);
                vtableArrayVA = imm;
                break;
            }
        }
    }
    else
    {
        for (int i = 0; i + 7 <= kScanWindow; i++)
        {
            const uint8_t* p = RvaToPtr(img, factoryRVA + i, 7);
            if (!p) break;
            if (p[0] == 0x48 && p[1] == 0x8D && (p[2] & 0xC7) == 0x05)
            {
                int32_t disp = 0;
                memcpy(&disp, p + 3, 4);
                ULONGLONG nextInstrVA = img.imageBase + factoryRVA + (DWORD)i + 7;
                vtableArrayVA = nextInstrVA + (int64_t)disp;
                break;
            }
        }
    }

    if (vtableArrayVA == 0) return out; // shape didn't match — bail out cleanly

    DWORD vtableArrayRVA = 0;
    if (!VaToRva(img, vtableArrayVA, vtableArrayRVA)) return out;

    // Step 2: walk the vtable array
    const int kMaxSlots = 64;
    const size_t ptrSize = img.is64 ? 8 : 4;
    for (int slot = 0; slot < kMaxSlots; slot++)
    {
        const uint8_t* slotPtr = RvaToPtr(img, vtableArrayRVA + (DWORD)(slot * ptrSize), ptrSize);
        if (!slotPtr) break;

        ULONGLONG funcVA = 0;
        memcpy(&funcVA, slotPtr, ptrSize);
        if (funcVA == 0) break;

        DWORD funcRVA = 0;
        if (!VaToRva(img, funcVA, funcRVA)) break;

        const uint8_t* code = RvaToPtr(img, funcRVA, 16);
        if (!code) break;

        if (code[0] == 0xE9) // jmp rel32 — incremental-link thunk stub
        {
            funcRVA = ResolveJmpThunk(img, funcRVA);
            code = RvaToPtr(img, funcRVA, 16);
            if (!code) break;
        }

        MetaRow field;
        bool matched = false;

        if (img.is64)
        {
            if (code[0] == 0x48 && code[1] == 0x8B && code[2] == 0x05 && code[7] == 0xC3)
            {
                // mov rax, [rip+disp32] ; retn -> pointer-sized field
                // Every sample seen stores a string pointer here
                int32_t disp = 0;
                memcpy(&disp, code + 3, 4);
                ULONGLONG nextInstrVA = img.imageBase + funcRVA + 7;
                ULONGLONG slotVA = nextInstrVA + (int64_t)disp;
                DWORD slotRVA = 0;
                if (VaToRva(img, slotVA, slotRVA))
                {
                    const uint8_t* slotBytes = RvaToPtr(img, slotRVA, 8);
                    if (slotBytes)
                    {
                        ULONGLONG strVA = 0;
                        memcpy(&strVA, slotBytes, 8);
                        DWORD strRVA = 0;
                        QString s;
                        if (VaToRva(img, strVA, strRVA) && ReadStaticCString(img, strRVA, s))
                        {
                            field.value = QString("(String) \"%1\"").arg(s);
                            matched = true;
                        }
                    }
                }
            }
            else if (code[0] == 0x8B && code[1] == 0x05 && code[6] == 0xC3)
            {
                // mov eax, [rip+disp32] ; retn -> raw int32 field, read
                // directly — no pointer indirection, unlike the string
                // case above
                int32_t disp = 0;
                memcpy(&disp, code + 2, 4);
                ULONGLONG nextInstrVA = img.imageBase + funcRVA + 6;
                ULONGLONG slotVA = nextInstrVA + (int64_t)disp;
                DWORD slotRVA = 0;
                if (VaToRva(img, slotVA, slotRVA))
                {
                    const uint8_t* slotBytes = RvaToPtr(img, slotRVA, 4);
                    if (slotBytes)
                    {
                        int32_t v;
                        memcpy(&v, slotBytes, 4);
                        field.value = QString("(Int32) %1").arg(v);
                        matched = true;
                    }
                }
            }
            else if (code[0] == 0x0F && code[1] == 0xB6 && code[2] == 0x05 && code[7] == 0xC3)
            {
                // movzx eax, byte ptr [rip+disp32] ; retn -> bool field
                // Not yet observed on Xbox One, included defensively to
                // match the corresponding PS4/PRX getter shape
                int32_t disp = 0;
                memcpy(&disp, code + 3, 4);
                ULONGLONG nextInstrVA = img.imageBase + funcRVA + 7;
                ULONGLONG slotVA = nextInstrVA + (int64_t)disp;
                DWORD slotRVA = 0;
                if (VaToRva(img, slotVA, slotRVA))
                {
                    const uint8_t* bp = RvaToPtr(img, slotRVA, 1);
                    if (bp)
                    {
                        field.value = QString("(Bool) %1").arg(*bp ? "true" : "false");
                        matched = true;
                    }
                }
            }
        }
        else
        {
            if (code[0] == 0xA1)
            {
                // mov eax, moffs32 — dereferences an absolute address
                // Could be a string pointer or a raw int; classify by
                // trying to read a real string at the stored value
                DWORD fieldVA = 0;
                memcpy(&fieldVA, code + 1, 4);
                DWORD fieldRVA = 0;
                if (VaToRva(img, fieldVA, fieldRVA))
                {
                    const uint8_t* storedValPtr = RvaToPtr(img, fieldRVA, 4);
                    if (storedValPtr)
                    {
                        DWORD storedVA = 0;
                        memcpy(&storedVA, storedValPtr, 4);
                        DWORD storedRVA = 0;
                        QString s;
                        if (VaToRva(img, storedVA, storedRVA) && ReadStaticCString(img, storedRVA, s))
                        {
                            field.value = QString("(String) \"%1\"").arg(s);
                            matched = true;
                        }
                        else
                        {
                            int32_t iv;
                            memcpy(&iv, storedValPtr, 4);
                            field.value = QString("(Int32) %1").arg(iv);
                            matched = true;
                        }
                    }
                }
            }
            else if (code[0] == 0x0F && code[1] == 0xB6 && code[2] == 0x05)
            {
                // movzx eax, byte ptr [addr32] — bool
                DWORD fieldVA = 0;
                memcpy(&fieldVA, code + 3, 4);
                DWORD fieldRVA = 0;
                if (VaToRva(img, fieldVA, fieldRVA))
                {
                    const uint8_t* bp = RvaToPtr(img, fieldRVA, 1);
                    if (bp)
                    {
                        field.value = QString("(Bool) %1").arg(*bp ? "true" : "false");
                        matched = true;
                    }
                }
            }
        }

        if (!matched && code[0] == 0x33 && code[1] == 0xC0) // xor eax,eax
        {
            field.value = "(Int32) 0";
            matched = true;
        }
        else if (!matched && code[0] == 0xB0) // mov al, imm8
        {
            field.value = QString("(Bool) %1").arg(code[1] != 0 ? "true" : "false");
            matched = true;
        }

        if (!matched) break; // unrecognized thunk shape = end of vtable
        out.append(field);
    }

    if (!out.isEmpty())
    {
        out.prepend({ "Factory Export", "getBuildInfo (static analysis)", RowKind::Normal, TableKind::BuildInfo });
        detectBuildMode(out); // detect BEFORE reorder
        assignFieldNames(out);
        reorderFields(out);
    }

    return out;
}

QList<BuildInfoWindow::MetaRow> BuildInfoWindow::extractDynamicFieldsPrx(const QString& prxPath) const
{
    QList<MetaRow> out;

    QFile f(prxPath);
    if (!f.open(QIODevice::ReadOnly))
        return out;
    QByteArray bytes = f.readAll();
    f.close();

    ElfImage img;
    if (!ParseElfImage(bytes, img))
        return out;

    uint64_t factoryVA = 0;
    if (!FindPrxFactoryVA(img, factoryVA))
        return out;

    // Step 1: decode the factory function
    const uint8_t* fnCode = ElfPtr(img, factoryVA, 8);
    if (!fnCode) return out;
    if (!(fnCode[0] == 0x48 && fnCode[1] == 0x8D && fnCode[2] == 0x05 && fnCode[7] == 0xC3))
        return out; // unrecognized factory shape

    int32_t disp;
    memcpy(&disp, fnCode + 3, 4);
    uint64_t objVA = factoryVA + 7 + (int64_t)disp;

    uint64_t vtableArrayVA = 0;
    if (!img.relocMap.contains(objVA)) return out;
    vtableArrayVA = img.relocMap.value(objVA);

    // Step 2: walk the vtable array
    const int kMaxSlots = 64;
    for (int slot = 0; slot < kMaxSlots; slot++)
    {
        uint64_t slotVA = vtableArrayVA + (uint64_t)slot * 8;
        if (!img.relocMap.contains(slotVA)) break;
        uint64_t funcVA = img.relocMap.value(slotVA);

        const uint8_t* code = ElfPtr(img, funcVA, 16);
        if (!code) break;

        MetaRow field;
        bool matched = false;

        if (code[0] == 0x48 && code[1] == 0x8D && code[2] == 0x05) // lea rax,[rip+disp32]
        {
            int32_t d;
            memcpy(&d, code + 3, 4);
            uint64_t targetVA = funcVA + 7 + (int64_t)d;

            if (code[7] == 0xC3)
            {
                // Direct — no second dereference. Every sample so far
                // uses this shape only for a baked string constant
                // (the "unknown" placeholder for empty fields)
                QString s;
                if (ReadElfCString(img, targetVA, s))
                {
                    field.value = QString("(String) \"%1\"").arg(s);
                    matched = true;
                }
            }
            else if (code[7] == 0x48 && code[8] == 0x8B && code[9] == 0x00 && code[10] == 0xC3)
            {
                // mov rax,[rax]; retn -> indirect string: targetVA is a
                // pointer-holding data slot, itself relocated
                if (img.relocMap.contains(targetVA))
                {
                    QString s;
                    if (ReadElfCString(img, img.relocMap.value(targetVA), s))
                    {
                        field.value = QString("(String) \"%1\"").arg(s);
                        matched = true;
                    }
                }
            }
            else if (code[7] == 0x8B && code[8] == 0x00 && code[9] == 0xC3)
            {
                // mov eax,[rax]; retn -> indirect int32: scalar values
                // aren't relocated (position-independent already), so
                // read the 4 bytes directly rather than via relocMap
                const uint8_t* ip = ElfPtr(img, targetVA, 4);
                if (ip)
                {
                    int32_t v;
                    memcpy(&v, ip, 4);
                    field.value = QString("(Int32) %1").arg(v);
                    matched = true;
                }
            }
            else if (code[7] == 0x0F && code[8] == 0xB6 && code[9] == 0x00 && code[10] == 0xC3)
            {
                // movzx eax, byte [rax]; retn -> indirect bool. Not yet
                // observed in any sample, included defensively to match
                // the shape of every other getter here — same as the
                // "unconfirmed pattern" caveats elsewhere in this reader
                const uint8_t* bp = ElfPtr(img, targetVA, 1);
                if (bp)
                {
                    field.value = QString("(Bool) %1").arg(*bp ? "true" : "false");
                    matched = true;
                }
            }
        }

        if (!matched && code[0] == 0xB8 && code[5] == 0xC3)
        {
            // mov eax, imm32 ; retn -> direct int32 constant
            int32_t v;
            memcpy(&v, code + 1, 4);
            field.value = QString("(Int32) %1").arg(v);
            matched = true;
        }
        else if (!matched && code[0] == 0xB0 && code[2] == 0xC3)
        {
            // mov al, imm8 ; retn -> direct bool constant
            field.value = QString("(Bool) %1").arg(code[1] ? "true" : "false");
            matched = true;
        }
        else if (!matched && code[0] == 0x31 && code[1] == 0xC0 && code[2] == 0xC3)
        {
            // xor eax, eax ; retn -> direct int32 constant, compiler's
            // zero-idiom for "mov eax, 0" (shorter encoding)
            field.value = QString("(Int32) 0");
            matched = true;
        }

        if (!matched) break; // truly unrecognized thunk shape = end of vtable
        out.append(field);
    }

    if (!out.isEmpty())
    {
        out.prepend({ "Factory Export", "getBuildInfo (PS4/PRX static analysis)", RowKind::Normal, TableKind::BuildInfo });
        detectBuildMode(out); // detect BEFORE reorder
        assignFieldNames(out); // best-effort — tuned against Windows field ordering, not PS4-specific
        reorderFields(out);
    }

    return out;
}

QList<BuildInfoWindow::MetaRow> BuildInfoWindow::extractDebugInfoPrx(const QString& prxPath) const
{
    QList<MetaRow> out;

    QFile f(prxPath);
    if (!f.open(QIODevice::ReadOnly))
        return out;
    QByteArray bytes = f.readAll();
    f.close();

    ElfImage img;
    if (!ParseElfImage(bytes, img))
        return out;

    out.append({ "Container Type",
        img.wasSelfWrapped ? "Signed ELF" : "Dumped ELF",
        RowKind::Normal, TableKind::Initial });

    // Re-walk the dynamic tags once more, just for DT_SCE_ORIGINAL_FILENAME
    // this time — the PRX equivalent of a PDB path, giving the original
    // build-machine path the .prx was compiled from
    const uint8_t* base = reinterpret_cast<const uint8_t*>(img.bytes.constData());
    auto* eh = reinterpret_cast<const Elf64_Ehdr*>(base);
    uint64_t dynOffset = 0, dynFilesz = 0;
    for (int i = 0; i < eh->e_phnum; i++)
    {
        uint64_t phAddr = eh->e_phoff + (uint64_t)i * eh->e_phentsize;
        if (phAddr + sizeof(Elf64_Phdr) > (uint64_t)img.bytes.size()) break;
        auto* ph = reinterpret_cast<const Elf64_Phdr*>(base + phAddr);
        if (ph->p_type == kPT_DYNAMIC) { dynOffset = ph->p_offset; dynFilesz = ph->p_filesz; break; }
    }

    int dynCount = (int)(dynFilesz / sizeof(Elf64_Dyn));
    for (int i = 0; i < dynCount; i++)
    {
        uint64_t entryOff = dynOffset + (uint64_t)i * sizeof(Elf64_Dyn);
        if (entryOff + sizeof(Elf64_Dyn) > (uint64_t)img.bytes.size()) break;
        auto* dyn = reinterpret_cast<const Elf64_Dyn*>(base + entryOff);
        if (dyn->d_tag == 0) break;

        if (dyn->d_tag == kDT_SCE_ORIGINAL_FILENAME && img.dynstrVA)
        {
            QString origPath;
            if (ReadRawCString(img.bytes, img.dynstrVA + dyn->d_val, origPath))
                out.append({ "Original Filename", origPath, RowKind::Normal, TableKind::PeDebug });
            break;
        }
    }

    return out;
}

QList<BuildInfoWindow::MetaRow> BuildInfoWindow::extractDynamicFieldsNrs(const QString& nrsPath) const
{
    QList<MetaRow> out;

    QFile f(nrsPath);
    if (!f.open(QIODevice::ReadOnly))
        return out;
    QByteArray bytes = f.readAll();
    f.close();

    ElfImage img;
    if (!ParseNrsElfImage(bytes, img))
        return out;

    static const char* kMangledPrefix = "_ZN2fb16BuildInformation";
    const int kMangledPrefixLen = 24; // strlen("_ZN2fb16BuildInformation")

    int symCount = (int)(img.dynsymSize / sizeof(Elf64_Sym));
    for (int i = 0; i < symCount; i++)
    {
        uint64_t symOff = img.dynsymVA + (uint64_t)i * sizeof(Elf64_Sym);
        if (symOff + sizeof(Elf64_Sym) > (uint64_t)img.bytes.size()) break;
        auto* sym = reinterpret_cast<const Elf64_Sym*>(
            reinterpret_cast<const uint8_t*>(img.bytes.constData()) + symOff);

        if ((sym->st_info & 0x0F) != kSTT_OBJECT) continue; // data members only
        if (sym->st_name == 0) continue;

        QString rawName;
        if (!ReadRawCString(img.bytes, img.dynstrVA + sym->st_name, rawName)) continue;
        if (!rawName.startsWith(kMangledPrefix)) continue;

        // Itanium mangling: prefix, then <len><identifier>, then a
        // trailing 'E' closing the nested-name — parse and validate that
        // shape rather than just chopping off a fixed suffix, so a
        // differently-sized identifier can't silently misparse
        QString rest = rawName.mid(kMangledPrefixLen);
        int digitEnd = 0;
        while (digitEnd < rest.size() && rest.at(digitEnd).isDigit()) digitEnd++;
        if (digitEnd == 0) continue;
        bool lenOk = false;
        int identLen = rest.left(digitEnd).toInt(&lenOk);
        if (!lenOk || identLen <= 0) continue;
        QString ident = rest.mid(digitEnd, identLen);
        if (ident.size() != identLen) continue;
        if (rest.mid(digitEnd + identLen) != QLatin1String("E")) continue;
        if (!ident.startsWith("g_")) continue;

        QString fieldName = HumanizeFieldIdentifier(ident.mid(2)); // "branchName" -> "Branch Name"

        MetaRow field;
        field.param = fieldName;
        field.rowKind = RowKind::Normal;
        field.tableKind = TableKind::BuildInfo;

        uint64_t fieldVA = sym->st_value;
        bool isAutoBuildField = (ident == "g_isAutoBuild");

        if (sym->st_size == 8)
        {
            uint64_t targetVA = 0;
            if (img.relocMap.contains(fieldVA))
                targetVA = img.relocMap.value(fieldVA);
            else
            {
                const uint8_t* p = ElfPtr(img, fieldVA, 8);
                if (!p) continue; // BSS/unbacked — nothing static to show
                memcpy(&targetVA, p, 8);
            }
            QString s;
            if (targetVA != 0 && ReadElfCString(img, targetVA, s))
                field.value = QString("(String) \"%1\"").arg(s);
            else
                continue; // not a real string — skip rather than guess
        }
        else if (sym->st_size == 4)
        {
            const uint8_t* p = ElfPtr(img, fieldVA, 4);
            if (!p) continue;
            int32_t v = 0;
            memcpy(&v, p, 4);
            field.value = QString("(Int32) %1").arg(v);
        }
        else if (sym->st_size == 1)
        {
            const uint8_t* p = ElfPtr(img, fieldVA, 1);
            if (!p) continue;
            bool boolVal = (*p != 0);
            field.value = QString("(Bool) %1").arg(boolVal ? "true" : "false");
            // g_isAutoBuild is the clearest available signal for the
            // "Build Type" row shown in Initial Information — unverified
            // against multiple samples, so treat this as a reasonable
            // first pass rather than a confirmed convention
            if (isAutoBuildField)
                m_buildMode = boolVal ? "Retail" : "Dev Build";
        }
        else
        {
            continue; // unrecognized field width
        }

        out.append(field);
    }

    if (!out.isEmpty())
        out.prepend({ "Factory Export", "fb::BuildInformation statics (static analysis)", RowKind::Normal, TableKind::BuildInfo });

    return out;
}

QList<BuildInfoWindow::MetaRow> BuildInfoWindow::extractDebugInfoNrs(const QString& nrsPath) const
{
    QList<MetaRow> rows;

    QFile f(nrsPath);
    if (!f.open(QIODevice::ReadOnly))
        return rows;
    QByteArray bytes = f.readAll();
    f.close();

    ElfImage img;
    if (!ParseNrsElfImage(bytes, img))
        return rows;

    rows.append({ "Container Type", "Switch ELF", RowKind::Normal, TableKind::Initial });

    if (img.commentSize > 0 && img.commentOff + img.commentSize <= (uint64_t)bytes.size())
    {
        // .comment holds two null-terminated strings back to back —
        // linker version, then compiler version, in that order
        QByteArray comment = bytes.mid((int)img.commentOff, (int)img.commentSize);
        QList<QByteArray> parts = comment.split('\0');
        QStringList nonEmpty;
        for (const auto& p : parts)
            if (!p.isEmpty()) nonEmpty.append(QString::fromLatin1(p));

        if (nonEmpty.size() >= 1)
            rows.append({ "Linker", nonEmpty[0], RowKind::Normal, TableKind::PeDebug });
        if (nonEmpty.size() >= 2)
            rows.append({ "Compiler", nonEmpty[1], RowKind::Normal, TableKind::PeDebug });
    }

    if (img.buildIdSize > 0)
    {
        // GNU build-id notes: a fixed 12-byte header (namesz, descsz,
        // type), then the (4-byte-aligned) name, then the build-id bytes
        // themselves — extracted manually rather than pulling in a full
        // ELF notes parser for one field
        uint64_t off = img.buildIdOff;
        if (off + 12 <= (uint64_t)bytes.size())
        {
            uint32_t nameSz = 0, descSz = 0, noteType = 0;
            memcpy(&nameSz, bytes.constData() + off, 4);
            memcpy(&descSz, bytes.constData() + off + 4, 4);
            memcpy(&noteType, bytes.constData() + off + 8, 4);
            uint64_t nameAlignedSz = (nameSz + 3) & ~3ULL;
            uint64_t descOff = off + 12 + nameAlignedSz;
            if (descSz > 0 && descOff + descSz <= (uint64_t)bytes.size())
            {
                QByteArray idBytes = bytes.mid((int)descOff, (int)descSz);
                rows.append({ "Build ID", QString::fromLatin1(idBytes.toHex()), RowKind::Normal, TableKind::PeDebug });
            }
        }
    }

    return rows;
}

void BuildInfoWindow::reorderFields(QList<MetaRow>& fields) const
{
    // Canonical output order, regardless of vtable slot order:
    static const char* kOrder[] = {
            "Branch Name",
            "Stream Name",
            "Licensee ID",
            "Studio Name",
            "Changelist",
            "Source Changelist",
            "Frostbite Changelist",
            "Frostbite Release",
            "Frostbite Release Name",
            "Is Auto Build",
            "Username",
            "User Group",
            "Build Time",
            "Build Date",
            "Build ISO Date",
            "Depot Port",
            "Host Name",
    };

    auto rankOf = [&](const QString& param) -> int {
        for (int i = 0; i < (int)(sizeof(kOrder) / sizeof(kOrder[0])); ++i)
            if (param == kOrder[i]) return i;
        return 9999; // unknowns go last
        };

    std::stable_sort(fields.begin(), fields.end(),
        [&](const MetaRow& a, const MetaRow& b) {
            return rankOf(a.param) < rankOf(b.param);
        });
}

#else // !Q_OS_WIN

bool BuildInfoWindow::loadDll(const QString&)
{
    showError("Build Info Reader is only supported on Windows.");
    return false;
}

#endif

// ============================================================
// Table population
// ============================================================
void BuildInfoWindow::populateTable(const QList<MetaRow>& rows)
{
    static QRegularExpression reType(R"(^\((\w+)\)\s*)");

    // Partition rows into the three tables
    QList<MetaRow> initialRows, peRows, buildRows;
    for (const MetaRow& r : rows)
    {
        switch (r.tableKind)
        {
        case TableKind::Initial:  initialRows.append(r);  break;
        case TableKind::PeDebug:  peRows.append(r);       break;
        case TableKind::BuildInfo: buildRows.append(r);   break;
        }
    }

    // Drives the Frost2/3/4 badge below the main icon off whatever
    // "Frostbite Release" field (if any) ended up in buildRows
    updateFrostbiteEngineBadge(buildRows);

    // Grows a table to fit exactly as many rows as it holds, so it never
    // needs its own scrollbar — the outer QScrollArea scrolls the whole
    // window instead
    auto sizeToContents = [](QTableWidget* t) {
        t->resizeColumnToContents(0);
        int h = t->horizontalHeader()->height() + 2 * t->frameWidth();
        for (int i = 0; i < t->rowCount(); ++i)
            h += t->rowHeight(i);
        t->setFixedHeight(h);
        };

    // Fill a 2-column table (no type column)
    auto fill2 = [&](QTableWidget* t, const QList<MetaRow>& list) {
        t->setRowCount(list.size());
        for (int i = 0; i < list.size(); ++i)
        {
            const MetaRow& r = list[i];
            auto* paramItem = new QTableWidgetItem(r.param);
            auto* valueItem = new QTableWidgetItem(r.value);
            t->setItem(i, 0, paramItem);
            t->setItem(i, 1, valueItem);
        }
        sizeToContents(t);
        };

    // Fill a 3-column table (with type column, section headers span all 3)
    auto fill3 = [&](QTableWidget* t, const QList<MetaRow>& list) {
        t->setRowCount(list.size());
        for (int i = 0; i < list.size(); ++i)
        {
            const MetaRow& r = list[i];

            QString displayType;
            QString displayValue = r.value;
            if (r.rowKind != RowKind::SectionHeader)
            {
                QRegularExpressionMatch m = reType.match(r.value);
                if (m.hasMatch())
                {
                    displayType = m.captured(1);
                    displayValue = r.value.mid(m.capturedLength());
                }
            }

            auto* paramItem = new QTableWidgetItem(r.param);
            auto* typeItem = new QTableWidgetItem(displayType);
            auto* valueItem = new QTableWidgetItem(displayValue);

            if (r.rowKind == RowKind::SectionHeader)
            {
                QFont f = paramItem->font(); f.setBold(true); paramItem->setFont(f);
                auto noSel = [](QTableWidgetItem* it) {
                    it->setFlags(it->flags() & ~Qt::ItemIsSelectable);
                    };
                noSel(paramItem); noSel(typeItem); noSel(valueItem);
                QColor bg = QColor(0x2a, 0x2a, 0x2a); // dark always for build table
                paramItem->setBackground(bg);
                typeItem->setBackground(bg);
                valueItem->setBackground(bg);
                t->setSpan(i, 0, 1, 3);
            }

            t->setItem(i, 0, paramItem);
            t->setItem(i, 1, typeItem);
            t->setItem(i, 2, valueItem);
        }
        sizeToContents(t);
        };

    fill2(m_tableInitial, initialRows);
    fill2(m_tablePe, peRows);
    fill3(m_tableBuild, buildRows);

    // .BuildSettings files
    bool hasPeRows = !peRows.isEmpty();
    m_tablePe->setVisible(hasPeRows);
    if (m_labelPe)
        m_labelPe->setVisible(hasPeRows);
}