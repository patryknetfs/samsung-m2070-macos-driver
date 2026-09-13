// Native arm64 Image Capture device module (ICDM) for Samsung M2070 series
// network scanners (SANE xerox_mfp backend at _scanner._tcp / port 9400).
//
// Iteration 1: contract skeleton. Mirrors the parameter construction logic
// of the original (x86_64 Rosetta-only) "Samsung Scanner.app" driver, decoded
// in a prior session, but logs every dict it sees and returns so the real
// runtime contract can be captured and copied exactly.
//
// Build/install target: /Library/Image Capture/Devices/Samsung Scanner.app

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <math.h>
#include <sys/stat.h>

#include <CoreFoundation/CoreFoundation.h>
#include <CoreServices/CoreServices.h>
#include <IOKit/IOKitLib.h>

#include <ImageIO/ImageIO.h>
#include <CoreGraphics/CoreGraphics.h>

#include <ICADevices/ICADevices.h>

// ---------------------------------------------------------------------------
// constants that are not (or no longer) exported in the modern SDK headers
// ---------------------------------------------------------------------------

#ifndef kICANoSession
#define kICANoSession (ICAScannerSessionID)0
#endif

#ifndef kICANoObject
#define kICANoObject (ICAObject)0
#endif

// TWON / TWAIN constants
enum {
    TWON_ONEVALUE = 0x0005,
    TWON_RANGE    = 0x0006,
    TWON_ENUMERATION = 0x0008
};

enum {
    TWPT_BW    = 0,
    TWPT_GRAY  = 1,
    TWPT_RGB   = 2
};

enum {
    TWUN_INCHES = 0,
    TWUN_CENTIMETERS = 1,
    TWUN_PICAS = 2,
    TWUN_POINTS = 3,
    TWUN_TWIPS = 4,
    TWUN_PIXELS = 5,
    TWUN_MILLIMETERS = 6
};

// ---------------------------------------------------------------------------
// CFString keys used in the ICA parameters dictionary (same literal keys the
// original Samsung driver emits)
// ---------------------------------------------------------------------------

static CFStringRef ICAP_XRESOLUTION       = CFSTR("ICAP_XRESOLUTION");
static CFStringRef ICAP_YRESOLUTION       = CFSTR("ICAP_YRESOLUTION");
static CFStringRef ICAP_BITDEPTH          = CFSTR("ICAP_BITDEPTH");
static CFStringRef ICAP_PIXELTYPE         = CFSTR("ICAP_PIXELTYPE");
static CFStringRef ICAP_LIGHTPATH         = CFSTR("ICAP_LIGHTPATH");
static CFStringRef ICAP_PHYSICALWIDTH     = CFSTR("ICAP_PHYSICALWIDTH");
static CFStringRef ICAP_PHYSICALHEIGHT    = CFSTR("ICAP_PHYSICALHEIGHT");
static CFStringRef ICAP_UNITS             = CFSTR("ICAP_UNITS");
static CFStringRef ICAP_XNATIVERESOLUTION = CFSTR("ICAP_XNATIVERESOLUTION");
static CFStringRef ICAP_YNATIVERESOLUTION = CFSTR("ICAP_YNATIVERESOLUTION");
static CFStringRef ICAP_XSCALING          = CFSTR("ICAP_XSCALING");
static CFStringRef ICAP_YSCALING          = CFSTR("ICAP_YSCALING");
static CFStringRef ColorSyncMode          = CFSTR("ColorSyncMode");
static CFStringRef deviceKey              = CFSTR("device");

static CFStringRef typeKey                = CFSTR("type");
static CFStringRef valueKey               = CFSTR("value");
static CFStringRef minKey                 = CFSTR("min");
static CFStringRef maxKey                 = CFSTR("max");
static CFStringRef stepSizeKey            = CFSTR("stepSize");
static CFStringRef defaultKey             = CFSTR("default");
static CFStringRef currentKey             = CFSTR("current");

static CFStringRef kTWON_ONEVALUE_Str     = CFSTR("TWON_ONEVALUE");
static CFStringRef kTWON_RANGE_Str        = CFSTR("TWON_RANGE");
static CFStringRef kTWON_ENUMERATION_Str  = CFSTR("TWON_ENUMERATION");

static CFStringRef kUserScanAreaParamKey  = CFSTR("userScanArea");

static CFStringRef kMDLKey                = CFSTR("MDL");
static CFStringRef kMFGKey                = CFSTR("MFG");
static CFStringRef kTXTKey                = CFSTR("TXT");
static CFStringRef kUSERKey               = CFSTR("USER");

// ---------------------------------------------------------------------------
// CFDictionary helpers
// ---------------------------------------------------------------------------

static CFMutableDictionaryRef CreateMutableDict(void)
{
    return CFDictionaryCreateMutable(kCFAllocatorDefault, 0,
                                     &kCFTypeDictionaryKeyCallBacks,
                                     &kCFTypeDictionaryValueCallBacks);
}

static void DictSetString(CFMutableDictionaryRef d, CFStringRef key, CFStringRef v)
{
    CFDictionarySetValue(d, key, v);
}

static void DictSetLong(CFMutableDictionaryRef d, CFStringRef key, long v)
{
    CFNumberRef n = CFNumberCreate(kCFAllocatorDefault, kCFNumberLongType, &v);
    CFDictionarySetValue(d, key, n);
    CFRelease(n);
}

static void DictSetLongArray(CFMutableDictionaryRef d, CFStringRef key, const long* vals, size_t count)
{
    CFMutableArrayRef a = CFArrayCreateMutable(kCFAllocatorDefault, 0, &kCFTypeArrayCallBacks);
    for (size_t i = 0; i < count; i++) {
        CFNumberRef n = CFNumberCreate(kCFAllocatorDefault, kCFNumberLongType, &vals[i]);
        CFArrayAppendValue(a, n);
        CFRelease(n);
    }
    CFDictionarySetValue(d, key, a);
    CFRelease(a);
}

// type + value (+ optional range fields)
static CFMutableDictionaryRef MakeOneValue(long value)
{
    CFMutableDictionaryRef d = CreateMutableDict();
    DictSetString(d, typeKey, kTWON_ONEVALUE_Str);
    DictSetLong(d, valueKey, value);
    return d;
}

static CFMutableDictionaryRef MakeRange(long min, long max, long stepSize,
                                        long deflt, long current)
{
    CFMutableDictionaryRef d = CreateMutableDict();
    DictSetString(d, typeKey, kTWON_RANGE_Str);
    DictSetLong(d, minKey, min);
    DictSetLong(d, maxKey, max);
    DictSetLong(d, stepSizeKey, stepSize);
    DictSetLong(d, defaultKey, deflt);
    DictSetLong(d, currentKey, current);
    return d;
}

static CFMutableDictionaryRef MakeEnumeration(const long* values, size_t count,
                                              long deflt, long current)
{
    CFMutableDictionaryRef d = CreateMutableDict();
    DictSetString(d, typeKey, kTWON_ENUMERATION_Str);
    DictSetLongArray(d, valueKey, values, count);
    DictSetLong(d, defaultKey, deflt);
    DictSetLong(d, currentKey, current);
    return d;
}

// ---------------------------------------------------------------------------
// parameter dictionary for the Samsung M2070 (mono laser MFP)
// ---------------------------------------------------------------------------

static CFMutableDictionaryRef BuildDeviceParams(void)
{
    CFMutableDictionaryRef params = CreateMutableDict();

    static const long resolutions[] = { 75, 100, 150, 200, 300, 600 };
    CFMutableDictionaryRef rx = MakeEnumeration(resolutions, 6, 300, 200);
    CFMutableDictionaryRef ry = MakeEnumeration(resolutions, 6, 300, 200);

    static const long bitdepths[] = { 1, 8 };
    CFMutableDictionaryRef bd = MakeEnumeration(bitdepths, 2, 8, 8);

    static const long pixeltypes[] = { TWPT_BW, TWPT_GRAY, TWPT_RGB };
    CFMutableDictionaryRef pt = MakeEnumeration(pixeltypes, 3, TWPT_RGB, TWPT_RGB);

    CFMutableDictionaryRef lp = MakeOneValue(0);               // TWLP_REFLECTIVE
    CFMutableDictionaryRef un = MakeOneValue(TWUN_PIXELS);     // 5
    CFMutableDictionaryRef pw = MakeOneValue(5104);            // 8.5 in @ 600 dpi
    CFMutableDictionaryRef ph = MakeOneValue(7019);            // 11.7 in @ 600 dpi
    CFMutableDictionaryRef nx = MakeOneValue(600);
    CFMutableDictionaryRef ny = MakeOneValue(600);
    CFMutableDictionaryRef sx = MakeRange(100, 100, 1, 100, 100);
    CFMutableDictionaryRef sy = MakeRange(100, 100, 1, 100, 100);
    CFMutableDictionaryRef cs = MakeOneValue(0);

    CFDictionarySetValue(params, ICAP_XRESOLUTION, rx);
    CFDictionarySetValue(params, ICAP_YRESOLUTION, ry);
    CFDictionarySetValue(params, ICAP_BITDEPTH, bd);
    CFDictionarySetValue(params, ICAP_PIXELTYPE, pt);
    CFDictionarySetValue(params, ICAP_LIGHTPATH, lp);
    CFDictionarySetValue(params, ICAP_UNITS, un);
    CFDictionarySetValue(params, ICAP_PHYSICALWIDTH, pw);
    CFDictionarySetValue(params, ICAP_PHYSICALHEIGHT, ph);
    CFDictionarySetValue(params, ICAP_XNATIVERESOLUTION, nx);
    CFDictionarySetValue(params, ICAP_YNATIVERESOLUTION, ny);
    CFDictionarySetValue(params, ICAP_XSCALING, sx);
    CFDictionarySetValue(params, ICAP_YSCALING, sy);
    CFDictionarySetValue(params, ColorSyncMode, cs);

    CFRelease(rx); CFRelease(ry); CFRelease(bd); CFRelease(pt);
    CFRelease(lp); CFRelease(un); CFRelease(pw); CFRelease(ph);
    CFRelease(nx); CFRelease(ny); CFRelease(sx); CFRelease(sy);
    CFRelease(cs);

    return params;
}

