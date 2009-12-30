/* pluginlogger 0.1, a logging plugin wrapper */

/* system headers for useful things */
#include <dlfcn.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

/* load the npapi headers */
#include "nptypes.h"
#include "npapi.h"
#include "npfunctions.h"
#include "npruntime.h"

#define MIN(A,B) (A<B?A:B)

/* types for plugin functions */
typedef NPError (*NP_Initialize_Func)(NPNetscapeFuncs*, NPPluginFuncs*);
typedef char* (*NP_GetPluginVersion_Func)();
typedef char* (*NP_GetMIMEDescription_Func)();
typedef NPError (*NP_GetValue_Func)(void*, NPPVariable, void*);
typedef NPError (*NP_Shutdown_Func)();

typedef struct {
  NP_Initialize_Func initialize;
  NP_GetPluginVersion_Func getPluginVersion;
  NP_GetMIMEDescription_Func getMIMEDescription;
  NP_GetValue_Func getValue;
  NP_Shutdown_Func shutdown;
} ExportedPluginFunctions;

bool gInitialized = false;

static NPNetscapeFuncs* gBrowserFuncs = NULL; // browser functions
static NPNetscapeFuncs* gWrappedBrowserFuncs = NULL; // wrapped browser functions
static NPPluginFuncs* gPluginFuncs = NULL; // plugin functions

static void* gPlugin = NULL;
static ExportedPluginFunctions gExportedFlashFunctions = { NULL };

static FILE* gLogFile = NULL;
static void log(const char* format, ...) 
  __attribute__((__format__ (__printf__, 1, 2)));
static void log(const char* format, ...) {
  va_list argp;
  va_start(argp, format);
  vfprintf(gLogFile, format, argp);
  va_end(argp);
  fflush(gLogFile);
}

/* helper to get the value of an NPIdentifier */
class IdentifierWrapper {
  private:
    NPIdentifier mId;
    char* mPrintable;
  public:
    IdentifierWrapper(NPIdentifier aId) 
      : mId(aId), mPrintable(NULL) {};
    const char* printable() {
      if (mPrintable == NULL) {
        if (gBrowserFuncs->identifierisstring(mId)) {
          NPUTF8* utf8 = gBrowserFuncs->utf8fromidentifier(mId);
          mPrintable = strdup(utf8);
          gBrowserFuncs->memfree(utf8);
        } else {
          int32_t intvalue = gBrowserFuncs->intfromidentifier(mId);
          mPrintable = (char*) malloc(256);
          snprintf(mPrintable, 256, "%d", intvalue);
        }
      }
      return mPrintable;
    }
    ~IdentifierWrapper() {
      if (mPrintable != NULL) {
        free(mPrintable);
      }
    }
};


/* wrapped browser functions */
NPError wrap_NPN_GetValue (NPP npp, NPNVariable variable, void *ret_value) {
  log("NPN_GetValue(npp=%p, variable=%d, value=%p)\n", npp, variable, ret_value);
  NPError e = gBrowserFuncs->getvalue(npp, variable, ret_value);
  log(" returned %d\n", e);
  return e;
}

NPError wrap_NPN_SetValue (NPP npp, NPPVariable variable, void *value) {
  log("NPN_SetValue(npp=%p, variable=%d, value=%p)\n", npp, variable, value);
  NPError e = gBrowserFuncs->setvalue(npp, variable, value);
  log(" returned %d\n", e);
  return e;
}

NPError wrap_NPN_GetURLNotify (NPP npp, const char* url, const char* window, void* notifyData) {
  log("NPN_GetURLNotify(npp=%p, url=\"%s\", window=\"%s\", notifydata=%p)\n", npp, url, window, notifyData);
  NPError e = gBrowserFuncs->geturlnotify(npp, url, window, notifyData);
  log(" returned %d\n", e);
  return e;
}

NPError wrap_NPN_PostURLNotify (NPP npp, const char* url, const char* window, uint32_t len, const char* buf, NPBool file, void* notifyData) {
  log("NPN_PostURLNotify(npp=%p, url=\"%s\", window=\"%s\", len=%d, buf=%p, file=%d, notifyData=%p)\n", npp, url, window, len, buf, file, notifyData);
  NPError e = gBrowserFuncs->posturlnotify(npp, url, window, len, buf, file, notifyData);
  log(" returned %d\n", e);
  return e;
}

NPError wrap_NPN_GetURL (NPP npp, const char* url, const char* window) {
  log("NPN_GetURL(npp=%p, url=\"%s\", window=\"%s\")\n", npp, url, window);
  NPError e = gBrowserFuncs->geturl(npp, url, window);
  log(" returned %d\n", e);
  return e;
}

