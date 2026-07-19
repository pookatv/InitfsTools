#pragma once
// FrostbiteConsole.h
// ─────────────────────────────────────────────────────────────────────────────
// Runtime bridge to the Frostbite Console / ConsoleRegistry APIs.
//
// Resolution strategy (in priority order):
//   1. tryResolveDynamic() — pure pattern/string scanning, no hardcoded addresses
//   2. tryResolveByRVAs()  — hardcoded VA table fallback (k_SkateRVAs / k_BF2RVAs)
//
// ARCHITECTURE NOTES (x64 MSVC, confirmed from source + IDA on Skate)
// ─────────────────────────────────────────────────────────────────────────────
// ConsoleMethod (Console.h):
//   +0x00  pfn         (void*)         8 bytes
//   +0x08  name        (const char*)   8 bytes
//   +0x10  groupName   (const char*)   8 bytes  (may be null)
//   +0x18  description (const char*)   8 bytes  (may be null)
//   total = 0x20 bytes
//
// s_consoleMethods = fixed_vector<const ConsoleMethod*, 8048>
//   Stores POINTERS to ConsoleMethod, not the structs inline.
//   Layout: mpBegin(+0), mpEnd(+8), mpCapacity(+16), inline buffer(+24)
//   Each element is 8 bytes (pointer).
//   Stride through elements: 8 bytes per slot.
//   Dereference each slot to get ConsoleMethod*.
//
// s_outputHandlers = eastl::vector<FastDelegate<void(const char*,const char*,uint)>>
//   FastDelegate layout:
//     Skate: { void* m_pThis(+0); void* m_pFunction(+8) }  = 16 bytes total
//     BF2:   stride=0x28 bytes; fn ptr at slot+0x20, this ptr at slot+0x18
//
// executeConsoleCommand:
//   x64 MSVC RVO: hidden return ptr in RCX, cmdString in RDX, force in R8
//   Returns eastl::string (layout differs per game — see EastlString below)
//
// eastl::string layouts:
//   SSO layout (Skate):
//     +0x00  char[16]  SSO inline buffer (or heap ptr stored at +0x00 when heap)
//     +0x0F  int8_t    SSO sentinel: >= 0 means SSO active; bit7 set = heap
//     +0x10  uint64_t  size (character count, excluding null)
//     total = 24 bytes
//
//   3-pointer layout (BF2, confirmed by x64dbg):
//     +0x00  char*     mpBegin   (data pointer)
//     +0x08  char*     mpEnd     (one past last char)
//     +0x10  char*     mpCapacity
//     +0x18  void*     allocator
//     total = 32 bytes; size = mpEnd - mpBegin; no SSO
//
// g_settingsManager:
//   Global pointer (uint8_t*) to a 336-byte SettingsManager::Impl object.
//   Map header is at smObj + 0x90 (offset 144):
//     map + 0x08 = bucket array pointer
//     map + 0x10 = bucket count (uint32)
//     map + 0x14 = element count (uint32)
//   Each map node is 120 bytes:
//     +0x00  const char*  key
//     +0x08  value data (typed)
//     +0x70  next node pointer (bucket chain)
// ─────────────────────────────────────────────────────────────────────────────

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <psapi.h>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#pragma comment(lib, "psapi.lib")

namespace FrostbiteConsole {

    // ─────────────────────────────────────────────────────────────────────────
    // IngameConsoleInfo — result of probing for the in-game overlay console.
    //
    // Detection chain (cross-game):
    //   1. Scan the game module for "> " (3E 20 00). Present = real
    //      IngameConsoleImpl was compiled in; absent = dummy stub (NFS Rivals).
    //   2. If present: find the LEA instruction referencing that string.
    //      That instruction is inside IngameConsoleImpl::update. Walk up to
    //      the function prologue, hook it, capture rcx = IngameConsoleImpl*.
    //   3. Write 0x01 to IngameConsoleImpl* + 0x178 to show the overlay.
    //   4. Find the outer caller's rcx (outer singleton, r14 in SWBF2):
    //      write 0x01 to outerObject + 0x2638 to enable the ~ key gate.
    //
    // Confirmed offsets (x64 PC builds, SWBF2):
    //   m_enabled byte   = IngameConsoleImpl* + 0x178
    //   ~ key gate byte  = outerObject       + 0x2638
    // ─────────────────────────────────────────────────────────────────────────
    struct IngameConsoleInfo
    {
        bool     hasRealImpl    = false;  // "> " string found in module
        bool     hooked         = false;  // update function hook is installed
        uint8_t* implPtr        = nullptr; // live IngameConsoleImpl* (from rcx at hook)
        uint8_t* outerObjPtr    = nullptr; // live outer singleton (from caller's rcx)

