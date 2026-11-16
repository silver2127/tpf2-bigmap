// gigantomaniac.h -- one "Gigantomaniac" size row whose ratio dropdown lists the
// sizes (GOG 2024-12-12 build; the Steam build keeps one row per size).
//
// THE PROBLEM
// The New Game page builds the ratio dropdown ONCE, when the page opens
// (RatioBuilder, called from CreatePageNewGame at 0x66d14d): five items, labels
// "1:1".."1:5", items 3 and 4 enabled only with experimental map sizes. Nothing
// rebuilds it when the size changes, so a size row cannot simply "mean" other
// ratio labels. This module rebuilds the ratio dropdown's items itself whenever
// the size dropdown's selection moves onto or off the Gigantomaniac row.
//
// THE WIDGET (measured on the GOG TransportFever2.exe)
// The combo is a 0x4f0-byte widget, vtable 0x3873928:
//   +0x460 int                    selected index (-1 = none)
//   +0x468 vector<std::function>  items, 0x40 bytes each; the impl pointer of
//                                 each std::function is at +0x38
//   +0x480 vector<bool>           enabled flags (words at +0x480, bit count at +0x498)
//   +0x4a8 widget*                the box showing the current item
//   +0x4e0 callback*              onChange, fired by Select(.., notify=true)
// An item is std::function<Widget*()>: AddItem (0x226fcf0) wraps the caller's
// function in a heap 0x48-byte functor (vtable 0x38740a0) whose inner
// std::function holds the label functor { vtable 0x300e390, std::string } in
// its small buffer. Calling it builds a text widget from that string, so the
// label is read again every time the box or the popup is drawn.
//   Select     0x2270a70  (combo, index, notify) -- every user path (click,
//                         wheel, keys) goes through it
//   SetEnabled 0x2270a40  (combo, index, on)
//   ~Combo     0x226e540  the destructor body (the scalar deleting dtor at
//                         vtable[0] calls it)
//
// WHAT IS HOOKED
//   * RatioBuilder: remember the ratio combo it returns, snapshot its stock
//     items (labels, enabled flags), and bring it in line with the size combo.
//   * Select: after the size combo's selection changes, rebuild the ratio
//     items for the new mode -- the ladder's size labels on Gigantomaniac, the
//     stock ratios everywhere else.
//   * ~Combo: forget a combo that is being destroyed, so a stale pointer is
//     never dereferenced after the page closes.
// The size combo itself comes from the combo factory hook (ComboDetour).
// Every object is checked against the vtables above before it is touched; a
// mismatch switches the module off for the session with a log line.
#pragma once

static bool g_gigaOn   = true;     // cfg gigantomaniac
static bool g_gigaLive = false;    // all hooks in place
static char g_gigaLabel[16] = "Gigantomaniac";
static MsvcString g_gigaRowString;

static const uintptr_t GIGA_RVA_SELECT       = 0x2270a70;
static const uintptr_t GIGA_RVA_SET_ENABLED  = 0x2270a40;
static const uintptr_t GIGA_RVA_ADD_ITEM     = 0x226fcf0;
static const uintptr_t GIGA_RVA_DTOR_BODY    = 0x226e540;
static const uintptr_t GIGA_RVA_RATIO_BUILD  = 0x662b50;
static const uintptr_t GIGA_RVA_COMBO_VT     = 0x3873928;
static const uintptr_t GIGA_RVA_WRAP_VT      = 0x38740a0;
static const uintptr_t GIGA_RVA_LABEL_VT     = 0x300e390;
static const uintptr_t GIGA_RET_RATIO_BUILD  = 0x66d152;   // after the page's call
static const uint8_t   GIGA_CALL_RATIO_BUILD[5] = { 0xE8,0xFE,0x59,0xFF,0xFF };

//   push rdi; sub rsp,0x30; mov [rsp+0x20],-2
static const uint8_t GIGA_EXPECT_SELECT[15] = {
    0x40,0x57,0x48,0x83,0xEC,0x30,0x48,0xC7,0x44,0x24,0x20,0xFE,0xFF,0xFF,0xFF };
