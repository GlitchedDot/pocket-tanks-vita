#include <falso_jni/FalsoJNI.h>
#include <falso_jni/FalsoJNI_Impl.h>
#include <falso_jni/FalsoJNI_Logger.h>

#include "utils.h"
#include "asset_mgr.h"
#include "fake_motionevent.h"

#include <psp2/io/dirent.h>
#include <psp2/kernel/processmgr.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DATA_PATH "ux0:data/pockettanks/"

// Fake singleton objects handed out for AssetManager / Rect / Activity / View.
static int dummy_asset_manager;
static int dummy_rect;
static int dummy_activity;
static int dummy_view;
static int dummy_application;

// ---------------------------------------------------------------------------
// CPJNILib callbacks
// ---------------------------------------------------------------------------

static jstring jni_getDataPath(jmethodID id, va_list args) {
    (void)id; (void)args;
    return jni->NewStringUTF(&jni, DATA_PATH);
}

static jstring jni_getExternalCachePath(jmethodID id, va_list args) {
    (void)id; (void)args;
    return jni->NewStringUTF(&jni, DATA_PATH "cache/");
}

static jobject jni_getAssetManager(jmethodID id, va_list args) {
    (void)id; (void)args;
    return (jobject)&dummy_asset_manager;
}

static jstring jni_getDeviceModel(jmethodID id, va_list args) {
    (void)id; (void)args;
    return jni->NewStringUTF(&jni, "PS Vita");
}

static jstring jni_getDeviceOS(jmethodID id, va_list args) {
    (void)id; (void)args;
    return jni->NewStringUTF(&jni, "VitaOS");
}

static jint jni_getIPAddress(jmethodID id, va_list args) {
    (void)id; (void)args;
    return 0;
}

static jstring jni_getExpansionPath(jmethodID id, va_list args) {
    (void)id; (void)args;
    return jni->NewStringUTF(&jni, DATA_PATH);
}

static jobject jni_getSafeArea(jmethodID id, va_list args) {
    (void)id; (void)args;
    // android.graphics.Rect(0, 0, 960, 544) — fake object, never inspected.
    return (jobject)&dummy_rect;
}

static jboolean jni_isBillingEnabled(jmethodID id, va_list args) {
    (void)id; (void)args;
    // Shop kill-switch: the whole billing path is disabled.
    return JNI_FALSE;
}

static jint jni_checkPurchaseStatus(jmethodID id, va_list args) {
    jstring product = va_arg(args, jstring);
    (void)id;
    const char *p = product ? jni->GetStringUTFChars(&jni, product, NULL) : "(null)";
    log_info("checkPurchaseStatus(%s) -> UNKNOWN(0)", p);
    return 0; // UNKNOWN
}

static jint jni_getRestoringPurchaseStatus(jmethodID id, va_list args) {
    (void)id; (void)args;
    return 0;
}

static jobject jni_getProductInfo(jmethodID id, va_list args) {
    (void)id; (void)args;
    return NULL;
}

static void jni_purchaseProduct(jmethodID id, va_list args) {
    (void)id; (void)args;
    log_info("purchaseProduct: stubbed (billing disabled)");
}

static void jni_requestProductInfo(jmethodID id, va_list args) {
    (void)id; (void)args;
}

static void jni_restorePurchases(jmethodID id, va_list args) {
    (void)id; (void)args;
}

static void jni_launchWebPage(jmethodID id, va_list args) {
    (void)id; (void)args;
}

static void jni_setScreensaverMode(jmethodID id, va_list args) {
    (void)id; (void)args;
}

static jboolean jni_isKeyboardActive(jmethodID id, va_list args) {
    (void)id; (void)args;
    return JNI_FALSE;
}

static void jni_exit(jmethodID id, va_list args) {
    (void)id; (void)args;
    log_info("exit() called by game");
    sceKernelExitProcess(0);
}

static jobject jni_getActivity(jmethodID id, va_list args) {
    (void)id; (void)args;
    return (jobject)&dummy_activity;
}

static jobject jni_getView(jmethodID id, va_list args) {
    (void)id; (void)args;
    return (jobject)&dummy_view;
}

static jobject jni_getApplication(jmethodID id, va_list args) {
    (void)id; (void)args;
    return (jobject)&dummy_application;
}

// ---------------------------------------------------------------------------
// CPJNISound shims (Java SoundPool path is dormant in v3.0.0; keep shims so
// the native JNI lookups resolve)
// ---------------------------------------------------------------------------

static jboolean jni_loadMod(jmethodID id, va_list args) {
    (void)id; (void)args;
    return JNI_FALSE;
}

static jint jni_loadWav(jmethodID id, va_list args) {
    (void)id; (void)args;
    return 0;
}

static jboolean jni_playMod(jmethodID id, va_list args) {
    (void)id; (void)args;
    return JNI_FALSE;
}