NPError wrap_NPN_PostURL (NPP npp, const char* url, const char* window, uint32_t len, const char* buf, NPBool file) {
  log("NPN_PostURL(npp=%p, url=\"%s\", window=\"%s\", len=%d, buf=%p, file=%d)\n", npp, url, window, len, buf, file);
  NPError e = gBrowserFuncs->posturl(npp, url, window, len, buf, file);
  log(" returned %d\n", e);
  return e;
}

NPError wrap_NPN_RequestRead (NPStream* stream, NPByteRange* rangeList) {
  log("NPN_RequestRead(stream=%p)\n", stream);
  for (NPByteRange* r=rangeList; r!=NULL; r=r->next) {
    log("  range offset=%d length=%d\n", r->offset, r->length);    
  }
  NPError e = gBrowserFuncs->requestread(stream, rangeList);
  log(" returned %d\n", e);
  return e;
}

NPError wrap_NPN_NewStream (NPP npp, NPMIMEType type, const char* window, NPStream** stream) {
  log("NPN_NewStream(npp=%p, type=\"%s\", window=\"%s\", stream=%p)\n", npp, type, window, stream);
  NPError e = gBrowserFuncs->newstream(npp, type, window, stream);
  log(" returned %d\n", e);
  return e;
}

int32_t wrap_NPN_Write (NPP npp, NPStream* stream, int32_t len, void* buffer) {
  log("NPN_Write(npp=%p, stream=%p, len=%d, buffer=%p\n", npp, stream, len, buffer);
  int32_t r = gBrowserFuncs->write(npp, stream, len, buffer);
  log(" returned %d\n", r);
  return r;
}

NPError wrap_NPN_DestroyStream (NPP npp, NPStream* stream, NPReason reason) {
  log("NPN_DestroySTream(npp=%p, stream=%p, reason=%d)\n", npp, stream, reason);
  NPError e = gBrowserFuncs->destroystream(npp, stream, reason);
  log(" returned %d\n", e);
  return e;
}

void wrap_NPN_Status (NPP npp, const char* message) {
  log("NPN_Status(npp=%p, message=\"%s\"\n", npp, message);
  gBrowserFuncs->status(npp, message);
  return;
}

const char* wrap_NPN_UserAgent (NPP npp) {
  log("NPN_UserAgent(npp=%p)\n", npp);
  const char* r = gBrowserFuncs->uagent(npp);
  log(" returned \"%s\"\n", r);
  return r;
}

void* wrap_NPN_MemAlloc (uint32_t size) {
  log("NPN_MemAlloc(size=%d)\n", size);
  void* r = gBrowserFuncs->memalloc(size);
  log(" returned \"%p\"\n", r);
  return r;
}

void wrap_NPN_MemFree (void* ptr) {
  log("NPN_MemFree(ptr=%p)\n", ptr);
  gBrowserFuncs->memfree(ptr);
  return;
}

uint32_t wrap_NPN_MemFlush (uint32_t size) {
  log("NPN_MemFlush(size=%d)\n", size);
  uint32_t r = gBrowserFuncs->memflush(size);
  log(" returned %d\n", r);
  return r;
}

void wrap_NPN_ReloadPlugins (NPBool reloadPages) {
  log("NPN_ReloadPlugins(reloadPages=%d)\n", reloadPages);
  gBrowserFuncs->reloadplugins(reloadPages);
}

void* wrap_NPN_GetJavaEnv () {
  log("NPN_GetJavaEnv()\n");
  void* r = gBrowserFuncs->getJavaEnv();
  log(" returned %p\n", r);
  return r;
}

void* wrap_NPN_GetJavaPeer (NPP npp) {
  log("NPN_GetJavaPeer(npp=%p)\n", npp);
  void* r = gBrowserFuncs->getJavaPeer(npp);
  log(" returned %p\n", r);
  return r;
}

void wrap_NPN_InvalidateRect (NPP npp, NPRect *rect) {
  log("NPN_InvalidateRect(npp=%p rect={top=%d, left=%d, bottom=%d, right=%d})\n", npp, rect->top, rect->left, rect->bottom, rect->right);
  gBrowserFuncs->invalidaterect(npp, rect);
}

void wrap_NPN_InvalidateRegion (NPP npp, NPRegion region) {
  log("NPN_InvalidateRegion(npp=%p, region=%p\n", npp, region);
  gBrowserFuncs->invalidateregion(npp, region);
}

void wrap_NPN_ForceRedraw (NPP npp) {
  log("NPN_ForceRedraw(npp=%p)\n", npp);
  gBrowserFuncs->forceredraw(npp);
}