// ---------------------------------------------------------------------------
// per-device runtime state
// ---------------------------------------------------------------------------

typedef struct DeviceState {
    char name[64];
    char ip[64];
    char sanedev[96];
} DeviceState;

typedef struct ScanSettings {
    long resolution;     // dpi
    long pixelType;      // 0 BW, 1 Gray, 2 RGB
    long bitDepth;
    double offsetX, offsetY;   // pixels at resolution
    double width, height;      // pixels at resolution
    int isOverview;            // "scan mode" == overview
    unsigned gen;              // request generation (single-flight)
    char sanedev[96];
    ICAObject object;
    char docFolder[512];       // NoData scans: client's security-scoped output folder
    char docName[128];
    char docExt[32];
} ScanSettings;

static ScanSettings gSettings;

// live device ScannerObjectInfo pointer from the icdd callback — passed as
// the parent to ICDScannerNewObjectInfoCreated.  icdd resolves the parent via
// the ScannerObjectInfo.reserved field (icdd's internal pointer to the
// ICDDevice), so zeroing it returns kICADeviceNotFoundErr / -9901.  The
// reserved value icdd pre-fills at open time is remembered here as fallback
// in case a later callback hands us a fresh copy where reserved is zero.
static const ScannerObjectInfo* gLiveDeviceObjectInfo = NULL;
static int gDeviceObjectInfoValid = 0;
static unsigned long gDeviceReservedFallback = 0;

// the most recently completed scan, surfaced to the client as a new ICAFile
// object (the original driver's "trigger new object" handshake)
static ICAObject gLastScanObject = kICANoObject;
static int gLastScanValid = 0;
static char gLastScanPng[256] = "";
static ScannerObjectInfo gLastScanObjInfo;
static pthread_mutex_t gScanObjectLock = PTHREAD_MUTEX_INITIALIZER;

static void StateFromUserInfo(DeviceState* st, CFDictionaryRef params)
{
    memset(st, 0, sizeof *st);
    CFStringRef name = NULL;
    CFTypeRef user = CFDictionaryGetValue(params, kUSERKey);
    if (user && CFGetTypeID(user) == CFStringGetTypeID()) {
        name = (CFStringRef)user;
    } else {
        CFTypeRef txt = CFDictionaryGetValue(params, kTXTKey);
        if (txt) {
            CFTypeRef mdl = CFDictionaryGetValue((CFDictionaryRef)txt, kMDLKey);
            if (mdl) name = (CFStringRef)mdl;
            else {
                CFTypeRef mfg = CFDictionaryGetValue((CFDictionaryRef)txt, kMFGKey);
                if (mfg) name = (CFStringRef)mfg;
            }
        }
    }
    if (name) {
        CFStringGetCString(name, st->name, sizeof st->name, kCFStringEncodingUTF8);
    }
    if (!st->name[0]) strncpy(st->name, "Samsung M2070 Series", sizeof st->name - 1);

    // build the SANE device string from the open params
    CFStringRef ip = CFDictionaryGetValue(params, CFSTR("ipAddress"));
    if (ip) {
        CFStringGetCString(ip, st->ip, sizeof st->ip, kCFStringEncodingUTF8);
    } else {
        strncpy(st->ip, "127.0.0.1", sizeof st->ip - 1);
    }
    CFTypeRef port = CFDictionaryGetValue(params, CFSTR("ipPort"));
    long p = 9400;
    CFNumberGetValue((CFNumberRef)port, kCFNumberLongType, &p);
    snprintf(st->sanedev, sizeof st->sanedev, "xerox_mfp:tcp %s %ld", st->ip, p);
}

// ---------------------------------------------------------------------------
// callbacks
// ---------------------------------------------------------------------------

static ICAError OpenTCPIPDevice(CFDictionaryRef params, ScannerObjectInfo* objectInfo)
{

    

    if (!objectInfo) return kICADeviceInvalidParamErr;

    // icdd pre-fills `reserved` with its internal pointer back to the
    // ICDDevice object.  ICDScannerNewObjectInfoCreated resolves the parent via
    // this field (ldur x0,[x0,#4]); zeroing it makes icdd reply
    // ICLegacyReturnCodeDeviceNotFoundErr (-9901).  Preserve it.
    unsigned long reserved = objectInfo->reserved;
    gDeviceReservedFallback = reserved;

    memset(objectInfo, 0, sizeof(*objectInfo));
    objectInfo->reserved = reserved;
    objectInfo->icaObjectInfo.objectType = kICADevice;
    objectInfo->icaObjectInfo.objectSubtype = kICADeviceScanner;

    DeviceState* st = malloc(sizeof(DeviceState));
    if (!st) return kICADeviceMemoryAllocationErr;
    StateFromUserInfo(st, params);
    strncpy((char*)objectInfo->name, st->name, 31);
    objectInfo->name[31] = 0;
    objectInfo->flags = 0;
    objectInfo->privateData = (Ptr)st;

    return noErr;
}

static ICAError OpenUSBDevice(UInt32 locationID, ScannerObjectInfo* objectInfo)
{
    (void)locationID; (void)objectInfo;
    
    return kICADeviceUnsupportedErr;
}

static ICAError OpenUSBDeviceWithIORegPath(UInt32 locationID, io_string_t ioregPath,
                                           ScannerObjectInfo* objectInfo)
{
    (void)locationID; (void)ioregPath; (void)objectInfo;
    return kICADeviceUnsupportedErr;
}

static ICAError OpenFireWireDevice(UInt64 guid, ScannerObjectInfo* objectInfo)
{
    (void)guid; (void)objectInfo;
    return kICADeviceUnsupportedErr;
}

static ICAError OpenFireWireDeviceWithIORegPath(UInt64 guid, io_string_t ioregPath,
                                                ScannerObjectInfo* objectInfo)
{
    (void)guid; (void)ioregPath; (void)objectInfo;
    return kICADeviceUnsupportedErr;
}

static ICAError OpenBluetoothDevice(CFDictionaryRef params, ScannerObjectInfo* objectInfo)
{
    (void)params; (void)objectInfo;
    return kICADeviceUnsupportedErr;
}

static ICAError OpenMassStorageDevice(CFStringRef diskBSDName, DASessionRef daSession,
                                      ScannerObjectInfo* objectInfo)
{
    (void)diskBSDName; (void)daSession; (void)objectInfo;
    return kICADeviceUnsupportedErr;
}

static ICAError CloseDevice(ScannerObjectInfo* objectInfo)
{
    
    if (objectInfo) {
        free(objectInfo->privateData);
        objectInfo->privateData = NULL;
    }
    return noErr;
}

static ICAError PeriodicTask(ScannerObjectInfo* objectInfo)
{
    (void)objectInfo;
    return noErr;
}

static ICAError GetObjectInfo(const ScannerObjectInfo* parentInfo, UInt32 index,
                              ScannerObjectInfo* newInfo)
{
    if (!newInfo) return kICADeviceInvalidParamErr;
    pthread_mutex_lock(&gScanObjectLock);
    int valid = gLastScanValid;
    ScannerObjectInfo saved = gLastScanObjInfo;
    pthread_mutex_unlock(&gScanObjectLock);

    if (index == 0 && valid) {
        // expose the most recent scan result as child object #0
        memset(newInfo, 0, sizeof(*newInfo));
        *newInfo = saved;
        return noErr;
    }
    (void)parentInfo;
    return kICADeviceNotFoundErr;
}