        // Confirmed static offsets
        static constexpr uint32_t kEnabledOffset  = 0x178;   // m_enabled byte
        static constexpr uint32_t kGateOffset      = 0x2638;  // ~ key gate byte in outer obj

        bool canShow()  const { return hooked && implPtr    != nullptr; }
        bool canGate()  const { return hooked && outerObjPtr != nullptr; }
    };

    // ─────────────────────────────────────────────────────────────────────────
    // Structures
    // ─────────────────────────────────────────────────────────────────────────

    // fb::ConsoleMethod — matches Console.h exactly (x64 layout)
    struct ConsoleMethod {
        void* pfn;         // +0x00
        const char* name;        // +0x08
        const char* groupName;   // +0x10  (may be null)
        const char* description; // +0x18  (may be null)
    };
    static_assert(sizeof(ConsoleMethod) == 0x20, "ConsoleMethod size mismatch");

    // FastDelegate memento — 16 bytes (Skate layout).
    // For a free/static fn: m_pThis = nullptr, m_pFunction = fn ptr.
    struct HandlerDelegate {
        void* m_pThis = nullptr;      // +0x00
        void* m_pFunction = nullptr;  // +0x08
    };
    static_assert(sizeof(HandlerDelegate) == 16, "HandlerDelegate size mismatch");

    // eastl::string — two layouts depending on game build.
    //
    // SSO layout (Skate):
    //   +0x00  char[16]   inline SSO buffer (or heap ptr when bit7 of sentinel set)
    //   +0x0F  int8_t     sentinel: bit7 set = heap, ptr at raw[0]; clear = SSO inline
    //   +0x10  uint64_t   size (char count, excl. null)
    //   total = 24 bytes
    //
    // 3-pointer layout (BF2, confirmed by x64dbg at ret):
    //   +0x00  char*      mpBegin   (data pointer)
    //   +0x08  char*      mpEnd     (one past last char)
    //   +0x10  char*      mpCapacity
    //   +0x18  void*      allocator
    //   total = 32 bytes; size = mpEnd - mpBegin; no SSO
    //
    // The buffer is allocated at 256 bytes so either layout fits safely.
    struct EastlString {
        uint8_t raw[256];

        // Set by executeCommand() before calling the game fn.
        bool isBF2Layout = false;

        const char* c_str() const {
            if (isBF2Layout) {
                const char* ptr = *reinterpret_cast<const char* const*>(raw);
                if (!ptr || reinterpret_cast<uintptr_t>(ptr) < 0x10000ULL) return "";
                return ptr;
            }
            // SSO layout
            if (raw[0x0F] & 0x80)
                return *reinterpret_cast<const char* const*>(raw);
            return reinterpret_cast<const char*>(raw);
        }

        size_t size() const {
            if (isBF2Layout) {
                const char* begin = *reinterpret_cast<const char* const*>(raw);
                const char* end = *reinterpret_cast<const char* const*>(raw + 0x08);
                if (!begin || !end || end < begin) return 0;
                return static_cast<size_t>(end - begin);
            }
            return *reinterpret_cast<const uint64_t*>(raw + 0x10);
        }

        bool empty() const { return size() == 0; }
    };

    // ─────────────────────────────────────────────────────────────────────────
    // Function pointer typedefs
    // ─────────────────────────────────────────────────────────────────────────

    // RVO: hidden return ptr in RCX, cmd in RDX, force (bool) in R8
    using ExecuteConsoleCmdFn = void(__fastcall*)(EastlString* ret,
        const char* cmd,
        bool         force);

    // Console::addOutputHandler(HandlerDelegate* delegate)
    using AddOutputHandlerFn = void(__fastcall*)(HandlerDelegate* delegate);
    using RemoveOutputHandlerFn = void(__fastcall*)(HandlerDelegate* delegate);

    // SettingsManager get/set — calling convention inferred from IDA:
    //   RCX = smPtr (the object, NOT the global pointer)
    //   RDX = varName  (const char*, via reinterpret_cast<uint64_t>)
    //   R8  = outBuf / value  (EastlString* or const char*)
    // Returns non-zero on success.
    using SettingsGetFn = int(__fastcall*)(uint64_t smPtr,
        uint64_t varName,
        uint64_t outResult);
    using SettingsSetFn = int(__fastcall*)(uint64_t smPtr,
        uint64_t varName,
        uint64_t value);

    // ─────────────────────────────────────────────────────────────────────────
    // Output handler callback typedefs
    // ─────────────────────────────────────────────────────────────────────────

