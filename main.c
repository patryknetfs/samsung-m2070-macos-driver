// Native arm64 Image Capture device module (ICDM) for Samsung M2070 series
// network scanners (SANE xerox_mfp backend at _scanner._tcp / port 9400).
//
// Mirrors the parameter construction logic of the original (x86_64
// Rosetta-only) "Samsung Scanner.app" driver.  The same xerox_mfp command set
// is spoken over two transports: TCP/9400 (Wi-Fi / Ethernet, found via
// Bonjour) and the vendor-specific bulk interface of the USB device.
//
// Build/install target: /Library/Image Capture/Devices/Samsung Scanner.app

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
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
#include <IOKit/IOCFPlugIn.h>
#include <IOKit/usb/IOUSBLib.h>

#include <ImageIO/ImageIO.h>
#include <CoreGraphics/CoreGraphics.h>

#include <ICADevices/ICADevices.h>
#include <os/log.h>

// diagnostics:  log stream --info --predicate 'subsystem == "pl.m2070.scanner"'
static os_log_t DrvLog(void)
{
    static os_log_t l;
    if (!l) l = os_log_create("pl.m2070.scanner", "driver");
    return l;
}
#define LOG(fmt, ...) os_log(DrvLog(), fmt, ##__VA_ARGS__)

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
    int isUSB;                 // 1: talk over USB bulk pipes, 0: TCP
    UInt32 usbLocationID;
    char usbIORegPath[512];    // io_string_t
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
    int isUSB;
    UInt32 usbLocationID;
    char usbIORegPath[512];
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
        if (txt && CFGetTypeID(txt) == CFDictionaryGetTypeID()) {
            CFTypeRef mdl = CFDictionaryGetValue((CFDictionaryRef)txt, kMDLKey);
            CFTypeRef mfg = CFDictionaryGetValue((CFDictionaryRef)txt, kMFGKey);
            if (mdl && CFGetTypeID(mdl) == CFStringGetTypeID()) name = (CFStringRef)mdl;
            else if (mfg && CFGetTypeID(mfg) == CFStringGetTypeID()) name = (CFStringRef)mfg;
        }
    }
    if (name) {
        CFStringGetCString(name, st->name, sizeof st->name, kCFStringEncodingUTF8);
    }
    if (!st->name[0]) strncpy(st->name, "Samsung M2070 Series", sizeof st->name - 1);

    // build the SANE device string from the open params
    CFTypeRef ip = CFDictionaryGetValue(params, CFSTR("ipAddress"));
    if (!ip || CFGetTypeID(ip) != CFStringGetTypeID() ||
        !CFStringGetCString((CFStringRef)ip, st->ip, sizeof st->ip, kCFStringEncodingUTF8)) {
        strncpy(st->ip, "127.0.0.1", sizeof st->ip - 1);
    }
    CFTypeRef port = CFDictionaryGetValue(params, CFSTR("ipPort"));
    long p = 9400;
    if (port && CFGetTypeID(port) == CFNumberGetTypeID())
        CFNumberGetValue((CFNumberRef)port, kCFNumberLongType, &p);
    if (p <= 0 || p > 65535) p = 9400;
    snprintf(st->sanedev, sizeof st->sanedev, "xerox_mfp:tcp %s %ld", st->ip, p);
}

// ---------------------------------------------------------------------------
// callbacks
// ---------------------------------------------------------------------------

// Shared by the TCP and USB open paths: reset the object info while keeping
// icdd's `reserved` back-pointer, and attach a fresh DeviceState.
static DeviceState* InitDeviceObject(ScannerObjectInfo* objectInfo)
{
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

    DeviceState* st = calloc(1, sizeof(DeviceState));
    if (!st) return NULL;
    objectInfo->flags = 0;
    objectInfo->privateData = (Ptr)st;
    return st;
}

static void SetDeviceObjectName(ScannerObjectInfo* objectInfo, const DeviceState* st)
{
    strncpy((char*)objectInfo->name, st->name, 31);
    objectInfo->name[31] = 0;
}