NPIdentifier wrap_NPN_GetStringIdentifier (const NPUTF8* name) {
  log("NPN_GetStringIdentifier(name=\"%s\")\n", name);
  NPIdentifier r = gBrowserFuncs->getstringidentifier(name);
  log(" returned %p\n", r);
  return r;
}

void wrap_NPN_GetStringIdentifiers (const NPUTF8** names, int32_t nameCount, NPIdentifier* identifiers) {
  log("NPN_GetStringIdentifiers(nameCount=%d)\n", nameCount);
  gBrowserFuncs->getstringidentifiers(names, nameCount, identifiers);
  log(" returned: \n");
  for (int i=0; i<nameCount; i++) {
    log("  \"%s\" -> %p\n", names[i], identifiers[i]);
  }
}

NPIdentifier wrap_NPN_GetIntIdentifier (int32_t intid) {
  log("NPN_GetIntIdentifier(intid=%d)\n", intid);
  NPIdentifier r = gBrowserFuncs->getintidentifier(intid);
  log(" returned %p\n", r);
  return r;
}

bool wrap_NPN_IdentifierIsString (NPIdentifier identifier) {
  log("NPN_IdentifierIsString(identifier=%p)\n", identifier);
  bool r = gBrowserFuncs->identifierisstring(identifier);
  return r;
}

NPUTF8* wrap_NPN_UTF8FromIdentifier (NPIdentifier identifier) {
  log("NPN_UTF8FromIdentifier(identifier=%p)\n", identifier);
  NPUTF8* r = gBrowserFuncs->utf8fromidentifier(identifier);
  log(" returned \"%s\"\n", r);
  return r;
}

int32_t wrap_NPN_IntFromIdentifier (NPIdentifier identifier) {
  log("NPN_IntFromIdentifier(identifier=%p)\n", identifier);
  int32_t r = gBrowserFuncs->intfromidentifier(identifier);
  log(" returned %d\n", r);
  return r;
}

NPObject* wrap_NPN_CreateObject (NPP npp, NPClass *aClass) {
  log("NPN_CreateObject(npp=%p, class=%p)\n", npp, aClass);
  NPObject* r = gBrowserFuncs->createobject(npp, aClass);
  log(" returned %p\n", r);
  return r;
}

NPObject* wrap_NPN_RetainObject (NPObject *obj) {
  log("NPN_RetainObject(obj=%p)\n", obj);
  NPObject* r = gBrowserFuncs->retainobject(obj);
  log(" returned %p\n", r);
  return r;
}

void wrap_NPN_ReleaseObject (NPObject *obj) {
  log("NPN_ReleaseObject(obj=%p)\n", obj);
  gBrowserFuncs->releaseobject(obj);
}

bool wrap_NPN_Invoke (NPP npp, NPObject* obj, NPIdentifier methodName, const NPVariant *args, uint32_t argCount, NPVariant *result) {
  log("NPN_Invoke(npp=%p, obj=%p, methodName=%s args=%p, argCount=%d, "
      "result=%p)\n", npp, obj, IdentifierWrapper(methodName).printable(), 
      args, argCount, result);
  bool r =  gBrowserFuncs->invoke(npp, obj, methodName, args, argCount, result);
  log(" returned %d\n", r);
  return r;
}

bool wrap_NPN_InvokeDefault (NPP npp, NPObject* obj, const NPVariant *args, uint32_t argCount, NPVariant *result) {
  log("NPN_InvokeDefault(npp=%p, obj=%p, args=%p, argCount=%d, result=%p)\n", npp, obj, args, argCount, result);
  bool r = gBrowserFuncs->invokeDefault(npp, obj, args, argCount, result);
  log(" returned %d\n", r);
  return r;
}

bool wrap_NPN_Evaluate (NPP npp, NPObject *obj, NPString *script, NPVariant *result) {
  log("NPN_Evaluate(npp=%p, obj=%p, script=\"%s\", result=%p)\n", npp, obj, script->UTF8Characters, result);
  bool r = gBrowserFuncs->evaluate(npp, obj, script, result);
  log(" returned %d\n", r);
  return r;
}

bool wrap_NPN_GetProperty (NPP npp, NPObject *obj, NPIdentifier propertyName, 
    NPVariant *result) {
  log("NPN_GetProperty(npp=%p, obj=%p, propertyName=\"%s\", result=%p)\n", 
      npp, obj, IdentifierWrapper(propertyName).printable(), result);
  bool r = gBrowserFuncs->getproperty(npp, obj, propertyName, result);
  log(" returned %d\n", r);
  return r;
}