//   mov r9,[rcx+0x480]; movsxd rcx,edx; mov rax,rcx; and ecx,0x1f
static const uint8_t GIGA_EXPECT_SET_ENABLED[16] = {
    0x4C,0x8B,0x89,0x80,0x04,0x00,0x00,0x48,0x63,0xCA,0x48,0x8B,0xC1,0x83,0xE1,0x1F };
//   mov r11,rsp; mov [r11+0x10],rdx; push rbx; push rsi; push rdi; sub rsp,0xb0
static const uint8_t GIGA_EXPECT_ADD_ITEM[17] = {
    0x4C,0x8B,0xDC,0x49,0x89,0x53,0x10,0x53,0x56,0x57,0x48,0x81,0xEC,0xB0,0x00,0x00,0x00 };
//   mov [rsp+8],rcx; push rdi; sub rsp,0x90; mov [rsp+0x20],-2
static const uint8_t GIGA_EXPECT_DTOR_BODY[22] = {
    0x48,0x89,0x4C,0x24,0x08,0x57,0x48,0x81,0xEC,0x90,0x00,0x00,0x00,
    0x48,0xC7,0x44,0x24,0x20,0xFE,0xFF,0xFF,0xFF };
//   mov rax,rsp; push rbp; lea rbp,[rax-0x5f]; sub rsp,0xc0
static const uint8_t GIGA_EXPECT_RATIO_BUILD[15] = {
    0x48,0x8B,0xC4,0x55,0x48,0x8D,0x68,0xA1,0x48,0x81,0xEC,0xC0,0x00,0x00,0x00 };

typedef void  (__fastcall *GigaSelectFn)(void* combo, int index, bool notify);
typedef void  (__fastcall *GigaSetEnabledFn)(void* combo, int index, bool on);
typedef void  (__fastcall *GigaAddItemFn)(void* combo, void* function);
typedef void* (__fastcall *GigaDtorFn)(void* combo);
typedef void* (__fastcall *GigaRatioBuildFn)(void* a, void* b, void* c, void* d);
typedef void  (__fastcall *GigaDeleteThisFn)(void* impl, bool dealloc);

static GigaSelectFn     g_gigaSelectOrig = nullptr;
static GigaSetEnabledFn g_gigaSetEnabled = nullptr;
static GigaAddItemFn    g_gigaAddItem    = nullptr;
static GigaDtorFn       g_gigaDtorOrig   = nullptr;
static GigaRatioBuildFn g_gigaRatioOrig  = nullptr;

static void* g_sizeCombo  = nullptr;   // the New Game size dropdown, while it lives
static void* g_ratioCombo = nullptr;   // the New Game ratio dropdown, while it lives
static int   g_ratioMode  = -1;        // -1 unknown, 0 stock ratios, 1 Gigantomaniac sizes
static bool  g_gigaBroken = false;     // a check failed: leave every combo alone

// The stock ratio items, snapshotted from the first ratio combo of a page.
static const int GIGA_MAX_ITEMS = 32;
static MsvcString g_stockRatioLabels[GIGA_MAX_ITEMS];
static int        g_stockRatioCount   = 0;
static uint32_t   g_stockRatioEnabled = 0;

// The game's std::function<Widget*()> as AddItem takes it: the label functor
// sits in the small buffer and the impl pointer points at it.
struct GigaLabelFunction {
    void*      vtable;
    MsvcString label;
    uint8_t    pad[0x38 - 8 - sizeof(MsvcString)];
    void*      impl;
};
static_assert(sizeof(GigaLabelFunction) == 0x40, "MSVC x64 std::function is 0x40 bytes");