static ICAError OpenTCPIPDevice(CFDictionaryRef params, ScannerObjectInfo* objectInfo)
{
    if (!objectInfo || !params) return kICADeviceInvalidParamErr;

    DeviceState* st = InitDeviceObject(objectInfo);
    if (!st) return kICADeviceMemoryAllocationErr;
    StateFromUserInfo(st, params);
    SetDeviceObjectName(objectInfo, st);
    LOG("OpenTCPIPDevice name=%{public}s dev=%{public}s", st->name, st->sanedev);
    return noErr;
}

// Model name for a USB-attached device, from its idProduct.
static const char* USBModelName(io_service_t service)
{
    const char* name = "Samsung M2070 Series";
    if (!service) return name;
    CFTypeRef pid = IORegistryEntryCreateCFProperty(service, CFSTR("idProduct"),
                                                    kCFAllocatorDefault, 0);
    if (pid) {
        long v = 0;
        if (CFGetTypeID(pid) == CFNumberGetTypeID() &&
            CFNumberGetValue((CFNumberRef)pid, kCFNumberLongType, &v) && v == 0x3488)
            name = "Samsung M2060 Series";
        CFRelease(pid);
    }
    return name;
}

static ICAError OpenUSBCommon(UInt32 locationID, const char* ioregPath,
                              ScannerObjectInfo* objectInfo)
{
    if (!objectInfo) return kICADeviceInvalidParamErr;

    DeviceState* st = InitDeviceObject(objectInfo);
    if (!st) return kICADeviceMemoryAllocationErr;
    st->isUSB = 1;
    st->usbLocationID = locationID;
    if (ioregPath) snprintf(st->usbIORegPath, sizeof st->usbIORegPath, "%s", ioregPath);

    io_service_t service = ioregPath && ioregPath[0]
        ? IORegistryEntryFromPath(kIOMainPortDefault, ioregPath) : IO_OBJECT_NULL;
    snprintf(st->name, sizeof st->name, "%s", USBModelName(service));
    if (service) IOObjectRelease(service);
    snprintf(st->sanedev, sizeof st->sanedev, "xerox_mfp:libusb 0x%08x", (unsigned)locationID);
    SetDeviceObjectName(objectInfo, st);
    LOG("OpenUSBDevice loc=0x%08x path=%{public}s name=%{public}s",
        (unsigned)locationID, st->usbIORegPath, st->name);
    return noErr;
}

static ICAError OpenUSBDevice(UInt32 locationID, ScannerObjectInfo* objectInfo)
{
    return OpenUSBCommon(locationID, NULL, objectInfo);
}

static ICAError OpenUSBDeviceWithIORegPath(UInt32 locationID, io_string_t ioregPath,
                                           ScannerObjectInfo* objectInfo)
{
    return OpenUSBCommon(locationID, ioregPath, objectInfo);
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
    if ((long)want > total - (long)offset) want = (UInt32)(total - (long)offset);
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
    LOG("OpenSession");
    
    (void)deviceObjectInfo;
    return noErr;
}

