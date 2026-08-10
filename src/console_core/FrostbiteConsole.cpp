// FrostbiteConsole.cpp
//
// DYNAMIC RESOLUTION STEPS
//
// Step 1: executeConsoleCommand
//   Anchor: "Unknown console command" string in .rdata
//   Method: find LEA Rxx,[anchor] -> walk back to CC-padded prologue
//
// Step 2: addOutputHandler
//   Anchor: "Same handler added multiple times." string in .rdata
//   Method: find LEA Rxx,[anchor] -> walk back to prologue
//
// Step 3: s_outputHandlers
//   Method: scan addOutputHandler body (256 bytes) for ANY RIP-relative data
//           reference (MOV r64, LEA r64, MOVUPS xmm, MOVDQU xmm) whose target
//           passes validateVectorHeader. The game uses MOVUPS to load the full
//           xmmword_s_outputHandlers (mpBegin+mpEnd) atomically — the old code
//           only checked for 64-bit MOV (0x8B) and missed this
//
// Steps 4+5: s_consoleMethods, s_instanceMethods
//   Method: scan executeConsoleCommand for ALL CALL rel32 targets that match
//           the Init_thread_header static-getter pattern. Collect unique
//           vector addresses in call-site order:
//             [0] = s_consoleMethods   (first getter called)
//             [1] = s_instanceMethods  (second getter called)
//
// Step 6: g_settingsManager, settingsManager_get, settingsManager_set
//   Anchor: "applyPendingVars" string (more reliable than "SettingsManager"
//           because "SettingsManager" is referenced via non-LEA instructions
//           in this build, making it invisible to findLEAToTarget)
//   Method: string -> LEA xref -> walk back to prologue (ctor or callee)
//           -> scan ctor for MOV cs:[RIP+rel],Rxx -> g_settingsManager address
//           -> scan entire image for MOV RCX,cs:[g_settingsManager] + CALL
//           -> top-2 callees by occurrence = get + set.
//   Bug fixed: old code had `ctorFn[0] == 0 &&` guard in the image scan,
//              making the entire scan a no-op (ctorFn[0] is never zero)
//
// BF2-SPECIFIC SUPPLEMENT (tryResolveDynamicBF2)
//
// Step A: s_consoleMethods  (consoleRegistry::s_consoleMethods() in source)
//   The vector is a fixed_vector<const ConsoleMethod*, 8048> whose getter is
//   called twice near the top of executeConsoleCommand
//
//   Sub-path 1 — direct getter LEA (BF2015, NFS Heat):
//     Follow CALL rel32 targets from execCmd one level deep (through JMP
//     thunks). For each callee, scan up to 64 bytes for LEA Rxx,[RIP+rel]
//     If the LEA target itself looks like a valid non-empty vector object
//     (mpBegin non-null, mpEnd > mpBegin, mpCapacity ≥ mpEnd), use it
//     CORRECTION: if [tgt-0x28] is readable and [tgt-0x28+0x00] == tgt,
//     then tgt is the inline buffer of a fixed_vector and (tgt-0x28) is the
//     true object base — store that instead (fixes BF2015 beta)
//
//   Sub-path 2 — mpCapacity byte-search (BF2, MEA, and others):
//     Collect all LEA Rxx,[RIP+rel] targets directly in execCmd body. For
//     each LEA target T, VirtualQuery-walk all non-executable committed pages
//     within the module looking for a qword-aligned slot at +0x18 == T (the
//     mpEndOfStorage/mpCapacity field of the fixed_vector object). Validate
//     the candidate with structural checks AND content validation: dereference
//     slot[0] as a ConsoleMethod*, confirm its +0x00 (pfn) is executable and
//     its +0x08 (name) is a readable string starting with [A-Za-z_]. This
//     rejects false-positive vectors (e.g. MEA's asset-path table) whose
//     first elements are raw const char* strings, not ConsoleMethod pointers
//
//   After Step A, init() re-validates the result using the same ConsoleMethod
//   content check and clears it if invalid, allowing tryResolveDynamicFixedVector
//   (FV scan) to find the correct getter and vector for titles like MEA where
//   Step A's byte-search finds a structural false positive
//
// Step B: Console::writeConsole / s_outputHandlers
//   (Console::writeConsole in source iterates s_outputHandlers vector.)
//   Scan every executable committed region for the writeConsole prologue:
//     48 89 5C 24 08   mov [rsp+08], rbx
//     48 89 6C 24 10   mov [rsp+10], rbp
//     48 89 74 24 18   mov [rsp+18], rsi
//     48 89 7C 24 20   mov [rsp+20], rdi
//     41 56            push r14
//     48 83 EC 20      sub rsp, 20h
//   For each prologue match, search forward up to 128 bytes collecting
//   MOV RBX,[RIP+rel] and MOV RDI,[RIP+rel] targets. If the two targets are
//   exactly 8 bytes apart (adjacent mpBegin/mpEnd of s_outputHandlers) AND
//   ADD RBX,10 (the iteration stride) exists within 512 bytes, record this
//   function as writeConsoleFunc and the lower target as s_outputHandlers
//
// Step C: writeConsoleFunc via s_outputHandlers slot (fallback)
//   If Step B found s_outputHandlers but not writeConsoleFunc, read the
//   function pointer at slot[0]+8 of the vector as a last resort
//
// tryResolveDynamicFixedVector (FV scan):
//   For titles whose getter uses the fixed_vector multi-init pattern
//   (BF2015, GW2, NFS Rivals, MEA) rather than Init_thread_header:
//   Scan execCmd for CALL rel32 targets. For each callee fingerprint:
//     TEST AL,1  (A8 01)       — guard byte check
//     JNE short  (75 xx)       — fast-path branch
//     LEA RAX,[RIP+rel32]      — 48 8D 05 xx xx xx xx
//     ADD RSP,28  (48 83 C4 28)
//     RET         (C3)
//   Decode the LEA target as the vector object, validate as non-empty with
//   a readable mpBegin. First matching callee wins
//
// Functions:
// 
// executeConsoleCommand
//   Anchor: "Unknown console command" string in .rdata
//   Also anchors: "No more arguments to parse", "Win32 result:", "Expected arguments: "
//
// s_consoleMethods
//   fixed_vector<const ConsoleMethod*, 8048> — getter called from executeConsoleCommand
//   Also reachable via: ConsoleRegistry::getConsoleMethods(), registerConsoleMethods(),
//   unregisterConsoleMethods()
//
// s_instanceMethods
//   fixed_vector<InstanceMethod, 128> — iterated after s_consoleMethods in executeConsoleCommand
//
// s_outputHandlers
//   Anchor: "Same handler added multiple times." -> Console::addOutputHandler -> s_outputHandlers
//
// addOutputHandler
//   Anchor: "Same handler added multiple times." string
//
// removeOutputHandler
//   No unique string anchor; found by proximity to addOutputHandler or
//   by scanning addOutputHandler's callers for a sibling function
//
// writeConsoleFunc
//   No unique string anchor. Found via Step B prologue scan (iterates s_outputHandlers)
//   The two-argument overload writeConsole(eastl::string&, eastl::string&) is a thunk
//   into this one — not useful as a hook target
//
// g_settingsManager
//   Anchor: "applyPendingVars" string appears in ConsoleObjectUtil::applyPendingVars()
//   which references g_settingsManager directly. Also referenced in varFunc/varGroupFunc
//   via g_settingsManager->get() / ->set()
//
// settingsManager_get
//   top callee after MOV RCX,[g_settingsManager] by occurrence count
//
// settingsManager_set
//   Second most frequent callee after MOV RCX,[g_settingsManager]

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include "FrostbiteConsole.h"
#include <cstdio>
#include <cctype>
#include <algorithm>

// Internal logging
static void (*g_fcLogCallback)(const char* line) = nullptr;

static void FC_Log(const char* fmt, ...)
{
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    // Timestamp for OutputDebugString (includes [FC] tag for debugger clarity)
    OutputDebugStringA("[FC] ");
    OutputDebugStringA(buf);
    OutputDebugStringA("\n");

    if (g_fcLogCallback)
    {
        // Pass bare message — pipeLogLine in DLLmain adds the timestamp and
        // strips any bracket prefix, so we don't need to do either here
        g_fcLogCallback(buf);
    }
}

namespace FrostbiteConsole {

    void setLogCallback(void (*cb)(const char* line)) { g_fcLogCallback = cb; }
    std::string getLastLog() { return {}; }
    void        clearLastLog() {}

    // Singleton
    ConsoleBridge& ConsoleBridge::instance()
    {
        static ConsoleBridge s;
        return s;
    }