bool wrap_NPN_SetProperty (NPP npp, NPObject *obj, NPIdentifier propertyName, 
    const NPVariant *value) {
  log("NPN_SetProperty(npp=%p, obj=%p, propertyName=\"%s\", value=%p)\n", 
      npp, obj, IdentifierWrapper(propertyName).printable(), value);
  bool r = gBrowserFuncs->setproperty(npp, obj, propertyName, value);
  log(" returned %d\n", r);
  return r;
}

bool wrap_NPN_RemoveProperty (NPP npp, NPObject *obj, NPIdentifier propertyName) {
  log("NPN_RemoveProperty(npp=%p, obj=%p, properyName=\"%s\")\n", 
      npp, obj, IdentifierWrapper(propertyName).printable());
  bool r = gBrowserFuncs->removeproperty(npp, obj, propertyName);
  log(" returned %d\n", r);
  return r;
}

bool wrap_NPN_HasProperty (NPP npp, NPObject *obj, NPIdentifier propertyName) {
  log("NPN_HasProperty(npp=%p, obj=%p, propertyName=\"%s\")\n", 
      npp, obj, IdentifierWrapper(propertyName).printable());
  bool r = gBrowserFuncs->hasproperty(npp, obj, propertyName);
  log(" returned %d\n", r);
  return r;
}

bool wrap_NPN_HasMethod (NPP npp, NPObject *obj, NPIdentifier propertyName) {
  log("NPN_HasMethod(npp=%p, obj=%p, propertyName=\"%s\")\n", 
      npp, obj, IdentifierWrapper(propertyName).printable());
  bool r = gBrowserFuncs->hasmethod(npp, obj, propertyName);
  log(" returned %d\n", r);
  return r;
}

void wrap_NPN_ReleaseVariantValue (NPVariant *variant) {
  log("NPN_ReleaseVariantValue(variant=%p)\n", variant);
  gBrowserFuncs->releasevariantvalue(variant);
}

void wrap_NPN_SetException (NPObject *obj, const NPUTF8 *message) {
  log("NPN_SetException(obj=%p, message=\"%s\")\n", obj, message);
  gBrowserFuncs->setexception(obj, message);
}

bool wrap_NPN_PushPopupsEnabledState (NPP npp, NPBool enabled) {
  log("NPN_PushPopupsEnabledState(npp=%p, enabled=%d)\n", npp, enabled);
  bool r = gBrowserFuncs->pushpopupsenabledstate(npp, enabled);
  log(" returned %d\n", r);
  return r;
}

bool wrap_NPN_PopPopupsEnabledState (NPP npp) {
  log("NPN_PopPopupsEnabledState(npp=%p)\n", npp);
  bool r = gBrowserFuncs->poppopupsenabledstate(npp);
  log(" returned %d\n", r);
  return r;
}

bool wrap_NPN_Enumerate (NPP npp, NPObject *obj, NPIdentifier **identifier, uint32_t *count) {
  log("NPN_Enumerate(npp=%p, obj=%p, identifier=%p, count=%p\n", npp, obj, identifier, count);
  bool r = gBrowserFuncs->enumerate(npp, obj, identifier, count);
  log(" returned %d\n", r);
  return r;
}

void wrap_NPN_PluginThreadAsyncCall (NPP npp, void (*func)(void *), void *userData) {
  log("NPN_PluginThreadAsyncCall(npp=%p, func=%p, userData=%p)\n", npp, func, userData);
  gBrowserFuncs->pluginthreadasynccall(npp, func, userData);
}

bool wrap_NPN_Construct (NPP npp, NPObject* obj, const NPVariant *args, uint32_t argCount, NPVariant *result) {
  log("NPN_Construct(npp=%p, obj=%p, args=%p, argCount=%d, result=%p)\n", 
      npp, obj, args, argCount, result);
  bool r = gBrowserFuncs->construct(npp, obj, args, argCount, result);
  log(" returned %d\n", r);
  return r;
}

NPError wrap_NPN_GetValueForURL (NPP npp, NPNURLVariable variable, const char *url, char **value, uint32_t *len) {
  log("NPN_GetValueForURL(npp=%p variable=%s, url=\"%s\", value=%p len=%p)\n", 
      npp, (variable==NPNURLVCookie)?"cookie":
        ((variable==NPNURLVProxy)?"proxy":"unknown"), url, value, len);
  NPError e = gBrowserFuncs->getvalueforurl(npp, variable, url, value, len);
  log(" returned %d\n", e);
  return e;
}

