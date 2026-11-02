/*
 * kernel_xbox.c - Xbox Identity & Hardware Stubs
 *
 * Provides Xbox-specific exported data (hardware info, kernel version,
 * keys, image filename) and section loading stubs.
 *
 * Most Xbox-specific hardware data is stubbed with plausible retail
 * values. Crypto keys are zeroed since we don't need Xbox Live or
 * EEPROM-based encryption on PC.
 */

#include "kernel.h"
#include "xbox_memory_layout.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern ptrdiff_t g_xbox_mem_offset;

/* The console's factory game region: 1 North America, 2 Japan, 4 the rest of
 * the world. A console only runs discs of its own region, so the honest answer
 * is the one the title's certificate allows (+0xA0 of the certificate, whose
 * address is at +0x118 of the XBE header mapped at 0x00010000). Several
 * allowed regions take the lowest bit, North America first, in line with the
 * NTSC-M answer for XC_FACTORY_AV_REGION. RECOMP_GAME_REGION overrides it.
 *
 * Unhandled, it came back 0, which no console has. HP: Chamber of Secrets
 * picks US English only when the region is 1, so its NTSC release played
 * with the PAL release's UK English. */
static ULONG title_game_region(void)
{
    const char *env = getenv("RECOMP_GAME_REGION");
    const BYTE *mem = (const BYTE *)g_xbox_mem_offset;
    ULONG cert, allowed = 0, region = 1;
    static int logged;

    if (env && *env) {
        region = (ULONG)strtoul(env, NULL, 0);
    } else {
        cert = *(const ULONG *)(mem + XBOX_BASE_ADDRESS + 0x118);
        if (cert >= XBOX_BASE_ADDRESS && cert < XBOX_BASE_ADDRESS + 0x10000)
            allowed = *(const ULONG *)(mem + cert + 0xA0) & 7;
        if (allowed)
            region = allowed & (0u - allowed);
    }
    if (!logged++)
        fprintf(stderr, "  [KERNEL] game region %u (%s, certificate allows 0x%X)\n",
                (unsigned)region, env && *env ? "RECOMP_GAME_REGION" : "certificate",
                (unsigned)allowed);
    return region;
}


/* ============================================================================
 * Exported Data Objects
 *
 * These are global variables exported by the Xbox kernel at known ordinals.
 * Game code accesses them directly via the thunk table.
 * ============================================================================ */

/* Hardware info - default report is a standard retail Xbox.
 * A target may override via xbox_kernel_set_version() or by patching the
 * global directly; no title-specific values are baked in. */
XBOX_HARDWARE_INFO xbox_HardwareInfo = {
    .Flags       = 0x00000020,  /* Retail Xbox */
    .GpuRevision = 0xD2,       /* NV2A D2 revision */
    .McpRevision = 0xD4,       /* MCPX D4 revision */
    .Reserved    = {0, 0}
};

/* Kernel version - default is a stock retail value. Titles built against a
 * different XDK can report their own through xbox_kernel_set_version(). */
XBOX_KRNL_VERSION xbox_KrnlVersion = {
    .Major = 1,
    .Minor = 0,
    .Build = 5849,
    .Qfe   = 1
};

/* Override the kernel version reported to game code (ordinal 324).
 * Match the XDK revision the target title was built with if it checks it. */
void xbox_kernel_set_version(USHORT major, USHORT minor, USHORT build, USHORT qfe)
{
    xbox_KrnlVersion.Major = major;
    xbox_KrnlVersion.Minor = minor;
    xbox_KrnlVersion.Build = build;
    xbox_KrnlVersion.Qfe   = qfe;
}

/* Crypto keys - zeroed, not needed for PC operation.
 * These are unique per-console on real hardware. */
UCHAR xbox_EEPROMKey[16]              = {0};
UCHAR xbox_HDKey[16]                  = {0};
UCHAR xbox_SignatureKey[16]           = {0};
UCHAR xbox_LANKey[16]                 = {0};
UCHAR xbox_AlternateSignatureKeys[16][16] = {{0}};

/* Public key data for Xbox Live signature verification - not needed */
UCHAR xbox_XePublicKeyData[284] = {0};

/* Image filename - the XBE path as seen by the kernel */
static char g_image_filename[] = "\\Device\\CdRom0\\default.xbe";
XBOX_ANSI_STRING xbox_XeImageFileName = {
    .Length        = sizeof(g_image_filename) - 1,
    .MaximumLength = sizeof(g_image_filename),
    .Buffer        = g_image_filename
};

/* Launch data page - used for title-to-title launches (e.g., Xbox Dashboard → game).
 * Allocated dynamically and zeroed for normal game boot. */
static XBOX_LAUNCH_DATA_PAGE g_launch_data_page = {0};
XBOX_LAUNCH_DATA_PAGE* xbox_LaunchDataPage = &g_launch_data_page;