static ICAError Cleanup(ScannerObjectInfo* objectInfo)
{
    
    if (objectInfo) {
        free(objectInfo->privateData);
        objectInfo->privateData = NULL;
    }
    return noErr;
}

static ICAError StandardGetPropertyData(const ScannerObjectInfo* objectInfo, void* pb)
{
    
    return ICDScannerGetStandardPropertyData(objectInfo, pb);
}

static ICAError SetPropertyData(const ScannerObjectInfo* objectInfo, const void* pb)
{
    (void)objectInfo; (void)pb;
    
    return noErr;
}

static ICAError ReadFileData(const ScannerObjectInfo* objectInfo, UInt32 dataType,
                             Ptr buffer, UInt32 offset, UInt32* length)
{
    if (!objectInfo || !buffer || !length) return kICADeviceInvalidParamErr;

    pthread_mutex_lock(&gScanObjectLock);
    char path[256];
    snprintf(path, sizeof path, "%s", gLastScanPng);
    bool matches = (objectInfo->icaObject == gLastScanObject);
    pthread_mutex_unlock(&gScanObjectLock);

    if (!matches || !path[0]) return kICADeviceNotFoundErr;

    FILE* f = fopen(path, "rb");
    if (!f) return kICADeviceNotFoundErr;
    fseek(f, 0, SEEK_END);
    long total = ftell(f);
    if (offset >= (UInt32)total) {
        *length = 0;
        fclose(f);
        return noErr;
    }
    UInt32 want = *length;
    if (offset + want > (UInt32)total) want = (UInt32)total - offset;
    fseek(f, offset, SEEK_SET);
    size_t got = fread(buffer, 1, want, f);
    fclose(f);
    *length = (UInt32)got;
    return noErr;
}

static ICAError WriteFileData(const ScannerObjectInfo* parentInfo, const char* filename,
                              UInt32 dataType, Ptr buffer, UInt32 offset, UInt32* length)
{
    (void)parentInfo; (void)filename; (void)dataType; (void)buffer; (void)offset; (void)length;
    
    return kICADeviceUnsupportedErr;
}

static ICAError SendMessage(const ScannerObjectInfo* objectInfo,
                            ICD_ScannerObjectSendMessagePB* pb, ICDCompletion completion)
{
    (void)objectInfo; (void)pb; (void)completion;
    
    return kICADeviceUnsupportedErr;
}

static ICAError AddPropertiesToCFDictionary(ScannerObjectInfo* objectInfo,
                                            CFMutableDictionaryRef dict)
{
    if (dict) CFDictionarySetValue(dict, kICAUserAssignedDeviceNameKey,
                                   CFSTR("Samsung M2070"));
    (void)objectInfo;
    
    return noErr;
}

static ICAError OpenSession(const ScannerObjectInfo* deviceObjectInfo,
                            ICD_ScannerOpenSessionPB* pb)
{
    
    (void)deviceObjectInfo;
    return noErr;
}

static ICAError CloseSession(const ScannerObjectInfo* deviceObjectInfo,
                             ICD_ScannerCloseSessionPB* pb)
{
    
    (void)deviceObjectInfo;
    return noErr;
}

static ICAError Initialize(const ScannerObjectInfo* deviceObjectInfo,
                           ICD_ScannerInitializePB* pb)
{
    
    (void)deviceObjectInfo;
    return noErr;
}

static ICAError GetParameters(const ScannerObjectInfo* deviceObjectInfo,
                              ICD_ScannerGetParametersPB* pb)
{

    if (pb->theDict == NULL) {
        pb->theDict = CreateMutableDict();
    }
    CFMutableDictionaryRef params = BuildDeviceParams();
    CFDictionarySetValue(pb->theDict, deviceKey, params);

    CFRelease(params);
    (void)deviceObjectInfo;
    return noErr;
}

static ICAError SetParameters(const ScannerObjectInfo* deviceObjectInfo,
                              ICD_ScannerSetParametersPB* pb)
{

    

    // the settings arrive under "userScanArea" (as observed) or "device"
    CFDictionaryRef d = NULL;
    CFTypeRef a = CFDictionaryGetValue(pb->theDict, kUserScanAreaParamKey);
    if (a && CFGetTypeID(a) == CFDictionaryGetTypeID()) d = (CFDictionaryRef)a;
    else {
        a = CFDictionaryGetValue(pb->theDict, deviceKey);
        if (a && CFGetTypeID(a) == CFDictionaryGetTypeID()) d = (CFDictionaryRef)a;
    }
    if (!d) { (void)deviceObjectInfo; return noErr; }

    static const CFStringRef kScanModeKey = CFSTR("scan mode");
    static const CFStringRef kOffsetXKey = CFSTR("offsetX");
    static const CFStringRef kOffsetYKey = CFSTR("offsetY");

    long defs[5][2] = { {0,0},{0,0},{0,0},{0,0},{0,0} };
    ScanSettings st;
    memset(&st, 0, sizeof st);
    st.resolution = 150;
    st.pixelType = 2;
    st.bitDepth = 8;
    st.offsetX = st.offsetY = 0.0;
    st.width  = 5104;
    st.height = 7019;
    (void)defs;

    CFNumberRef n;
    long v;
    n = CFDictionaryGetValue(d, ICAP_XRESOLUTION);
    if (n && CFGetTypeID(n) == CFDictionaryGetTypeID()) {
        // the value arrives as a TWON struct; read its "value"
        CFNumberRef inner = CFDictionaryGetValue((CFDictionaryRef)n, valueKey);
        if (inner && CFNumberGetValue(inner, kCFNumberLongType, &v)) st.resolution = v;
    } else if (n && CFNumberGetValue(n, kCFNumberLongType, &v)) st.resolution = v;
    n = CFDictionaryGetValue(d, ICAP_PIXELTYPE);
    if (n && CFGetTypeID(n) == CFDictionaryGetTypeID()) {
        CFNumberRef inner = CFDictionaryGetValue((CFDictionaryRef)n, valueKey);
        if (inner && CFNumberGetValue(inner, kCFNumberLongType, &v)) st.pixelType = v;
    } else if (n && CFNumberGetValue(n, kCFNumberLongType, &v)) st.pixelType = v;
    n = CFDictionaryGetValue(d, ICAP_BITDEPTH);
    if (n && CFGetTypeID(n) == CFDictionaryGetTypeID()) {
        CFNumberRef inner = CFDictionaryGetValue((CFDictionaryRef)n, valueKey);
        if (inner && CFNumberGetValue(inner, kCFNumberLongType, &v)) st.bitDepth = v;
    } else if (n && CFNumberGetValue(n, kCFNumberLongType, &v)) st.bitDepth = v;

    double dv;
    n = CFDictionaryGetValue(d, kOffsetXKey);
    if (n && CFNumberGetValue(n, kCFNumberDoubleType, &dv)) st.offsetX = dv;
    else if (n && CFNumberGetValue(n, kCFNumberLongType, &v)) st.offsetX = v;
    n = CFDictionaryGetValue(d, kOffsetYKey);
    if (n && CFNumberGetValue(n, kCFNumberDoubleType, &dv)) st.offsetY = dv;
    else if (n && CFNumberGetValue(n, kCFNumberLongType, &v)) st.offsetY = v;
    n = CFDictionaryGetValue(d, CFSTR("width"));
    if (n && CFNumberGetValue(n, kCFNumberLongType, &v)) st.width = v;
    n = CFDictionaryGetValue(d, CFSTR("height"));
    if (n && CFNumberGetValue(n, kCFNumberLongType, &v)) st.height = v;

    CFTypeRef sm = CFDictionaryGetValue(d, kScanModeKey);
    if (sm && CFGetTypeID(sm) == CFStringGetTypeID()) {
        st.isOverview = CFStringCompare((CFStringRef)sm, CFSTR("overview"), 0) == kCFCompareEqualTo;
    }

    // NoData full scans carry the client's security-scoped output folder plus
    // the desired document name/extension (observed: "ScanResultsTemp_*/Scanned
    // Image.jpeg"). The driver writes the finished file there itself; icdd does
    // NOT create it (verified via fs_usage against the working original driver).
    static const CFStringRef kDocFolderKey = CFSTR("document folder");
    static const CFStringRef kDocNameKey = CFSTR("document name");
    static const CFStringRef kDocExtKey = CFSTR("document extension");
    st.docFolder[0] = st.docName[0] = st.docExt[0] = 0;
    CFTypeRef df = CFDictionaryGetValue(d, kDocFolderKey);
    if (df && CFGetTypeID(df) == CFStringGetTypeID())
        CFStringGetCString((CFStringRef)df, st.docFolder, sizeof st.docFolder, kCFStringEncodingUTF8);
    CFTypeRef dn = CFDictionaryGetValue(d, kDocNameKey);
    if (dn && CFGetTypeID(dn) == CFStringGetTypeID())
        CFStringGetCString((CFStringRef)dn, st.docName, sizeof st.docName, kCFStringEncodingUTF8);
    CFTypeRef de = CFDictionaryGetValue(d, kDocExtKey);
    if (de && CFGetTypeID(de) == CFStringGetTypeID())
        CFStringGetCString((CFStringRef)de, st.docExt, sizeof st.docExt, kCFStringEncodingUTF8);

    memcpy(&gSettings, &st, sizeof gSettings);
    
    (void)deviceObjectInfo;
    return noErr;
}