    // Safe memory helpers
    /*static*/ bool ConsoleBridge::safeRead64(void* addr, uint64_t* out)
    {
        __try {
            *out = *reinterpret_cast<const uint64_t*>(addr);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    /*static*/ bool ConsoleBridge::safeRead32(void* addr, uint32_t* out)
    {
        __try {
            *out = *reinterpret_cast<const uint32_t*>(addr);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    bool safeRead64(void* addr, uint64_t* out) { return ConsoleBridge::safeRead64(addr, out); }
    bool safeRead32(void* addr, uint32_t* out) { return ConsoleBridge::safeRead32(addr, out); }

    // Memory region helpers
    static bool isExecutable(void* addr)
    {
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery(addr, &mbi, sizeof(mbi))) return false;
        if (mbi.State != MEM_COMMIT) return false;
        return !!(mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ |
            PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY));
    }

    static bool isReadWrite(void* addr)
    {
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery(addr, &mbi, sizeof(mbi))) return false;
        if (mbi.State != MEM_COMMIT) return false;
        return !!(mbi.Protect & (PAGE_READWRITE | PAGE_WRITECOPY |
            PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY));
    }

    static bool isReadable(void* addr)
    {
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery(addr, &mbi, sizeof(mbi))) return false;
        if (mbi.State != MEM_COMMIT) return false;
        return !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD));
    }

    // looksLikeString
    /*static*/ bool ConsoleBridge::looksLikeString(uint64_t ptr, int maxLen)
    {
        if (ptr < 0x10000ULL) return false;
        if (!isReadable(reinterpret_cast<void*>(static_cast<uintptr_t>(ptr)))) return false;
        const char* p = reinterpret_cast<const char*>(static_cast<uintptr_t>(ptr));
        bool hasAlpha = false;
        for (int i = 0; i < maxLen; ++i) {
            unsigned char c = static_cast<unsigned char>(p[i]);
            if (c == 0) break;
            if (c < 0x20 || c > 0x7E) return false;
            if (isalpha(c)) hasAlpha = true;
        }
        return hasAlpha;
    }

    // decodeRIPRel — decode a RIP-relative instruction operand
    // operandOffset = byte offset from instruction start to the rel32 field
    // instrLen      = total instruction length (operandOffset + 4)
    /*static*/ uint8_t* ConsoleBridge::decodeRIPRel(uint8_t* insn, int operandOffset)
    {
        int32_t rel = 0;
        memcpy(&rel, insn + operandOffset, 4);
        return insn + operandOffset + 4 + rel;
    }

    // scanForString — find a literal string anywhere in the mapped image
    // Returns pointer to first byte of the string, or nullptr
    static uint8_t* scanForString(uint8_t* base, size_t size, const char* needle)
    {
        size_t nlen = strlen(needle);
        if (nlen == 0 || nlen > size) return nullptr;
        for (size_t i = 0; i + nlen <= size; ++i) {
            if (memcmp(base + i, needle, nlen) == 0)
                return base + i;
        }
        return nullptr;
    }

    // findLEAToTarget — find a LEA Rxx,[RIP+rel] or MOV Rxx,[RIP+rel]
    // instruction anywhere in [searchBase, searchBase+searchSize) whose
    // resolved target equals targetAddr
    // Returns pointer to the start of the instruction, or nullptr
    static uint8_t* findLEAToTarget(uint8_t* searchBase, size_t searchSize,
        uint8_t* targetAddr)
    {
        if (searchSize < 7) return nullptr;
        for (size_t i = 0; i + 7 <= searchSize; ++i) {
            uint8_t b0 = searchBase[i];
            uint8_t b1 = searchBase[i + 1];
            uint8_t b2 = searchBase[i + 2];

            // REX prefix: 48-4F
            if (b0 < 0x48 || b0 > 0x4F) continue;

            // LEA (8D) or MOV (8B)
            if (b1 != 0x8D && b1 != 0x8B) continue;

            // ModRM: mod=00, rm=101 (RIP-relative) — upper 2 bits = 00, lower 3 = 101
            // Also accept any register destination: (b2 & 0xC7) == 0x05
            if ((b2 & 0xC7) != 0x05) continue;

            int32_t rel = 0;
            memcpy(&rel, searchBase + i + 3, 4);
            uint8_t* resolved = searchBase + i + 7 + rel;

            if (resolved == targetAddr)
                return searchBase + i;
        }
        return nullptr;
    }

    // walkBackToPrologue — given an instruction inside a function, walk
    // backward to find the function prologue (CC-padded or 16-byte aligned)
    // Returns the prologue address, or nullptr
    // maxBack: maximum bytes to search backward (default 128KB)
    static uint8_t* walkBackToPrologue(uint8_t* insideFunc, uint8_t* moduleBase,
        int maxBack = 128 * 1024)
    {
        for (int back = 1; back < maxBack; ++back) {
            uint8_t* cand = insideFunc - back;
            if (cand < moduleBase) break;

            // Must be preceded by CC (int3 alignment padding) or be 16-byte aligned
            bool preceded_by_cc = (cand > moduleBase &&
                (cand[-1] == 0xCC || cand[-1] == 0x90));
            if (!preceded_by_cc) continue;

            // Check for common x64 MSVC prologues
            uint8_t b0 = cand[0], b1 = cand[1], b2 = cand[2];
            bool ok = false;

            // MOV [RSP+N], Rxx  (48/4C 89 xx 24 xx)
            if ((b0 == 0x48 || b0 == 0x4C) && b1 == 0x89 &&
                (b2 == 0x4C || b2 == 0x54 || b2 == 0x5C ||
                    b2 == 0x44 || b2 == 0x74 || b2 == 0x7C)) ok = true;
            // PUSH Rxx (with or without REX prefix)
            if (b0 == 0x55 || b0 == 0x53 || b0 == 0x56 || b0 == 0x57) ok = true;
            // REX + PUSH Rxx (e.g. 40 55 = push rbp, 41 54 = push r12, etc.)
            if ((b0 & 0xF0) == 0x40 && (b1 == 0x55 || b1 == 0x53 || b1 == 0x56 || b1 == 0x57)) ok = true;
            // REX + PUSH extended regs (41 54=r12, 41 55=r13, 41 56=r14, 41 57=r15)
            if (b0 == 0x41 && (b1 >= 0x54 && b1 <= 0x57)) ok = true;
            // SUB RSP, imm
            if (b0 == 0x48 && b1 == 0x83 && b2 == 0xEC) ok = true;
            if (b0 == 0x48 && b1 == 0x81 && b2 == 0xEC) ok = true;
            // MOV RAX, RSP  (48 8B C4) — used by large-frame MSVC ABI
            // NOTE: 48 8B 05 (MOV RAX,[RIP+rel]) removed — too common mid-function,
            // causes false prologue hits. Security-cookie functions always have a
            // PUSH or MOV [RSP] instruction that matches an earlier pattern
            if (b0 == 0x48 && b1 == 0x8B && b2 == 0xC4) ok = true;

            if (!ok) continue;
            if (!isExecutable(cand)) continue;

            return cand;
        }
        return nullptr;
    }

    // findFirstRIPRelDataRef — scan [fn, fn+scanLen) for the first instruction
    // that RIP-relatively addresses a writable data location, and return that
    // address.  Handles all common x64 load patterns:
    //
    //   REX  8B  ModRM  rel32        MOV r64, [RIP+rel]       7 bytes
    //   REX  8D  ModRM  rel32        LEA r64, [RIP+rel]       7 bytes
    //        0F  10     ModRM  rel32 MOVUPS xmm, [RIP+rel]    7 bytes (no REX)
    //   REX  0F  10     ModRM  rel32 MOVUPS xmm, [RIP+rel]    8 bytes (with REX)
    //   66       0F  6F ModRM  rel32 MOVDQU xmm, [RIP+rel]    8 bytes (no REX)
    //   66  REX  0F  6F ModRM  rel32 MOVDQU xmm, [RIP+rel]    9 bytes (with REX)
    //
    // The SSE forms are generated by MSVC for eastl::vector header loads
    // (loading mpBegin+mpEnd as an xmmword in one instruction)
    //
    // Returns the resolved target address, or nullptr
    static uint8_t* findFirstRIPRelDataRef(uint8_t* fn, size_t scanLen)
    {
        for (size_t i = 0; i + 7 <= scanLen; ++i) {
            uint8_t b0 = fn[i];

            // Case 1: REX 8B/8D ModRM rel32  (standard MOV/LEA, 7 bytes)
            if (b0 >= 0x48 && b0 <= 0x4F) {
                if (i + 7 > scanLen) continue;
                uint8_t b1 = fn[i + 1], b2 = fn[i + 2];
                if ((b1 == 0x8B || b1 == 0x8D) && (b2 & 0xC7) == 0x05) {
                    int32_t rel = 0;
                    memcpy(&rel, fn + i + 3, 4);
                    uint8_t* target = fn + i + 7 + rel;
                    if (isReadWrite(target)) return target;
                }
                // Case 2: REX 0F 10 ModRM rel32  (MOVUPS with REX, 8 bytes)
                if (b1 == 0x0F && i + 8 <= scanLen) {
                    uint8_t b2r = fn[i + 2], b3 = fn[i + 3];
                    if (b2r == 0x10 && (b3 & 0xC7) == 0x05) {
                        int32_t rel = 0;
                        memcpy(&rel, fn + i + 4, 4);
                        uint8_t* target = fn + i + 8 + rel;
                        if (isReadWrite(target)) return target;
                    }
                }
                continue;
            }

            // Case 3: 0F 10 ModRM rel32  (MOVUPS without REX, 7 bytes)
            if (b0 == 0x0F && i + 7 <= scanLen) {
                uint8_t b1 = fn[i + 1], b2 = fn[i + 2];
                if (b1 == 0x10 && (b2 & 0xC7) == 0x05) {
                    int32_t rel = 0;
                    memcpy(&rel, fn + i + 3, 4);
                    uint8_t* target = fn + i + 7 + rel;
                    if (isReadWrite(target)) return target;
                }
                continue;
            }

            // Case 4: 66 [REX] 0F 6F ModRM rel32  (MOVDQU, 8 or 9 bytes)
            if (b0 == 0x66 && i + 8 <= scanLen) {
                uint8_t b1 = fn[i + 1];
                // With REX prefix (9 bytes total)
                if (b1 >= 0x48 && b1 <= 0x4F && i + 9 <= scanLen) {
                    uint8_t b2 = fn[i + 2], b3 = fn[i + 3];
                    if (b2 == 0x0F && b3 == 0x6F) {
                        uint8_t modrm = fn[i + 4];
                        if ((modrm & 0xC7) == 0x05) {
                            int32_t rel = 0;
                            memcpy(&rel, fn + i + 5, 4);
                            uint8_t* target = fn + i + 9 + rel;
                            if (isReadWrite(target)) return target;
                        }
                    }
                }
                // Without REX prefix (8 bytes total)
                if (b1 == 0x0F && i + 8 <= scanLen) {
                    uint8_t b2 = fn[i + 2], b3 = fn[i + 3];
                    if (b2 == 0x6F && (b3 & 0xC7) == 0x05) {
                        int32_t rel = 0;
                        memcpy(&rel, fn + i + 4, 4);
                        uint8_t* target = fn + i + 8 + rel;
                        if (isReadWrite(target)) return target;
                    }
                }
            }
        }
        return nullptr;
    }

    // validateVectorHeader — check that addr looks like an eastl::vector header
    // (mpBegin <= mpEnd <= mpCapacity, sane size)
    static bool validateVectorHeader(void* addr, size_t maxBytes = 512 * 1024)
    {
        if (!addr) return false;
        if (!isReadWrite(addr)) return false;
        uint64_t b = 0, e = 0, c = 0;
        if (!FrostbiteConsole::safeRead64(addr, &b)) return false;
        if (!FrostbiteConsole::safeRead64(reinterpret_cast<uint8_t*>(addr) + 8, &e)) return false;
        if (!FrostbiteConsole::safeRead64(reinterpret_cast<uint8_t*>(addr) + 16, &c)) return false;
        // All-zero is valid (not yet populated)
        if (b == 0 && e == 0 && c == 0) return true;
        if (b > e || e > c) return false;
        if ((c - b) > maxBytes) return false;
        return true;
    }

    // findCALLTarget — find a CALL rel32 (E8 xx xx xx xx) near offset `hint`
    // in [fn, fn+scanLen) and return its target
    // If hint >= 0, start searching there; otherwise search from 0
    static uint8_t* findCALLTarget(uint8_t* fn, size_t scanLen, int hint = 0)
    {
        size_t start = (hint >= 0 && (size_t)hint < scanLen) ? (size_t)hint : 0;
        for (size_t i = start; i + 5 <= scanLen; ++i) {
            if (fn[i] != 0xE8) continue;
            int32_t rel = 0;
            memcpy(&rel, fn + i + 1, 4);
            uint8_t* target = fn + i + 5 + rel;
            if (isExecutable(target))
                return target;
        }
        return nullptr;
    }

    // isInitThreadHeaderPattern — check if a small function contains the
    // Init_thread_header / Init_thread_footer pattern used for local statics
    // These getters are tiny (< 80 bytes) and call Init_thread_header
    static bool isInitThreadHeaderPattern(uint8_t* fn, size_t maxLen = 80)
    {
        // Look for: CMP dword ptr [RIP+rel], val (83 3D or 39)
        // followed by LEA/MOV RAX,[RIP+rel]
        // followed by RET (C3)
        bool hasRet = false;
        bool hasCmp = false;
        for (size_t i = 0; i < maxLen; ++i) {
            if (fn[i] == 0xC3) { hasRet = true; break; }
            // CMP [RIP+rel32], imm8
            if (fn[i] == 0x83 && fn[i + 1] == 0x3D) hasCmp = true;
            // CMP [RIP+rel32], r32
            if (fn[i] == 0x39 || fn[i] == 0x3B) hasCmp = true;
        }
        return hasRet && hasCmp;
    }

    // extractRIPRelTarget — given a function, scan for any LEA or MOV
    // [RAX/RCX/RDX] = cs:[RIP+rel] and return the resolved data address
    // Used to pull the vector address out of a static getter
    static uint8_t* extractRIPRelTarget(uint8_t* fn, size_t maxLen,
        bool requireWritable = true)
    {
        for (size_t i = 0; i + 7 <= maxLen; ++i) {
            uint8_t b0 = fn[i], b1 = fn[i + 1], b2 = fn[i + 2];
            if (b0 < 0x48 || b0 > 0x4F) continue;
            if (b1 != 0x8D && b1 != 0x8B) continue;
            if ((b2 & 0xC7) != 0x05) continue;
            int32_t rel = 0;
            memcpy(&rel, fn + i + 3, 4);
            uint8_t* target = fn + i + 7 + rel;
            if (requireWritable && !isReadWrite(target)) continue;
            if (!requireWritable && !isReadable(target)) continue;
            return target;
        }
        return nullptr;
    }

    // tryResolveDynamic
    // Pure dynamic resolution — no hardcoded addresses
    // Returns true when all required fields are resolved
    bool ConsoleBridge::tryResolveDynamic(uint8_t* modBase, size_t modSize)
    {
        FC_Log("tryResolveDynamic: base=%p size=%zu", modBase, modSize);

        // STEP 1: Find executeConsoleCommand
        FC_Log("Step 1: finding executeConsoleCommand");

        static const char kUnknownCmd[] = "Unknown console command";
        uint8_t* unknownCmdStr = scanForString(modBase, modSize, kUnknownCmd);
        if (!unknownCmdStr) {
            FC_Log("Step 1 FAIL: anchor string not found");
            return false;
        }
        FC_Log("Step 1: anchor string at %p", unknownCmdStr);

        uint8_t* xrefInsn = findLEAToTarget(modBase, modSize, unknownCmdStr);
        if (!xrefInsn) {
            FC_Log("Step 1 FAIL: no LEA xref to anchor string");
            return false;
        }
        FC_Log("Step 1: LEA xref at %p", xrefInsn);

        // Walk back to prologue, but then verify no CLOSER prologue exists
        // between our candidate and the xref (handles the case where walkBack
        // overshoots into a preceding function)
        uint8_t* execCmd = walkBackToPrologue(xrefInsn, modBase);
        if (!execCmd) {
            FC_Log("Step 1 FAIL: could not find prologue");
            return false;
        }

        // Scan forward from execCmd+1 to xrefInsn looking for any CC/NOP-padded
        // prologue that is CLOSER to the xref — if found, that is the real
        // function entry and the initial walk overshot
        // We track the highest-address valid candidate (best) rather than
        // updating execCmd on every hit, so intermediate unrelated functions
        // between the overshot start and the real entry don't win
        // Two padding styles are accepted:
        //   1. Single-byte: CC (int3) or 90 (nop) — the common MSVC case
        //   2. Multi-byte NOP ending in 0x00: 66 2E 0F 1F 84 00 ... 00
        //      These appear when MSVC aligns a function to 16 bytes using the
        //      long NOP form.  We detect them by checking that the 0x00
        //      predecessor is reachable within 12 bytes of a 0F 1F or 66 2E
        //      signature, ensuring we don't misfire on 0x00 bytes inside
        //      ordinary instruction encodings
        {
            uint8_t* best = nullptr;

            auto isPrologueByte = [](uint8_t b0, uint8_t b1, uint8_t b2) -> bool {
                if (b0 == 0x55 || b0 == 0x53 || b0 == 0x56 || b0 == 0x57) return true;
                if ((b0 & 0xF0) == 0x40 && (b1 == 0x55 || b1 == 0x53 ||
                    b1 == 0x56 || b1 == 0x57)) return true;
                if (b0 == 0x41 && b1 >= 0x54 && b1 <= 0x57) return true;
                if ((b0 == 0x48 || b0 == 0x4C) && b1 == 0x89 &&
                    (b2 == 0x4C || b2 == 0x54 || b2 == 0x5C ||
                        b2 == 0x44 || b2 == 0x74 || b2 == 0x7C)) return true;
                if (b0 == 0x48 && b1 == 0x83 && b2 == 0xEC) return true;
                if (b0 == 0x48 && b1 == 0x81 && b2 == 0xEC) return true;
                // NOTE: 48 8B 05 (MOV RAX,[RIP+rel]) intentionally excluded —
                // it appears mid-function (e.g. security cookie loads after
                // stack frame setup) and causes false prologue hits in some games
                // 48 8B C4 (MOV RAX,RSP) is the legitimate frame-pointer ABI
                // used by large-frame MSVC functions and stays
                if (b0 == 0x48 && b1 == 0x8B && b2 == 0xC4) return true;
                return false;
                };

            uint8_t* closer = execCmd + 1;
            while (closer < xrefInsn) {
                uint8_t prev = closer[-1];
                bool validPad = false;

                // Style 1: single-byte padding
                if (prev == 0xCC || prev == 0x90)
                    validPad = true;

                // Style 2: multi-byte NOP (66 2E 0F 1F 84 00 ... 00)
                // Only fire when prev==0x00 AND a NOP signature exists nearby
                if (!validPad && prev == 0x00 && closer > modBase + 12) {
                    for (int back = 2; back <= 12; ++back) {
                        uint8_t pb = closer[-back];
                        uint8_t pb1 = closer[-back + 1];
                        if ((pb == 0x0F && pb1 == 0x1F) ||
                            (pb == 0x66 && pb1 == 0x2E)) {
                            validPad = true;
                            break;
                        }
                    }
                }

                if (validPad) {
                    uint8_t b0 = closer[0], b1 = closer[1], b2 = closer[2];
                    if (isPrologueByte(b0, b1, b2) && isExecutable(closer))
                        best = closer;
                }
                ++closer;
            }

            if (best) {
                FC_Log("Step 1: closer prologue found at %p (overshot from %p)",
                    best, execCmd);
                execCmd = best;
            }
        }

        FC_Log("Step 1: executeConsoleCommand at %p", execCmd);
        m_execCmd = reinterpret_cast<ExecuteConsoleCmdFn>(execCmd);
        uint8_t* execCmdPtr = execCmd;

        // STEP 2: Find addOutputHandler
        // Anchor: "Same handler added multiple times."
        // Method: string -> LEA xref -> walk back to prologue
        FC_Log("Step 2: finding addOutputHandler");

        static const char kSameHandler[] = "Same handler added multiple times.";
        uint8_t* sameHandlerStr = scanForString(modBase, modSize, kSameHandler);
        if (!sameHandlerStr) {
            FC_Log("Step 2 FAIL: anchor string not found");
            goto step3;
        }
        FC_Log("Step 2: anchor string at %p", sameHandlerStr);

        {
            uint8_t* addHandlerXref = findLEAToTarget(modBase, modSize, sameHandlerStr);
            if (!addHandlerXref) {
                FC_Log("Step 2 FAIL: no LEA xref to anchor string");
                goto step3;
            }
            FC_Log("Step 2: LEA xref at %p", addHandlerXref);

            uint8_t* addHandlerFn = walkBackToPrologue(addHandlerXref, modBase);
            if (!addHandlerFn) {
                FC_Log("Step 2 FAIL: could not find prologue");
                goto step3;
            }
            uintptr_t addHandlerAddr = reinterpret_cast<uintptr_t>(addHandlerFn);
            addHandlerAddr &= ~0xFULL;
            addHandlerFn = reinterpret_cast<uint8_t*>(addHandlerAddr);
            FC_Log("Step 2: addOutputHandler at %p", addHandlerFn);
            m_addOutputHandler = reinterpret_cast<AddOutputHandlerFn>(addHandlerFn);

            // STEP 3: Find s_outputHandlers from addOutputHandler body
            // Scan 256 bytes using findFirstRIPRelDataRef
            // which handles MOV r64, LEA r64, MOVUPS xmm, and MOVDQU xmm
            // Accept the first target that passes validateVectorHeader
            FC_Log("Step 3: finding s_outputHandlers from addOutputHandler body");
            {
                // Scan addOutputHandler body collecting every distinct writable
                // RIP-relative data target. We cannot use validateVectorHeader
                // because at inject-time the vector may be unpopulated (all zeros
                // passes) but the game loads mpBegin+mpEnd as one MOVUPS xmmword
                // so the *first* writable data ref in this function is always
                // s_outputHandlers. Accept it directly; skip any address that
                // already matched a consoleMethods or instanceMethods vector
                const size_t kStep3Len = 512;
                uint8_t* step3Hits[16];
                int       step3HitCount = 0;

                for (size_t si = 0; si + 7 <= kStep3Len && step3HitCount < 16; ) {
                    uint8_t b0 = addHandlerFn[si];
                    uint8_t* target = nullptr;
                    size_t advance = 1;

                    // REX 8B/8D ModRM rel32  (7 bytes)
                    if (b0 >= 0x48 && b0 <= 0x4F && si + 7 <= kStep3Len) {
                        uint8_t b1 = addHandlerFn[si + 1], b2 = addHandlerFn[si + 2];
                        if ((b1 == 0x8B || b1 == 0x8D) && (b2 & 0xC7) == 0x05) {
                            int32_t rel = 0; memcpy(&rel, addHandlerFn + si + 3, 4);
                            target = addHandlerFn + si + 7 + rel; advance = 7;
                        }
                        // REX 0F 10 ModRM rel32  (8 bytes)
                        else if (b1 == 0x0F && si + 8 <= kStep3Len) {
                            uint8_t b2r = addHandlerFn[si + 2], b3 = addHandlerFn[si + 3];
                            if (b2r == 0x10 && (b3 & 0xC7) == 0x05) {
                                int32_t rel = 0; memcpy(&rel, addHandlerFn + si + 4, 4);
                                target = addHandlerFn + si + 8 + rel; advance = 8;
                            }
                        }
                    }
                    // 0F 10 ModRM rel32  (7 bytes, no REX)
                    else if (b0 == 0x0F && si + 7 <= kStep3Len) {
                        uint8_t b1 = addHandlerFn[si + 1], b2 = addHandlerFn[si + 2];
                        if (b1 == 0x10 && (b2 & 0xC7) == 0x05) {
                            int32_t rel = 0; memcpy(&rel, addHandlerFn + si + 3, 4);
                            target = addHandlerFn + si + 7 + rel; advance = 7;
                        }
                    }
                    // 66 [REX] 0F 6F ModRM rel32  (MOVDQU 8/9 bytes)
                    else if (b0 == 0x66 && si + 8 <= kStep3Len) {
                        uint8_t b1 = addHandlerFn[si + 1];
                        if (b1 >= 0x48 && b1 <= 0x4F && si + 9 <= kStep3Len) {
                            if (addHandlerFn[si + 2] == 0x0F && addHandlerFn[si + 3] == 0x6F &&
                                (addHandlerFn[si + 4] & 0xC7) == 0x05) {
                                int32_t rel = 0; memcpy(&rel, addHandlerFn + si + 5, 4);
                                target = addHandlerFn + si + 9 + rel; advance = 9;
                            }
                        }
                        else if (b1 == 0x0F && addHandlerFn[si + 2] == 0x6F &&
                            (addHandlerFn[si + 3] & 0xC7) == 0x05) {
                            int32_t rel = 0; memcpy(&rel, addHandlerFn + si + 4, 4);
                            target = addHandlerFn + si + 8 + rel; advance = 8;
                        }
                    }

                    if (target && isReadWrite(target)) {
                        bool dup = false;
                        for (int k = 0; k < step3HitCount; ++k)
                            if (step3Hits[k] == target) { dup = true; break; }
                        if (!dup) step3Hits[step3HitCount++] = target;
                    }
                    si += advance;
                }

                FC_Log("Step 3: found %d writable RIP-rel targets in addOutputHandler",
                    step3HitCount);
                for (int k = 0; k < step3HitCount; ++k)
                    FC_Log("  target[%d]=%p vhdr=%d", k, step3Hits[k],
                        (int)validateVectorHeader(step3Hits[k]));

                // Pick the first target that passes validateVectorHeader
                // (The first writable ref may be a CriticalSection or other
                // data object that isn't a vector — we need the header check.)
                for (int k = 0; k < step3HitCount && !m_outputHandlersVecAddr; ++k) {
                    if (!validateVectorHeader(step3Hits[k])) continue;
                    m_outputHandlersVecAddr = step3Hits[k];
                    FC_Log("Step 3: s_outputHandlers at %p (target[%d])", step3Hits[k], k);
                }
                if (!m_outputHandlersVecAddr)
                    FC_Log("Step 3 FAIL: no target passed validateVectorHeader");
            }
        }

    step3:
        // STEPS 4 + 5: Find s_consoleMethods and s_instanceMethods
        // Both live in Init_thread_header-guarded static getters called from
        // executeConsoleCommand. The bug in the old code: it stopped at the
        // FIRST getter found, which happened to be s_instanceMethods (at a
        // lower call-site offset), and never reached s_consoleMethods
        //
        // Fix: collect ALL valid getter targets in one forward pass, then
        // assign in order of appearance:
        //   [0] -> s_consoleMethods  (first unique, non-outputHandlers target)
        //   [1] -> s_instanceMethods (second unique target)
        //
        // Scan limit: 16 KB of executeConsoleCommand body
        FC_Log("Steps 4+5: collecting Init_thread_header getter targets");
        {
            const size_t kScanLen = 16384;
            uint8_t* fn = reinterpret_cast<uint8_t*>(m_execCmd); // always use corrected addr

            // Collected unique vector addresses from getters, in call-site order
            static uint8_t* getterVecs[8];
            int getterCount = 0;

            for (size_t off = 0; off + 5 <= kScanLen && getterCount < 8; ++off) {
                if (fn[off] != 0xE8) continue;
                int32_t rel = 0;
                memcpy(&rel, fn + off + 1, 4);
                uint8_t* callee = fn + off + 5 + rel;

                if (!isExecutable(callee)) continue;
                if (!isInitThreadHeaderPattern(callee, 80)) continue;

                uint8_t* vecAddr = extractRIPRelTarget(callee, 80, /*requireWritable=*/true);
                if (!vecAddr) continue;
                if (!validateVectorHeader(vecAddr, 64 * 1024 * 1024)) continue;

                // Deduplicate — same getter can be called multiple times
                bool dup = false;
                for (int k = 0; k < getterCount; ++k) {
                    if (getterVecs[k] == vecAddr) { dup = true; break; }
                }
                if (dup) continue;

                // Skip s_outputHandlers if already resolved
                if (vecAddr == reinterpret_cast<uint8_t*>(m_outputHandlersVecAddr)) continue;

                FC_Log("Steps 4+5: getter[%d] at %p -> vec %p (call off=%zu)",
                    getterCount, callee, vecAddr, off);
                getterVecs[getterCount++] = vecAddr;
            }

            FC_Log("Steps 4+5: found %d unique getter targets", getterCount);

            // Assign in order of first appearance in executeConsoleCommand body
            // The first getter called is always s_consoleMethods; the second is
            // s_instanceMethods
            if (getterCount >= 1) {
                m_consoleMethodsVecAddr = getterVecs[0];
                FC_Log("Step 4: s_consoleMethods at %p", getterVecs[0]);
            }
            else {
                FC_Log("Step 4 FAIL: s_consoleMethods not found");
            }

            if (getterCount >= 2) {
                m_instanceMethodsVecAddr = getterVecs[1];
                FC_Log("Step 5: s_instanceMethods at %p", getterVecs[1]);
            }
            else {
                FC_Log("Step 5 WARN: s_instanceMethods not found (non-critical)");
            }
        }

        // STEP 6: Find g_settingsManager, settingsManager_get, settingsManager_set
        FC_Log("Step 6: finding g_settingsManager + get/set");
        {
            // Scan entire image for: 48 8B 0D rel32 (MOV RCX, cs:[RIP+rel])
            // followed within 15 bytes by E8 rel32 (CALL)
            // Bucket by the load-target address; the one with the most hits is
            // g_settingsManager (referenced by hundreds of get/set call-sites)
            struct SmCandidate { uint8_t* addr; int readCount; int writeCount; };
            static SmCandidate smCands[512];
            int smCandCount = 0;

            for (size_t i = 0; i + 12 <= modSize; ++i) {
                if (modBase[i] != 0x48 || modBase[i + 1] != 0x8B || modBase[i + 2] != 0x0D)
                    continue;
                int32_t rel = 0; memcpy(&rel, modBase + i + 3, 4);
                uint8_t* lt = modBase + i + 7 + rel;
                if (!isReadWrite(lt)) continue;
                // Check for a CALL within next 15 bytes
                bool hasCall = false;
                for (int j = 7; j <= 15 && i + j + 5 <= modSize; ++j) {
                    if (modBase[i + j] == 0xE8) { hasCall = true; break; }
                }
                if (!hasCall) continue;
                // Bucket
                bool found = false;
                for (int k = 0; k < smCandCount; ++k) {
                    if (smCands[k].addr == lt) { ++smCands[k].readCount; found = true; break; }
                }
                if (!found && smCandCount < 512)
                    smCands[smCandCount++] = { lt, 1, 0 };
            }

            // Sort by readCount descending
            for (int a = 0; a < smCandCount - 1; ++a)
                for (int b2 = a + 1; b2 < smCandCount; ++b2)
                    if (smCands[b2].readCount > smCands[a].readCount)
                        std::swap(smCands[a], smCands[b2]);

            FC_Log("Step 6: top MOV-RCX+CALL candidates:");
            for (int k = 0; k < smCandCount && k < 6; ++k)
                FC_Log("  cand[%d]=%p readCount=%d", k, smCands[k].addr, smCands[k].readCount);

            // Count how many distinct MOV cs:[RIP+rel],Rxx store sites write
            // each candidate. g_settingsManager is written by exactly 2
            // instructions (constructor sets it, destructor zeros it)
            // High-traffic subsystem globals tend to have many more write sites
            for (int k = 0; k < smCandCount; ++k) smCands[k].writeCount = 0;

            for (size_t i = 0; i + 7 <= modSize; ++i) {
                uint8_t b0 = modBase[i], b1 = modBase[i + 1], b2 = modBase[i + 2];
                if ((b0 != 0x48 && b0 != 0x4C) || b1 != 0x89) continue;
                if ((b2 & 0xC7) != 0x05) continue;
                int32_t rel = 0; memcpy(&rel, modBase + i + 3, 4);
                uint8_t* wt = modBase + i + 7 + rel;
                for (int k = 0; k < smCandCount; ++k)
                    if (smCands[k].addr == wt) { ++smCands[k].writeCount; break; }
            }

            FC_Log("Step 6: top candidates with write counts:");
            for (int k = 0; k < smCandCount && k < 8; ++k)
                FC_Log("  cand[%d]=%p readCount=%d writeCount=%d",
                    k, smCands[k].addr, smCands[k].readCount, smCands[k].writeCount);

            // Pick the highest-readCount candidate written by exactly 2 store sites
            // writeCount=0 means the "read" was actually an indirect load through a
            // pointer, not a true global — skip it
            uint8_t* smGlobalAddr = nullptr;
            for (int k = 0; k < smCandCount; ++k) {
                if (smCands[k].readCount < 10) break; // sorted descending, done
                if (smCands[k].writeCount == 2) {
                    smGlobalAddr = smCands[k].addr;
                    m_settingsManagerAddr = smGlobalAddr;
                    FC_Log("Step 6: g_settingsManager at %p (readCount=%d writeCount=%d)",
                        smGlobalAddr, smCands[k].readCount, smCands[k].writeCount);
                    break;
                }
            }
            if (!smGlobalAddr) {
                FC_Log("Step 6 WARN: no suitable candidate found (non-critical)");
                goto done;
            }

            // Phase C: collect unique callees after MOV RCX,cs:[smGlobalAddr]
            {
                struct CalleeStat { uint8_t* addr; int count; };
                static CalleeStat stats[32];
                int statCount = 0;

                for (size_t i = 0; i + 12 <= modSize; ++i) {
                    if (modBase[i] != 0x48 || modBase[i + 1] != 0x8B || modBase[i + 2] != 0x0D)
                        continue;
                    int32_t rel = 0; memcpy(&rel, modBase + i + 3, 4);
                    uint8_t* lt = modBase + i + 7 + rel;
                    if (lt != smGlobalAddr) continue;

                    for (int j = 7; j <= 15 && i + j + 5 <= modSize; ++j) {
                        if (modBase[i + j] != 0xE8) continue;
                        int32_t crel = 0; memcpy(&crel, modBase + i + j + 1, 4);
                        uint8_t* callee = modBase + i + j + 5 + crel;
                        if (!isExecutable(callee)) continue;
                        bool found2 = false;
                        for (int k = 0; k < statCount; ++k)
                            if (stats[k].addr == callee) { ++stats[k].count; found2 = true; break; }
                        if (!found2 && statCount < 32)
                            stats[statCount++] = { callee, 1 };
                        break;
                    }
                }

                for (int a = 0; a < statCount - 1; ++a)
                    for (int b2 = a + 1; b2 < statCount; ++b2)
                        if (stats[b2].count > stats[a].count)
                            std::swap(stats[a], stats[b2]);

                FC_Log("Step 6: %d unique callees after MOV RCX,[g_settingsManager]",
                    statCount);
                for (int k = 0; k < statCount && k < 4; ++k)
                    FC_Log("  callee[%d]=%p count=%d", k, stats[k].addr, stats[k].count);

                if (statCount >= 1) {
                    m_settingsGet = reinterpret_cast<SettingsGetFn>(stats[0].addr);
                    FC_Log("Step 6: settingsManager_get=%p", stats[0].addr);
                }
                if (statCount >= 2) {
                    m_settingsSet = reinterpret_cast<SettingsSetFn>(stats[1].addr);
                    FC_Log("Step 6: settingsManager_set=%p", stats[1].addr);
                }
                if (statCount == 0)
                    FC_Log("Step 6 WARN: no get/set callees found (non-critical)");
            }
        }

    done:
        bool ok = (m_execCmd != nullptr) &&
            (m_consoleMethodsVecAddr != nullptr) &&
            (m_outputHandlersVecAddr != nullptr) &&
            (m_addOutputHandler != nullptr);

        FC_Log("tryResolveDynamic: execCmd=%p consoleMethods=%p outputHandlers=%p "
            "addHandler=%p instanceMethods=%p smAddr=%p get=%p set=%p ok=%d",
            (void*)m_execCmd, m_consoleMethodsVecAddr, m_outputHandlersVecAddr,
            (void*)m_addOutputHandler, m_instanceMethodsVecAddr,
            m_settingsManagerAddr, (void*)m_settingsGet, (void*)m_settingsSet,
            (int)ok);
        return ok;
    }

    // tryResolveDynamicBF2 — BF2-specific supplement to tryResolveDynamic
    //
    // BF2 architecture notes:
    //   - s_outputHandlers exists but is EMPTY ([mpBegin..mpEnd) = 8 bytes,
    //     no actual delegates).  Output is captured by hooking writeConsoleFunc
    //     directly — s_outputHandlers is never needed at runtime
    //   - writeConsoleFunc (0x1454CCF70) is the true dispatch target
    //     It iterates s_outputHandlers with stride=16, call at [slot+8]
    //     Signature: (tag: const char*, buf: const char*, size: uint32_t)
    //   - s_consoleMethods IS populated (83 entries) and is needed for __LIST__
    //   - addOutputHandler does not exist in BF2 — direct vector insertion
    //     is also unused; we hook writeConsoleFunc instead.
    //
    // Resolution steps:
    //   A: s_consoleMethods — scan execCmd LEA targets; find matching vector
    //      by mpCapacity pointer in writable memory.
    //   B: writeConsoleFunc — FIXED: flexible two-pass scan
    //      Pass 1: find "48 8B 1D" (MOV RBX,[RIP+rel]) anywhere in each
    //              executable region.  For each hit, scan forward up to 64
    //              bytes for "48 8B 3D" (MOV RDI,[RIP+rel]). If the resolved
    //              targets are 8 bytes apart AND the surrounding ±512-byte
    //              window contains "48 83 C3 10" (ADD RBX,10), it's a match
    //      Pass 2: walk back to prologue from the first MOV instruction
    //      This replaces the old rigid back-to-back offset test which failed
    //      when any instruction appeared between the two MOVs
    //   C: writeConsoleFunc via s_outputHandlers slot+8 (fallback)
    //
    // Success condition: execCmd (already set) + consoleMethods + writeConsoleFunc
    bool ConsoleBridge::tryResolveDynamicBF2(uint8_t* modBase, size_t modSize)
    {
        FC_Log("tryResolveDynamicBF2: entering");

        if (!m_execCmd) {
            FC_Log("tryResolveDynamicBF2: execCmd not set, cannot proceed");
            return false;
        }

        m_bf2StringLayout = true;
        m_use3ArgHandler = true;
        m_delegateStride = 16;
        m_delegateFnOffset = 8;
        FC_Log("tryResolveDynamicBF2: BF2 layout constants set");

        uint8_t* execCmd = reinterpret_cast<uint8_t*>(m_execCmd);

        // Step A: s_consoleMethods
        // Scan execCmd body for all LEA Rxx,[RIP+rel] targets
        // The fixed_vector's mpCapacity field at +0x18 holds a pointer matching one
        // of those LEA targets.  Scan writable memory for a qword-aligned slot at
        // +0x18 equal to each LEA value, validate as a vector with live mpBegin
        FC_Log("tryResolveDynamicBF2 Step A: s_consoleMethods via mpCapacity byte-search");

        if (!m_consoleMethodsVecAddr) {
            const size_t kScanExec = 16384;
            uint8_t* leaTargets[64];
            int      leaCount = 0;

            // Collect LEA targets from execCmd body directly
            for (size_t i = 0; i + 7 <= kScanExec && leaCount < 64; ++i) {
                uint8_t b0 = execCmd[i], b1 = execCmd[i + 1], b2 = execCmd[i + 2];
                if (b0 < 0x48 || b0 > 0x4F) continue;
                if (b1 != 0x8D) continue;
                if ((b2 & 0xC7) != 0x05) continue;
                int32_t rel = 0;
                memcpy(&rel, execCmd + i + 3, 4);
                uint8_t* tgt = execCmd + i + 7 + rel;
                if (!isReadable(tgt)) continue;
                bool dup = false;
                for (int k = 0; k < leaCount; ++k)
                    if (leaTargets[k] == tgt) { dup = true; break; }
                if (!dup) leaTargets[leaCount++] = tgt;
            }

            // Also follow CALL targets one level deep (through JMP thunks if needed)
            // to collect LEA targets from static getter functions called by execCmd
            // Some games store s_consoleMethods in a getter that is JMP-thunked and
            // whose LEA target does not appear directly in the execCmd body
            for (size_t i = 0; i + 5 <= kScanExec && leaCount < 64; ++i) {
                if (execCmd[i] != 0xE8) continue;
                int32_t rel = 0;
                memcpy(&rel, execCmd + i + 1, 4);
                uint8_t* callee = execCmd + i + 5 + rel;
                if (!isExecutable(callee)) continue;

                // Follow a single JMP thunk (E9 rel32 or FF 25 abs) if present
                uint8_t* fn = callee;
                if (fn[0] == 0xE9) {
                    int32_t jrel = 0;
                    memcpy(&jrel, fn + 1, 4);
                    uint8_t* jdest = fn + 5 + jrel;
                    if (isExecutable(jdest)) fn = jdest;
                }
                else if (fn[0] == 0xFF && fn[1] == 0x25) {
                    int32_t jrel = 0;
                    memcpy(&jrel, fn + 2, 4);
                    uint8_t* slot = fn + 6 + jrel;
                    uint64_t dest = 0;
                    if (safeRead64(slot, &dest) && dest && isExecutable(
                        reinterpret_cast<void*>(static_cast<uintptr_t>(dest))))
                        fn = reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(dest));
                }

                // Scan up to 64 bytes of the (possibly thunk-resolved) callee for LEAs
                const size_t kCalleeLen = 64;
                for (size_t j = 0; j + 7 <= kCalleeLen; ++j) {
                    uint8_t c0 = fn[j], c1 = fn[j + 1], c2 = fn[j + 2];
                    if (c0 < 0x48 || c0 > 0x4F) continue;
                    if (c1 != 0x8D) continue;
                    if ((c2 & 0xC7) != 0x05) continue;
                    int32_t lrel = 0;
                    memcpy(&lrel, fn + j + 3, 4);
                    uint8_t* tgt = fn + j + 7 + lrel;
                    if (!isReadable(tgt)) continue;

                    // Alternative getter pattern: callee is a thin getter that does
                    // LEA RAX,[vecObj] + RET — the LEA target IS the vector object
                    // itself (not its mpCapacity field). Detect this by checking if
                    // tgt looks like a valid vector object directly
                    // Guard: only apply if consoleMethods not already found by a
                    // more reliable path (Steps 4/5 Init_thread_header scan)
                    //
                    // BF2015 fixed_vector correction: for a fixed_vector<T*,N> the
                    // getter does LEA RAX,[objBase] where objBase layout is:
                    //   +0x00 mpBegin (points INTO the inline buffer at +0x28)
                    //   +0x08 mpEnd
                    //   +0x10 mpCapacity
                    //   +0x18 mpEndOfStorage (= objBase + 0x28 + N*8)
                    //   +0x20 padding/flags
                    //   +0x28 inline buffer (element data)
                    // The LEA target is objBase (+0x00), so tgt IS the correct base
                    // However, if the getter instead LEAs to the inline buffer itself
                    // (tgt = objBase+0x28), reading [tgt+0..+16] as a vector header
                    // gives garbage that may spuriously pass validation
                    // Detect and correct this: if [tgt-0x28] is readable and
                    // [tgt-0x28+0x00] (mpBegin value) == tgt, then tgt is the inline
                    // buffer and (tgt-0x28) is the true object base
                    if (!m_consoleMethodsVecAddr) {
                        uint64_t vpBegin = 0, vpEnd = 0, vpCap = 0;
                        if (safeRead64(tgt, &vpBegin) &&
                            safeRead64(tgt + 8, &vpEnd) &&
                            safeRead64(tgt + 16, &vpCap) &&
                            vpBegin != 0 &&
                            vpBegin <= vpEnd && vpEnd <= vpCap &&
                            (vpEnd - vpBegin) >= 32 &&
                            (vpCap - vpBegin) <= 64 * 1024 * 1024 &&
                            tgt != reinterpret_cast<uint8_t*>(m_outputHandlersVecAddr) &&
                            isReadable(reinterpret_cast<void*>(
                                static_cast<uintptr_t>(vpBegin))))
                        {
                            // Check if tgt is actually the inline buffer of a
                            // fixed_vector whose object base is at tgt-0x28.
                            // Signature: [tgt-0x28] (mpBegin field of the object)
                            // holds the value tgt itself (points to inline buffer)
                            uint8_t* candidateBase = tgt;
                            if (tgt > reinterpret_cast<uint8_t*>(0x28ULL)) {
                                uint8_t* possibleObjBase = tgt - 0x28;
                                uint64_t objMpBegin = 0;
                                if (isReadable(possibleObjBase) &&
                                    safeRead64(possibleObjBase, &objMpBegin) &&
                                    objMpBegin == reinterpret_cast<uint64_t>(tgt))
                                {
                                    // tgt is the inline buffer; the real object
                                    // base is possibleObjBase
                                    candidateBase = possibleObjBase;
                                    FC_Log("tryResolveDynamicBF2 Step A: "
                                        "correcting inline-buffer LEA target %p "
                                        "-> fixed_vector base %p",
                                        tgt, candidateBase);
                                }
                            }
                            m_consoleMethodsVecAddr = candidateBase;
                            m_consoleMethodsFromStepA = true;
                            FC_Log("tryResolveDynamicBF2 Step A: s_consoleMethods=%p "
                                "via direct getter LEA (begin=%p end=%p)",
                                candidateBase,
                                (void*)(uintptr_t)vpBegin,
                                (void*)(uintptr_t)vpEnd);
                        }
                    }

                    bool dup = false;
                    for (int k = 0; k < leaCount; ++k)
                        if (leaTargets[k] == tgt) { dup = true; break; }
                    if (!dup) leaTargets[leaCount++] = tgt;
                }
            }

            FC_Log("tryResolveDynamicBF2 Step A: %d LEA targets in execCmd", leaCount);

            for (int li = 0; li < leaCount && !m_consoleMethodsVecAddr; ++li) {
                uint8_t* T = leaTargets[li];
                uint64_t Tval = reinterpret_cast<uint64_t>(T);
                uint8_t  needle[8];
                memcpy(needle, &Tval, 8);

                // The fixed_vector object (mpCapacity at +0x18 = T) must live in
                // the module's own writable BSS/data — not beyond it
                // Clamp the scan to actual committed writable pages within the
                // module's virtual extent, using VirtualQuery to walk regions so
                // we skip the huge gaps that inflate modSize in large images
                uintptr_t scanStart = reinterpret_cast<uintptr_t>(modBase);
                uintptr_t scanEnd = scanStart + modSize;
                uintptr_t cursor = scanStart;

                while (cursor + 0x18 + 8 <= scanEnd && !m_consoleMethodsVecAddr)
                {
                    MEMORY_BASIC_INFORMATION pmbi{};
                    if (!VirtualQuery(reinterpret_cast<void*>(cursor), &pmbi, sizeof(pmbi)))
                        break;

                    uintptr_t regionBase = reinterpret_cast<uintptr_t>(pmbi.BaseAddress);
                    uintptr_t regionEnd = regionBase + pmbi.RegionSize;

                    // Advance cursor past this region regardless of whether we scan it
                    cursor = regionEnd;

                    // Scan committed, readable, non-executable pages
                    // The s_consoleMethods fixed_vector object may live in read-only
                    // .rdata as well as writable .bss
                    // We validate the vector's mpBegin pointer is writable separately
                    bool readable = (pmbi.State == MEM_COMMIT) &&
                        !(pmbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) &&
                        !(pmbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ |
                            PAGE_EXECUTE_READWRITE |
                            PAGE_EXECUTE_WRITECOPY));
                    if (!readable) continue;

                    // Clamp region to module bounds
                    uintptr_t lo = regionBase > scanStart ? regionBase : scanStart;
                    uintptr_t hi = regionEnd < scanEnd ? regionEnd : scanEnd;
                    if (lo + 0x18 + 8 > hi) continue;

                    for (uintptr_t j = lo; j + 0x18 + 8 <= hi && !m_consoleMethodsVecAddr; j += 8)
                    {
                        if (memcmp(reinterpret_cast<void*>(j + 0x18), needle, 8) != 0) continue;
                        uint8_t* V = reinterpret_cast<uint8_t*>(j);
                        if (V == reinterpret_cast<uint8_t*>(m_outputHandlersVecAddr)) continue;
                        // Use isReadable instead of validateVectorHeader (which requires
                        // writable) since the vector object itself may be in .rdata
                        if (!isReadable(V)) continue;
                        uint64_t vpBegin = 0, vpEnd = 0, vpCap = 0;
                        if (!safeRead64(V, &vpBegin)) continue;
                        if (!safeRead64(V + 8, &vpEnd))   continue;
                        if (!safeRead64(V + 16, &vpCap))   continue;
                        // Basic sanity: begin <= end <= capacity, non-zero
                        if (vpBegin == 0) continue;
                        if (vpBegin > vpEnd || vpEnd > vpCap) continue;
                        if ((vpCap - vpBegin) > 64 * 1024 * 1024) continue;
                        // Must have at least 4 entries (32 bytes) — reject stub
                        // vectors used as sentinels (size < 32) which are false
                        // positives. The real s_consoleMethods always has methods
                        if ((vpEnd - vpBegin) < 32) continue;
                        // mpBegin must point into readable (element data) memory
                        if (!isReadable(reinterpret_cast<void*>(
                            static_cast<uintptr_t>(vpBegin)))) continue;
                        m_consoleMethodsVecAddr = V;
                        m_consoleMethodsFromStepA = true;
                        FC_Log("tryResolveDynamicBF2 Step A: s_consoleMethods=%p "
                            "(mpCap=0x%016llX LEA-target=%p)",
                            V, (unsigned long long)Tval, T);
                    }
                }
            }

            if (!m_consoleMethodsVecAddr)
                FC_Log("tryResolveDynamicBF2 Step A FAIL: s_consoleMethods not found");
        }

        // Step B: writeConsoleFunc + s_outputHandlers
        //
        // The dispatcher (IDA: 0x1454CCF70) has this distinctive prologue:
        //
        //   48 89 5C 24 08       mov  [rsp+08h], rbx
        //   48 89 6C 24 10       mov  [rsp+10h], rbp
        //   48 89 74 24 18       mov  [rsp+18h], rsi
        //   48 89 7C 24 20       mov  [rsp+20h], rdi   <- 4th [rsp+N] store
        //   41 56                push r14
        //   48 83 EC 20          sub  rsp, 20h
        //   48 8B 1D xx xx xx xx mov  rbx, [s_outputHandlers.mpBegin]
        //   ...
        //   48 8B 3D xx xx xx xx mov  rdi, [s_outputHandlers.mpEnd]
        //   ...
        //   48 83 C3 10          add  rbx, 10h          <- iteration stride
        //
        // Strategy:
        //   Scan every executable region for the 5-instruction prologue prefix
        //   (four [rsp+N] stores + push r14 + sub rsp,20).  For each match walk
        //   the next 128 bytes collecting MOV RBX/RDI [RIP+rel] targets; if two
        //   are found 8 bytes apart AND ADD RBX,10 appears nearby, record it
        //
        //   This is far more specific than the old approach (which matched any
        //   function containing adjacent MOV RBX/RDI loads) and will not pick up
        //   earlier false-positive functions that share only part of the pattern
        FC_Log("tryResolveDynamicBF2 Step B: writeConsoleFunc via prologue+pattern scan");

        if (!m_writeConsoleFunc) {
            static const uint8_t kProlog[] = {
                0x48, 0x89, 0x5C, 0x24, 0x08,   // mov [rsp+08], rbx
                0x48, 0x89, 0x6C, 0x24, 0x10,   // mov [rsp+10], rbp
                0x48, 0x89, 0x74, 0x24, 0x18,   // mov [rsp+18], rsi
                0x48, 0x89, 0x7C, 0x24, 0x20,   // mov [rsp+20], rdi
                0x41, 0x56,                       // push r14
                0x48, 0x83, 0xEC, 0x20           // sub rsp, 20h
            };
            static const size_t kPrologLen = sizeof(kProlog); // 26

            // Clamp scan to the module's own executable regions only
            // The old code walked addr=0 through the entire VA space which
            // meant scanning every loaded DLL and mapped file before reaching
            // the game module — extremely slow on large images
            MEMORY_BASIC_INFORMATION mbi{};
            uintptr_t cursor = reinterpret_cast<uintptr_t>(modBase);
            uintptr_t modEnd = cursor + modSize;

            while (cursor < modEnd && !m_writeConsoleFunc) {
                if (!VirtualQuery(reinterpret_cast<void*>(cursor), &mbi, sizeof(mbi))) break;

                uintptr_t regionBase = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
                uintptr_t regionEnd = regionBase + mbi.RegionSize;
                if (regionEnd > modEnd) regionEnd = modEnd;

                bool isExec = (mbi.State == MEM_COMMIT) &&
                    (mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ |
                        PAGE_EXECUTE_READWRITE |
                        PAGE_EXECUTE_WRITECOPY));

                if (isExec) {
                    uint8_t* region = reinterpret_cast<uint8_t*>(regionBase);
                    size_t   rsize = static_cast<size_t>(regionEnd - regionBase);

                    for (size_t i = 0;
                        i + kPrologLen + 128 <= rsize && !m_writeConsoleFunc;
                        ++i)
                    {
                        // Match the prologue prefix
                        if (memcmp(region + i, kProlog, kPrologLen) != 0) continue;

                        // Scan the next 128 bytes for MOV RBX,[RIP+rel] and
                        // MOV RDI,[RIP+rel].
                        uint8_t* targetRBX = nullptr;
                        uint8_t* targetRDI = nullptr;

                        for (size_t j = i + kPrologLen;
                            j + 7 <= i + kPrologLen + 128 && j + 7 <= rsize;
                            ++j)
                        {
                            if (region[j] == 0x48 && region[j + 1] == 0x8B && region[j + 2] == 0x1D)
                            {
                                int32_t rel = 0;
                                memcpy(&rel, region + j + 3, 4);
                                uint8_t* t = region + j + 7 + rel;
                                if (isReadWrite(t)) targetRBX = t;
                            }
                            if (region[j] == 0x48 && region[j + 1] == 0x8B && region[j + 2] == 0x3D)
                            {
                                int32_t rel = 0;
                                memcpy(&rel, region + j + 3, 4);
                                uint8_t* t = region + j + 7 + rel;
                                if (isReadWrite(t)) targetRDI = t;
                            }
                        }

                        if (!targetRBX || !targetRDI) continue;

                        // The two targets must be exactly 8 bytes apart
                        uint8_t* vecBase = nullptr;
                        if (targetRDI == targetRBX + 8) vecBase = targetRBX;
                        else if (targetRBX == targetRDI + 8) vecBase = targetRDI;
                        else continue;

                        // Confirm ADD RBX,10 (48 83 C3 10) in ±256 bytes
                        bool hasLoop = false;
                        size_t ls = (i >= 256) ? i - 256 : 0;
                        size_t le = (i + 256 + 4 < rsize) ? i + 256 : rsize - 4;
                        for (size_t lk = ls; lk + 4 <= le; ++lk) {
                            if (region[lk] == 0x48 && region[lk + 1] == 0x83 &&
                                region[lk + 2] == 0xC3 && region[lk + 3] == 0x10) {
                                hasLoop = true;
                                break;
                            }
                        }
                        if (!hasLoop) continue;

                        m_writeConsoleFunc = reinterpret_cast<uintptr_t>(region + i);
                        m_outputHandlersVecAddr = vecBase;

                        FC_Log("tryResolveDynamicBF2 Step B: FOUND "
                            "writeConsoleFunc=%p s_outputHandlers=%p",
                            region + i, vecBase);
                    }
                }

                if (regionEnd <= cursor) break;
                cursor = regionEnd;
            }

            if (!m_writeConsoleFunc)
                FC_Log("tryResolveDynamicBF2 Step B FAIL: prologue pattern not found");
        }

        // Step C: writeConsoleFunc via s_outputHandlers slot+8 (fallback)
        FC_Log("tryResolveDynamicBF2 Step C: writeConsoleFunc via s_outputHandlers slot");

        if (!m_writeConsoleFunc && m_outputHandlersVecAddr) {
            uint64_t mpBegin = 0, mpEnd = 0;
            if (safeRead64(m_outputHandlersVecAddr, &mpBegin) &&
                safeRead64(reinterpret_cast<uint8_t*>(m_outputHandlersVecAddr) + 8, &mpEnd) &&
                mpBegin != 0 && mpEnd > mpBegin)
            {
                uint64_t fnPtr = 0;
                void* slot8 = reinterpret_cast<void*>(
                    static_cast<uintptr_t>(mpBegin + 8));
                if (safeRead64(slot8, &fnPtr) && fnPtr > 0x10000ULL &&
                    isExecutable(reinterpret_cast<void*>(
                        static_cast<uintptr_t>(fnPtr))))
                {
                    m_writeConsoleFunc = static_cast<uintptr_t>(fnPtr);
                    FC_Log("tryResolveDynamicBF2 Step C: writeConsoleFunc=%p "
                        "(from slot+8, mpBegin=%p)",
                        (void*)m_writeConsoleFunc, (void*)(uintptr_t)mpBegin);
                }
                else
                {
                    FC_Log("tryResolveDynamicBF2 Step C FAIL: slot fn ptr invalid "
                        "(mpBegin=%p fnPtr=%p)",
                        (void*)(uintptr_t)mpBegin, (void*)(uintptr_t)fnPtr);
                }
            }
            else
            {
                FC_Log("tryResolveDynamicBF2 Step C FAIL: vec empty or invalid "
                    "(mpBegin=%p mpEnd=%p)",
                    (void*)(uintptr_t)mpBegin, (void*)(uintptr_t)mpEnd);
            }
        }
        else if (!m_writeConsoleFunc)
        {
            FC_Log("tryResolveDynamicBF2 Step C SKIP: s_outputHandlers not resolved");
        }

        // Result
        bool ok = (m_execCmd != nullptr) &&
            (m_consoleMethodsVecAddr != nullptr) &&
            (m_writeConsoleFunc != 0);

        // Unbound: re-scan executeConsoleCommand for the correct
        // s_consoleMethods getter when the resolved vector is empty
        //
        // Steps 4+5 found getter 0x142747F30 -> vec 0x144D461F0 (empty)
        // The real getter is 0x142747D40 -> returns 0x14557C810 (RB-tree root)
        // That getter has a different pattern: no Init_thread_header guard;
        // it uses a thread-local slot check then falls through to
        //   LEA RAX,[14557C810]  (48 8D 05 xx xx xx xx)
        //   RET
        // We detect this by scanning execCmd for CALL rel32 targets whose
        // body contains, within the first 32 bytes:
        //   48 8D 05 xx xx xx xx  (LEA RAX,[RIP+rel])
        //   C3                    (RET, possibly with ADD RSP,xx before it)
        // and whose LEA target differs from the already-resolved (empty) vec
        if (m_consoleMethodsVecAddr && m_execCmd) {
            uint64_t vpB = 0, vpE = 0;
            safeRead64(m_consoleMethodsVecAddr, &vpB);
            safeRead64(reinterpret_cast<uint8_t*>(m_consoleMethodsVecAddr) + 8, &vpE);
            if (vpB == 0 || vpE == vpB) {
                FC_Log("tryResolveDynamicBF2: consoleMethods empty, scanning for alternate getter");
                uint8_t* execBody = reinterpret_cast<uint8_t*>(m_execCmd);

                // Scan execCmd for: CALL rel32 whose return value (in RAX/RSI/RDI)
                // is used as a tree root within the next 16 bytes via [reg+0x10]
                // Pattern after the getter call:
                //   MOV rsi, rax        (48 8B F0)  or similar reg move
                //   MOV rcx, [rsi+0x10] (48 8B 4E 10) — tree root dereference
                // The +0x10 dereference is the unique signature of the tree root
                // access (left/right/parent at +0, +8, +10 in the node struct)
                // No string utility produces this pattern after its call site
                for (size_t i = 0; i + 5 <= 0x2000; ++i) {
                    if (execBody[i] != 0xE8) continue;
                    int32_t rel = 0;
                    memcpy(&rel, execBody + i + 1, 4);
                    uint8_t* callee = execBody + i + 5 + rel;
                    if (!isExecutable(callee)) continue;

                    // Check the 20 bytes after this CALL for a [reg+0x10] dereference
                    // In x64 MSVC: MOV r64,[r64+0x10] encodes as
                    // 48/4C/49/4D  8B  ModRM(disp8)  10
                    // where ModRM = 0x4? (mod=01, rm=reg, reg=dst)
                    // We just look for the byte sequence: (REX) 8B ?? 10
                    // with mod=01 field in ModRM (upper 2 bits = 01)
                    bool hasTreeDeref = false;
                    size_t windowEnd = i + 5 + 20;
                    if (windowEnd > 0x2000) windowEnd = 0x2000;
                    for (size_t j = i + 5; j + 3 <= windowEnd; ++j) {
                        uint8_t b0 = execBody[j];
                        // REX prefix optional: 48-4F
                        int skip = 0;
                        if (b0 >= 0x48 && b0 <= 0x4F) { skip = 1; }
                        if (j + skip + 3 > windowEnd) break;
                        uint8_t op = execBody[j + skip];
                        uint8_t mrm = execBody[j + skip + 1];
                        uint8_t disp = execBody[j + skip + 2];
                        // MOV r64,[r64+disp8]: opcode=8B, mod=01 (bits 7:6 = 01)
                        if (op == 0x8B &&
                            (mrm & 0xC0) == 0x40 &&  // mod = 01
                            disp == 0x10)             // displacement = +0x10
                        {
                            hasTreeDeref = true;
                            break;
                        }
                    }
                    if (!hasTreeDeref) continue;

                    // This CALL is followed by a [reg+0x10] tree dereference
                    // Now extract the LEA target from the callee
                    uint8_t* fn = callee;
                    if (fn[0] == 0xE9) {
                        int32_t jrel = 0;
                        memcpy(&jrel, fn + 1, 4);
                        fn = fn + 5 + jrel;
                        if (!isExecutable(fn)) continue;
                    }

                    // Scan up to 128 bytes for LEA RAX,[RIP+rel] + RET
                    for (size_t ci = 0; ci + 8 <= 128; ++ci) {
                        if (fn[ci] != 0x48) continue;
                        if (fn[ci + 1] != 0x8D) continue;
                        if (fn[ci + 2] != 0x05) continue;

                        bool hasRet = false;
                        for (size_t ri = ci + 7; ri < ci + 22 && ri < 150; ++ri) {
                            if (fn[ri] == 0xC3) { hasRet = true; break; }
                        }
                        if (!hasRet) continue;

                        int32_t leaRel = 0;
                        memcpy(&leaRel, fn + ci + 3, 4);
                        uint8_t* tgt = fn + ci + 7 + leaRel;

                        if (!isReadWrite(tgt)) continue;
                        if (tgt == m_consoleMethodsVecAddr) continue;

                        uint64_t t0 = 0;
                        if (!safeRead64(tgt, &t0) || !t0) continue;
                        if (!isReadable(reinterpret_cast<void*>(
                            static_cast<uintptr_t>(t0)))) continue;

                        FC_Log("tryResolveDynamicBF2: alternate getter at %p -> vec %p "
                            "(tree-deref call site confirmed)",
                            callee, tgt);
                        m_consoleMethodsVecAddr = tgt;
                        m_consoleMethodsFromStepA = false;
                        m_consoleMethodsFromAltGetter = true;
                        goto altGetterDone;
                    }
                }
            altGetterDone:;
            }
        }

        // Unbound tree detection
        // Only run for consoleMethods found via the alternate getter path
        // Step A (direct getter LEA) and Init_thread_header finds are always
        // fixed_vector arrays — their firstQword is a ConsoleMethod* with an
        // executable pfn, but on some games it incidentally passes
        // the readable+non-executable heuristic, causing a false positive
        if (m_consoleMethodsVecAddr && !m_consoleMethodsIsTree &&
            m_consoleMethodsFromAltGetter) {
            uint64_t firstQword = 0;
            if (safeRead64(m_consoleMethodsVecAddr, &firstQword) && firstQword) {
                void* firstPtr = reinterpret_cast<void*>(static_cast<uintptr_t>(firstQword));
                // If the value at [firstQword+0x00] is itself readable (node ptr)
                // but NOT executable (not a pfn), this is a tree node, not an array
                if (isReadable(firstPtr) && !isExecutable(firstPtr)) {
                    uint64_t nodeChild = 0;
                    if (safeRead64(firstPtr, &nodeChild) && nodeChild) {
                        void* childPtr = reinterpret_cast<void*>(static_cast<uintptr_t>(nodeChild));
                        if (isReadable(childPtr) && !isExecutable(childPtr)) {
                            m_consoleMethodsIsTree = true;
                            FC_Log("tryResolveDynamicBF2: detected RB-tree consoleMethods at %p",
                                m_consoleMethodsVecAddr);
                        }
                    }
                }
            }
        }

        FC_Log("tryResolveDynamicBF2: consoleMethods=%p outputHandlers=%p writeConsoleFunc=%p bf2Str=%d use3Arg=%d ok=%d isTree=%d",
            m_consoleMethodsVecAddr, m_outputHandlersVecAddr,
            (void*)m_writeConsoleFunc,
            (int)m_bf2StringLayout, (int)m_use3ArgHandler, (int)ok,
            (int)m_consoleMethodsIsTree);
        return ok;
    }