static uint8_t* GigaU8(void* p) { return static_cast<uint8_t*>(p); }
static int      GigaSel(void* c) { return *reinterpret_cast<int*>(GigaU8(c) + 0x460); }
static uint8_t* GigaItemsBegin(void* c) { return *reinterpret_cast<uint8_t**>(GigaU8(c) + 0x468); }
static uint8_t* GigaItemsEnd(void* c)   { return *reinterpret_cast<uint8_t**>(GigaU8(c) + 0x470); }
static int      GigaCount(void* c) { return (int)((GigaItemsEnd(c) - GigaItemsBegin(c)) / 0x40); }

static bool GigaIsCombo(void* c)
{
    return c && *reinterpret_cast<uintptr_t*>(c) == g_base + GIGA_RVA_COMBO_VT;
}

// The label std::string inside item i, or null when the item is not the
// game's own wrapped label functor.
static MsvcString* GigaItemLabel(void* c, int i)
{
    uint8_t* impl = *reinterpret_cast<uint8_t**>(GigaItemsBegin(c) + (size_t)i * 0x40 + 0x38);
    if (!impl) return nullptr;
    if (*reinterpret_cast<uintptr_t*>(impl) != g_base + GIGA_RVA_WRAP_VT) return nullptr;
    if (*reinterpret_cast<uintptr_t*>(impl + 8) != g_base + GIGA_RVA_LABEL_VT) return nullptr;
    if (*reinterpret_cast<uint8_t**>(impl + 0x40) != impl + 8) return nullptr;   // inner impl -> its buffer
    return reinterpret_cast<MsvcString*>(impl + 0x10);
}

static bool GigaValidRatioCombo(void* c)
{
    if (!GigaIsCombo(c)) return false;
    const int n = GigaCount(c);
    if (n < 1 || n > GIGA_MAX_ITEMS) return false;
    for (int i = 0; i < n; ++i)
        if (!GigaItemLabel(c, i)) return false;
    return true;
}

static void GigaFail(const char* why)
{
    if (!g_gigaBroken)
        H->log("gigantomaniac: %s -- the ratio dropdown is left alone for this session", why);
    g_gigaBroken = true;
    g_ratioCombo = nullptr;
}

static void GigaSso(MsvcString* out, const char* s)
{
    memset(out, 0, sizeof *out);
    size_t n = strlen(s);
    if (n > 15) n = 15;
    memcpy(out->buf, s, n);
    out->size = n;
    out->cap  = 15;
}

static void GigaSnapshotStock(void* c)
{
    const int n = GigaCount(c);
    g_stockRatioCount = n;
    g_stockRatioEnabled = 0;
    const uint32_t* words = *reinterpret_cast<uint32_t**>(GigaU8(c) + 0x480);
    const uint64_t bits = *reinterpret_cast<uint64_t*>(GigaU8(c) + 0x498);
    for (int i = 0; i < n; ++i) {
        const MsvcString* s = GigaItemLabel(c, i);
        if (s && s->cap <= 15 && s->size <= 15) {
            g_stockRatioLabels[i] = *s;
        } else {
            char t[16]; _snprintf_s(t, sizeof t, _TRUNCATE, "1:%d", i + 1);
            GigaSso(&g_stockRatioLabels[i], t);
        }
        if (words && (uint64_t)i < bits && (words[i >> 5] >> (i & 31)) & 1u)
            g_stockRatioEnabled |= 1u << i;
    }
}

// Destroy every item exactly as the game's own vector would, then empty the
// enabled flags. The words of the vector<bool> stay allocated; AddItem resizes
// them from the bit count.
static void GigaRemoveAll(void* c)
{
    uint8_t* b = GigaItemsBegin(c);
    uint8_t* e = GigaItemsEnd(c);
    for (uint8_t* it = b; it < e; it += 0x40) {
        void** slot = reinterpret_cast<void**>(it + 0x38);
        void* impl = *slot;
        if (!impl) continue;
        GigaDeleteThisFn del = reinterpret_cast<GigaDeleteThisFn*>(*reinterpret_cast<void***>(impl))[4];
        del(impl, impl != it);
        *slot = nullptr;
    }
    *reinterpret_cast<uint8_t**>(GigaU8(c) + 0x470) = b;
    *reinterpret_cast<uint64_t*>(GigaU8(c) + 0x498) = 0;
}