static ICAError Status(const ScannerObjectInfo* deviceObjectInfo, ICD_ScannerStatusPB* pb)
{
    
    pb->status = 0; // kScannerStatusOK
    (void)deviceObjectInfo;
    return noErr;
}

// ---------------------------------------------------------------------------
// push-model scan delivery (mirrors the original driver's Notifications.cpp)
// ---------------------------------------------------------------------------

// The runtime ICASendNotificationPB layout used by icdd does NOT match the SDK
// header: both the original Samsung driver and Apple's AirScanScanner place the
// notificationDictionary at byte offset 10 and receive the replyCode at offset 18.
// Marshal the dictionary there explicitly.
static ICAError SendNotificationRaw(CFMutableDictionaryRef d, int waitForReply,
                                    uint32_t* outReply)
{
    uint8_t pbbuf[48];
    memset(pbbuf, 0, sizeof pbbuf);
    *(uint64_t*)(pbbuf + 10) = (uintptr_t)d;
    ICAError err = waitForReply
        ? ICDSendNotificationAndWaitForReply((ICASendNotificationPB*)pbbuf)
        : ICDSendNotification((ICASendNotificationPB*)pbbuf);
    if (outReply) *outReply = *(uint32_t*)(pbbuf + 18);
    static int sDumpOnce = 1;
    if (sDumpOnce) {
        sDumpOnce = 0;

    }
    return err;
}

static ICAError SendSimpleNotification(ICAObject object, CFStringRef type)
{
    CFMutableDictionaryRef d = CreateMutableDict();
    DictSetLong(d, kICANotificationICAObjectKey, object);
    CFDictionarySetValue(d, kICANotificationTypeKey, type);

    ICAError err = SendNotificationRaw(d, 0, NULL);
    
    CFRelease(d);
    return err;
}

// The original driver stamps the saved document's full path into the
// PageDone notification via kICANotificationScannerDocumentNameKey when the
// noData gate is armed (disasm 13a05-13a61, 23276-23280).  Preview uses that
// name to associate the file the driver wrote into the client's destination
// folder, so NoData scans must carry it.
static ICAError SendPageDone(ICAObject object, CFStringRef docName)
{
    CFMutableDictionaryRef d = CreateMutableDict();
    DictSetLong(d, kICANotificationICAObjectKey, object);
    CFDictionarySetValue(d, kICANotificationTypeKey, kICANotificationTypeScannerPageDone);
    if (docName) CFDictionarySetValue(d, kICANotificationScannerDocumentNameKey, docName);

    ICAError err = SendNotificationRaw(d, 0, NULL);
    
    CFRelease(d);
    return err;
}

static ICAError SendProgressBand(ICAObject object, UInt32 width, UInt32 height,
                                 UInt32 bytesPerLine, UInt32 startRow, UInt32 numRows,
                                 UInt32 dataSize, void* buffer, int pixelType)
{
    CFMutableDictionaryRef d = CreateMutableDict();
    DictSetLong(d, kICANotificationICAObjectKey, object);
    CFDictionarySetValue(d, kICANotificationTypeKey, kICANotificationTypeScanProgressStatus);

    ICAError err = ICDAddImageInfoToNotificationDictionary(d, width, height,
                                                           bytesPerLine, startRow, numRows,
                                                           dataSize, (Ptr)buffer);
    
    if (err == noErr) {
        uint32_t reply = 0;
        err = SendNotificationRaw(d, 1, &reply);
        
        if (err == noErr && reply != noErr) err = (ICAError)reply;
    }
    CFRelease(d);
    return err;
}

static pthread_mutex_t gScanLock = PTHREAD_MUTEX_INITIALIZER;

// resample a pixel buffer to exactly the requested client dimensions
// bpp = bytes per pixel (1 for gray, 3 for RGB)
static unsigned char* ResamplePixels(const unsigned char* src, UInt32 sw, UInt32 sh,
                                     UInt32 dw, UInt32 dh, UInt32 bpp)
{
    if (sw == dw && sh == dh) {
        unsigned char* copy = malloc((size_t)sw * sh * bpp);
        if (copy) memcpy(copy, src, (size_t)sw * sh * bpp);
        return copy;
    }

    // upscale to RGBA for CG resampling, then pack back
    size_t sbpr = sw * 4;
    unsigned char* rgba = calloc((size_t)sh * sbpr, 1);
    if (!rgba) return NULL;
    for (UInt32 y = 0; y < sh; y++) {
        for (UInt32 x = 0; x < sw; x++) {
            unsigned char val = src[(y * sw + x) * bpp];
            rgba[y * sbpr + x * 4 + 0] = val;
            rgba[y * sbpr + x * 4 + 1] = (bpp >= 3) ? src[(y * sw + x) * bpp + 1] : val;
            rgba[y * sbpr + x * 4 + 2] = (bpp >= 3) ? src[(y * sw + x) * bpp + 2] : val;
            rgba[y * sbpr + x * 4 + 3] = 255;
        }
    }
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CGContextRef ctx = CGBitmapContextCreate(rgba, sw, sh, 8, sbpr, cs,
                                             kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big);
    CGColorSpaceRelease(cs);
    if (!ctx) { free(rgba); return NULL; }
    CGImageRef cimg = CGBitmapContextCreateImage(ctx);
    CGContextRelease(ctx);
    if (!cimg) { free(rgba); return NULL; }

    size_t dbpr = dw * 4;
    unsigned char* drgba = calloc((size_t)dh * dbpr, 1);
    if (!drgba) { CGImageRelease(cimg); free(rgba); return NULL; }
    CGContextRef dctx = CGBitmapContextCreate(drgba, dw, dh, 8, dbpr, cs,
                                              kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big);
    if (!dctx) { free(drgba); CGImageRelease(cimg); free(rgba); return NULL; }
    CGContextSetInterpolationQuality(dctx, kCGInterpolationHigh);
    CGContextDrawImage(dctx, CGRectMake(0, 0, dw, dh), cimg);
    CGContextRelease(dctx);
    CGImageRelease(cimg);
    free(rgba);

    unsigned char* out = malloc((size_t)dw * dh * bpp);
    if (!out) { free(drgba); return NULL; }
    for (UInt32 y = 0; y < dh; y++) {
        for (UInt32 x = 0; x < dw; x++) {
            out[(y * dw + x) * bpp + 0] = drgba[y * dbpr + x * 4 + 0];
            if (bpp >= 3) {
                out[(y * dw + x) * bpp + 1] = drgba[y * dbpr + x * 4 + 1];
                out[(y * dw + x) * bpp + 2] = drgba[y * dbpr + x * 4 + 2];
            }
        }
    }
    free(drgba);
    return out;
}

// ---------------------------------------------------------------------------
// In-process TWAIN acquisition over TCP (port 9400), the same wire protocol
// the working original driver speaks (decoded from sane-backends' xerox_mfp
// backend and from the original's disassembly). Replaces the scanimage
// subprocess: no fork, no SIGKILL-induced wedges, and we issue a CMD_ABORT on
// open/close so the MFP's scan module never holds a stale session.
// ---------------------------------------------------------------------------

#define TWAIN_REQ_A     0x1b
#define TWAIN_REQ_B     0xa8
#define TWAIN_RES_CODE  0xa8

#define TWAIN_CMD_ABORT          0x06
#define TWAIN_CMD_INQUIRY        0x12
#define TWAIN_CMD_RESERVE_UNIT   0x16
#define TWAIN_CMD_RELEASE_UNIT   0x17
#define TWAIN_CMD_SET_WINDOW     0x24
#define TWAIN_CMD_READ           0x28
#define TWAIN_CMD_READ_IMAGE     0x29
#define TWAIN_CMD_OBJECT_POS     0x31

#define TWAIN_STATUS_GOOD        0x00
#define TWAIN_STATUS_CHECK       0x02
#define TWAIN_STATUS_CANCEL      0x04
#define TWAIN_STATUS_BUSY        0x08