NPError wrap_NPN_SetValueForURL (NPP npp, NPNURLVariable variable, const char *url, const char *value, uint32_t len) {
  log("NPN_SetValueForURL(npp=%p, variable=%s, url=\"%s\", value=\"%s\", len=%d)\n", 
      npp, (variable==NPNURLVCookie)?"cookie":
      ((variable==NPNURLVProxy)?"proxy":"unknown"), url, value, len);
  NPError e = gBrowserFuncs->setvalueforurl(npp, variable, url, value, len);
  log(" returned %d\n", e);
  return e;
}

NPError wrap_NPN_GetAuthenticationInfo (NPP npp, const char *protocol, const char *host, int32_t port, const char *scheme, const char *realm, char **username, uint32_t *ulen, char **password, uint32_t *plen) {
  log("NPN_GetAuthenticationInfo(npp=%p, protocol=\"%s\", host=\"%s\", "
      "port=%d, scheme=\"%s\", realm=\"%s\", username=%p, ulen=%p, "
      "password=%p, plen=%p\n",
      npp, protocol, host, port, scheme, realm, username, ulen, password, plen);
  NPError e = gBrowserFuncs->getauthenticationinfo(npp, protocol, host, port, scheme, realm, username, ulen, password, plen);
  log(" returned %d\n", e);
  return e;
}

uint32_t wrap_NPN_ScheduleTimer (NPP npp, uint32_t interval, NPBool repeat, void (*timerFunc)(NPP npp, uint32_t timerID)) {
  log("NPN_ScheduleTimer(npp=%p, interval=%d, repeat=%d, timerFunc=%p)\n", npp, interval, repeat, timerFunc);
  uint32_t r = gBrowserFuncs->scheduletimer(npp, interval, repeat, timerFunc);
  log(" returned %d\n", r);
  return r;
}

void wrap_NPN_UnscheduleTimer (NPP npp, uint32_t timerID) {
  log("NPN_UnscheduleTimer(npp=%p, timerID=%d)\n", npp, timerID);
  gBrowserFuncs->unscheduletimer(npp, timerID);
}

NPError wrap_NPN_PopUpContextMenu (NPP npp, NPMenu* menu) {
  log("NPN_PopUpContextMenu(npp=%p, NPMenu=%p)\n", npp, menu);
  NPError e = gBrowserFuncs->popupcontextmenu(npp, menu);
  log(" returned %d\n", e);
  return e;
}

NPBool wrap_NPN_ConvertPoint (NPP npp, double sourceX, double sourceY, NPCoordinateSpace sourceSpace, double *destX, double *destY, NPCoordinateSpace destSpace) {
  log("NPN_ConvertPoint(npp=%p, sourceX=%f, sourceY=%f, sourceSpace=%d, destX=%p, destY=%p, destSpace=%d)\n", npp, sourceX, sourceY, sourceSpace, destX, destY, destSpace);
  NPBool r = gBrowserFuncs->convertpoint(npp, sourceX, sourceY, sourceSpace, destX, destY, destSpace);
  log(" returned %d\n", r);
  return r;
}


/* wrapped plugin functions */
NPError
wrap_NPP_New(NPMIMEType   pluginType, 
             NPP          instance, 
             uint16_t     mode, 
             int16_t      argc, 
             char*        argn[], 
             char*        argv[], 
             NPSavedData* saved) {
  log("NPP_New(pluginType=\"%s\", instance=%p, mode=%d, argc=%d, saved=%p)\n",
      pluginType, instance, mode, argc, saved);
  for (int i=0; i<argc; i++) {
    log(" arg[%d] %s=\"%s\"\n", i, argn[i], argv[i]);
  }
  NPError e = gPluginFuncs->newp(pluginType, instance, mode, 
      argc, argn, argv, saved);
  log(" returned %d\n", e);
  return e;
}


NPError
wrap_NPP_Destroy(NPP instance, NPSavedData** save) {
  log("NPP_Destroy(instance=%p, save=%p)\n", instance, save);
  NPError e = gPluginFuncs->destroy(instance, save);
  log(" returned %d\n", e);
  return e;
}

NPError
wrap_NPP_SetWindow(NPP instance, NPWindow* window) {
  log("NPP_SetWindow(instance=%p, window=%p)\n", instance, window);
  NPError e = gPluginFuncs->setwindow(instance, window);
  log(" returned %d\n", e);
  return e;
};

NPError
wrap_NPP_NewStream(NPP instance, NPMIMEType type, NPStream* stream, 
    NPBool seekable, uint16_t* stype) {
  log("NPP_NewStream(instance=%p, type=\"%s\", stream=%p, seekable=%d, "
      "stype=%p)\n", instance, type, stream, seekable, stype);
  NPError e = gPluginFuncs->newstream(instance, type, stream, seekable, stype);
  log(" returned %d\n", e);
  return e;
}