static ICAError CloseSession(const ScannerObjectInfo* deviceObjectInfo,
                             ICD_ScannerCloseSessionPB* pb)
{
    LOG("CloseSession");
    
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
    LOG("GetParameters");

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
    if (!d) { LOG("SetParameters: no userScanArea/device dict"); (void)deviceObjectInfo; return noErr; }

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
    LOG("SetParameters res=%ld type=%ld depth=%ld off=%.0fx%.0f size=%.0fx%.0f overview=%d ext=%{public}s",
        st.resolution, st.pixelType, st.bitDepth, st.offsetX, st.offsetY, st.width, st.height,
        st.isOverview, st.docExt);
    
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

// ---------------------------------------------------------------------------
// transport: the same command packets go over TCP/9400 or USB bulk pipes
// ---------------------------------------------------------------------------

typedef struct ScanConn {
    int fd;                                   // TCP socket, -1 when USB
    IOUSBInterfaceInterface300** intf;        // USB scanner interface, opened
    UInt8 pipeIn, pipeOut;
} ScanConn;

static int conn_send(ScanConn* c, const unsigned char* buf, size_t len, int timeoutSec)
{
    if (timeoutSec <= 0) timeoutSec = 5;
    if (c->intf) {
        IOReturn kr = (*c->intf)->WritePipeTO(c->intf, c->pipeOut, (void*)buf, (UInt32)len,
                                              (UInt32)timeoutSec * 1000, (UInt32)timeoutSec * 1000);
        if (kr == kIOUSBPipeStalled) {
            (*c->intf)->ClearPipeStallBothEnds(c->intf, c->pipeOut);
            kr = (*c->intf)->WritePipeTO(c->intf, c->pipeOut, (void*)buf, (UInt32)len,
                                         (UInt32)timeoutSec * 1000, (UInt32)timeoutSec * 1000);
        }
        return kr == kIOReturnSuccess ? 0 : -1;
    }

    size_t off = 0;
    struct timeval tv;
    tv.tv_sec = timeoutSec;
    tv.tv_usec = 0;
    setsockopt(c->fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    while (off < len) {
        ssize_t n = send(c->fd, buf + off, len - off, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        off += (size_t)n;
    }
    return 0;
}

// read exactly `len` bytes (USB: possibly over several bulk transfers)
static int conn_recv_exact(ScanConn* c, unsigned char* buf, size_t len, int timeoutSec)
{
    if (timeoutSec <= 0) timeoutSec = 8;
    size_t got = 0;
    if (c->intf) {
        while (got < len) {
            UInt32 n = (UInt32)((len - got) > (1u << 20) ? (1u << 20) : (len - got));
            IOReturn kr = (*c->intf)->ReadPipeTO(c->intf, c->pipeIn, buf + got, &n,
                                                 (UInt32)timeoutSec * 1000,
                                                 (UInt32)timeoutSec * 1000);
            if (kr == kIOUSBPipeStalled) {
                (*c->intf)->ClearPipeStallBothEnds(c->intf, c->pipeIn);
                return -1;
            }
            if (kr != kIOReturnSuccess) return -1;
            if (n == 0) continue;   // zero-length packet
            got += n;
        }
        return 0;
    }

    struct timeval tv;
    tv.tv_sec = timeoutSec;
    tv.tv_usec = 0;
    setsockopt(c->fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    while (got < len) {
        ssize_t n = recv(c->fd, buf + got, len - got, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        got += (size_t)n;
    }
    return 0;
}

// USB only: discard stale bulk-in data left by an interrupted session so the
// next command does not read a previous reply (sane usb_dev_flush)
static void conn_flush(ScanConn* c)
{
    if (!c->intf) return;
    unsigned char buf[512];
    for (int i = 0; i < 64; i++) {
        UInt32 n = sizeof buf;
        if ((*c->intf)->ReadPipeTO(c->intf, c->pipeIn, buf, &n, 100, 100) != kIOReturnSuccess || n == 0)
            break;
    }
}

static void conn_close(ScanConn* c)
{
    if (c->intf) {
        (*c->intf)->USBInterfaceClose(c->intf);
        (*c->intf)->Release(c->intf);
        c->intf = NULL;
    }
    if (c->fd >= 0) {
        close(c->fd);
        c->fd = -1;
    }
}

// connect with a timeout; a fresh socket per address/attempt (a socket whose
// connect() failed cannot be reused on BSD)
static int tcp_connect(const char* host, int port, int timeoutSec)
{
    char portStr[16];
    snprintf(portStr, sizeof portStr, "%d", port);
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, portStr, &hints, &res) != 0 || !res) return -1;

    int fd = -1;
    for (struct addrinfo* ai = res; ai && fd < 0; ai = ai->ai_next) {
        int s = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (s < 0) continue;
        int one = 1;
        setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
        setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
        int fl = fcntl(s, F_GETFL, 0);
        fcntl(s, F_SETFL, fl | O_NONBLOCK);
        int rc = connect(s, ai->ai_addr, ai->ai_addrlen);
        if (rc != 0 && errno == EINPROGRESS) {
            fd_set wfds;
            FD_ZERO(&wfds);
            FD_SET(s, &wfds);
            struct timeval tv = { timeoutSec, 0 };
            rc = -1;
            if (select(s + 1, NULL, &wfds, NULL, &tv) == 1) {
                int soerr = 0;
                socklen_t sl = sizeof soerr;
                if (getsockopt(s, SOL_SOCKET, SO_ERROR, &soerr, &sl) == 0 && soerr == 0) rc = 0;
            }
        }
        if (rc == 0) {
            fcntl(s, F_SETFL, fl);
            fd = s;
        } else {
            close(s);
        }
    }
    freeaddrinfo(res);
    return fd;
}

// locate the IOUSBHostDevice for the scanner: by registry path, else by
// vendor + locationID
static io_service_t FindUSBService(const ScanSettings* s)
{
    if (s->usbIORegPath[0]) {
        io_service_t e = IORegistryEntryFromPath(kIOMainPortDefault, s->usbIORegPath);
        if (e) return e;
    }
    static const char* classes[] = { "IOUSBHostDevice", "IOUSBDevice" };
    for (int ci = 0; ci < 2; ci++) {
        CFMutableDictionaryRef m = IOServiceMatching(classes[ci]);
        if (!m) continue;
        DictSetLong(m, CFSTR("idVendor"), 0x04e8);
        io_iterator_t it = IO_OBJECT_NULL;
        if (IOServiceGetMatchingServices(kIOMainPortDefault, m, &it) != KERN_SUCCESS) continue;
        io_service_t svc, found = IO_OBJECT_NULL;
        while (!found && (svc = IOIteratorNext(it))) {
            CFTypeRef loc = IORegistryEntryCreateCFProperty(svc, CFSTR("locationID"),
                                                            kCFAllocatorDefault, 0);
            long v = -1;
            if (loc) {
                if (CFGetTypeID(loc) == CFNumberGetTypeID())
                    CFNumberGetValue((CFNumberRef)loc, kCFNumberLongType, &v);
                CFRelease(loc);
            }
            if ((UInt32)v == s->usbLocationID) found = svc;
            else IOObjectRelease(svc);
        }
        IOObjectRelease(it);
        if (found) return found;
    }
    return IO_OBJECT_NULL;
}

// open the scanner's bulk interface.  Samsung MFPs expose the scanner as a
// vendor-specific (0xFF) interface next to the printer-class (7) interface
// that CUPS uses; never send scan commands to the printer interface.
static int usb_open(const ScanSettings* s, ScanConn* c)
{
    io_service_t service = FindUSBService(s);
    if (!service) return -1;

    IOCFPlugInInterface** plugin = NULL;
    SInt32 score = 0;
    IOUSBDeviceInterface** dev = NULL;
    kern_return_t kr = IOCreatePlugInInterfaceForService(service, kIOUSBDeviceUserClientTypeID,
                                                         kIOCFPlugInInterfaceID, &plugin, &score);
    IOObjectRelease(service);
    if (kr != KERN_SUCCESS || !plugin) return -1;
    (*plugin)->QueryInterface(plugin, CFUUIDGetUUIDBytes(kIOUSBDeviceInterfaceID), (LPVOID*)&dev);
    IODestroyPlugInInterface(plugin);
    if (!dev) return -1;

    IOUSBFindInterfaceRequest req;
    req.bInterfaceClass = kIOUSBFindInterfaceDontCare;
    req.bInterfaceSubClass = kIOUSBFindInterfaceDontCare;
    req.bInterfaceProtocol = kIOUSBFindInterfaceDontCare;
    req.bAlternateSetting = kIOUSBFindInterfaceDontCare;
    io_iterator_t it = IO_OBJECT_NULL;
    if ((*dev)->CreateInterfaceIterator(dev, &req, &it) != kIOReturnSuccess) {
        (*dev)->Release(dev);
        return -1;
    }

    // two passes: vendor-specific interfaces first, then any non-printer one
    int ok = 0;
    for (int pass = 0; pass < 2 && !ok; pass++) {
        IOIteratorReset(it);
        io_service_t isvc;
        while (!ok && (isvc = IOIteratorNext(it))) {
            IOCFPlugInInterface** ip = NULL;
            IOUSBInterfaceInterface300** intf = NULL;
            kr = IOCreatePlugInInterfaceForService(isvc, kIOUSBInterfaceUserClientTypeID,
                                                   kIOCFPlugInInterfaceID, &ip, &score);
            IOObjectRelease(isvc);
            if (kr != KERN_SUCCESS || !ip) continue;
            (*ip)->QueryInterface(ip, CFUUIDGetUUIDBytes(kIOUSBInterfaceInterfaceID300),
                                  (LPVOID*)&intf);
            IODestroyPlugInInterface(ip);
            if (!intf) continue;

            UInt8 cls = 0;
            (*intf)->GetInterfaceClass(intf, &cls);
            int wanted = (pass == 0) ? (cls == kUSBVendorSpecificClass)
                                     : (cls != kUSBPrintingInterfaceClass && cls != kUSBVendorSpecificClass);
            if (!wanted || (*intf)->USBInterfaceOpen(intf) != kIOReturnSuccess) {
                (*intf)->Release(intf);
                continue;
            }
            UInt8 nep = 0, in = 0, out = 0;
            (*intf)->GetNumEndpoints(intf, &nep);
            for (UInt8 pipe = 1; pipe <= nep; pipe++) {
                UInt8 dir, num, type, interval;
                UInt16 mps;
                if ((*intf)->GetPipeProperties(intf, pipe, &dir, &num, &type, &mps, &interval) != kIOReturnSuccess)
                    continue;
                if (type != kUSBBulk) continue;
                if (dir == kUSBIn && !in) in = pipe;
                if (dir == kUSBOut && !out) out = pipe;
            }
            if (in && out) {
                c->intf = intf;
                c->pipeIn = in;
                c->pipeOut = out;
                ok = 1;
            } else {
                (*intf)->USBInterfaceClose(intf);
                (*intf)->Release(intf);
            }
        }
    }
    IOObjectRelease(it);
    (*dev)->Release(dev);
    if (!ok) return -1;
    conn_flush(c);
    return 0;
}

static int conn_open(const ScanSettings* s, ScanConn* c)
{
    c->fd = -1;
    c->intf = NULL;
    if (s->isUSB) return usb_open(s, c);

    // parse "xerox_mfp:tcp <ip> <port>" from sanedev
    char devstr[128];
    snprintf(devstr, sizeof devstr, "%s", s->sanedev[0] ? s->sanedev : "xerox_mfp:tcp 127.0.0.1 9400");
    char ip[64] = "127.0.0.1";
    int port = 9400;
    char* toks[8] = { NULL };
    int ti = 0;
    char* save = NULL;
    for (char* t = strtok_r(devstr, " ", &save); t && ti < 8; t = strtok_r(NULL, " ", &save))
        toks[ti++] = t;
    if (ti >= 2) snprintf(ip, sizeof ip, "%s", toks[1]);
    if (ti >= 3) {
        port = (int)strtol(toks[2], NULL, 10);
        if (port <= 0 || port > 65535) port = 9400;
    }

    // The M2070's network stack can be slow to wake after idle (Wi-Fi power
    // save); the client fires an auto-overview immediately on session open.
    // Retry a few times before giving up.
    for (int attempt = 0; attempt < 6; attempt++) {
        c->fd = tcp_connect(ip, port, 5);
        if (c->fd >= 0) return 0;
        sleep(1);
    }
    return -1;
}

static int twain_command(ScanConn* c, unsigned char cmd0, unsigned char* resp, size_t respLen)
{
    unsigned char pkt[4];
    if (cmd0 == TWAIN_CMD_READ_IMAGE) {
        // raw data transfer has no response header; caller reads blocklen bytes
        resp = NULL;
        respLen = 0;
    }
    pkt[0] = TWAIN_REQ_A;
    pkt[1] = TWAIN_REQ_B;
    pkt[2] = cmd0;
    pkt[3] = 0x00;   // SET_WINDOW is sent separately (25-byte packet)

    if (conn_send(c, pkt, sizeof pkt, 10) != 0) return -1;
    if (resp && respLen) {
        if (conn_recv_exact(c, resp, respLen, 12) != 0) return -1;
    }
    return 0;
}

// send a one-byte-opcode command and read its short response; returns state
static int twain_state(ScanConn* c, unsigned char cmd0, unsigned char* respOut)
{
    unsigned char res[32];
    if (twain_command(c, cmd0, res, 32) != 0) {
        conn_flush(c);
        return -1;
    }
    if (respOut) memcpy(respOut, res, 32);
    if (res[0] != TWAIN_RES_CODE) return -1;
    return res[1];
}

// issue a command, retrying while the fw reports busy; 1 on ready/good
static int twain_until_ready(ScanConn* c, unsigned char cmd0)
{
    int slept = 0;
    for (;;) {
        int st = twain_state(c, cmd0, NULL);
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

    ScanConn conn;
    ScanConn* c = &conn;
    if (conn_open(s, c) != 0) {
        LOG("ScanRaw: cannot open %{public}s (usb=%d)", s->isUSB ? "USB device" : s->sanedev, s->isUSB);
        return kICADeviceNotOpenErr;
    }
    LOG("ScanRaw: connected via %{public}s", s->isUSB ? "USB" : "TCP");

    // clear any stale engine session, like the original driver does on open
    twain_state(c, TWAIN_CMD_ABORT, NULL);

    // INQUIRY
    unsigned char inq[70];
    memset(inq, 0, sizeof inq);
    if (twain_command(c, TWAIN_CMD_INQUIRY, inq, sizeof inq) != 0 || inq[0] != TWAIN_RES_CODE) {
        LOG("ScanRaw: INQUIRY failed (hdr 0x%02x)", inq[0]);
        conn_close(c);
        return kICADeviceNotFoundErr;
    }
    if (inq[3] != TWAIN_MSG_PRODUCT_INFO) {
        LOG("ScanRaw: unexpected INQUIRY msg 0x%02x", inq[3]);
        conn_close(c);
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
    if (!winWidthPts || !winLenPts) { conn_close(c); return kICADeviceInvalidParamErr; }
    if (maxWinWidth && winWidthPts > maxWinWidth) winWidthPts = maxWinWidth;
    if (maxWinLen && winLenPts > maxWinLen) winLenPts = maxWinLen;

    double offXIn = oxmm / 25.4;
    double offYIn = oymm / 25.4;

    double pxToLen = 1213.9 / s->resolution;   // xerox: no-JPEG factor (gray)
    if (s->pixelType == 2) pxToLen = 1200.0 / s->resolution;  // JPEG: engine prints 1:1 (probe: 2480px/210mm @300dpi)
    unsigned paraW = (unsigned)(winWidthPts / pxToLen);
    unsigned paraH = (unsigned)(winLenPts / pxToLen);
    if (paraW == 0 || paraH == 0) { conn_close(c); return kICADeviceInvalidParamErr; }
    unsigned bpp = (s->pixelType == 2) ? 3 : 1;
    unsigned modeByte = (s->pixelType == 2) ? TWAIN_MODE_RGB24 : TWAIN_MODE_GRAY8;
    unsigned compByte = (s->pixelType == 2) ? TWAIN_COMP_JPG : 0x00;

    // RESERVE
    if (twain_until_ready(c, TWAIN_CMD_RESERVE_UNIT) != 1) {
        LOG("ScanRaw: RESERVE_UNIT failed");
        twain_state(c, TWAIN_CMD_ABORT, NULL);
        conn_close(c);
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
        if (conn_send(c, sw, 25, 10) != 0 || conn_recv_exact(c, rsw, 32, 12) != 0 || rsw[0] != TWAIN_RES_CODE) {
            
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
        LOG("ScanRaw: SET_WINDOW failed (status 0x%02x)", rsw[1]);
        twain_state(c, TWAIN_CMD_RELEASE_UNIT, NULL);
        conn_close(c);
        return kICADeviceInternalErr;
    }

    // issue OBJPOS once, like the working probe: the engine may answer busy
    // during warmup; readiness is handled by the READ polling loop below.
    // Re-issuing OBJPOS repeatedly during warmup (twain_until_ready) is the
    // one protocol behavior that differs from the proven probe path.
    unsigned char ors[32];
    memset(ors, 0, sizeof ors);
    if (twain_command(c, TWAIN_CMD_OBJECT_POS, ors, sizeof ors) != 0) {
        
    } else {
        
    }
    // original driver pauses Timeouts[0x3c]=300ms after OBJPOS before reading
    usleep(300 * 1000);

    unsigned char* pixels = calloc((size_t)paraW * paraH * bpp, 1);
    if (!pixels) {
        twain_state(c, TWAIN_CMD_RELEASE_UNIT, NULL);
        conn_close(c);
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
            if (twain_command(c, TWAIN_CMD_READ, rb, sizeof rb) != 0 || rb[0] != TWAIN_RES_CODE) {
                
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
        usleep(200 * 1000);
        busyPolls++;
        continue;
    }

    // request the block data: READ_IMAGE (0x29) triggers the fw to send the
    // announced blocklen raw bytes (the 0x28 reply was a header only)
    if (twain_command(c, TWAIN_CMD_READ_IMAGE, NULL, 0) != 0) {
        
        failed = 1;
        break;
    }

    unsigned char* blk = malloc(blocklen);
    if (!blk) { failed = 1; break; }
    if (conn_recv_exact(c, blk, blocklen, 12) != 0) { free(blk); failed = 1; break; }

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
        // tail blocks past the window bottom are drained but not stored
        // (paraH - yOff would underflow and overrun `pixels`)
        if (yOff >= paraH) {
            CGImageRelease(jimg);
            free(blk);
            goto block_done;
        }
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
block_done: ;

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
        twain_state(c, TWAIN_CMD_ABORT, NULL);
        twain_state(c, TWAIN_CMD_RELEASE_UNIT, NULL);
    }
    conn_close(c);

    if (failed || !done) {
        LOG("ScanRaw: transfer failed (failed=%d done=%d rows=%u/%u)", failed, done, yOff, paraH);
        free(pixels);
        return kICADeviceInternalErr;
    }

    LOG("ScanRaw: done %ux%u bpp=%u", paraW, paraH, bpp);
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

// file name components come from the client; refuse anything that could
// escape the destination folder
static int SafePathComponent(const char* c)
{
    if (!c || !c[0] || strchr(c, '/') || strcmp(c, ".") == 0 || strcmp(c, "..") == 0) return 0;
    return 1;
}

static CFStringRef ImageTypeForExtension(const char* ext)
{
    if (!strcasecmp(ext, "tif") || !strcasecmp(ext, "tiff")) return CFSTR("public.tiff");
    if (!strcasecmp(ext, "png"))  return CFSTR("public.png");
    if (!strcasecmp(ext, "heic")) return CFSTR("public.heic");
    if (!strcasecmp(ext, "bmp"))  return CFSTR("com.microsoft.bmp");
    if (!strcasecmp(ext, "pdf"))  return CFSTR("com.adobe.pdf");
    return CFSTR("public.jpeg");
}

// NoData full scans complete by the DRIVER writing the finished image into the
// client's security-scoped output folder (verified vs the working original
// driver with fs_usage). Mirror its atomic pattern: hidden temp then rename.
// The format follows the requested extension (Preview/Image Capture offer
// JPEG, TIFF, PNG, HEIC, PDF ...); unknown extensions fall back to JPEG.
static ICAError WriteDocumentFile(const unsigned char* pixels, unsigned w, unsigned h, int bpp,
                                  long dpi, const char* folder, const char* name, const char* ext,
                                  char* outPath, size_t outPathSize)
{
    if (!pixels || w == 0 || h == 0 || !folder || !folder[0] ||
        !SafePathComponent(name) || !SafePathComponent(ext))
        return kICADeviceInvalidParamErr;
    if (dpi <= 0) dpi = 300;

    char tmpPath[768], finalPath[768];
    if (snprintf(finalPath, sizeof finalPath, "%s/%s.%s", folder, name, ext) >= (int)sizeof finalPath)
        return kICADeviceInvalidParamErr;
    if (snprintf(tmpPath, sizeof tmpPath, "%s/.%s.%s-%05x", folder, name, ext,
                 (unsigned)(getpid() ^ (unsigned)time(0)) & 0xfffffu) >= (int)sizeof tmpPath)
        return kICADeviceInvalidParamErr;

    CGColorSpaceRef cs = (bpp == 1) ? CGColorSpaceCreateDeviceGray() : CGColorSpaceCreateDeviceRGB();
    CGDataProviderRef prov = CGDataProviderCreateWithData(NULL, pixels, (size_t)w * h * bpp, NULL);
    CGBitmapInfo bi = (CGBitmapInfo)kCGImageAlphaNone;
    CGImageRef img = CGImageCreate(w, h, 8, (size_t)bpp * 8, (size_t)w * bpp, cs, bi, prov, NULL,
                                   false, kCGRenderingIntentDefault);
    CGDataProviderRelease(prov);
    CGColorSpaceRelease(cs);
    if (!img) return kICADeviceInternalErr;

    CFURLRef url = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8*)tmpPath,
                                                           strlen(tmpPath), false);
    if (!url) { CGImageRelease(img); return kICADeviceMemoryAllocationErr; }
    CFStringRef type = ImageTypeForExtension(ext);
    bool ok = false;

    if (CFStringCompare(type, CFSTR("com.adobe.pdf"), 0) == kCFCompareEqualTo) {
        // one page sized to the physical scan area
        CGRect box = CGRectMake(0, 0, (CGFloat)w * 72.0 / dpi, (CGFloat)h * 72.0 / dpi);
        CGContextRef pdf = CGPDFContextCreateWithURL(url, &box, NULL);
        if (pdf) {
            CGPDFContextBeginPage(pdf, NULL);
            CGContextDrawImage(pdf, box, img);
            CGPDFContextEndPage(pdf);
            CGPDFContextClose(pdf);
            CGContextRelease(pdf);
            ok = true;
        }
    } else {
        CGImageDestinationRef dst = CGImageDestinationCreateWithURL(url, type, 1, NULL);
        if (!dst) dst = CGImageDestinationCreateWithURL(url, CFSTR("public.jpeg"), 1, NULL);
        if (dst) {
            double d = (double)dpi;
            CFNumberRef dn = CFNumberCreate(NULL, kCFNumberDoubleType, &d);
            CFMutableDictionaryRef props = CreateMutableDict();
            CFDictionarySetValue(props, kCGImagePropertyDPIWidth, dn);
            CFDictionarySetValue(props, kCGImagePropertyDPIHeight, dn);
            CFRelease(dn);
            CGImageDestinationAddImage(dst, img, props);
            CFRelease(props);
            ok = CGImageDestinationFinalize(dst);
            CFRelease(dst);
        }
    }
    CFRelease(url);
    CGImageRelease(img);
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
        LOG("scan failed: %d", (int)err);
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
        if (rs) {
            free(pixels);
            pixels = rs;
            w = dw; h = dh;
        }
        // on failure keep the native-size image rather than crashing
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
        ICAError werr = WriteDocumentFile(pixels, w, h, bpp, s.resolution,
                                          s.docFolder, s.docName, s.docExt,
                                          tmpPath, sizeof(tmpPath));
        if (werr != noErr) {
            LOG("WriteDocumentFile failed %d folder=%{public}s name=%{public}s.%{public}s",
                (int)werr, s.docFolder, s.docName, s.docExt);
            free(pixels);
            SendSimpleNotification(s.object, kICANotificationTypeScannerScanDone);
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
        ICAError oerr = ICDScannerNewObjectInfoCreated(parent, 0, &newObj);
        LOG("NewObjectInfoCreated err=%d obj=%u file=%{public}s", (int)oerr, (unsigned)newObj, finalPath);
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
    LOG("Start object=%u", (unsigned)pb->object);

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
        gSettings.isUSB = st->isUSB;
        gSettings.usbLocationID = st->usbLocationID;
        snprintf(gSettings.usbIORegPath, sizeof gSettings.usbIORegPath, "%s", st->usbIORegPath);
    } else {
        snprintf(gSettings.sanedev, sizeof gSettings.sanedev, "xerox_mfp:tcp 127.0.0.1 9400");
        gSettings.isUSB = 0;
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
    LOG("driver starting (pid %d, argc %d)", getpid(), argc);
    gICDScannerCallbackFunctions = gCallbacks;
    return ICD_ScannerMain(argc, argv);
}