#define TWAIN_MSG_PRODUCT_INFO   0x10
#define TWAIN_MSG_SCANNER_STATE  0x20
#define TWAIN_MSG_SCANNING_PARAM 0x30
#define TWAIN_MSG_LINK_BLOCK     0x80
#define TWAIN_MSG_END_BLOCK      0x81

#define TWAIN_STATE_NO_ERROR     0x001
#define TWAIN_STATE_WARMING      0x080
#define TWAIN_STATE_LOCKING      0x100
#define TWAIN_STATE_BUSY         0x400

#define TWAIN_MODE_GRAY8         0x03
#define TWAIN_MODE_RGB24         0x05
#define TWAIN_COMP_JPG           0x06   // JPEG lossy: REQUIRED for RGB24 on this engine
#define TWAIN_DOC_FLATBED        0x40

#define TWAIN_PNT_PER_MM         (1200.0 / 25.4)

static int twain_send_all(int fd, const unsigned char* buf, size_t len, int timeoutSec)
{
    size_t off = 0;
    struct timeval tv;
    tv.tv_sec = timeoutSec > 0 ? timeoutSec : 5;
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    while (off < len) {
        ssize_t n = send(fd, buf + off, len - off, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        off += (size_t)n;
    }
    return 0;
}

// opcodes 25/32/70 with proper override quirks, mirroring xerox_mfp.c
static int twain_recv_exact(int fd, unsigned char* buf, size_t len, int timeoutSec)
{
    struct timeval tv;
    tv.tv_sec = timeoutSec > 0 ? timeoutSec : 8;
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    size_t got = 0;
    while (got < len) {
        ssize_t n = recv(fd, buf + got, len - got, 0);
        if (n <= 0) return -1;
        got += (size_t)n;
    }
    return 0;
}

static int twain_command(int fd, unsigned char cmd0, unsigned char* resp, size_t respLen)
{
    unsigned char pkt[64];
    size_t sendLen;
    if (cmd0 == TWAIN_CMD_SET_WINDOW) {
        respLen = 32;
    }
    if (cmd0 == TWAIN_CMD_READ_IMAGE) {
        // raw data transfer has no response header; caller reads blocklen bytes
        resp = NULL;
        respLen = 0;
    }
    memset(pkt, 0, sizeof pkt);
    pkt[0] = TWAIN_REQ_A;
    pkt[1] = TWAIN_REQ_B;
    pkt[2] = cmd0;
    if (cmd0 == TWAIN_CMD_READ_IMAGE) {
        // raw data transfer has no response header; caller reads blocklen bytes
        resp = NULL;
        respLen = 0;
    }
    memset(pkt, 0, sizeof pkt);
    pkt[0] = TWAIN_REQ_A;
    pkt[1] = TWAIN_REQ_B;
    pkt[2] = cmd0;
    if (cmd0 == TWAIN_CMD_SET_WINDOW) {
        pkt[3] = 0x13; // declared length
        sendLen = 25;  // actual wire length the firmware expects
    } else {
        pkt[3] = 0x00;
        sendLen = 4;
    }

    if (twain_send_all(fd, pkt, sendLen, 10) != 0) return -1;
    
    if (resp && respLen) {
        if (twain_recv_exact(fd, resp, respLen, 12) != 0) return -1;
        
    }
    return 0;
}

// send a one-byte-opcode command and read its short response; returns state
static int twain_state(int fd, unsigned char cmd0, unsigned char* respOut)
{
    unsigned char res[32];
    if (twain_command(fd, cmd0, res, 32) != 0) return -1;
    if (respOut) memcpy(respOut, res, 32);
    if (res[0] != TWAIN_RES_CODE) return -1;
    return res[1];
}

// issue a command, retrying while the fw reports busy; 1 on ready/good
static int twain_until_ready(int fd, unsigned char cmd0)
{
    int slept = 0;
    for (;;) {
        int st = twain_state(fd, cmd0, NULL);
        if (st < 0) return -1;
        if (st != TWAIN_STATUS_BUSY && st != TWAIN_STATUS_CHECK)
            return (st == TWAIN_STATUS_GOOD) ? 1 : -1;
        if (slept > 300000) return -1;
        usleep(50 * 1000);
        slept += 50;
    }
}

// In-process scan. Returns raw pixel buffer in *out (gray==bpp1 light, or
// RGB for pixelType==2), geometry from the negotiated window.
static ICAError ScanRaw(const ScanSettings* s,
                        unsigned char** out, unsigned* outW, unsigned* outH, unsigned* outBpp,
                        int (*progressFn)(int pct, void* ctx), void* ctx)
{
    if (out) *out = NULL;
    if (outW) *outW = 0;
    if (outH) *outH = 0;
    if (outBpp) *outBpp = 0;
    if (s->resolution < 1 || s->resolution > 1200) return kICADeviceInvalidParamErr;

    // parse "xerox_mfp:tcp <ip> <port>" from sanedev
    char devstr[128];
    snprintf(devstr, sizeof devstr, "%s", s->sanedev[0] ? s->sanedev : "xerox_mfp:tcp 127.0.0.1 9400");
    char ip[64] = "127.0.0.1";
    int port = 9400;
    {
        char* toks[8] = { NULL };
        int ti = 0;
        char* save = NULL;
        for (char* t = strtok_r(devstr, " ", &save); t && ti < 8; t = strtok_r(NULL, " ", &save))
            toks[ti++] = t;
        // toks: [xerox_mfp:tcp, ip, port]
        if (ti >= 3) {
            snprintf(ip, sizeof ip, "%s", toks[1]);
            port = (int)strtol(toks[2], NULL, 10);
            if (port <= 0) port = 9400;
        } else if (ti >= 2) {
            snprintf(ip, sizeof ip, "%s", toks[1]);
        }
    }

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return kICADeviceInternalErr;
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons((unsigned short)port);
    if (inet_pton(AF_INET, ip, &sa.sin_addr) != 1) {
        struct hostent* he = gethostbyname(ip);
        if (!he) { close(fd); return kICADeviceNotFoundErr; }
        memcpy(&sa.sin_addr, he->h_addr, he->h_length);
    }
    if (connect(fd, (struct sockaddr*)&sa, sizeof sa) != 0) {
        // The M2070's network stack can be slow to wake after idle; the
        // client fires an auto-overview immediately on session open and
        // aborts to the full scan on the first connect failure.  Retry a
        // few times before giving up so the overview has time to connect.
        
        int retried = 0;
        for (; retried < 5; retried++) {
            struct timeval tv = { 1, 0 };
            select(0, NULL, NULL, NULL, &tv);
            if (connect(fd, (struct sockaddr*)&sa, sizeof sa) == 0) break;
            
        }
        if (retried == 5) {
            close(fd);
            return kICADeviceNotOpenErr;
        }
    }

    // clear any stale engine session, like the original driver does on open
    twain_state(fd, TWAIN_CMD_ABORT, NULL);

    // INQUIRY
    unsigned char inq[70];
    memset(inq, 0, sizeof inq);
    if (twain_command(fd, TWAIN_CMD_INQUIRY, inq, sizeof inq) != 0 || inq[0] != TWAIN_RES_CODE) {
        
        close(fd);
        return kICADeviceNotFoundErr;
    }
    if (inq[3] != TWAIN_MSG_PRODUCT_INFO) {
        
        close(fd);
        return kICADeviceNotFoundErr;
    }

    unsigned maxWinWidth = (inq[0x28] << 24) | (inq[0x29] << 16) | (inq[0x2a] << 8) | inq[0x2b];
    unsigned maxWinLen   = (inq[0x2c] << 24) | (inq[0x2d] << 16) | (inq[0x2e] << 8) | inq[0x2f];

    // window geometry from the client request (mm)
    double wmm = s->width / s->resolution * 25.4;
    double hmm = s->height / s->resolution * 25.4;
    double oxmm = s->offsetX / s->resolution * 25.4;
    double oymm = s->offsetY / s->resolution * 25.4;
    if (wmm < 1) wmm = 1;
    if (hmm < 1) hmm = 1;

    // resolution -> resolution code
    static const int resCodes[] = { 75, 0, 150, 0, 0, 300, 0, 600, 1200, 200, 100, 2400, 4800, 9600 };
    int rcode = 0;
    for (int i = 0; i < (int)(sizeof resCodes / sizeof resCodes[0]); i++) {
        if (resCodes[i] == s->resolution) { rcode = i; break; }
    }

    unsigned winWidthPts  = (unsigned)(wmm * TWAIN_PNT_PER_MM);
    unsigned winLenPts    = (unsigned)(hmm * TWAIN_PNT_PER_MM);
    if (!winWidthPts || !winLenPts) { close(fd); return kICADeviceInvalidParamErr; }
    if (maxWinWidth && winWidthPts > maxWinWidth) winWidthPts = maxWinWidth;
    if (maxWinLen && winLenPts > maxWinLen) winLenPts = maxWinLen;

    double offXIn = oxmm / 25.4;
    double offYIn = oymm / 25.4;

    double pxToLen = 1213.9 / s->resolution;   // xerox: no-JPEG factor (gray)
    if (s->pixelType == 2) pxToLen = 1200.0 / s->resolution;  // JPEG: engine prints 1:1 (probe: 2480px/210mm @300dpi)
    unsigned paraW = (unsigned)(winWidthPts / pxToLen);
    unsigned paraH = (unsigned)(winLenPts / pxToLen);
    if (paraW == 0 || paraH == 0) { close(fd); return kICADeviceInvalidParamErr; }
    unsigned bpp = (s->pixelType == 2) ? 3 : 1;
    unsigned modeByte = (s->pixelType == 2) ? TWAIN_MODE_RGB24 : TWAIN_MODE_GRAY8;
    unsigned compByte = (s->pixelType == 2) ? TWAIN_COMP_JPG : 0x00;

    // RESERVE
    if (twain_until_ready(fd, TWAIN_CMD_RESERVE_UNIT) != 1) {
        
        twain_state(fd, TWAIN_CMD_ABORT, NULL);
        close(fd);
        return kICADeviceInternalErr;
    }
    int reserved = 1;

    unsigned char sw[25];
    memset(sw, 0, sizeof sw);
    sw[0] = TWAIN_REQ_A; sw[1] = TWAIN_REQ_B; sw[2] = TWAIN_CMD_SET_WINDOW; sw[3] = 0x13;
    sw[4] = TWAIN_MSG_SCANNING_PARAM;
    sw[5] = winWidthPts >> 24; sw[6] = winWidthPts >> 16; sw[7] = winWidthPts >> 8; sw[8] = winWidthPts;
    sw[9] = winLenPts >> 24; sw[10] = winLenPts >> 16; sw[11] = winLenPts >> 8; sw[12] = winLenPts;
    sw[13] = (unsigned char)rcode;
    sw[14] = (unsigned char)rcode;
    sw[15] = (unsigned char)floor(offXIn);
    sw[16] = (unsigned char)((offXIn - floor(offXIn)) * 100);
    sw[17] = (unsigned char)floor(offYIn);
    sw[18] = (unsigned char)((offYIn - floor(offYIn)) * 100);
    sw[19] = modeByte;
    sw[20] = compByte; // JPEG: off for gray, REQUIRED (0x06) for RGB24 on this engine
    sw[22] = 0x02; // threshold (50%: (50-30)/10)
    sw[23] = TWAIN_DOC_FLATBED;

    unsigned char rsw[32];
    memset(rsw, 0, sizeof rsw);
    int sw_err = 1;
    for (int attempt = 0; attempt < 20 && sw_err; attempt++) {
        memset(rsw, 0, sizeof rsw);
        if (twain_send_all(fd, sw, 25, 10) != 0 || twain_recv_exact(fd, rsw, 32, 12) != 0 || rsw[0] != TWAIN_RES_CODE) {
            
            break;
        }

        int st = rsw[1];
        if (st == TWAIN_STATUS_GOOD || st == TWAIN_STATUS_BUSY) {
            sw_err = 0;
            break;
        }
        if (st != TWAIN_STATUS_CHECK) {
            
            break;
        }
        
        usleep(500 * 1000);
    }
    if (sw_err) {
        twain_state(fd, TWAIN_CMD_RELEASE_UNIT, NULL);
        close(fd);
        return kICADeviceInternalErr;
    }

    // issue OBJPOS once, like the working probe: the engine may answer busy
    // during warmup; readiness is handled by the READ polling loop below.
    // Re-issuing OBJPOS repeatedly during warmup (twain_until_ready) is the
    // one protocol behavior that differs from the proven probe path.
    unsigned char ors[32];
    memset(ors, 0, sizeof ors);
    if (twain_command(fd, TWAIN_CMD_OBJECT_POS, ors, sizeof ors) != 0) {
        
    } else {
        
    }
    // original driver pauses Timeouts[0x3c]=300ms after OBJPOS before reading
    usleep(300 * 1000);

    unsigned char* pixels = calloc((size_t)paraW * paraH * bpp, 1);
    if (!pixels) {
        twain_state(fd, TWAIN_CMD_RELEASE_UNIT, NULL);
        close(fd);
        return kICADeviceInternalErr;
    }

    unsigned yOff = 0;
    int done = 0;
    int failed = 0;
    int totalPct = -1;
    int busyPolls = 0;
    int tailPolls = 0;
    long readStart = 0;
    { struct timeval tv; gettimeofday(&tv, 0); readStart = (long)tv.tv_sec * 1000 + tv.tv_usec / 1000; }
    while (!done && !failed) {
        {
            struct timeval tv; gettimeofday(&tv, 0);
            long now = (long)tv.tv_sec * 1000 + tv.tv_usec / 1000;
            // hard watchdog: never hang the icdd callback for more than 8 min
            if (now - readStart > 600000) { failed = 1; break; }
        }
        // READ (CheckBlock): get block attributes or a busy indication.
        unsigned char rb[32];
        int st = -1;
        int timeouts = 0;
        for (;;) {
            memset(rb, 0, sizeof rb);
            if (twain_command(fd, TWAIN_CMD_READ, rb, sizeof rb) != 0 || rb[0] != TWAIN_RES_CODE) {
                
                failed = 1;
                break;
            }
            st = rb[1];
            if (st == TWAIN_STATUS_GOOD || st == TWAIN_STATUS_CHECK) break; // parse block below
            if (st == TWAIN_STATUS_CANCEL) {
                
                failed = 1;
                break;
            }
            if (st != TWAIN_STATUS_BUSY) {
                
                failed = 1;
                break;
            }
            if (timeouts > 1500) { failed = 1; break; }
            // original driver: Timeouts[0x34]=200ms between polls, up to
            // Timeouts[0x30]=300000ms (5 min) total - the lamp needs this long
            // to warm up after a cold boot, and abandoning early wedges the engine.
            usleep(200 * 1000);
            timeouts++;
        }
        if (failed) break;
        if (st != TWAIN_STATUS_GOOD && st != TWAIN_STATUS_CHECK) { failed = 1; break; }

        unsigned blocklen = ((unsigned)rb[4] << 24) | ((unsigned)rb[5] << 16) | ((unsigned)rb[6] << 8) | rb[7];
        unsigned vertical  = ((unsigned)rb[8] << 8) | rb[9];
        unsigned horizontal = ((unsigned)rb[10] << 8) | rb[11];
        int lastBlock = (rb[3] == TWAIN_MSG_END_BLOCK);
        if (horizontal > 32768 || vertical > 32768 || blocklen > 64 * 1024 * 1024) {
            
            failed = 1;
            break;
        }

// busy / progress indication: the fw announces a <=16-byte block but
    // does NOT send a payload; just keep polling (original LooksLikeBusyIndication).
    if (blocklen <= 16) {
        if (lastBlock) { done = 1; break; }
        if (busyPolls > 1500) { failed = 1; break; }   // 5 min at 200ms
        struct timeval tv; gettimeofday(&tv, 0);
        if (busyPolls % 25 == 0)
            
        usleep(200 * 1000);
        busyPolls++;
        continue;
    }

    struct timeval btv; gettimeofday(&btv, 0);

    // request the block data: READ_IMAGE (0x29) triggers the fw to send the
    // announced blocklen raw bytes (the 0x28 reply was a header only)
    if (twain_command(fd, TWAIN_CMD_READ_IMAGE, NULL, 0) != 0) {
        
        failed = 1;
        break;
    }

    unsigned char* blk = malloc(blocklen);
    if (!blk) { failed = 1; break; }
    size_t got = 0;
    while (got < blocklen) {
        struct timeval tv; tv.tv_sec = 12; tv.tv_usec = 0;
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        ssize_t n = recv(fd, blk + got, blocklen - got, 0);
        if (n <= 0) { free(blk); failed = 1; break; }
        got += (size_t)n;
    }
    if (failed) { free(blk); break; }

    if (bpp == 1) {
        // copy this block's rows into the output plane (trim to para)
        for (unsigned r = 0; r < vertical; r++) {
            unsigned dstRow = yOff + r;
            if (dstRow >= paraH) continue;
            const unsigned char* src = blk + (size_t)r * horizontal;
            if ((size_t)r * horizontal + paraW > blocklen) break;
            for (unsigned c = 0; c < paraW && c < horizontal; c++) {
                pixels[(size_t)dstRow * paraW + c] = src[c];
            }
        }
        free(blk);
    } else {
        // RGB24: the engine encodes every block as a self-contained JPEG frame
        // (probe: SOI..EOI, ~2480x32). Decode it with ImageIO and write the
        // real rows. `horizontal/vertical` from the READ reply describe the
        // encoded block and are NOT trustworthy pixel dimensions.
        CGImageRef jimg = NULL;
        CFDataRef cfdata = CFDataCreate(NULL, blk, blocklen);
        if (cfdata) {
            CGImageSourceRef src = CGImageSourceCreateWithData(cfdata, NULL);
            CFRelease(cfdata);
            if (src) {
                jimg = CGImageSourceCreateImageAtIndex(src, 0, NULL);
                CFRelease(src);
            }
        }
        if (!jimg) {
            
            failed = 1;
            free(blk);
            break;
        }
        size_t dw = CGImageGetWidth(jimg), dh = CGImageGetHeight(jimg);
        if (dw < 1 || dw > 32768 || dh < 1 || dh > 32768) {
            
            CGImageRelease(jimg);
            failed = 1;
            free(blk);
            break;
        }
        if (dw > paraW) dw = paraW;   // engine may over-send width; trim to para
        if (dh > paraH - yOff) dh = paraH - yOff;
        // CGBitmapContextCreate refuses 24-bit contexts; draw into RGBA (like
        // ResamplePixels) and pack down to RGB24 per row.
        size_t sbpr = dw * 4;
        unsigned char* rgba = calloc(dh * sbpr, 1);
        if (!rgba) { CGImageRelease(jimg); free(blk); failed = 1; break; }
        CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
        CGContextRef cctx = CGBitmapContextCreate(rgba, dw, dh, 8, sbpr, cs,
                                                  kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big);
        CGColorSpaceRelease(cs);
        if (!cctx) { free(rgba); CGImageRelease(jimg); free(blk); failed = 1; break; }
        CGContextSetInterpolationQuality(cctx, kCGInterpolationNone);
        CGContextDrawImage(cctx, CGRectMake(0, 0, dw, dh), jimg);
        CGContextRelease(cctx);
        CGImageRelease(jimg);
        for (unsigned r = 0; r < dh; r++) {
            unsigned char* drow = pixels + (size_t)(yOff + r) * paraW * 3;
            const unsigned char* srow = rgba + (size_t)r * sbpr;
            for (size_t x = 0; x < dw; x++) {
                drow[x * 3 + 0] = srow[x * 4 + 0];
                drow[x * 3 + 1] = srow[x * 4 + 1];
                drow[x * 3 + 2] = srow[x * 4 + 2];
            }
        }
        free(rgba);
        free(blk);
        vertical = (unsigned)dh;
    }

    yOff += vertical;

        int pct = (int)((double)yOff / paraH * 100.0);
        if (pct > 100) pct = 100;
        if (pct != totalPct) {
            totalPct = pct;
            if (pct < 2) pct = 2;
            if (pct > 98) pct = 98;
            if (progressFn) progressFn(pct, ctx);
        }

        // end the transfer ONLY on the END_BLOCK (0x81) trailer: the fw delivers
        // it once the rows reach the window bottom, and stopping early leaves the
        // engine mid-session (this is what wedged it repeatedly).
        if (lastBlock) done = 1;
        else if (yOff >= paraH) {
            if (tailPolls++ > 2000) { failed = 1; break; }
            continue;
        }
    }

    // teardown in the original's ModelFinishScanning order: ABORT then RELEASE.
    if (reserved) {
        twain_state(fd, TWAIN_CMD_ABORT, NULL);
        twain_state(fd, TWAIN_CMD_RELEASE_UNIT, NULL);
    }
    close(fd);

    if (failed || !done) {
        
        free(pixels);
        return kICADeviceInternalErr;
    }

    if (progressFn) progressFn(100, ctx);

    if (out) *out = pixels;
    if (outW) *outW = paraW;
    if (outH) *outH = paraH;
    if (outBpp) *outBpp = bpp;
    return noErr;
}

static int gProgWidth, gProgHeight, gProgPixelType;

static int ProgressNoData(int pct, void* ctx)
{
    ICAObject obj = *(ICAObject*)ctx;
    UInt32 bpp = (gProgPixelType == 1 || gProgPixelType == 0) ? 1 : 3;
    UInt32 bpl = (UInt32)gProgWidth * bpp;
    UInt32 startRow = (UInt32)((long)pct * gProgHeight / 100);
    if ((long)startRow > gProgHeight) startRow = (UInt32)gProgHeight;
    SendProgressBand(obj, (UInt32)gProgWidth, (UInt32)gProgHeight,
                     bpl, startRow, 1, 0, NULL, gProgPixelType);
    return 0;
}

// NoData full scans complete by the DRIVER writing the finished image into the
// client's security-scoped output folder (verified vs the working original
// driver with fs_usage). Mirror its atomic pattern: hidden temp then rename.
static ICAError WriteDocumentFile(const unsigned char* pixels, unsigned w, unsigned h, int bpp,
                                  const char* folder, const char* name, const char* ext,
                                  char* outPath, size_t outPathSize)
{
    if (!pixels || w == 0 || h == 0 || !folder || !folder[0] || !name || !name[0] || !ext || !ext[0])
        return kICADeviceInvalidParamErr;

    char tmpPath[768], finalPath[768];
    snprintf(finalPath, sizeof finalPath, "%s/%s.%s", folder, name, ext);
    snprintf(tmpPath, sizeof tmpPath, "%s/.%s.%s-%05x", folder, name, ext,
             (unsigned)(getpid() ^ (unsigned)time(0)) & 0xfffffu);
    tmpPath[767] = 0;

    CGColorSpaceRef cs = (bpp == 1) ? CGColorSpaceCreateDeviceGray() : CGColorSpaceCreateDeviceRGB();
    CGDataProviderRef prov = CGDataProviderCreateWithData(NULL, pixels, (size_t)w * h * bpp, NULL);
    CGBitmapInfo bi = (bpp == 1) ? (CGBitmapInfo)kCGImageAlphaNone
                                 : (CGBitmapInfo)(kCGImageAlphaNone | kCGImageByteOrderDefault);
    CGImageRef img = CGImageCreate(w, h, 8, (size_t)bpp * 8, (size_t)w * bpp, cs, bi, prov, NULL,
                                   false, kCGRenderingIntentDefault);
    CGDataProviderRelease(prov);
    CGColorSpaceRelease(cs);
    if (!img) return kICADeviceInternalErr;

    CFURLRef url = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8*)tmpPath,
                                                           strlen(tmpPath), false);
    CGImageDestinationRef dst = CGImageDestinationCreateWithURL(url, CFSTR("public.jpeg"), 1, NULL);
    CFRelease(url);
    if (!dst) { CGImageRelease(img); return kICADeviceMemoryAllocationErr; }

    CGImageDestinationAddImage(dst, img, NULL);
    CGImageRelease(img);
    bool ok = CGImageDestinationFinalize(dst);
    CFRelease(dst);
    if (!ok) { unlink(tmpPath); return kICADeviceInternalErr; }

    if (rename(tmpPath, finalPath) != 0) { unlink(tmpPath); return kICADeviceInternalErr; }
    if (outPath && outPathSize > 0) snprintf(outPath, outPathSize, "%s", finalPath);
    
    return noErr;
}