static void GigaAdd(void* c, const MsvcString& label)
{
    GigaLabelFunction f;
    memset(&f, 0, sizeof f);
    f.vtable = reinterpret_cast<void*>(g_base + GIGA_RVA_LABEL_VT);
    f.label  = label;   // SSO only (<= 15 chars): a plain copy owns nothing
    f.impl   = &f;
    g_gigaAddItem(c, &f);   // moves the functor into a heap wrapper, empties f
}

// Rebuild the ratio items for the given mode. Keeps the selected position when
// the new list has it and it is enabled, else the nearest enabled one below.
static void GigaApplyMode(int mode)
{
    void* c = g_ratioCombo;
    if (!c || g_gigaBroken || mode == g_ratioMode) return;
    if (!GigaValidRatioCombo(c)) { GigaFail("the ratio dropdown no longer looks like the game's"); return; }
    const int prev = GigaSel(c);
    const int n = mode ? g_numExtraSizeRows : g_stockRatioCount;
    if (n < 1) return;
    GigaRemoveAll(c);
    for (int i = 0; i < n; ++i) {
        if (mode) { MsvcString s; GigaSso(&s, g_extraSizeRows[i].label); GigaAdd(c, s); }
        else      GigaAdd(c, g_stockRatioLabels[i]);
    }
    uint32_t enabled = 0xFFFFFFFFu;
    if (!mode) {
        enabled = g_stockRatioEnabled;
        for (int i = 0; i < n; ++i) g_gigaSetEnabled(c, i, ((enabled >> i) & 1u) != 0);
    }
    int sel = prev < 0 ? 0 : prev;
    if (sel >= n) sel = n - 1;
    while (sel > 0 && !((enabled >> sel) & 1u)) --sel;
    *reinterpret_cast<int*>(GigaU8(c) + 0x460) = -1;   // makes Select rebuild the box
    g_gigaSelectOrig(c, sel, true);
    g_ratioMode = mode;
    if (g_logEvery)
        H->log("gigantomaniac: ratio dropdown -> %s (%d item(s), selected %d)",
               mode ? "sizes" : "stock ratios", n, sel);
}

static void GigaSyncFromSize()
{
    if (!g_sizeCombo || !g_ratioCombo) return;
    const int sel = GigaSel(g_sizeCombo);
    GigaApplyMode(sel >= 0 && sel == (int)g_stockSizeRows ? 1 : 0);
}

static void __fastcall GigaSelectDetour(void* c, int index, bool notify)
{
    g_gigaSelectOrig(c, index, notify);
    if (c && c == g_sizeCombo) GigaSyncFromSize();
}

static void* __fastcall GigaRatioBuildDetour(void* a, void* b, void* c, void* d)
{
    void* combo = g_gigaRatioOrig(a, b, c, d);
    if ((uintptr_t)_ReturnAddress() == g_base + GIGA_RET_RATIO_BUILD && !g_gigaBroken) {
        if (!GigaValidRatioCombo(combo)) {
            GigaFail("the ratio dropdown the page built is not the expected widget");
        } else {
            if (g_stockRatioCount == 0) GigaSnapshotStock(combo);
            g_ratioCombo = combo;
            g_ratioMode  = 0;
            GigaSyncFromSize();
        }
    }
    return combo;
}

static void* __fastcall GigaDtorDetour(void* c)
{
    if (c == g_sizeCombo)  g_sizeCombo = nullptr;
    if (c == g_ratioCombo) { g_ratioCombo = nullptr; g_ratioMode = -1; }
    return g_gigaDtorOrig(c);
}

// Called by ComboDetour once the factory returns the size combo.
static void GigaNoteSizeCombo(void* combo)
{
    if (!g_gigaLive) return;
    g_ratioCombo = nullptr;   // a new page: its ratio combo is built next
    g_ratioMode  = -1;
    g_sizeCombo  = GigaIsCombo(combo) ? combo : nullptr;
    if (!g_sizeCombo) GigaFail("the size dropdown is not the expected widget");
}