/* ============================================================================
 * Section Loading
 *
 * XeLoadSection/XeUnloadSection manage on-demand loading of XBE sections.
 * On Xbox, some sections are demand-paged from disc. In our recompilation,
 * the entire executable is loaded into memory, so these are reference-counting
 * no-ops.
 * ============================================================================ */

NTSTATUS __stdcall xbox_XeLoadSection(PXBE_SECTION_HEADER Section)
{
    if (!Section)
        return STATUS_INVALID_PARAMETER;

    InterlockedIncrement(&Section->SectionReferenceCount);

    xbox_log(XBOX_LOG_DEBUG, XBOX_LOG_XBOX,
        "XeLoadSection: '%s' at %p (size=%u, refcount=%d)",
        Section->SectionName ? Section->SectionName : "<null>",
        Section->VirtualAddress, Section->VirtualSize,
        Section->SectionReferenceCount);

    return STATUS_SUCCESS;
}

NTSTATUS __stdcall xbox_XeUnloadSection(PXBE_SECTION_HEADER Section)
{
    if (!Section)
        return STATUS_INVALID_PARAMETER;

    LONG new_count = InterlockedDecrement(&Section->SectionReferenceCount);

    xbox_log(XBOX_LOG_DEBUG, XBOX_LOG_XBOX,
        "XeUnloadSection: '%s' (refcount=%d)",
        Section->SectionName ? Section->SectionName : "<null>",
        new_count);

    return STATUS_SUCCESS;
}

/* ============================================================================
 * EEPROM / Non-Volatile Settings
 *
 * ExQueryNonVolatileSetting / ExSaveNonVolatileSetting read and write EEPROM
 * settings. On real hardware these are stored in the 256-byte EEPROM on the
 * SMBus. For recompilation, we return sensible defaults:
 *   - Region: North America
 *   - Video: NTSC, widescreen+HDTV enabled
 *   - Language: English
 *   - Audio: Stereo, Dolby Digital
 *   - DVD Region: Region 1 (North America)
 * ============================================================================ */

/* xboxrecomp: the host's time zone in the console's terms (see XC_TIMEZONE_BIAS below); UTC where the host has no
 * Windows time zone API (the POSIX build) */
typedef struct {
    LONG bias, std_bias, dlt_bias;
    BYTE std_date[4], dlt_date[4];  /* month, week of the month (5 the last), day of the week, hour */
    BYTE std_name[4], dlt_name[4];
} HostTimeZone;

static void host_time_zone(HostTimeZone *z)
{
    memset(z, 0, sizeof(*z));
#ifdef _WIN32
    TIME_ZONE_INFORMATION tz;
    memset(&tz, 0, sizeof(tz));
    GetTimeZoneInformation(&tz);
    z->bias = tz.Bias;
    z->std_bias = tz.StandardBias;
    z->dlt_bias = tz.DaylightBias;
    {
        const SYSTEMTIME *d[2] = {&tz.StandardDate, &tz.DaylightDate};
        BYTE *o[2] = {z->std_date, z->dlt_date};
        for (int k = 0; k < 2; k++) {
            o[k][0] = (BYTE)d[k]->wMonth;  /* 0: no daylight saving in this zone */
            o[k][1] = (BYTE)d[k]->wDay;
            o[k][2] = (BYTE)d[k]->wDayOfWeek;
            o[k][3] = (BYTE)d[k]->wHour;
        }
    }
    for (int i = 0; i < 4 && tz.StandardName[i]; i++)  /* the first four letters of Windows' names, e.g. "GTB " */
        z->std_name[i] = tz.StandardName[i] < 0x80 ? (BYTE)tz.StandardName[i] : (BYTE)'?';
    for (int i = 0; i < 4 && tz.DaylightName[i]; i++)
        z->dlt_name[i] = tz.DaylightName[i] < 0x80 ? (BYTE)tz.DaylightName[i] : (BYTE)'?';
#endif
}

/* xboxrecomp: RECOMP_LANGUAGE (a game's launcher sets it from its settings): the dashboard's language by name or by
 * its XC_LANGUAGE number - 1 English, 2 Japanese, 3 German, 4 French, 5 Spanish, 6 Italian, 7 Korean, 8 Chinese,
 * 9 Portuguese. Unset or unknown: English. Read once; a title asks as it starts */