NPError
wrap_NPP_DestroyStream(NPP instance, NPStream* stream, NPReason reason) {
  log("NPP_DestroyStream(instance=%p, stream=%p, reason=%d)\n", instance, stream, reason);
  NPError e = gPluginFuncs->destroystream(instance, stream, reason);
  log(" returned %d\n", e);
  return e;
};

void
wrap_NPP_StreamAsFile(NPP instance, NPStream* stream, const char* fname) {
  log("NPP_StreamAsFile(instance=%p, stream=%p, fname=\"%s\")\n", instance, stream, fname);
  gPluginFuncs->asfile(instance, stream, fname);
}

int32_t
wrap_NPP_WriteReady(NPP instance, NPStream* stream) {
  log("NPP_WriteReady(instance=%p, stream=%p)\n", instance, stream);
  int32_t r = gPluginFuncs->writeready(instance, stream);
  log(" returned %d\n", r);
  return r;
}

int32_t
wrap_NPP_Write(NPP instance, NPStream* stream, int32_t offset, int32_t len, void* buffer) {
  log("NPP_Write(instance=%p, stream=%p, offset=%d, len=%d, buffer=%p)\n", instance, stream, offset, len, buffer);
  int32_t r = gPluginFuncs->write(instance, stream, offset, len, buffer);
  log(" returned %d\n", r);
  return r;
}

void
wrap_NPP_Print(NPP instance, NPPrint* platformPrint) {
  log("NPP_Print(instance=%p, platformPrint=%p)\n", instance, platformPrint);
  gPluginFuncs->print(instance, platformPrint);
  return;
}

int16_t
wrap_NPP_HandleEvent(NPP instance, void* event) {
  log("NPP_HandleEvent(instance=%p, event=%p)\n", instance, event);
  int16_t r = gPluginFuncs->event(instance, event);
  log(" returned %d\n", r);
  return r;
}

void
wrap_NPP_URLNotify(NPP instance, const char* url, NPReason reason, void* notifyData) {
  log("NPP_URLNotify(instance=%p, url=\"%s\", reason=%d, notifyData=%p)\n",
      instance, url, reason, notifyData);
  gPluginFuncs->urlnotify(instance, url, reason, notifyData);
  return;
}

NPError
wrap_NPP_GetValue(NPP instance, NPPVariable variable, void* ret) {
  log("NPP_GetValue(instance=%p, variable=%d, ret=%p)\n", instance, variable, ret);
  NPError e = gPluginFuncs->getvalue(instance, variable, ret);
  return e;
}

NPError
wrap_NPP_SetValue(NPP instance, NPNVariable variable, void* ret) {
  log("NPP_SetValue(instance=%p, variable=%d, ret=%p)\n", instance, variable, ret);
  NPError e = gPluginFuncs->setvalue(instance, variable, ret);
  return e;
}