    // 4-arg form (Skate): FastDelegate passes m_pThis as RCX (discarded),
    // then tag=RDX, buf=R8, size=R9.
    using OutputHandlerFn = void(__fastcall*)(void* thisDiscarded,
        const char* tag,
        const char* buf,
        unsigned int size);

    // BF2 handler: same 4-arg form as Skate — (slot[0], tag, buf, size).
        // slot[0] is the object ptr stored in the delegate, passed as RCX (ignored).
    using OutputHandlerFn3 = void(__fastcall*)(void* thisIgnored,
        const char* tag,
        const char* buf,
        unsigned int size);

    // ─────────────────────────────────────────────────────────────────────────
    // ConsoleBridge
    // ─────────────────────────────────────────────────────────────────────────
    class ConsoleBridge
    {
    public:
        static ConsoleBridge& instance();

        /// Initialise the bridge.
        /// Tries dynamic resolution (pattern scanning) first, then falls back
        /// to the hardcoded RVA table matching the running game.
        /// targetModule: name of the exe to scan (e.g. "skate.crack.exe").
        ///               Pass nullptr to scan the first loaded module.
        bool init(const char* targetModule = nullptr);

        bool isReady()    const { return m_execCmd != nullptr || m_enqueueCmd != nullptr; }
        bool hasVecAddr() const { return m_outputHandlersVecAddr != nullptr; }

        /// true if the current game uses the 3-arg output handler form (BF2).
        bool use3ArgHandler() const { return m_use3ArgHandler; }

        /// Execute a Frostbite console command.  Returns the result string.
        std::string executeCommand(const char* cmd);

        /// Return the s_consoleMethods pointer array.
        /// outCount = number of valid ConsoleMethod* pointers.
        const ConsoleMethod* const* getMethods(int& outCount);

        /// Return the s_instanceMethods base pointer and element info.
        const void* getInstanceMethodsBase(int& outCount, int& outStride);

        /// Register a 4-arg output handler (Skate).
        /// Uses the game's addOutputHandler function (reliable for Skate).
        void addOutputHandler(OutputHandlerFn fn);
        void removeOutputHandler(OutputHandlerFn fn);

        /// Register a 3-arg output handler (BF2).
        /// Uses direct vector insertion with the BF2 delegate layout.
        void addOutputHandler3(OutputHandlerFn3 fn);
        void removeOutputHandler3(OutputHandlerFn3 fn);

        /// Query a settings variable by name.
        bool settingsGet(const char* varName, char* outBuf, size_t outLen);

        /// Set a settings variable by name.
        bool settingsSet(const char* varName, const char* value);

        std::string diagnosticInfo() const { return m_diagInfo; }

        void resetForReinit()
        {
            m_initDone = false;
            m_consoleMethodsVecAddr = nullptr;
            m_consoleMethodsFromStepA = false;
            m_consoleMethodsFromAltGetter = false;
            m_consoleMethodsIsTree = false;
            m_treeMethodCache.clear();
        }

        /// Expose resolved settings manager address and get function for DLLmain.
        uint64_t getSettingsManagerAddr() const
        {
            return reinterpret_cast<uint64_t>(m_settingsManagerAddr);
        }
        uint64_t getSettingsGet() const
        {
            return reinterpret_cast<uint64_t>(m_settingsGet);
        }

        // Safe read helpers — public so free-function wrappers and DLLmain can call them
        static bool safeRead64(void* addr, uint64_t* out);
        static bool safeRead32(void* addr, uint32_t* out);

        // BF Labs queue-based enqueue function type — exposed so shimEnqueueCmd
                // (a file-scope static) can reference it without friendship.
        using EnqueueCmdFn = void(__fastcall*)(const char* cmd, void* ctx, uint8_t addToLog);

    private:
        ConsoleBridge() = default;

        // ── Resolution helpers ────────────────────────────────────────────────
        bool tryResolveDynamic(uint8_t* modBase, size_t modSize);
        bool tryResolveDynamicBF2(uint8_t* modBase, size_t modSize);
        bool tryResolveDynamicFixedVector(uint8_t* modBase, size_t modSize);
        bool tryResolveDynamicBFLabs(uint8_t* modBase, size_t modSize);
        static uint8_t* decodeRIPRel(uint8_t* insn, int operandOffset);
        static bool     looksLikeString(uint64_t ptr, int maxLen = 256);
        static uint8_t* scanPattern(uint8_t* base, size_t size, const char* pattern);