// Run the full scan synchronously.  Called from Start() on icdd's callback
// thread, matching the original Samsung driver's blocking behaviour.
static ICAError RunScanSynchronous(const ScannerObjectInfo* deviceObjectInfo,
                                  ICAObject scanObject)
{
    ScanSettings s;
    memcpy(&s, &gSettings, sizeof s);
    s.object = scanObject;

    pthread_mutex_lock(&gScanLock);

    gProgWidth = (int)s.width;
    gProgHeight = (int)s.height;
    gProgPixelType = (int)s.pixelType;

    UInt32 w = 0, h = 0;
    int bpp = (s.pixelType == 2) ? 3 : 1;
    unsigned char* pixels = NULL;
    ICAError err = noErr;

    err = ScanRaw(&s, &pixels, &w, &h, (unsigned*)&bpp, ProgressNoData, &s.object);
    if (err != noErr) {
        
        SendSimpleNotification(s.object, kICANotificationTypeScannerScanDone);
        pthread_mutex_unlock(&gScanLock);
        return err;
    }

    if (pixels == NULL) {
        
        SendSimpleNotification(s.object, kICANotificationTypeScannerScanDone);
        pthread_mutex_unlock(&gScanLock);
        return kICADeviceInternalErr;
    }

    UInt32 dw = (UInt32)s.width, dh = (UInt32)s.height;
    if (dw > 0 && dh > 0 && (dw != w || dh != h)) {
        unsigned char* rs = ResamplePixels(pixels, w, h, dw, dh, bpp);
        free(pixels);
        if (!rs) rs = NULL;
        pixels = rs;
        w = dw; h = dh;
        
    }

    UInt32 bpl = w * bpp;

    ICAError bandErr = noErr;
    ICAObject newObj = kICANoObject;
    char finalPath[768];
    finalPath[0] = 0;

    if (s.isOverview) {
        // WithData overview: push raw bands (proven reliable path).
        bandErr = SendProgressBand(s.object, w, h, bpl, 0, 0, 0, NULL, s.pixelType);
        const UInt32 chunkRows = 64;
        for (UInt32 row = 0; row < h && bandErr == noErr; row += chunkRows) {
            UInt32 n = (h - row < chunkRows) ? (h - row) : chunkRows;
            bandErr = SendProgressBand(s.object, w, h, bpl, row, n, bpl * n, pixels + row * bpl,
                                       s.pixelType);
        }
    } else {
        // NoData full scan: the driver writes the finished JPEG into the
        // client's security-scoped output folder (mirrors the original
        // driver's SaveImage2).  Arm the last-scan state BEFORE the object
        // creation below: icdd's newObjectCreated:-chain calls our
        // f_ICD_ScannerGetObjectInfo and returns its result verbatim, so
        // without gLastScanValid set it replies kICADeviceNotFoundErr (-9901).
        char tmpPath[768];
        tmpPath[0] = 0;
        ICAError werr = WriteDocumentFile(pixels, w, h, bpp,
                                          s.docFolder, s.docName, s.docExt,
                                          tmpPath, sizeof(tmpPath));
        if (werr != noErr) {
            
            pthread_mutex_unlock(&gScanLock);
            return werr;
        }
        snprintf(finalPath, sizeof(finalPath), "%s", tmpPath);

        // dataSize = the file's on-disk size (matches the original driver,
        // which lstat()s the saved document for GetObjectInfo)
        struct stat st;
        memset(&st, 0, sizeof st);
        long fsize = 0;
        if (stat(finalPath, &st) == 0) fsize = (long)st.st_size;

        pthread_mutex_lock(&gScanObjectLock);
        memset(&gLastScanObjInfo, 0, sizeof(gLastScanObjInfo));
        gLastScanObjInfo.icaObjectInfo.objectType = kICAFile;    // 'file'
        gLastScanObjInfo.icaObjectInfo.objectSubtype = kICAFileImage; // 'imag'
        snprintf((char*)gLastScanObjInfo.name, sizeof(gLastScanObjInfo.name),
                 "%s.%s", s.docName, s.docExt);
        strncpy((char*)gLastScanObjInfo.creationDate, "0000:00:00 00:00:00",
                sizeof(gLastScanObjInfo.creationDate));
        gLastScanObjInfo.dataSize = (UInt32)fsize;
        gLastScanPng[0] = 0;
        snprintf(gLastScanPng, sizeof(gLastScanPng), "%s", finalPath);
        gLastScanValid = 1;
        pthread_mutex_unlock(&gScanObjectLock);
        
    }
    free(pixels);

    CFStringRef docName = NULL;
    if (finalPath[0] != 0)
        docName = CFStringCreateWithCString(kCFAllocatorDefault, finalPath,
                                            kCFStringEncodingUTF8);
    SendPageDone(s.object, docName);
    if (docName) CFRelease(docName);
    SendSimpleNotification(s.object, kICANotificationTypeScannerScanDone);

    // object creation completes the NoData document delivery: icdd resolves
    // the parent via ScannerObjectInfo.reserved (its internal ICDDevice
    // pointer).  If the live pointer lost that field, fall back to the value
    // captured at open time so icdd does not reply kICADeviceNotFoundErr.
    if (gDeviceObjectInfoValid && !s.isOverview) {
        const ScannerObjectInfo* parent = gLiveDeviceObjectInfo;
        ScannerObjectInfo parentCopy;
        if (parent->reserved == 0 && gDeviceReservedFallback != 0) {
            parentCopy = *parent;
            parentCopy.reserved = gDeviceReservedFallback;
            parent = &parentCopy;
        }
        ICDScannerNewObjectInfoCreated(parent, 0, &newObj);
        if (newObj != kICANoObject) {
            // record the live ICAObject so GetObjectInfo -> newObj and
            // ReadFileData can serve the saved JPEG for this document
            pthread_mutex_lock(&gScanObjectLock);
            gLastScanObjInfo.icaObject = newObj;
            gLastScanObject = newObj;
            pthread_mutex_unlock(&gScanObjectLock);
            SendSimpleNotification(newObj, kICANotificationTypeObjectAdded);
            
        }
    } else if (gDeviceObjectInfoValid && s.isOverview) {
        
    } else if (!gDeviceObjectInfoValid) {
    }

    pthread_mutex_unlock(&gScanLock);
    return noErr;
}