static void
initialize() {
  // initialize the plugin when it's first called
  gLogFile = fopen(LOGFILE, "w");

  log("loading the plugin so from: %s\n", PLUGIN);

  // load the plugin shared object
  gPlugin = dlopen(PLUGIN, RTLD_LOCAL);
  log("loaded the plugin as %p\n", gPlugin);
  if (gPlugin == NULL) {
    log("dlerror returns: %s\n", dlerror());
  }

  // get handles to all of the global function pointers from the plugin
  gExportedFlashFunctions.initialize = (NP_Initialize_Func)
    dlsym(gPlugin, "NP_Initialize");
  gExportedFlashFunctions.getPluginVersion = (NP_GetPluginVersion_Func)
    dlsym(gPlugin, "NP_GetPluginVersion");
  gExportedFlashFunctions.getMIMEDescription = (NP_GetMIMEDescription_Func)
    dlsym(gPlugin, "NP_GetMIMEDescription");
  gExportedFlashFunctions.getValue = (NP_GetValue_Func)
    dlsym(gPlugin, "NP_GetValue");
  gExportedFlashFunctions.shutdown = (NP_Shutdown_Func)
    dlsym(gPlugin, "NP_Shutdown");

  // set up our browser api wrappers
  gWrappedBrowserFuncs = new NPNetscapeFuncs;
  gWrappedBrowserFuncs->size = sizeof(NPNetscapeFuncs);
  gWrappedBrowserFuncs->version = 23;
  gWrappedBrowserFuncs->geturl = wrap_NPN_GetURL;
  gWrappedBrowserFuncs->posturl = wrap_NPN_PostURL;
  gWrappedBrowserFuncs->requestread = wrap_NPN_RequestRead;
  gWrappedBrowserFuncs->newstream = wrap_NPN_NewStream;
  gWrappedBrowserFuncs->write = wrap_NPN_Write;
  gWrappedBrowserFuncs->destroystream = wrap_NPN_DestroyStream;
  gWrappedBrowserFuncs->status = wrap_NPN_Status;
  gWrappedBrowserFuncs->uagent = wrap_NPN_UserAgent;
  gWrappedBrowserFuncs->memalloc = wrap_NPN_MemAlloc;
  gWrappedBrowserFuncs->memfree = wrap_NPN_MemFree;
  gWrappedBrowserFuncs->memflush = wrap_NPN_MemFlush;
  gWrappedBrowserFuncs->reloadplugins = wrap_NPN_ReloadPlugins;
  gWrappedBrowserFuncs->getJavaEnv = wrap_NPN_GetJavaEnv;
  gWrappedBrowserFuncs->getJavaPeer = wrap_NPN_GetJavaPeer;
  gWrappedBrowserFuncs->geturlnotify = wrap_NPN_GetURLNotify;
  gWrappedBrowserFuncs->posturlnotify = wrap_NPN_PostURLNotify;
  gWrappedBrowserFuncs->getvalue = wrap_NPN_GetValue;
  gWrappedBrowserFuncs->setvalue = wrap_NPN_SetValue;
  gWrappedBrowserFuncs->invalidaterect = wrap_NPN_InvalidateRect;
  gWrappedBrowserFuncs->invalidateregion = wrap_NPN_InvalidateRegion;
  gWrappedBrowserFuncs->forceredraw = wrap_NPN_ForceRedraw;
  gWrappedBrowserFuncs->getstringidentifier = wrap_NPN_GetStringIdentifier;
  gWrappedBrowserFuncs->getstringidentifiers = wrap_NPN_GetStringIdentifiers;
  gWrappedBrowserFuncs->getintidentifier = wrap_NPN_GetIntIdentifier;
  gWrappedBrowserFuncs->identifierisstring = wrap_NPN_IdentifierIsString;
  gWrappedBrowserFuncs->utf8fromidentifier = wrap_NPN_UTF8FromIdentifier;
  gWrappedBrowserFuncs->intfromidentifier = wrap_NPN_IntFromIdentifier;
  gWrappedBrowserFuncs->createobject = wrap_NPN_CreateObject;
  gWrappedBrowserFuncs->retainobject = wrap_NPN_RetainObject;
  gWrappedBrowserFuncs->releaseobject = wrap_NPN_ReleaseObject;
  gWrappedBrowserFuncs->invoke = wrap_NPN_Invoke;
  gWrappedBrowserFuncs->invokeDefault = wrap_NPN_InvokeDefault;
  gWrappedBrowserFuncs->evaluate = wrap_NPN_Evaluate;
  gWrappedBrowserFuncs->getproperty = wrap_NPN_GetProperty;
  gWrappedBrowserFuncs->setproperty = wrap_NPN_SetProperty;
  gWrappedBrowserFuncs->removeproperty = wrap_NPN_RemoveProperty;
  gWrappedBrowserFuncs->hasproperty = wrap_NPN_HasProperty;
  gWrappedBrowserFuncs->hasmethod = wrap_NPN_HasMethod;
  gWrappedBrowserFuncs->releasevariantvalue = wrap_NPN_ReleaseVariantValue;
  gWrappedBrowserFuncs->setexception = wrap_NPN_SetException;
  gWrappedBrowserFuncs->pushpopupsenabledstate = wrap_NPN_PushPopupsEnabledState;
  gWrappedBrowserFuncs->poppopupsenabledstate = wrap_NPN_PopPopupsEnabledState;
  gWrappedBrowserFuncs->enumerate = wrap_NPN_Enumerate;
  gWrappedBrowserFuncs->pluginthreadasynccall = wrap_NPN_PluginThreadAsyncCall;
  gWrappedBrowserFuncs->construct = wrap_NPN_Construct;
  gWrappedBrowserFuncs->getvalueforurl = wrap_NPN_GetValueForURL;
  gWrappedBrowserFuncs->setvalueforurl = wrap_NPN_SetValueForURL;
  gWrappedBrowserFuncs->getauthenticationinfo = wrap_NPN_GetAuthenticationInfo;
  gWrappedBrowserFuncs->scheduletimer = wrap_NPN_ScheduleTimer;
  gWrappedBrowserFuncs->unscheduletimer = wrap_NPN_UnscheduleTimer;
  gWrappedBrowserFuncs->popupcontextmenu = wrap_NPN_PopUpContextMenu;
  gWrappedBrowserFuncs->convertpoint = wrap_NPN_ConvertPoint;

  gInitialized = true;

  log("initialized\n");
}