        // ── Resolved addresses ────────────────────────────────────────────────
        ExecuteConsoleCmdFn   m_execCmd = nullptr;
        AddOutputHandlerFn    m_addOutputHandler = nullptr;
        RemoveOutputHandlerFn m_removeOutputHandler = nullptr;
        void* m_consoleMethodsVecAddr = nullptr;
        void* m_outputHandlersVecAddr = nullptr;
        void* m_instanceMethodsVecAddr = nullptr;
        EnqueueCmdFn m_enqueueCmd = nullptr;

        // Settings manager
        uint8_t* m_settingsManagerAddr = nullptr;
        SettingsGetFn m_settingsGet = nullptr;
        SettingsSetFn m_settingsSet = nullptr;

        // Per-game FastDelegate layout in s_outputHandlers
        size_t m_delegateStride = 16;   // default: Skate
        size_t m_delegateFnOffset = 8;    // default: Skate

        // Per-game eastl::string return layout
        bool m_bf2StringLayout = false;   // default: Skate SSO layout

        // Per-game output handler calling convention
        bool      m_use3ArgHandler = false;
        uintptr_t m_writeConsoleFunc = 0;

        std::string m_diagInfo;
        bool        m_initDone = false;
        bool        m_consoleMethodsFromStepA = false;
        bool        m_consoleMethodsFromAltGetter = false;  // set only by the alternate getter scan in tryResolveDynamicBF2

        // NFS Unbound uses a red-black tree for s_consoleMethods instead of
        // a fixed_vector.  When this flag is set, m_consoleMethodsVecAddr points
        // to the tree root and getMethods walks it via walkConsoleTree().
        bool        m_consoleMethodsIsTree = false;

        // Flat cache built by walking the tree once per getMethods call.
        // Cleared whenever the tree root changes.
        mutable std::vector<const ConsoleMethod*> m_treeMethodCache;

        // Walk the RB-tree at root, appending one ConsoleMethod* per entry.
        // Node layout (0x28 bytes, confirmed from NFS Unbound x64dbg session):
        //   +0x00  left ptr   (8 bytes)
        //   +0x08  right ptr  (8 bytes)
        //   +0x10  parent ptr (8 bytes)
        //   +0x18  count      (8 bytes, 0 or 1)
        //   +0x20  hash       (8 bytes)
        //   +0x28  ptr to ConsoleMethod array (heap, count entries of 0x20 bytes each)
        // Each ConsoleMethod entry (0x20 bytes):
        //   +0x00  pfn         (8 bytes)
        //   +0x08  name        (const char*, direct pointer)
        //   +0x10  groupName   (const char*, direct pointer)
        //   +0x18  description (const char*, direct pointer)
        static void walkConsoleTree(uint8_t* node, uint8_t* root,
            std::vector<const ConsoleMethod*>& out,
            int depth = 0);
    };

    // ─────────────────────────────────────────────────────────────────────────
    // Convenience free functions
    // ─────────────────────────────────────────────────────────────────────────
    inline bool        init(const char* t = nullptr) { return ConsoleBridge::instance().init(t); }
    inline bool        isReady() { return ConsoleBridge::instance().isReady(); }
    inline std::string executeCommand(const char* c) { return ConsoleBridge::instance().executeCommand(c); }
    inline const ConsoleMethod* const* getMethods(int& n) { return ConsoleBridge::instance().getMethods(n); }
    inline void addOutputHandler(OutputHandlerFn f) { ConsoleBridge::instance().addOutputHandler(f); }
    inline void removeOutputHandler(OutputHandlerFn f) { ConsoleBridge::instance().removeOutputHandler(f); }
    inline void addOutputHandler3(OutputHandlerFn3 f) { ConsoleBridge::instance().addOutputHandler3(f); }
    inline void removeOutputHandler3(OutputHandlerFn3 f) { ConsoleBridge::instance().removeOutputHandler3(f); }
    inline std::string diagnosticInfo() { return ConsoleBridge::instance().diagnosticInfo(); }
    inline bool settingsGet(const char* n, char* b, size_t l) { return ConsoleBridge::instance().settingsGet(n, b, l); }
    inline bool settingsSet(const char* n, const char* v) { return ConsoleBridge::instance().settingsSet(n, v); }
    inline void resetForReinit() { ConsoleBridge::instance().resetForReinit(); }

    // Exposed for DLLmain diagnostics
    bool safeRead64(void* addr, uint64_t* out);
    bool safeRead32(void* addr, uint32_t* out);

    // Set a callback that receives each [FC] log line in real time.
        // Pass nullptr to disable. The callback is called on the worker thread.
    void setLogCallback(void (*cb)(const char* line));
    std::string getLastLog();   // kept for ABI compat, always returns ""
    void        clearLastLog(); // no-op

} // namespace FrostbiteConsole