// tryResolveDynamicFixedVector
//
// Resolves s_consoleMethods for games that use a fixed_vector multi-init getter
// instead of Init_thread_header guards
//
// Strategy:
//   Scan executeConsoleCommand body for CALL rel32 targets
//   For each callee, check if it matches the fixed_vector getter fingerprint:
//     - Small function (≤ 80 instructions / ≤ 512 bytes)
//     - Contains TEST AL,1 / JNE (the guard byte check)
//     - Ends with LEA RAX,[RIP+rel] before ADD RSP,28 / RET
//     - The LEA target is a writable address (vector object in .data)
//     - At runtime the vector is non-empty (mpBegin != mpEnd)
//
// The first matching callee's LEA target is s_consoleMethods
//
// Also sets layout constants (bf2StringLayout, use3ArgHandler) identically
// to tryResolveDynamicBF2, since these games share the same string/handler ABI
    bool ConsoleBridge::tryResolveDynamicFixedVector(uint8_t* modBase, size_t modSize)
    {
        FC_Log("tryResolveDynamicFV: entering");

        if (!m_execCmd) {
            FC_Log("tryResolveDynamicFV: execCmd not set, cannot proceed");
            return false;
        }

        m_bf2StringLayout = true;
        m_use3ArgHandler = true;
        m_delegateStride = 16;
        m_delegateFnOffset = 8;
        FC_Log("tryResolveDynamicFV: layout constants set");

        uint8_t* execCmd = reinterpret_cast<uint8_t*>(m_execCmd);

        // Scan execCmd for CALL rel32 targets
        const size_t kScanLen = 16384;

        for (size_t off = 0; off + 5 <= kScanLen && !m_consoleMethodsVecAddr; ++off) {
            if (execCmd[off] != 0xE8) continue;
            int32_t rel = 0;
            memcpy(&rel, execCmd + off + 1, 4);
            uint8_t* callee = execCmd + off + 5 + rel;

            if (!isExecutable(callee)) continue;

            // Fingerprint the callee as a fixed_vector getter
            // Scan up to 512 bytes / 80 instructions looking for:
            //   (a) TEST AL,1  (A8 01)
            //   (b) JNE short  (75 xx)
            //   (c) LEA RAX,[RIP+rel32]  (48 8D 05 xx xx xx xx)
            //       immediately followed by ADD RSP,28 (48 83 C4 28) and RET (C3)
            //
            // All three must be present; the LEA must be the last data-touching
            // instruction before the epilogue

            bool hasGuard = false;
            uint8_t* leaInsn = nullptr;

            const size_t kCalleeMax = 512;
            for (size_t ci = 0; ci + 4 <= kCalleeMax; ++ci) {
                // Guard: TEST AL,1
                if (callee[ci] == 0xA8 && callee[ci + 1] == 0x01) {
                    // Expect JNE immediately after (75 xx or 0F 85 xx xx xx xx)
                    if (ci + 2 < kCalleeMax &&
                        (callee[ci + 2] == 0x75 || callee[ci + 2] == 0x0F))
                        hasGuard = true;
                }

                // LEA RAX,[RIP+rel32]  =  48 8D 05 xx xx xx xx
                if (ci + 10 <= kCalleeMax &&
                    callee[ci] == 0x48 &&
                    callee[ci + 1] == 0x8D &&
                    callee[ci + 2] == 0x05)
                {
                    // Must be followed by ADD RSP,28 then RET
                    // ADD RSP,28 = 48 83 C4 28  (4 bytes)
                    // RET        = C3           (1 byte)
                    if (callee[ci + 7] == 0x48 && callee[ci + 8] == 0x83 &&
                        callee[ci + 9] == 0xC4 && callee[ci + 10] == 0x28 &&
                        ci + 11 < kCalleeMax && callee[ci + 11] == 0xC3)
                    {
                        leaInsn = callee + ci;
                    }
                }

                // Stop scanning only on RET that follows ADD RSP,28
                // (avoids breaking on 0xC3 bytes embedded in data or other instrs)
                if (callee[ci] == 0xC3 && ci >= 4 &&
                    callee[ci - 4] == 0x48 && callee[ci - 3] == 0x83 &&
                    callee[ci - 2] == 0xC4 && callee[ci - 1] == 0x28)
                    break;
            }

            if (!hasGuard || !leaInsn) continue;

            // Decode the LEA target
            int32_t leaRel = 0;
            memcpy(&leaRel, leaInsn + 3, 4);
            uint8_t* vecObj = leaInsn + 7 + leaRel;

            if (!isReadWrite(vecObj)) continue;

            // Validate: mpBegin (at +0) must be a readable non-null pointer
            // and mpEnd (at +8) must be > mpBegin (non-empty — methods registered)
            uint64_t vpBegin = 0, vpEnd = 0;
            if (!safeRead64(vecObj, &vpBegin)) continue;
            if (!safeRead64(vecObj + 8, &vpEnd))   continue;

            FC_Log("tryResolveDynamicFV: callee=%p leaTarget=%p "
                "begin=%p end=%p hasGuard=%d leaInsn=%p",
                callee, vecObj,
                (void*)(uintptr_t)vpBegin, (void*)(uintptr_t)vpEnd,
                (int)hasGuard, leaInsn);

            if (vpBegin == 0 || vpEnd <= vpBegin) {
                FC_Log("tryResolveDynamicFV: vec empty or invalid, skipping");
                continue;
            }

            if (!isReadable(reinterpret_cast<void*>(static_cast<uintptr_t>(vpBegin))))
                continue;

            m_consoleMethodsVecAddr = vecObj;
            FC_Log("tryResolveDynamicFV: s_consoleMethods=%p "
                "(callee=%p LEA off=%zu begin=%p end=%p count=%zu)",
                vecObj, callee, (size_t)(leaInsn - callee),
                (void*)(uintptr_t)vpBegin, (void*)(uintptr_t)vpEnd,
                (size_t)((vpEnd - vpBegin) / 8));
            break;
        }

        if (!m_consoleMethodsVecAddr)
            FC_Log("tryResolveDynamicFV: s_consoleMethods not found");

        bool ok = (m_execCmd != nullptr) &&
            (m_consoleMethodsVecAddr != nullptr) &&
            (m_writeConsoleFunc != 0);

        FC_Log("tryResolveDynamicFV: consoleMethods=%p writeConsoleFunc=%p ok=%d",
            m_consoleMethodsVecAddr, (void*)m_writeConsoleFunc, (int)ok);
        return ok;
    }

    // tryResolveDynamicBF6 — BF6-specific supplement
    //
    // BF6 changed console dispatch to a queue-based model:
    //   executeConsoleCommand is only called from a drain loop; external callers
    //   must use the enqueue function instead
    //
    // Enqueue function signature:
    //   void __fastcall enqueueCmd(const char* cmd, void* ctx, uint8_t addToLog)
    //   RVA ~ 0x4F49220 from 0x140000000 base
    //
    // Detection strategy:
    //   The function is uniquely identified by this byte sequence near its start:
    //     48 8B 0D [rel32]   MOV RCX, [g_settingsManager]  ; load allocator
    //     BA 38 00 00 00     MOV EDX, 0x38                 ; alloc size = 56
    //     41 B8 10 00 00 00  MOV R8D, 0x10                 ; alignment = 16
    //
    //   We scan all executable committed pages for this 14-byte signature
    //   Then confirm the candidate is followed within 64 bytes by a call to
    //   EnterCriticalSection (identified by an import thunk via [RIP+rel])
    //
    // Sets m_enqueueCmd when found. executeCommand() uses it in place of
    // direct m_execCmd call. All other games are unaffected
    bool ConsoleBridge::tryResolveDynamicBF6(uint8_t* modBase, size_t modSize)
    {
        FC_Log("tryResolveDynamicBF6: entering");

        if (!m_execCmd) {
            FC_Log("tryResolveDynamicBF6: execCmd not set, skipping");
            return false;
        }

        // Signature:
        //   48 8B 0D xx xx xx xx   MOV RCX, [RIP+rel]   (load settingsManager)
        //   BA 38 00 00 00         MOV EDX, 38h
        //   41 B8 10 00 00 00      MOV R8D, 10h
        // Total: 7 + 5 + 6 = 18 bytes
        static const uint8_t kSig[] = {
            0xBA, 0x38, 0x00, 0x00, 0x00,        // MOV EDX, 38h
            0x41, 0xB8, 0x10, 0x00, 0x00, 0x00   // MOV R8D, 10h
        };
        static const int kSigLen = sizeof(kSig);

        // We search for the MOV EDX,38 / MOV R8D,10 pair; the MOV RCX preceding
        // it must be a 7-byte RIP-relative load (48 8B 0D xx xx xx xx)
        // Walk all committed executable pages
        MEMORY_BASIC_INFORMATION mbi{};
        uint8_t* scanPtr = modBase;
        uint8_t* const modEnd = modBase + modSize;
        uint8_t* found = nullptr;

        while (scanPtr < modEnd && !found) {
            if (!VirtualQuery(scanPtr, &mbi, sizeof(mbi))) break;
            size_t regionSize = mbi.RegionSize;
            uint8_t* regionBase = reinterpret_cast<uint8_t*>(mbi.BaseAddress);
            uint8_t* regionEnd = regionBase + regionSize;
            if (regionEnd > modEnd) regionEnd = modEnd;

            if (mbi.State == MEM_COMMIT &&
                (mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ |
                    PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)))
            {
                size_t regionBytes = static_cast<size_t>(regionEnd - regionBase);
                for (size_t i = 7; i + kSigLen <= regionBytes && !found; ++i) {
                    // Check for MOV EDX,38 + MOV R8D,10 at position i
                    if (memcmp(regionBase + i, kSig, kSigLen) != 0) continue;

                    // Confirm the 7 bytes immediately before are MOV RCX,[RIP+rel]
                    uint8_t* preInsn = regionBase + i - 7;
                    if (preInsn < modBase) continue;
                    if (preInsn[0] != 0x48 || preInsn[1] != 0x8B || preInsn[2] != 0x0D)
                        continue;

                    // Walk back to function prologue (look for CC pad before)
                    // The function starts with: 48 89 5C 24 08  push/save pattern
                    // Walk back up to 256 bytes looking for CC or 90 padding byte
                    uint8_t* candidate = nullptr;
                    for (int back = 1; back <= 256; ++back) {
                        uint8_t* p = preInsn - back;
                        if (p < modBase) break;
                        if (p[0] == 0xCC || p[0] == 0x90) {
                            // p+1 should be the function start
                            uint8_t* fn = p + 1;
                            if (!isExecutable(fn)) continue;
                            // Validate: must start with MOV [rsp+N],reg pattern
                            // or PUSH rbx/rsi: 48 89 5C, 48 89 74, 48 89 7C,
                            // 40 53, 56, 57, 41 56, etc.
                            uint8_t b0 = fn[0], b1 = fn[1], b2 = fn[2];
                            bool validPrologue =
                                (b0 == 0x48 && b1 == 0x89 && (b2 == 0x5C || b2 == 0x74 || b2 == 0x7C || b2 == 0x4C)) ||
                                (b0 == 0x40 && b1 == 0x53) ||
                                (b0 == 0x56) || (b0 == 0x57) ||
                                (b0 == 0x41 && b1 == 0x56);
                            if (validPrologue) {
                                candidate = fn;
                                break;
                            }
                        }
                    }

                    if (!candidate) continue;

                    // Verify EnterCriticalSection call exists within 128 bytes
                    // after the signature match — the function acquires a lock
                    // before pushing to the command queue
                    // We detect this by finding any FF 15 [RIP+rel] (CALL [import])
                    // within 128 bytes forward of the signature match
                    bool hasCallImport = false;
                    for (int fwd = 0; fwd < 128 && (regionBase + i + kSigLen + fwd + 6) <= regionEnd; ++fwd) {
                        uint8_t* p = regionBase + i + kSigLen + fwd;
                        if (p[0] == 0xFF && p[1] == 0x15) {
                            hasCallImport = true;
                            break;
                        }
                    }
                    if (!hasCallImport) continue;

                    found = candidate;
                    FC_Log("tryResolveDynamicBF6: enqueue candidate at %p "
                        "(sig match at %p)", found, regionBase + i);
                }
            }
            scanPtr = regionEnd;
        }

        if (!found) {
            FC_Log("tryResolveDynamicBF6: enqueue function not found");
            return false;
        }

        m_enqueueCmd = reinterpret_cast<EnqueueCmdFn>(found);
        FC_Log("tryResolveDynamicBF6: m_enqueueCmd=%p", (void*)found);
        return true;
    }

    // init
    bool ConsoleBridge::init(const char* targetModule)
    {
        if (m_initDone) return isReady();
        m_initDone = true;
        m_consoleMethodsIsTree = false;  // reset tree flag on each init
        m_treeMethodCache.clear();

        FC_Log("init: targetModule=%s", targetModule ? targetModule : "(null)");

        // Find target module
        HMODULE hTarget = nullptr;
        if (targetModule)
            hTarget = GetModuleHandleA(targetModule);
        if (!hTarget) {
            HMODULE mods[1] = {};
            DWORD needed = 0;
            if (EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &needed) && mods[0])
                hTarget = mods[0];
        }
        if (!hTarget) {
            FC_Log("init: could not find target module");
            m_diagInfo = "Could not find target module";
            return false;
        }

        MODULEINFO mi{};
        if (!GetModuleInformation(GetCurrentProcess(), hTarget, &mi, sizeof(mi))) {
            FC_Log("init: GetModuleInformation failed GLE=%lu", GetLastError());
            m_diagInfo = "GetModuleInformation failed";
            return false;
        }

        uint8_t* modBase = reinterpret_cast<uint8_t*>(mi.lpBaseOfDll);
        size_t   modSize = static_cast<size_t>(mi.SizeOfImage);
        FC_Log("init: module base=%p size=%zu", modBase, modSize);

        // Detect ABI family structurally, without relying on exe name
        //
        // The two families differ in:
        //   Skate-family: addOutputHandler game fn exists ("Same handler added
        //                 multiple times." anchor found); SSO string layout;
        //                 Init_thread_header getter pattern for consoleMethods
        //   BF2-family:   no addOutputHandler; 3-pointer string layout;
        //                 fixed_vector multi-init getter OR mpCapacity byte-search
        //                 for consoleMethods; writeConsoleFunc hooked directly
        //
        // Detection probes (applied to the already-mapped module):
        //
        //   PROBE 1 — "Same handler added multiple times." string present?
        //             Yes -> Skate-family (addOutputHandler exists)
        //             No  -> BF2-family
        //
        //   PROBE 2 — Does tryResolveDynamic find m_addOutputHandler?
        //             Yes -> confirmed Skate-family; run no supplements
        //             No  -> treat as BF2-family; run BF2+FV supplements
        //
        // Structural probe 1: look for the addOutputHandler sentinel string
        static const char kSameHandler[] = "Same handler added multiple times.";
        const bool hasAddHandlerString =
            (scanForString(modBase, modSize, kSameHandler) != nullptr);
        FC_Log("init: hasAddHandlerString=%d", (int)hasAddHandlerString);

        // Try generic dynamic resolution
        FC_Log("init: trying dynamic resolution");
        bool dynamicOk = tryResolveDynamic(modBase, modSize);

        // Structural probe 2: did the generic pass resolve addOutputHandler?
        // If not, this is a BF2-family game
        const bool isBF2FamilyStructural =
            !hasAddHandlerString || (m_addOutputHandler == nullptr);
        FC_Log("init: isBF2FamilyStructural=%d (hasAddHandlerStr=%d addHandler=%p)",
            (int)isBF2FamilyStructural, (int)hasAddHandlerString,
            (void*)m_addOutputHandler);

        if (!dynamicOk && isBF2FamilyStructural) {
            // BF2 supplement: finds writeConsoleFunc + outputHandlers via
            // prologue scan, and attempts Step A for consoleMethods
            FC_Log("init: running BF2-specific dynamic supplement");
            tryResolveDynamicBF2(modBase, modSize);

            // SWBF2015-specific override: if consoleMethods vector has fewer
            // than 4 entries it was a false positive — clear it so FV scan
            // or RVA fallback can correct it
            // Only clear stub vectors that were found by Step A itself
            // Addresses found by the more reliable Init_thread_header scan
            // (Steps 4/5) may be legitimately empty at init time (BFN populates
            // consoleMethods lazily) and must not be cleared here
            if (m_consoleMethodsVecAddr && m_consoleMethodsFromStepA) {
                uint64_t vpB = 0, vpE = 0;
                safeRead64(m_consoleMethodsVecAddr, &vpB);
                safeRead64(reinterpret_cast<uint8_t*>(m_consoleMethodsVecAddr) + 8, &vpE);
                if (vpB == 0 || vpE <= vpB || (vpE - vpB) < 32) {
                    FC_Log("init: BF2 Step A found stub vector (size=%llu), clearing",
                        (unsigned long long)(vpE > vpB ? vpE - vpB : 0));
                    m_consoleMethodsVecAddr = nullptr;
                    m_consoleMethodsFromStepA = false;
                }
            }

            // Fixed-vector getter scan
            {
                uint64_t vpB = 0, vpE = 0;
                if (m_consoleMethodsVecAddr) {
                    safeRead64(m_consoleMethodsVecAddr, &vpB);
                    safeRead64(reinterpret_cast<uint8_t*>(
                        m_consoleMethodsVecAddr) + 8, &vpE);
                }
                // Only run FV scan if consoleMethods is genuinely missing or was
                // found by Step A (mpCapacity scan) and is empty. Never clear an
                // address found by the Init_thread_header scan (Steps 4/5) — those
                // vectors may be legitimately empty at init time
                // Also reject a Step-A result whose first element doesn't look
                // like a ConsoleMethod*: dereference slot[0] and check that
                // +0x00 (pfn) is executable and +0x08 (name) is a readable
                // string starting with [A-Za-z_]. This catches false-positive
                // vectors like MEA's asset-path table whose elements are raw
                // const char* strings, not ConsoleMethod pointers
                bool stepAContentInvalid = false;
                if (m_consoleMethodsVecAddr && m_consoleMethodsFromStepA &&
                    vpB != 0 && vpE > vpB)
                {
                    uint64_t firstSlot = 0, pfn = 0, namePtr = 0;
                    if (!safeRead64(reinterpret_cast<void*>(
                        static_cast<uintptr_t>(vpB)), &firstSlot) ||
                        firstSlot < 0x10000ULL ||
                        !isReadable(reinterpret_cast<void*>(
                            static_cast<uintptr_t>(firstSlot))) ||
                        !safeRead64(reinterpret_cast<void*>(
                            static_cast<uintptr_t>(firstSlot)), &pfn) ||
                        pfn < 0x10000ULL ||
                        !isExecutable(reinterpret_cast<void*>(
                            static_cast<uintptr_t>(pfn))) ||
                        !safeRead64(reinterpret_cast<void*>(
                            static_cast<uintptr_t>(firstSlot + 8)), &namePtr) ||
                        namePtr < 0x10000ULL ||
                        !isReadable(reinterpret_cast<void*>(
                            static_cast<uintptr_t>(namePtr))))
                    {
                        stepAContentInvalid = true;
                    }
                    else
                    {
                        const char* name = reinterpret_cast<const char*>(
                            static_cast<uintptr_t>(namePtr));
                        uint8_t c0 = static_cast<uint8_t>(name[0]);
                        if (!isalpha(c0) && c0 != '_')
                            stepAContentInvalid = true;
                    }
                    if (stepAContentInvalid)
                        FC_Log("init: Step A consoleMethods content invalid "
                            "(first element not a ConsoleMethod*), clearing");
                }

                if (!m_consoleMethodsVecAddr ||
                    (m_consoleMethodsFromStepA && (vpB == 0 || vpE == vpB)) ||
                    stepAContentInvalid) {
                    FC_Log("init: consoleMethods missing or invalid "
                        "(begin=%p end=%p stepABad=%d), clearing and trying FV scan",
                        (void*)(uintptr_t)vpB, (void*)(uintptr_t)vpE,
                        (int)stepAContentInvalid);
                    m_consoleMethodsVecAddr = nullptr;
                    m_consoleMethodsFromStepA = false;
                    tryResolveDynamicFixedVector(modBase, modSize);
                }
            }

            // BF6 supplement: find queue-based enqueue function
            // Runs regardless of bf2Ok — even if consoleMethods/writeConsole
            // failed, command execution via the queue may still work
            // Does not affect any other game (signature is BF6-specific)
            tryResolveDynamicBF6(modBase, modSize);

            bool bf2Ok = (m_execCmd != nullptr) &&
                (m_consoleMethodsVecAddr != nullptr) &&
                (m_writeConsoleFunc != 0);

            // For BF6: execCmd + enqueueCmd is sufficient to execute
            // commands even if consoleMethods/writeConsole are missing
            const bool bf6Ok = (m_execCmd != nullptr) && (m_enqueueCmd != nullptr);

            if (bf2Ok || bf6Ok) {
                FC_Log("init: resolved dynamically via BF2+FV supplements "
                    "(bf2Ok=%d bf6Ok=%d enqueue=%p)",
                    (int)bf2Ok, (int)bf6Ok, (void*)m_enqueueCmd);
                dynamicOk = true;
            }
            else {
                // Keep writeConsoleFunc and outputHandlers already found by
                // BF2 Step B — only consoleMethods needs RVA fallback
                FC_Log("init: BF2+FV consoleMethods missing, partial state preserved "
                    "(writeConsoleFunc=%p outputHandlers=%p)",
                    (void*)m_writeConsoleFunc, m_outputHandlersVecAddr);
            }
        }

        if (!dynamicOk) {
            FC_Log("init: dynamic resolution incomplete - try setting custom RVAs in FrostbiteConsole.h");
        }

        // ---- Manual override application (debug only, disabled by default) ----
        // Runs after normal resolution so overrides always win when set, but
        // any field left at 0 in kManualOverrides falls through to whatever
        // dynamic resolution already found above
        if (g_enableManualOverrides)
        {
            auto resolveAddr = [&](uint64_t v) -> uint64_t {
                if (v == 0) return 0;
                return g_manualOverridesAreRVAs
                    ? v + reinterpret_cast<uint64_t>(modBase)
                    : v;
                };

            if (kManualOverrides.execCmd) {
                m_execCmd = reinterpret_cast<ExecuteConsoleCmdFn>(
                    static_cast<uintptr_t>(resolveAddr(kManualOverrides.execCmd)));
                FC_Log("init: MANUAL OVERRIDE execCmd=%p", (void*)m_execCmd);
            }
            if (kManualOverrides.consoleMethods) {
                m_consoleMethodsVecAddr = reinterpret_cast<void*>(
                    static_cast<uintptr_t>(resolveAddr(kManualOverrides.consoleMethods)));
                FC_Log("init: MANUAL OVERRIDE consoleMethods=%p", m_consoleMethodsVecAddr);
            }
            if (kManualOverrides.outputHandlers) {
                m_outputHandlersVecAddr = reinterpret_cast<void*>(
                    static_cast<uintptr_t>(resolveAddr(kManualOverrides.outputHandlers)));
                FC_Log("init: MANUAL OVERRIDE outputHandlers=%p", m_outputHandlersVecAddr);
            }
            if (kManualOverrides.addHandler) {
                m_addOutputHandler = reinterpret_cast<AddOutputHandlerFn>(
                    static_cast<uintptr_t>(resolveAddr(kManualOverrides.addHandler)));
                FC_Log("init: MANUAL OVERRIDE addHandler=%p", (void*)m_addOutputHandler);
            }
            if (kManualOverrides.instanceMethods) {
                m_instanceMethodsVecAddr = reinterpret_cast<void*>(
                    static_cast<uintptr_t>(resolveAddr(kManualOverrides.instanceMethods)));
                FC_Log("init: MANUAL OVERRIDE instanceMethods=%p", m_instanceMethodsVecAddr);
            }
            if (kManualOverrides.settingsManagerAddr) {
                m_settingsManagerAddr = reinterpret_cast<uint8_t*>(
                    static_cast<uintptr_t>(resolveAddr(kManualOverrides.settingsManagerAddr)));
                FC_Log("init: MANUAL OVERRIDE smAddr=%p", (void*)m_settingsManagerAddr);
            }
            if (kManualOverrides.settingsGet) {
                m_settingsGet = reinterpret_cast<SettingsGetFn>(
                    static_cast<uintptr_t>(resolveAddr(kManualOverrides.settingsGet)));
                FC_Log("init: MANUAL OVERRIDE settingsGet=%p", (void*)m_settingsGet);
            }
            if (kManualOverrides.settingsSet) {
                m_settingsSet = reinterpret_cast<SettingsSetFn>(
                    static_cast<uintptr_t>(resolveAddr(kManualOverrides.settingsSet)));
                FC_Log("init: MANUAL OVERRIDE settingsSet=%p", (void*)m_settingsSet);
            }
            if (kManualOverrides.writeConsoleFunc) {
                m_writeConsoleFunc = static_cast<uintptr_t>(
                    resolveAddr(kManualOverrides.writeConsoleFunc));
                FC_Log("init: MANUAL OVERRIDE writeConsoleFunc=%p", (void*)m_writeConsoleFunc);
            }
            if (kManualOverrides.enqueueCmd) {
                m_enqueueCmd = reinterpret_cast<EnqueueCmdFn>(
                    static_cast<uintptr_t>(resolveAddr(kManualOverrides.enqueueCmd)));
                FC_Log("init: MANUAL OVERRIDE enqueueCmd=%p", (void*)m_enqueueCmd);
            }
        }

        bool ok = isReady();
        char diag[512];
        _snprintf_s(diag, sizeof(diag), _TRUNCATE,
            "execCmd=%p consoleMethods=%p outputHandlers=%p "
            "addHandler=%p instanceMethods=%p smAddr=%p isReady=%d",
            (void*)m_execCmd, m_consoleMethodsVecAddr, m_outputHandlersVecAddr,
            (void*)m_addOutputHandler, m_instanceMethodsVecAddr,
            m_settingsManagerAddr, (int)ok);
        m_diagInfo = diag;
        FC_Log("init: done — %s", diag);
        return ok;
    }

    // executeCommand
    __declspec(noinline) static bool shimExecCmd(
        ExecuteConsoleCmdFn fn, EastlString* ret, const char* cmd)
    {
        __try {
            fn(ret, cmd, false);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            // "##CMDCRASH##" prefix — detected by logToFile() in dxgi.cpp
            FC_Log("##CMDCRASH##executeCommand: EXCEPTION 0x%08X cmd='%s'",
                GetExceptionCode(), cmd ? cmd : "null");
            return false;
        }
    }

    // Extracts the string content from the EastlString result and copies it
    // into a plain buffer before the EastlString destructor can fire
    // The game's allocator may throw a C++ exception (0xE06D7363) during
    // destructor — this shim catches it so the caller never sees it
    __declspec(noinline) static bool shimExtractResult(
        EastlString* ret, char* outBuf, size_t outLen, size_t* outSize)
    {
        __try {
            const char* cs = ret->c_str();
            size_t n = ret->size();
            if (!cs || n == 0) { *outSize = 0; return true; }
            size_t copy = n < outLen - 1 ? n : outLen - 1;
            memcpy(outBuf, cs, copy);
            outBuf[copy] = 0;
            *outSize = copy;
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            *outSize = 0;
            return false;
        }
    }

    // Shim for BF6 queue-based enqueue — isolates __try from the
    // executeCommand frame which has C++ object unwinding (std::string)