NP_EXPORT(NPError)
NP_Initialize(NPNetscapeFuncs* aBrowserFuncs, 
              NPPluginFuncs* aPluginFuncs) {


  if (!gInitialized) initialize();

  log("NP_Initialize() browser version=%d, size=%d. "
      "wrapper version=%d, size=%d\n", 
      aBrowserFuncs->version, aBrowserFuncs->size, 
      gWrappedBrowserFuncs->version, gWrappedBrowserFuncs->size);

  // save off the browser functions
  gBrowserFuncs = aBrowserFuncs;

  // set up the wrapped plugin functions
  aPluginFuncs->size = sizeof(NPPluginFuncs);
  aPluginFuncs->version = 11;
  aPluginFuncs->newp = wrap_NPP_New;
  aPluginFuncs->destroy = wrap_NPP_Destroy;
  aPluginFuncs->setwindow = wrap_NPP_SetWindow;
  aPluginFuncs->newstream = wrap_NPP_NewStream;
  aPluginFuncs->destroystream = wrap_NPP_DestroyStream;
  aPluginFuncs->asfile = wrap_NPP_StreamAsFile;
  aPluginFuncs->writeready = wrap_NPP_WriteReady;
  aPluginFuncs->write = wrap_NPP_Write;
  aPluginFuncs->print = wrap_NPP_Print;
  aPluginFuncs->event = wrap_NPP_HandleEvent;
  aPluginFuncs->urlnotify = wrap_NPP_URLNotify;
  aPluginFuncs->javaClass = NULL; // javaClass - what to do?
  aPluginFuncs->getvalue = wrap_NPP_GetValue;
  aPluginFuncs->setvalue = wrap_NPP_SetValue;

  // don't claim to support more than the browser
  gWrappedBrowserFuncs->version = MIN(gWrappedBrowserFuncs->version, 
      gBrowserFuncs->version);
  gWrappedBrowserFuncs->size = MIN(gWrappedBrowserFuncs->size, 
      gBrowserFuncs->size);

  gPluginFuncs = new NPPluginFuncs;
  return gExportedFlashFunctions.initialize(gWrappedBrowserFuncs, gPluginFuncs);

  //return NPERR_NO_ERROR;
}

NP_EXPORT(char*)
NP_GetPluginVersion() {
  if (!gInitialized) initialize();
  log("NP_GetPluginVersion()\n");
  if (gExportedFlashFunctions.getPluginVersion != NULL) {
    char* v = gExportedFlashFunctions.getPluginVersion();
    log(" returned %s\n", v);
    return v;
  } else {
    log(" not defined on plugin, returning 1.0\n");
    return (char*)"1.0";
  }
}

NP_EXPORT(char*)
NP_GetMIMEDescription() {
  if (!gInitialized) initialize();
  log("NP_GetGetMIMEDescription()\n");
  char* md = gExportedFlashFunctions.getMIMEDescription();
  log(" returned %s\n", md);
  return md;
}

NP_EXPORT(NPError)
NP_GetValue(void* future, NPPVariable aVariable, void* aValue) {
  if (!gInitialized) initialize();
  log("NP_GetValue(%d)\n", aVariable);
  NPError e = NPERR_NO_ERROR;
  /*
  if (aVariable == NPPVpluginNameString) {
    *((char**)aValue) = (char*)NAME;
  } else {
  */
    e = gExportedFlashFunctions.getValue(future, aVariable, aValue);
    log("NP_GetValue returned %d\n", e);
  /*}*/
  switch (aVariable) {
    case NPPVpluginNameString:
      log("NP_GetValue pluginName=%s\n", *(const char**)aValue);
      break;
    case NPPVpluginDescriptionString:
      log("NP_GetValue pluginDescription=%s\n", *(const char**)aValue);
      break;
    case NPPVpluginWindowBool:
      log("NP_GetValue pluginWindowBool=%d\n", *(bool*)aValue);
      break;
    case NPPVpluginTransparentBool:
      log("NP_GetValue pluginTransparentBool=%d\n", *(bool*)aValue);
      break;
    default:
      // we only print values we might care about
      break;
  }
  return e;
}

NP_EXPORT(NPError)
NP_Shutdown()
{
  log("NP_Shutdown()\n");
  return NPERR_NO_ERROR;
}