static ICAError Start(const ScannerObjectInfo* deviceObjectInfo, ICD_ScannerStartPB* pb)
{

    gSettings.object = pb->object;

    if (deviceObjectInfo) {
        gLiveDeviceObjectInfo = deviceObjectInfo;
        gDeviceObjectInfoValid = 1;
        
    } else {
        gLiveDeviceObjectInfo = NULL;
        gDeviceObjectInfoValid = 0;
    }
    if (deviceObjectInfo && deviceObjectInfo->privateData) {
        DeviceState* st = (DeviceState*)deviceObjectInfo->privateData;
        snprintf(gSettings.sanedev, sizeof gSettings.sanedev, "%s", st->sanedev);
    } else {
        snprintf(gSettings.sanedev, sizeof gSettings.sanedev, "xerox_mfp:tcp 127.0.0.1 9400");
    }

    RunScanSynchronous(deviceObjectInfo, pb->object);
    return noErr;
}

static ICAError WriteDataToFile(const ScannerObjectInfo* objectInfo, FILE* file,
                                UInt32 offset, long* length)
{
    (void)objectInfo; (void)file; (void)offset; (void)length;
    
    return kICADeviceUnsupportedErr;
}

static ICAError WriteDataToFileDescriptor(const ScannerObjectInfo* objectInfo, int fd,
                                          UInt32 offset, long* length)
{
    (void)objectInfo; (void)fd; (void)offset; (void)length;
    
    return kICADeviceUnsupportedErr;
}