__declspec(noinline) static bool shimEnqueueCmd(
        ConsoleBridge::EnqueueCmdFn fn, const char* cmd)
    {
        // Job object: only [rdx+0] and [rdx+8] are read by the constructor
        // Zero both for fire-and-forget dispatch (no completion callback)
        uint64_t nullJob[2] = { 0, 0 };
        __try {
            fn(cmd, nullJob, 0);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            FC_Log("executeCommand(enqueue): EXCEPTION 0x%08X cmd='%s'",
                GetExceptionCode(), cmd ? cmd : "null");
            return false;
        }
    }

std::string ConsoleBridge::executeCommand(const char* cmd)
{
    m_lastExecCrashed = false;

    if (!cmd) return {};

    // BF6 queue-based path: enqueue the command and return immediately
    // The game drains the queue on its own thread; no return value is available
    // (queue-based dispatch doesn't go through shimExecCmd, so a failure
    // here never sets m_lastExecCrashed — there's nothing to catch)
    if (m_enqueueCmd) {
        shimEnqueueCmd(m_enqueueCmd, cmd);
        return {};
    }

    if (!m_execCmd) return {};

    EastlString ret{};
    memset(&ret, 0, sizeof(ret));
    ret.isBF2Layout = m_bf2StringLayout;

    if (!m_bf2StringLayout) {
        ret.raw[0x0F] = 15;
    }

    if (!shimExecCmd(m_execCmd, &ret, cmd)) {
        m_lastExecCrashed = true;
        return {};
    }

    char resultBuf[4096];
    size_t resultSize = 0;
    shimExtractResult(&ret, resultBuf, sizeof(resultBuf), &resultSize);
    if (resultSize == 0) return {};
    return std::string(resultBuf, resultSize);
}

    // walkConsoleTree — in-order RB-tree traversal
    /*static*/ void ConsoleBridge::walkConsoleTree(uint8_t* node, uint8_t* root,
        std::vector<const ConsoleMethod*>& out,
        int depth)
    {
        // Guard against cycles, null, and runaway recursion
        if (!node || node == root || depth > 64) return;
        if (!isReadable(node)) return;

        // Read left / right children
        uint64_t leftVal = 0, rightVal = 0;
        if (!safeRead64(node + 0x00, &leftVal)) return;
        if (!safeRead64(node + 0x08, &rightVal)) return;

        uint8_t* left = reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(leftVal));
        uint8_t* right = reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(rightVal));

        // In-order: visit left subtree first
        if (left && left != root && isReadable(left))
            walkConsoleTree(left, root, out, depth + 1);

        // Read the ConsoleMethod array pointer at node+0x28
        uint64_t arrayPtr = 0;
        if (!safeRead64(node + 0x28, &arrayPtr) || !arrayPtr) {
            // Try node+0x20 as fallback (some variants store ptr there)
            safeRead64(node + 0x20, &arrayPtr);
        }

        if (arrayPtr && isReadable(reinterpret_cast<void*>(static_cast<uintptr_t>(arrayPtr)))) {
            uint8_t* arr = reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(arrayPtr));

            // Read up to 256 entries (0x20 bytes each); stop on invalid pfn/name
            for (int i = 0; i < 256; ++i) {
                uint8_t* entry = arr + i * 0x20;
                if (!isReadable(entry)) break;

                uint64_t pfn = 0, namePtr = 0;
                if (!safeRead64(entry + 0x00, &pfn))   break;
                if (!safeRead64(entry + 0x08, &namePtr)) break;

                // pfn must be executable, name must look like a string
                if (!pfn || !isExecutable(reinterpret_cast<void*>(static_cast<uintptr_t>(pfn))))
                    break;
                if (!looksLikeString(namePtr))
                    break;

                out.push_back(reinterpret_cast<const ConsoleMethod*>(entry));
            }
        }

        // In-order: visit right subtree
        if (right && right != root && isReadable(right))
            walkConsoleTree(right, root, out, depth + 1);
    }

    // getMethods
    const ConsoleMethod* const* ConsoleBridge::getMethods(int& outCount)
    {
        outCount = 0;

        // Unbound tree path
        // When the resolver detected a RB-tree instead of a fixed_vector,
        // walk the tree and return a pointer into the flat cache
        // This path does NOT touch m_instanceMethodsVecAddr — it is completely
        // separate from the fixed_vector logic used by all other games
        if (m_consoleMethodsIsTree && m_consoleMethodsVecAddr) {
            m_treeMethodCache.clear();

            uint8_t* root = reinterpret_cast<uint8_t*>(m_consoleMethodsVecAddr);
            if (!isReadable(root)) {
                FC_Log("getMethods(tree): root not readable");
                return nullptr;
            }

            // The tree header node's left child is the actual first node
            // If left == root the tree is empty
            uint64_t firstVal = 0;
            if (!safeRead64(root, &firstVal) || !firstVal) {
                FC_Log("getMethods(tree): could not read first node ptr");
                return nullptr;
            }

            uint8_t* first = reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(firstVal));
            if (first == root) {
                FC_Log("getMethods(tree): tree is empty");
                return nullptr;
            }

            walkConsoleTree(first, root, m_treeMethodCache);

            FC_Log("getMethods(tree): walked %zu methods from RB-tree at %p",
                m_treeMethodCache.size(), root);

            if (m_treeMethodCache.empty()) return nullptr;
            outCount = static_cast<int>(m_treeMethodCache.size());
            return m_treeMethodCache.data();
        }

        // Standard fixed_vector path (all other games)
        // Some titles leave s_consoleMethods empty and register
        // all commands via s_instanceMethods instead. Fall back to it when
        // consoleMethods is absent or empty
        void* vecAddr = m_consoleMethodsVecAddr;
        if (vecAddr) {
            uint64_t vpB = 0, vpE = 0;
            safeRead64(vecAddr, &vpB);
            safeRead64(reinterpret_cast<uint8_t*>(vecAddr) + 8, &vpE);
            if (vpB == 0 || vpE == vpB) {
                FC_Log("getMethods: s_consoleMethods empty, trying s_instanceMethods");
                vecAddr = m_instanceMethodsVecAddr;
            }
        }
        if (!vecAddr) {
            vecAddr = m_instanceMethodsVecAddr;
        }
        if (!vecAddr) {
            FC_Log("getMethods: s_consoleMethods not resolved");
            return nullptr;
        }

        uint8_t* vec = reinterpret_cast<uint8_t*>(vecAddr);

        // Probe all offsets 0..0x38 to find the valid begin/end pair
        uint64_t candidates[8] = {};
        for (int i = 0; i < 8; ++i)
            safeRead64(vec + i * 8, &candidates[i]);

        if (!m_loggedGetMethodsResolution)
            FC_Log("getMethods: vec=%p +00=%p +08=%p +10=%p +18=%p",
                vec, (void*)candidates[0], (void*)candidates[1],
                (void*)candidates[2], (void*)candidates[3]);

        uint64_t begin = 0, end_ = 0;
        for (int i = 0; i < 7 && begin == 0; ++i) {
            for (int sw = 0; sw < 2; ++sw) {
                uint64_t b = sw ? candidates[i + 1] : candidates[i];
                uint64_t e = sw ? candidates[i] : candidates[i + 1];
                if (b == 0 || e == 0 || e < b) continue;
                uint64_t diff = e - b;
                if (diff == 0 || diff > 512 * 1024 || diff % 8 != 0) continue;
                if (!isReadable(reinterpret_cast<void*>(static_cast<uintptr_t>(b)))) continue;
                begin = b; end_ = e;
                if (!m_loggedGetMethodsResolution)
                    FC_Log("getMethods: begin=%p end=%p", (void*)b, (void*)e);
                break;
            }
        }

        if (begin && end_)
            m_loggedGetMethodsResolution = true;

        if (!begin || !end_) {
            FC_Log("getMethods: could not find valid begin/end");
            return nullptr;
        }

        size_t count = (end_ - begin) / 8;
        if (count == 0 || count > 65536) return nullptr;

        outCount = static_cast<int>(count);
        return reinterpret_cast<const ConsoleMethod* const*>(
            static_cast<uintptr_t>(begin));
    }

    const void* ConsoleBridge::getInstanceMethodsBase(int& outCount, int& outStride)
    {
        outCount = 0;
        outStride = 0x20;
        if (!m_instanceMethodsVecAddr) return nullptr;

        uint64_t begin = 0, end_ = 0;
        if (!safeRead64(m_instanceMethodsVecAddr, &begin)) return nullptr;
        if (!safeRead64(reinterpret_cast<uint8_t*>(m_instanceMethodsVecAddr) + 8, &end_)) return nullptr;

        if (!begin || end_ < begin) return nullptr;
        size_t count = (end_ - begin) / 0x20;
        if (!count || count > 65536) return nullptr;

        outCount = static_cast<int>(count);
        return reinterpret_cast<const void*>(static_cast<uintptr_t>(begin));
    }

    // addOutputHandler / removeOutputHandler (4-arg)
        //
        // Direct-insertion fallback buffer size. s_outputHandlers on titles that
        // never pre-reserve capacity (mpBegin==mpEnd==mpCap==0 at registration
        // time -- confirmed via x64dbg on Veilguard) needs real backing storage,
        // not just an mpEnd bump, since there is no spare room to write into.
        // 64 slots is far more than any title needs (a handful of overlay/proxy
        // handlers at most) and this buffer is allocated once and kept for the
        // life of the process, so the size cost is negligible
    static constexpr size_t kDirectInsertCapacitySlots = 64;

    void ConsoleBridge::addOutputHandler(OutputHandlerFn fn)
    {
        if (!fn) return;
        FC_Log("addOutputHandler: fn=%p", (void*)fn);

        if (m_addOutputHandler) {
            // Preferred path: use the game's own addOutputHandler function
            HandlerDelegate delegate;
            delegate.m_pThis = nullptr;
            delegate.m_pFunction = reinterpret_cast<void*>(fn);
            m_addOutputHandler(&delegate);
            FC_Log("addOutputHandler: registered via game function OK");
            return;
        }

        // Fallback: direct vector insertion
        if (!m_outputHandlersVecAddr) {
            FC_Log("addOutputHandler: game function not resolved and no vec addr");
            return;
        }
        FC_Log("addOutputHandler: game fn not resolved, using direct insertion (stride=%zu fnOff=%zu)",
            m_delegateStride, m_delegateFnOffset);
        void** vec = reinterpret_cast<void**>(m_outputHandlersVecAddr);
        uint8_t* mpBegin = reinterpret_cast<uint8_t*>(vec[0]);
        uint8_t* mpEnd = reinterpret_cast<uint8_t*>(vec[1]);
        uint8_t* mpCap = reinterpret_cast<uint8_t*>(vec[2]);

        // Dedup: our own registration can be called more than once for the
        // same fn pointer (e.g. the pipe-side handler re-registering on every
        // host reconnect in dxgi.cpp, or the overlay registering separately
        // from the pipe handler). Since the game's dispatch loop calls every
        // slot unconditionally, an unguarded duplicate insert here causes
        // every console command's output to fire that handler twice. Scan
        // existing slots first and bail out if this exact fn is already
        // registered -- mirrors the "Same handler added multiple times."
        // guard the real addOutputHandler() enforces on titles that have one.
        if (mpBegin && mpEnd && mpEnd > mpBegin) {
            size_t existingCount = static_cast<size_t>(mpEnd - mpBegin) / m_delegateStride;
            for (size_t i = 0; i < existingCount; ++i) {
                uint8_t* slot = mpBegin + i * m_delegateStride;
                void* slotFn = *reinterpret_cast<void**>(slot + m_delegateFnOffset);
                if (slotFn == reinterpret_cast<void*>(fn)) {
                    FC_Log("addOutputHandler: fn=%p already registered at slot %zu, skipping duplicate",
                        (void*)fn, i);
                    return;
                }
            }
        }

        if (!mpEnd || mpEnd + m_delegateStride > mpCap) {
            size_t existingCount = 0;
            if (mpBegin && mpEnd && mpEnd > mpBegin)
                existingCount = static_cast<size_t>(mpEnd - mpBegin) / m_delegateStride;

            size_t newCapacityBytes = kDirectInsertCapacitySlots * m_delegateStride;
            uint8_t* newBuf = reinterpret_cast<uint8_t*>(
                VirtualAlloc(nullptr, newCapacityBytes, MEM_COMMIT | MEM_RESERVE,
                    PAGE_READWRITE));
            if (!newBuf) {
                FC_Log("addOutputHandler: VirtualAlloc failed, cannot grow vector");
                return;
            }
            memset(newBuf, 0, newCapacityBytes);

            if (existingCount > 0)
                memcpy(newBuf, mpBegin, existingCount * m_delegateStride);

            vec[0] = newBuf;
            vec[1] = newBuf + existingCount * m_delegateStride;
            vec[2] = newBuf + newCapacityBytes;

            FC_Log("addOutputHandler: grew vector -- new backing=%p capacity=%zu slots "
                "(copied %zu existing entries, old begin=%p)",
                (void*)newBuf, kDirectInsertCapacitySlots, existingCount, (void*)mpBegin);

            mpEnd = reinterpret_cast<uint8_t*>(vec[1]);
            mpCap = reinterpret_cast<uint8_t*>(vec[2]);
        }

        memset(mpEnd, 0, m_delegateStride);
        *reinterpret_cast<void**>(mpEnd + m_delegateFnOffset) = reinterpret_cast<void*>(fn);
        vec[1] = mpEnd + m_delegateStride;
        FC_Log("addOutputHandler: inserted at %p, new mpEnd=%p", (void*)mpEnd, (void*)vec[1]);
    }

    void ConsoleBridge::removeOutputHandler(OutputHandlerFn fn)
    {
        if (!fn || !m_outputHandlersVecAddr) return;

        void** vec = reinterpret_cast<void**>(m_outputHandlersVecAddr);
        uint8_t* mpBegin = reinterpret_cast<uint8_t*>(vec[0]);
        uint8_t* mpEnd = reinterpret_cast<uint8_t*>(vec[1]);

        if (!mpBegin || mpEnd <= mpBegin) return;

        const size_t kStride = m_delegateStride;
        const size_t kFnOff = m_delegateFnOffset;
        const size_t count = static_cast<size_t>(mpEnd - mpBegin) / kStride;

        for (size_t i = 0; i < count; ++i) {
            uint8_t* slot = mpBegin + i * kStride;
            void* slotFn = *reinterpret_cast<void**>(slot + kFnOff);
            if (slotFn != reinterpret_cast<void*>(fn)) continue;
            size_t remaining = count - i - 1;
            if (remaining > 0)
                memmove(slot, slot + kStride, remaining * kStride);
            vec[1] = mpEnd - kStride;
            FC_Log("removeOutputHandler: removed slot %zu", i);
            return;
        }
        FC_Log("removeOutputHandler: fn not found");
    }

    // addOutputHandler3 / removeOutputHandler3  (BF2 writeConsole hook)
    //
    // BF2 has no standalone addOutputHandler.  We patch writeConsoleFunc
    // (0x1454CCF70) directly with a 15-byte detour:
    //   FF 25 00 00 00 00   JMP [RIP+0]  — absolute indirect jump
    //   <8-byte address>
    //   90                  NOP (pad to 15 bytes)
    //
    // A trampoline in executable VirtualAlloc'd memory holds the displaced
    // original 15 bytes followed by a JMP back to site+15, so the original
    // function body continues to execute via the trampoline

    static uint8_t  s_bf2OrigBytes[15] = {};
    static uint8_t* s_bf2HookSite = nullptr;
    static OutputHandlerFn3 s_bf2UserFn3 = nullptr;

    // Trampoline stub — allocated executable memory, calls original then user fn
    // Layout:
    //   [0..14]  = saved original 15 bytes
    //   [15..28] = JMP back to hookSite+15  (FF 25 00 00 00 00 + 8-byte addr)
    static uint8_t* s_bf2Trampoline = nullptr;

    // The detour function — replaces writeConsole.
    // Called with original writeConsole ABI: rcx=tag, rdx=buf, r8d=size
    static void __fastcall bf2WriteConsoleDetour(const char* tag,
        const char* buf,
        unsigned int size)
    {
        // Call original via trampoline (displaced bytes + jmp back)
        if (s_bf2Trampoline) {
            using Fn = void(__fastcall*)(const char*, const char*, unsigned int);
            reinterpret_cast<Fn>(s_bf2Trampoline)(tag, buf, size);
        }
        // Call our handler
        if (s_bf2UserFn3)
            s_bf2UserFn3(nullptr, tag, buf, size);
    }

    void ConsoleBridge::addOutputHandler3(OutputHandlerFn3 fn)
    {
        if (!fn) return;
        FC_Log("addOutputHandler3(hook): fn=%p", (void*)fn);

        if (!m_writeConsoleFunc) {
            FC_Log("addOutputHandler3(hook): writeConsoleFunc not set");
            return;
        }

        s_bf2UserFn3 = fn;

        if (s_bf2HookSite) {
            FC_Log("addOutputHandler3(hook): already hooked, updated user fn");
            return;
        }

        uint8_t* site = reinterpret_cast<uint8_t*>(m_writeConsoleFunc);

        // Allocate trampoline: 15 saved bytes + 14 byte abs jmp back
        s_bf2Trampoline = reinterpret_cast<uint8_t*>(
            VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE,
                PAGE_EXECUTE_READWRITE));
        if (!s_bf2Trampoline) {
            FC_Log("addOutputHandler3(hook): VirtualAlloc failed");
            return;
        }

        // The first 3 instructions at writeConsoleFunc are [rsp+N] stores —
        // no RIP-relative operands — safe to copy verbatim to any address
        //   48 89 5C 24 08   mov [rsp+08], rbx   5 bytes
        //   48 89 6C 24 10   mov [rsp+10], rbp   5 bytes
        //   48 89 74 24 18   mov [rsp+18], rsi   5 bytes
        // Total = 15 bytes
        static const int kPatchLen = 15;

        // Save original bytes
        memcpy(s_bf2OrigBytes, site, kPatchLen);

        // Build trampoline: saved bytes + abs jmp back to site+kPatchLen
        memcpy(s_bf2Trampoline, s_bf2OrigBytes, kPatchLen);
        uint8_t* resumeAddr = site + kPatchLen;
        s_bf2Trampoline[kPatchLen + 0] = 0xFF;
        s_bf2Trampoline[kPatchLen + 1] = 0x25;
        *reinterpret_cast<uint32_t*>(s_bf2Trampoline + kPatchLen + 2) = 0;
        *reinterpret_cast<uint64_t*>(s_bf2Trampoline + kPatchLen + 6) =
            reinterpret_cast<uint64_t>(resumeAddr);

        // Build detour patch: FF 25 00 00 00 00 <abs addr> NOP — 15 bytes
        uint8_t patch[15];
        patch[0] = 0xFF; patch[1] = 0x25;
        *reinterpret_cast<uint32_t*>(patch + 2) = 0;
        *reinterpret_cast<uint64_t*>(patch + 6) =
            reinterpret_cast<uint64_t>(&bf2WriteConsoleDetour);
        patch[14] = 0x90; // NOP

        // Write patch
        DWORD oldProt = 0;
        VirtualProtect(site, kPatchLen, PAGE_EXECUTE_READWRITE, &oldProt);
        memcpy(site, patch, kPatchLen);
        VirtualProtect(site, kPatchLen, oldProt, &oldProt);
        FlushInstructionCache(GetCurrentProcess(), site, kPatchLen);

        s_bf2HookSite = site;
        FC_Log("addOutputHandler3(hook): patched site=%p trampoline=%p detour=%p",
            (void*)site, (void*)s_bf2Trampoline, (void*)&bf2WriteConsoleDetour);
    }

    void ConsoleBridge::removeOutputHandler3(OutputHandlerFn3 fn)
    {
        if (!s_bf2HookSite || !s_bf2Trampoline) return;

        DWORD oldProt = 0;
        static const int kPatchLen = 15;
        VirtualProtect(s_bf2HookSite, kPatchLen, PAGE_EXECUTE_READWRITE, &oldProt);
        memcpy(s_bf2HookSite, s_bf2OrigBytes, kPatchLen);
        VirtualProtect(s_bf2HookSite, kPatchLen, oldProt, &oldProt);
        FlushInstructionCache(GetCurrentProcess(), s_bf2HookSite, kPatchLen);

        VirtualFree(s_bf2Trampoline, 0, MEM_RELEASE);
        s_bf2Trampoline = nullptr;
        s_bf2HookSite = nullptr;
        s_bf2UserFn3 = nullptr;
        FC_Log("removeOutputHandler3(hook): restored original bytes");
    }

    // Settings manager helpers
    bool ConsoleBridge::settingsGet(const char* varName, char* outBuf, size_t outLen)
    {
        if (!m_settingsGet || !m_settingsManagerAddr || !varName || !outBuf) return false;
        uint64_t smPtr = 0;
        if (!safeRead64(m_settingsManagerAddr, &smPtr) || !smPtr) return false;

        // outBuf is passed as an eastl::string* — we zero it with SSO sentinel
        EastlString result{};
        memset(&result, 0, sizeof(result));
        result.raw[0x0F] = 15;

        __try {
            return m_settingsGet(smPtr, reinterpret_cast<uint64_t>(varName),
                reinterpret_cast<uint64_t>(&result)) != 0;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    bool ConsoleBridge::settingsSet(const char* varName, const char* value)
    {
        if (!m_settingsSet || !m_settingsManagerAddr || !varName || !value) return false;
        uint64_t smPtr = 0;
        if (!safeRead64(m_settingsManagerAddr, &smPtr) || !smPtr) return false;

        __try {
            return m_settingsSet(smPtr, reinterpret_cast<uint64_t>(varName),
                reinterpret_cast<uint64_t>(value)) != 0;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

}