static jboolean jni_playWav(jmethodID id, va_list args) {
    (void)id; (void)args;
    return JNI_FALSE;
}

// ---------------------------------------------------------------------------
// CPJNIDirectory over sceIoDopen (assets/<path>)
// ---------------------------------------------------------------------------

static jint jni_opendir(jmethodID id, va_list args) {
    jstring jpath = va_arg(args, jstring);
    (void)id;
    const char *path = jpath ? jni->GetStringUTFChars(&jni, jpath, NULL) : "";
    char full[512];
    snprintf(full, sizeof(full), "%sassets/%s", DATA_PATH, path);
    int fd = sceIoDopen(full);
    log_info("opendir(%s) -> %d", full, fd);
    return fd;
}

static jstring jni_readdir(jmethodID id, va_list args) {
    jint fd = va_arg(args, jint);
    (void)id;
    SceIoDirent dent;
    if (sceIoDread(fd, &dent) <= 0)
        return NULL;
    return jni->NewStringUTF(&jni, dent.d_name);
}

static void jni_closedir(jmethodID id, va_list args) {
    jint fd = va_arg(args, jint);
    (void)id;
    sceIoDclose(fd);
}

static void jni_rewinddir(jmethodID id, va_list args) {
    (void)id; (void)args;
    // No rewind primitive on Vita; reopen is handled by opendir callers.
}

// ---------------------------------------------------------------------------
// CPJNIHTTP stubs (update-check path must not block)
// ---------------------------------------------------------------------------

static jint jni_httpInitGet(jmethodID id, va_list args) {
    (void)id; (void)args;
    return -1;
}

static jint jni_httpInitPost(jmethodID id, va_list args) {
    (void)id; (void)args;
    return -1;
}

static jobject jni_httpGetData(jmethodID id, va_list args) {
    (void)id; (void)args;
    return NULL;
}

static jint jni_httpGetBytesDownloaded(jmethodID id, va_list args) {
    (void)id; (void)args;
    return 0;
}

static void jni_httpClose(jmethodID id, va_list args) {
    (void)id; (void)args;
}

// ---------------------------------------------------------------------------
// MessageBox / Sharing / Parse / Ad stubs
// ---------------------------------------------------------------------------

static void jni_showMessageBox(jmethodID id, va_list args) {
    (void)id; (void)args;
    log_info("showMessageBox: stubbed");
}

static jint jni_showConfirmationBox(jmethodID id, va_list args) {
    (void)id; (void)args;
    return 0;
}

static jboolean jni_shareOnSocial(jmethodID id, va_list args) {
    (void)id; (void)args;
    return JNI_FALSE;
}

static jboolean jni_composeEmail(jmethodID id, va_list args) {
    (void)id; (void)args;
    return JNI_FALSE;
}

// ---------------------------------------------------------------------------
// Name -> ID tables
// ---------------------------------------------------------------------------

NameToMethodID nameToMethodId[] = {
    // CPJNILib (object)
    {100, "getDataPath", METHOD_TYPE_OBJECT},
    {101, "getExternalCachePath", METHOD_TYPE_OBJECT},
    {102, "getAssetManager", METHOD_TYPE_OBJECT},
    {103, "getDeviceModel", METHOD_TYPE_OBJECT},
    {104, "getDeviceOS", METHOD_TYPE_OBJECT},
    {105, "getExpansionPath", METHOD_TYPE_OBJECT},
    {106, "getSafeArea", METHOD_TYPE_OBJECT},
    {107, "getProductInfo", METHOD_TYPE_OBJECT},
    {108, "getActivity", METHOD_TYPE_OBJECT},
    {109, "getView", METHOD_TYPE_OBJECT},
    {110, "getApplication", METHOD_TYPE_OBJECT},
    // CPJNILib (boolean)
    {120, "isBillingEnabled", METHOD_TYPE_BOOLEAN},
    {121, "isKeyboardActive", METHOD_TYPE_BOOLEAN},
    // CPJNILib (int)
    {130, "checkPurchaseStatus", METHOD_TYPE_INT},
    {131, "getIPAddress", METHOD_TYPE_INT},
    {132, "getRestoringPurchaseStatus", METHOD_TYPE_INT},
    // CPJNILib (void)
    {140, "launchWebPage", METHOD_TYPE_VOID},
    {141, "setScreensaverMode", METHOD_TYPE_VOID},
    {142, "purchaseProduct", METHOD_TYPE_VOID},
    {143, "requestProductInfo", METHOD_TYPE_VOID},
    {144, "restorePurchases", METHOD_TYPE_VOID},
    {145, "exit", METHOD_TYPE_VOID},
    {146, "setActivity", METHOD_TYPE_VOID},
    {147, "setView", METHOD_TYPE_VOID},
    {148, "setKeyboardVisibility", METHOD_TYPE_VOID},
    // MotionEvent (registered in fake_motionevent.c table below via shared IDs)
    {200, "getAction", METHOD_TYPE_INT},
    {201, "getActionMasked", METHOD_TYPE_INT},
    {202, "getActionIndex", METHOD_TYPE_INT},
    {203, "getPointerCount", METHOD_TYPE_INT},
    {204, "getPointerId", METHOD_TYPE_INT},
    {205, "getX", METHOD_TYPE_FLOAT},
    {206, "getY", METHOD_TYPE_FLOAT},
    // CPJNISound
    {210, "loadMod", METHOD_TYPE_BOOLEAN},
    {211, "loadWav", METHOD_TYPE_INT},
    {212, "playMod", METHOD_TYPE_BOOLEAN},
    {213, "playWav", METHOD_TYPE_BOOLEAN},
    // CPJNIDirectory
    {220, "opendir", METHOD_TYPE_INT},
    {221, "readdir", METHOD_TYPE_OBJECT},
    {222, "closedir", METHOD_TYPE_VOID},
    {223, "rewinddir", METHOD_TYPE_VOID},
    // CPJNIHTTP
    {230, "initGet", METHOD_TYPE_INT},
    {231, "initPost", METHOD_TYPE_INT},
    {232, "getData", METHOD_TYPE_OBJECT},
    {233, "getBytesDownloaded", METHOD_TYPE_INT},
    {234, "close", METHOD_TYPE_VOID},
    // MessageBox / Sharing
    {240, "showMessageBox", METHOD_TYPE_VOID},
    {241, "showConfirmationBox", METHOD_TYPE_INT},
    {242, "shareOnSocial", METHOD_TYPE_BOOLEAN},
    {243, "composeEmail", METHOD_TYPE_BOOLEAN},
};