// GOG only. Returns true when every site verifies and every hook is in.
static bool InstallGigantomaniac()
{
    if (!g_gigaOn || !g_gog) return false;
    if (g_numExtraSizeRows < 1) return false;
    if (g_numExtraSizeRows > GIGA_MAX_ITEMS) g_numExtraSizeRows = GIGA_MAX_ITEMS;
    if (!H->verifyBytes(GIGA_RVA_SELECT, GIGA_EXPECT_SELECT, sizeof GIGA_EXPECT_SELECT)
        || !H->verifyBytes(GIGA_RVA_SET_ENABLED, GIGA_EXPECT_SET_ENABLED, sizeof GIGA_EXPECT_SET_ENABLED)
        || !H->verifyBytes(GIGA_RVA_ADD_ITEM, GIGA_EXPECT_ADD_ITEM, sizeof GIGA_EXPECT_ADD_ITEM)
        || !H->verifyBytes(GIGA_RVA_DTOR_BODY, GIGA_EXPECT_DTOR_BODY, sizeof GIGA_EXPECT_DTOR_BODY)
        || !H->verifyBytes(GIGA_RVA_RATIO_BUILD, GIGA_EXPECT_RATIO_BUILD, sizeof GIGA_EXPECT_RATIO_BUILD)
        || !H->verifyBytes(GIGA_RET_RATIO_BUILD - 5, GIGA_CALL_RATIO_BUILD, sizeof GIGA_CALL_RATIO_BUILD)) {
        H->log("gigantomaniac: the GOG dropdown code does not byte-verify -- one row per size instead");
        return false;
    }
    g_gigaSetEnabled = reinterpret_cast<GigaSetEnabledFn>(g_base + GIGA_RVA_SET_ENABLED);
    g_gigaAddItem    = reinterpret_cast<GigaAddItemFn>(g_base + GIGA_RVA_ADD_ITEM);
    void* t1 = nullptr; void* t2 = nullptr; void* t3 = nullptr;
    // The destructor first: once it is in, no later hook can leave a pointer
    // to a dead combo behind.
    if (!H->installHook(g_base + GIGA_RVA_DTOR_BODY, (void*)&GigaDtorDetour, sizeof GIGA_EXPECT_DTOR_BODY, &t1)) {
        H->log("gigantomaniac: installHook failed on the combo destructor -- one row per size instead");
        return false;
    }
    g_gigaDtorOrig = reinterpret_cast<GigaDtorFn>(t1);
    if (!H->installHook(g_base + GIGA_RVA_SELECT, (void*)&GigaSelectDetour, sizeof GIGA_EXPECT_SELECT, &t2)) {
        H->log("gigantomaniac: installHook failed on Select -- one row per size instead");
        return false;   // the dtor hook alone only clears pointers nobody set
    }
    g_gigaSelectOrig = reinterpret_cast<GigaSelectFn>(t2);
    if (!H->installHook(g_base + GIGA_RVA_RATIO_BUILD, (void*)&GigaRatioBuildDetour, sizeof GIGA_EXPECT_RATIO_BUILD, &t3)) {
        H->log("gigantomaniac: installHook failed on the ratio builder -- one row per size instead");
        return false;   // Select passes through: g_sizeCombo is only set when live
    }
    g_gigaRatioOrig = reinterpret_cast<GigaRatioBuildFn>(t3);
    GigaSso(&g_gigaRowString, g_gigaLabel);
    g_gigaLive = true;
    H->log("gigantomaniac: live -- one '%s' size row, its ratio dropdown lists %d size(s) (%s .. %s)",
           g_gigaLabel, g_numExtraSizeRows, g_extraSizeRows[0].label,
           g_extraSizeRows[g_numExtraSizeRows - 1].label);
    return true;
}