static ULONG console_language(void)
{
    static ULONG lang;
    if (!lang) {
        static const char *const names[] = {"english", "japanese", "german", "french", "spanish",
                                            "italian", "korean", "chinese", "portuguese"};
        const char *e = getenv("RECOMP_LANGUAGE");
        lang = 1;
        if (e && *e) {
            char *end = NULL;
            unsigned long n = strtoul(e, &end, 10);
            if (end != e && n >= 1 && n <= 9) {
                lang = (ULONG)n;
            } else {
                for (ULONG i = 0; i < 9; i++) {
                    const char *a = e, *b = names[i];
                    while (*a && (*a | 0x20) == *b) a++, b++;
                    if (!*a && !*b) lang = i + 1;
                }
            }
        }
        xbox_log(XBOX_LOG_INFO, XBOX_LOG_XBOX, "console language %u (RECOMP_LANGUAGE %s)", (unsigned)lang,
                 e && *e ? e : "unset");
    }
    return lang;
}

NTSTATUS __stdcall xbox_ExQueryNonVolatileSetting(
    ULONG ValueIndex, PULONG Type, PVOID Value, ULONG ValueLength, PULONG ResultLength)
{
    if (!Value)
        return STATUS_INVALID_PARAMETER;

    xbox_log(XBOX_LOG_DEBUG, XBOX_LOG_XBOX,
        "ExQueryNonVolatileSetting: index=0x%02X len=%u", ValueIndex, ValueLength);

    switch (ValueIndex) {
    case XC_LANGUAGE:
        /* the console's language, which a title starts in: English = 1 unless RECOMP_LANGUAGE says otherwise */
        if (ValueLength >= sizeof(ULONG)) {
            *(PULONG)Value = console_language();
            if (Type) *Type = 4; /* REG_DWORD */
            if (ResultLength) *ResultLength = sizeof(ULONG);
        }
        break;

    case XC_VIDEO:
        /* NTSC with widescreen and HDTV support enabled */
        if (ValueLength >= sizeof(ULONG)) {
            *(PULONG)Value = XC_VIDEO_FLAGS_WIDESCREEN | XC_VIDEO_FLAGS_HDTV;
            if (Type) *Type = 4; /* REG_DWORD */
            if (ResultLength) *ResultLength = sizeof(ULONG);
        }
        break;

    case XC_AUDIO:
        /* Stereo PCM matches the two-channel host output. 1 means mono;
         * advertising AC3 requires an encoded-output path. */
        if (ValueLength >= sizeof(ULONG)) {
            *(PULONG)Value = 0x00000000;
            if (Type) *Type = 4; /* REG_DWORD */
            if (ResultLength) *ResultLength = sizeof(ULONG);
        }
        break;

    case XC_P_CONTROL_GAMES:
    case XC_P_CONTROL_MOVIES:
        /* Unrestricted. Titles compare their XBE certificate's rating against
         * this and refuse to run (Halo boots to the dashboard) if the console
         * is configured more strictly, so 0 means "no restriction". */
        if (ValueLength >= sizeof(ULONG)) {
            *(PULONG)Value = 0;
            if (Type) *Type = 4;
            if (ResultLength) *ResultLength = sizeof(ULONG);
        }
        break;

    case XC_DVD_REGION:
        /* Region 1 (North America) */
        if (ValueLength >= sizeof(ULONG)) {
            *(PULONG)Value = 1;
            if (Type) *Type = 4;
            if (ResultLength) *ResultLength = sizeof(ULONG);
        }
        break;

    case XC_MISC:
        /* Misc flags: 0 = no special flags */
        if (ValueLength >= sizeof(ULONG)) {
            *(PULONG)Value = 0;
            if (Type) *Type = 4;
            if (ResultLength) *ResultLength = sizeof(ULONG);
        }
        break;

    /* The console's time zone, as its dashboard stores it - here the PC's own (Windows' time zone settings), so the
     * title's local time (XAPI's GetLocalTime/FileTimeToLocalFileTime: the UTC clock, KeQuerySystemTime, less these
     * biases, daylight saving by these dates) is the PC's. Was a fixed -300 for XC_TIMEZONE_BIAS, meant as US Eastern
     * but of the wrong sign (UTC = local + bias, so -300 is UTC+5), the rest unanswered (zeros): every save was
     * stamped in UTC+5 wherever the player was. Same units as Windows: biases in minutes; a date is month, the
     * week of the month (5 the last), the day of the week and the hour, one byte each; names 4 characters. */
    case XC_TIMEZONE_BIAS:
    case XC_TZ_STD_NAME:
    case XC_TZ_DLT_NAME:
    case XC_TZ_STD_DATE:
    case XC_TZ_DLT_DATE:
    case XC_TZ_STD_BIAS:
    case XC_TZ_DLT_BIAS: {
        HostTimeZone tz;
        BYTE out[4] = {0, 0, 0, 0};
        host_time_zone(&tz);
        switch (ValueIndex) {
        case XC_TIMEZONE_BIAS: memcpy(out, &tz.bias, 4); break;
        case XC_TZ_STD_BIAS:   memcpy(out, &tz.std_bias, 4); break;
        case XC_TZ_DLT_BIAS:   memcpy(out, &tz.dlt_bias, 4); break;
        case XC_TZ_STD_DATE:   memcpy(out, tz.std_date, 4); break;
        case XC_TZ_DLT_DATE:   memcpy(out, tz.dlt_date, 4); break;
        case XC_TZ_STD_NAME:   memcpy(out, tz.std_name, 4); break;
        default:               memcpy(out, tz.dlt_name, 4); break;
        }
        memset(Value, 0, ValueLength);
        memcpy(Value, out, ValueLength < 4 ? ValueLength : 4);
        if (Type) *Type = ValueIndex == XC_TZ_STD_NAME || ValueIndex == XC_TZ_DLT_NAME ? 3 : 4;  /* binary / DWORD */
        if (ResultLength) *ResultLength = 4;
        break;
    }

    case XC_MAX_OS: {
        /* The whole user section at once, as XAPI's GetTimeZoneInformation (HP:PS 0x18E692) reads it to make the
         * title's local time: each field answered by its own index above, at its place in the console's layout.
         * Unhandled before, so it came back zeros - no time zone at all for that path. */
        static const struct { ULONG index, offset; } kUserBlock[] = {
            {XC_TIMEZONE_BIAS, 0x00}, {XC_TZ_STD_NAME, 0x04}, {XC_TZ_DLT_NAME, 0x08}, {XC_TZ_STD_DATE, 0x14},
            {XC_TZ_DLT_DATE, 0x18},   {XC_TZ_STD_BIAS, 0x24}, {XC_TZ_DLT_BIAS, 0x28}, {XC_LANGUAGE, 0x2C},
            {XC_VIDEO, 0x30},         {XC_AUDIO, 0x34},       {XC_P_CONTROL_GAMES, 0x38},
            {XC_P_CONTROL_MOVIES, 0x40}, {XC_MISC, 0x54},     {XC_DVD_REGION, 0x58},
        };
        const ULONG kUserBlockSize = 0x5C;
        memset(Value, 0, ValueLength);
        for (size_t i = 0; i < sizeof(kUserBlock) / sizeof(kUserBlock[0]); i++) {
            ULONG field = 0, t = 0, n = 0;
            if (kUserBlock[i].offset + 4 > ValueLength) continue;
            xbox_ExQueryNonVolatileSetting(kUserBlock[i].index, &t, &field, sizeof(field), &n);
            memcpy((BYTE *)Value + kUserBlock[i].offset, &field, 4);
        }
        /* misc bit 1: daylight saving off - a zone without it (Windows gives no daylight date) */
        if (ValueLength >= 0x58) {
            HostTimeZone tz;
            host_time_zone(&tz);
            if (!tz.dlt_date[0]) *(PULONG)((BYTE *)Value + 0x54) |= 2;
        }
        if (Type) *Type = 3;  /* binary */
        if (ResultLength) *ResultLength = ValueLength < kUserBlockSize ? ValueLength : kUserBlockSize;
        break;
    }

    case XC_FACTORY_GAME_REGION:
        if (ValueLength >= sizeof(ULONG)) {
            *(PULONG)Value = title_game_region();
            if (Type) *Type = 4;
            if (ResultLength) *ResultLength = sizeof(ULONG);
        }
        break;

    default:
        xbox_log(XBOX_LOG_WARN, XBOX_LOG_XBOX,
            "ExQueryNonVolatileSetting: unhandled index 0x%02X", ValueIndex);
        memset(Value, 0, ValueLength);
        if (Type) *Type = 4;
        if (ResultLength) *ResultLength = ValueLength;
        break;
    }

    return STATUS_SUCCESS;
}

NTSTATUS __stdcall xbox_ExSaveNonVolatileSetting(
    ULONG ValueIndex, ULONG Type, PVOID Value, ULONG ValueLength)
{
    (void)Type;
    (void)Value;
    (void)ValueLength;

    xbox_log(XBOX_LOG_INFO, XBOX_LOG_XBOX,
        "ExSaveNonVolatileSetting: index=0x%02X len=%u (ignored - read-only on PC)",
        ValueIndex, ValueLength);

    /* Accept writes silently but don't persist them */
    return STATUS_SUCCESS;
}

/* ============================================================================
 * Network / PHY
 *
 * PhyGetLinkState reports Ethernet link status. Xbox had a built-in 100Mbit
 * NIC. For PC, we report link-up since we'll handle networking differently.
 * ============================================================================ */

ULONG __stdcall xbox_PhyGetLinkState(BOOLEAN Verify)
{
    (void)Verify;
    /* Return link up, 100 Mbps, full duplex */
    return 0x01; /* XNET_ETHERNET_LINK_ACTIVE */
}

NTSTATUS __stdcall xbox_PhyInitialize(BOOLEAN ForceReset, PVOID Param2)
{
    (void)ForceReset;
    (void)Param2;
    return STATUS_SUCCESS;
}