MethodsObject methodsObject[] = {
    {100, jni_getDataPath},
    {101, jni_getExternalCachePath},
    {102, jni_getAssetManager},
    {103, jni_getDeviceModel},
    {104, jni_getDeviceOS},
    {105, jni_getExpansionPath},
    {106, jni_getSafeArea},
    {107, jni_getProductInfo},
    {108, jni_getActivity},
    {109, jni_getView},
    {110, jni_getApplication},
    {221, jni_readdir},
    {232, jni_httpGetData},
};

MethodsBoolean methodsBoolean[] = {
    {120, jni_isBillingEnabled},
    {121, jni_isKeyboardActive},
    {210, jni_loadMod},
    {212, jni_playMod},
    {213, jni_playWav},
    {242, jni_shareOnSocial},
    {243, jni_composeEmail},
};

MethodsInt methodsInt[] = {
    {130, jni_checkPurchaseStatus},
    {131, jni_getIPAddress},
    {132, jni_getRestoringPurchaseStatus},
    {200, motionevent_getAction},
    {201, motionevent_getActionMasked},
    {202, motionevent_getActionIndex},
    {203, motionevent_getPointerCount},
    {204, motionevent_getPointerId},
    {211, jni_loadWav},
    {220, jni_opendir},
    {230, jni_httpInitGet},
    {231, jni_httpInitPost},
    {233, jni_httpGetBytesDownloaded},
    {241, jni_showConfirmationBox},
};

MethodsFloat methodsFloat[] = {
    {205, motionevent_getX},
    {206, motionevent_getY},
};

MethodsVoid methodsVoid[] = {
    {140, jni_launchWebPage},
    {141, jni_setScreensaverMode},
    {142, jni_purchaseProduct},
    {143, jni_requestProductInfo},
    {144, jni_restorePurchases},
    {145, jni_exit},
    {146, NULL}, // setActivity: no-op default
    {147, NULL}, // setView: no-op default
    {148, NULL}, // setKeyboardVisibility: no-op default
    {222, jni_closedir},
    {223, jni_rewinddir},
    {234, jni_httpClose},
    {240, jni_showMessageBox},
};

MethodsShort methodsShort[] = {};
MethodsLong methodsLong[] = {};
MethodsDouble methodsDouble[] = {};
MethodsByte methodsByte[] = {};
MethodsChar methodsChar[] = {};

// Fields (unused by this game, but table must exist)
NameToFieldID nameToFieldId[] = {};
FieldsBoolean fieldsBoolean[] = {};
FieldsByte fieldsByte[] = {};
FieldsChar fieldsChar[] = {};
FieldsDouble fieldsDouble[] = {};
FieldsFloat fieldsFloat[] = {};
FieldsInt fieldsInt[] = {};
FieldsLong fieldsLong[] = {};
FieldsObject fieldsObject[] = {};
FieldsShort fieldsShort[] = {};

__FALSOJNI_IMPL_CONTAINER_SIZES