static ICAError WriteDataToFileDescriptor64(const ScannerObjectInfo* objectInfo, int fd)
{
    (void)objectInfo; (void)fd;
    
    return kICADeviceUnsupportedErr;
}

// ---------------------------------------------------------------------------
// callback table (order must match ICD_scanner_callback_functions)
// ---------------------------------------------------------------------------

static ICD_scanner_callback_functions gCallbacks = {
    .f_ICD_ScannerOpenUSBDevice                      = OpenUSBDevice,
    .f_ICD_ScannerOpenUSBDeviceWithIORegPath         = OpenUSBDeviceWithIORegPath,
    .f_ICD_ScannerCloseDevice                        = CloseDevice,
    .f_ICD_ScannerPeriodicTask                       = PeriodicTask,
    .f_ICD_ScannerGetObjectInfo                      = GetObjectInfo,
    .f_ICD_ScannerCleanup                            = Cleanup,
    .f_ICD_ScannerGetPropertyData                    = StandardGetPropertyData,
    .f_ICD_ScannerSetPropertyData                    = SetPropertyData,
    .f_ICD_ScannerReadFileData                       = ReadFileData,
    .f_ICD_ScannerWriteFileData                      = WriteFileData,
    .f_ICD_ScannerSendMessage                        = SendMessage,
    .f_ICD_ScannerAddPropertiesToCFDictionary        = AddPropertiesToCFDictionary,
    .f_ICD_ScannerOpenFireWireDevice                 = OpenFireWireDevice,
    .f_ICD_ScannerOpenFireWireDeviceWithIORegPath    = OpenFireWireDeviceWithIORegPath,
    .f_ICD_ScannerOpenSession                        = OpenSession,
    .f_ICD_ScannerCloseSession                       = CloseSession,
    .f_ICD_ScannerInitialize                         = Initialize,
    .f_ICD_ScannerGetParameters                      = GetParameters,
    .f_ICD_ScannerSetParameters                      = SetParameters,
    .f_ICD_ScannerStatus                             = Status,
    .f_ICD_ScannerStart                              = Start,
    .f_ICD_ScannerOpenBluetoothDevice                = OpenBluetoothDevice,
    .f_ICD_ScannerOpenTCPIPDevice                    = OpenTCPIPDevice,
    .f_ICD_ScannerWriteDataToFile                    = WriteDataToFile,
    .f_ICD_ScannerOpenMassStorageDevice              = OpenMassStorageDevice,
    .f_ICD_ScannerWriteDataToFileDescriptor          = WriteDataToFileDescriptor,
    .f_ICD_ScannerWriteDataToFileDescriptor64        = WriteDataToFileDescriptor64,
};

int main(int argc, const char* argv[])
{
    gICDScannerCallbackFunctions = gCallbacks;
    return ICD_ScannerMain(argc, argv);
}