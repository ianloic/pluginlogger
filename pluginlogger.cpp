/* pluginlogger 0.1, a logging plugin wrapper 
 * Copyright 2009 Ian McKellar <http://ian.mckellar.org/>
 * */

/*  This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/


/* system headers for useful things */
#include <dlfcn.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

/* everyone loves the STL */
#include <map>
#include <string>

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
static ExportedPluginFunctions gExportedPluginFunctions = { NULL };

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

/* NPObject and NPClass tracking */
typedef enum {
  ORIGIN_UNKNOWN,
  ORIGIN_BROWSER,
  ORIGIN_PLUGIN,
} NPObjectOrigin;
class NPObjectTracker {
  private:
    typedef std::map<NPObject*,NPObjectTracker*> NPObjectMap;
    static NPObjectMap byObject;
    NPObject* mObject;
    NPObjectOrigin mOrigin;
    std::string mPath;
    std::string mPrintable;
    NPObjectTracker(NPObject* aObject, NPObjectOrigin aOrigin, 
        std::string aPath)
        : mObject(aObject), mOrigin(aOrigin), mPath(aPath) {
      updatePrintable();
    }
    void updatePrintable() {
      char ptr[64];
      snprintf(ptr, 64, "%p", mObject);
      std::string printable = std::string(mOrigin==ORIGIN_BROWSER?"B":
            (mOrigin==ORIGIN_PLUGIN?"P":"?"));
      printable.append(ptr);
      printable.append(":");
      printable.append(mPath);
      mPrintable.assign(printable);
    }
    void setOrigin(NPObjectOrigin aOrigin){
      mOrigin = aOrigin;
      updatePrintable();
    }
  public:
    static NPObjectTracker* getTracker(NPObject* aObject, 
        NPObjectOrigin aOrigin = ORIGIN_UNKNOWN, std::string aPath="") {
      NPObjectMap::iterator i = byObject.find(aObject);
      if (i == byObject.end()) {
        NPObjectTracker* tracker = new NPObjectTracker(aObject, aOrigin, aPath);
        byObject[aObject] = tracker;
        return tracker;
      }
      NPObjectTracker* tracker = byObject[aObject];
      if (tracker->mOrigin == ORIGIN_UNKNOWN && aOrigin != ORIGIN_UNKNOWN) {
        tracker->mOrigin = aOrigin;
      }
      return tracker;
    }
    static const char* c_str(NPObject* aObject) {
      return getTracker(aObject)->c_str();
    }
    NPObject* getObject() const { return mObject; }
    const char* c_str() const { return mPrintable.c_str(); }

    NPObjectTracker* trackChild(NPObject* aChildObject, 
        std::string aRelationship) {
      return getTracker(aChildObject, mOrigin, mPath + aRelationship);
    }

    NPObjectTracker* trackChild(NPObject* aChildObject,
        NPIdentifier aIdentifier, std::string aExtra="") {
      char buf[256];
      if (gBrowserFuncs->identifierisstring(aIdentifier)) {
        NPUTF8* utf8 = gBrowserFuncs->utf8fromidentifier(aIdentifier);
        snprintf(buf, 256, ".%s", utf8);
        gBrowserFuncs->memfree(utf8);
      } else {
        int32_t intvalue = gBrowserFuncs->intfromidentifier(aIdentifier);
        snprintf(buf, 256, "[%d]", intvalue);
      }
      return trackChild(aChildObject, std::string(buf) + aExtra);
    }
};
NPObjectTracker::NPObjectMap NPObjectTracker::byObject;

#if 0
class NPClassTracker {
  private:
    typedef std::map<NPClass*,NPClassTracker*> NPClassMap;
    static NPClassMap byClass;
    NPClass* mClass;
    bool mInPlugin;
    NPClassTracker(NPClass* aClass, bool aInPlugin) 
      : mClass(aClass), mInPlugin(aInPlugin) {
    }
  public:
    static NPClassTracker* getTracker(NPClass* aClass, bool aInPlugin) {
      NPClassMap::iterator i = byClass.find(aClass);
      if (i == byClass.end()) {
        byClass[aClass] = new NPClassTracker(aClass, aInPlugin);
      }
      return byClass[aClass];
    }
    NPClass* getClass() { return mClass; }
    bool inPlugin() { return mInPlugin; }
};
NPClassTracker::NPClassMap NPClassTracker::byClass;
#endif

/* helper to get the value of an NPIdentifier */
class NPIdentifierPrintable {
  private:
    char mPrintable[256];
  public:
    NPIdentifierPrintable(NPIdentifier& aIdentifier) {
      if (gBrowserFuncs->identifierisstring(aIdentifier)) {
        NPUTF8* utf8 = gBrowserFuncs->utf8fromidentifier(aIdentifier);
        snprintf(mPrintable, 256, "\"%s\"", utf8);
        gBrowserFuncs->memfree(utf8);
      } else {
        int32_t intvalue = gBrowserFuncs->intfromidentifier(aIdentifier);
        snprintf(mPrintable, 256, "%d", intvalue);
      }
    }
    operator const char*() const {
      return mPrintable;
    }
    const char* c_str() const {
      return mPrintable;
    }
};

/* helper to print the value of an NPString */
class NPStringPrintable {
  private:
    char* mPrintable;
  public:
    NPStringPrintable(const NPString* aString) {
      mPrintable = (char*)malloc(aString->UTF8Length + 3);
      snprintf(mPrintable, aString->UTF8Length + 3, "\"%s\"", 
          aString->UTF8Characters);
    }
    operator const char*() const {
      return mPrintable;
    }
    const char* c_str() const {
      return mPrintable;
    }
    ~NPStringPrintable() {
      free(mPrintable);
    }
};

/* helper to print the value of an NPVariant */
class NPVariantPrintable {
  private:
    const char *mPrintable;
    char mBuffer[32];
    NPStringPrintable* mString;
  public:
    NPVariantPrintable(const NPVariant* aVariant) 
      : mString(NULL) {
      switch(aVariant->type) {
        case NPVariantType_Void:
          mPrintable = "(void)";
          break;
        case NPVariantType_Null:
          mPrintable = "(null)";
          break;
        case NPVariantType_Bool:
          mPrintable = aVariant->value.boolValue?"(true)":"(false)";
          break;
        case NPVariantType_Int32:
          snprintf(mBuffer, 32, "%d", aVariant->value.intValue);
          mPrintable = mBuffer;
          break;
        case NPVariantType_Double:
          snprintf(mBuffer, 32, "%lf", aVariant->value.doubleValue);
          mPrintable = mBuffer;
          break;
        case NPVariantType_String:
          if (mString == NULL) {
            mString = new NPStringPrintable(&aVariant->value.stringValue);
          }
          mPrintable = mString->c_str();
          break;
        case NPVariantType_Object:
          NPObjectTracker* tracker = 
            NPObjectTracker::getTracker(aVariant->value.objectValue);
          mPrintable = tracker->c_str();
          break;
      }
    }
    operator const char*() const {
      return mPrintable;
    }
    const char* c_str() const {
      return mPrintable;
    }
    ~NPVariantPrintable() {
      if (mString != NULL) {
        delete mString;
      }
    }
};

/* helper to get the printable name of an NPNVariable */
const char* 
NPNVariableName(NPNVariable variable) {
  switch (variable) {
    case NPNVxDisplay: return "NPNVxDisplay"; break;
    case NPNVxtAppContext: return "NPNVxtAppContext"; break;
    case NPNVnetscapeWindow: return "NPNVnetscapeWindow"; break;
    case NPNVjavascriptEnabledBool: return "NPNVjavascriptEnabledBool"; break;
    case NPNVasdEnabledBool: return "NPNVasdEnabledBool"; break;
    case NPNVisOfflineBool: return "NPNVisOfflineBool"; break;
    case NPNVserviceManager: return "NPNVserviceManager"; break;
    case NPNVDOMElement: return "NPNVDOMElement"; break;
    case NPNVDOMWindow: return "NPNVDOMWindow"; break;
    case NPNVToolkit: return "NPNVToolkit"; break;
    case NPNVSupportsXEmbedBool: return "NPNVSupportsXEmbedBool"; break;
    case NPNVWindowNPObject: return "NPNVWindowNPObject"; break;
    case NPNVPluginElementNPObject: return "NPNVPluginElementNPObject"; break;
    case NPNVSupportsWindowless: return "NPNVSupportsWindowless"; break;
    case NPNVprivateModeBool: return "NPNVprivateModeBool"; break;
  }
  return "(unknown NPNVariable)";
}

/* helper to get the printable name of an NPPVariable */
const char* 
NPPVariableName(NPPVariable variable) {
  switch (variable) {
    case NPPVpluginNameString: return "NPPVpluginNameString"; break;
    case NPPVpluginDescriptionString: return "NPPVpluginDescriptionString"; break;
    case NPPVpluginWindowBool: return "NPPVpluginWindowBool"; break;
    case NPPVpluginTransparentBool: return "NPPVpluginTransparentBool"; break;
    case NPPVjavaClass: return "NPPVjavaClass"; break;
    case NPPVpluginWindowSize: return "NPPVpluginWindowSize"; break;
    case NPPVpluginTimerInterval: return "NPPVpluginTimerInterval"; break;
    case NPPVpluginScriptableInstance: return "NPPVpluginScriptableInstance"; break;
    case NPPVpluginScriptableIID: return "NPPVpluginScriptableIID"; break;
    case NPPVjavascriptPushCallerBool: return "NPPVjavascriptPushCallerBool"; break;
    case NPPVpluginKeepLibraryInMemory: return "NPPVpluginKeepLibraryInMemory"; break;
    case NPPVpluginNeedsXEmbed        : return "NPPVpluginNeedsXEmbed        "; break;
    case NPPVpluginScriptableNPObject : return "NPPVpluginScriptableNPObject "; break;
    case NPPVformValue: return "NPPVformValue"; break;
    case NPPVpluginUrlRequestsDisplayedBool: return "NPPVpluginUrlRequestsDisplayedBool"; break;
    case NPPVpluginWantsAllNetworkStreams: return "NPPVpluginWantsAllNetworkStreams"; break;
  }
  return "(unknown NPPVariable)";
}

/* helper to get the printable name of an NPError */
const char*
NPErrorName(NPError e) {
  switch(e) {
    case NPERR_NO_ERROR: return "NO_ERROR"; break;
    case NPERR_GENERIC_ERROR: return "GENERIC_ERROR"; break;
    case NPERR_INVALID_INSTANCE_ERROR: return "INVALID_INSTANCE_ERROR"; break;
    case NPERR_INVALID_FUNCTABLE_ERROR: return "INVALID_FUNCTABLE_ERROR"; break;
    case NPERR_MODULE_LOAD_FAILED_ERROR: return "MODULE_LOAD_FAILED_ERROR"; break;
    case NPERR_OUT_OF_MEMORY_ERROR: return "OUT_OF_MEMORY_ERROR"; break;
    case NPERR_INVALID_PLUGIN_ERROR: return "INVALID_PLUGIN_ERROR"; break;
    case NPERR_INVALID_PLUGIN_DIR_ERROR: return "INVALID_PLUGIN_DIR_ERROR"; break;
    case NPERR_INCOMPATIBLE_VERSION_ERROR: return "INCOMPATIBLE_VERSION_ERROR"; break;
    case NPERR_INVALID_PARAM: return "INVALID_PARAM"; break;
    case NPERR_INVALID_URL: return "INVALID_URL"; break;
    case NPERR_FILE_NOT_FOUND: return "FILE_NOT_FOUND"; break;
    case NPERR_NO_DATA: return "NO_DATA"; break;
    case NPERR_STREAM_NOT_SEEKABLE: return "STREAM_NOT_SEEKABLE"; break;
  }
  return "UNKNOWN NPError";
}

/* wrapped browser functions */
NPError 
wrap_NPN_GetValue (NPP npp, NPNVariable variable, void *ret_value) {
  log("NPN_GetValue(npp=%p, variable=%s, value=%p)\n", 
      npp, NPNVariableName(variable), ret_value);
  NPError e = gBrowserFuncs->getvalue(npp, variable, ret_value);
  if (e == NPERR_NO_ERROR) {
    switch(variable) {
      case NPNVxDisplay: 
        log("  NPNVxDisplay = %p\n", *(void**)ret_value);
        break;
      case NPNVxtAppContext: 
        log("  NPNVxtAppContext = %p\n", *(void**)ret_value);
        break;
      case NPNVnetscapeWindow: 
        log("  NPNVnetscapeWindow = %p\n", *(void**)ret_value);
        break;
      case NPNVjavascriptEnabledBool: 
        log("  NPNVjavascriptEnabledBool = %s\n", 
            (*(bool*)ret_value)?"true":"false");
        break;
      case NPNVasdEnabledBool: 
        log("  NPNVasdEnabledBool = %s\n", 
            (*(bool*)ret_value)?"true":"false");
        break;
      case NPNVisOfflineBool: 
        log("  NPNVisOfflineBool = %s\n", 
            (*(bool*)ret_value)?"true":"false");
        break;
      case NPNVserviceManager: 
        log("  NPNVserviceManager = %p\n", *(void**)ret_value);
        break;
      case NPNVDOMElement: 
        log("  NPNVDOMElement = %p\n", *(void**)ret_value);
        break;
      case NPNVDOMWindow: 
        log("  NPNVDOMWindow = %p\n", *(void**)ret_value);
        break;
      case NPNVToolkit: 
        log("  NPNVToolkit = %p\n", *(void**)ret_value);
        break;
      case NPNVSupportsXEmbedBool: 
        log("  NPNVSupportsXEmbedBool = %s\n", 
            (*(bool*)ret_value)?"true":"false");
        break;
      case NPNVWindowNPObject: 
        {
        NPObject* obj = *(NPObject**)ret_value;
        NPObjectTracker* tracker = 
          NPObjectTracker::getTracker(obj, ORIGIN_BROWSER, "window");
        log("  NPNVWindowNPObject = %s\n", tracker->c_str());
        }
        break;
      case NPNVPluginElementNPObject: 
        {
        NPObject* obj = *(NPObject**)ret_value;
        NPObjectTracker* tracker = 
          NPObjectTracker::getTracker(obj, ORIGIN_BROWSER, "plugin");
        log("  NPNVPluginElementNPObject = %s\n", tracker->c_str());
        }
        break;
      case NPNVSupportsWindowless: 
        log("  NPNVSupportsWindowless = %s\n", 
            (*(bool*)ret_value)?"true":"false");
        break;
      case NPNVprivateModeBool: 
        log("  NPNVprivateModeBool = %s\n", 
            (*(bool*)ret_value)?"true":"false");
        break;
    }
  }
  log(" returned %s\n", NPErrorName(e));
  return e;
}

NPError 
wrap_NPN_SetValue (NPP npp, NPPVariable variable, void *value) {
  log("NPN_SetValue(npp=%p, variable=%s, value=%p)\n", 
      npp, NPPVariableName(variable), value);
  NPError e = gBrowserFuncs->setvalue(npp, variable, value);
  log(" returned %s\n", NPErrorName(e));
  return e;
}

NPError 
wrap_NPN_GetURLNotify (NPP npp, const char* url, const char* window, 
    void* notifyData) {
  log("NPN_GetURLNotify(npp=%p, url=\"%s\", window=\"%s\", notifydata=%p)\n", 
      npp, url, window, notifyData);
  NPError e = gBrowserFuncs->geturlnotify(npp, url, window, notifyData);
  log(" returned %s\n", NPErrorName(e));
  return e;
}

NPError 
wrap_NPN_PostURLNotify (NPP npp, const char* url, const char* window, 
    uint32_t len, const char* buf, NPBool file, void* notifyData) {
  log("NPN_PostURLNotify(npp=%p, url=\"%s\", window=\"%s\", len=%d, buf=%p, "
      "file=%d, notifyData=%p)\n", 
      npp, url, window, len, buf, file, notifyData);
  NPError e = gBrowserFuncs->posturlnotify(npp, url, window, len, buf, file, 
      notifyData);
  log(" returned %s\n", NPErrorName(e));
  return e;
}

NPError 
wrap_NPN_GetURL (NPP npp, const char* url, const char* window) {
  log("NPN_GetURL(npp=%p, url=\"%s\", window=\"%s\")\n", npp, url, window);
  NPError e = gBrowserFuncs->geturl(npp, url, window);
  log(" returned %s\n", NPErrorName(e));
  return e;
}

NPError 
wrap_NPN_PostURL (NPP npp, const char* url, const char* window, 
    uint32_t len, const char* buf, NPBool file) {
  log("NPN_PostURL(npp=%p, url=\"%s\", window=\"%s\", len=%d, "
      "buf=%p, file=%d)\n", npp, url, window, len, buf, file);
  NPError e = gBrowserFuncs->posturl(npp, url, window, len, buf, file);
  log(" returned %s\n", NPErrorName(e));
  return e;
}

NPError 
wrap_NPN_RequestRead (NPStream* stream, NPByteRange* rangeList) {
  log("NPN_RequestRead(stream=%p)\n", stream);
  for (NPByteRange* r=rangeList; r!=NULL; r=r->next) {
    log("  range offset=%d length=%d\n", r->offset, r->length);    
  }
  NPError e = gBrowserFuncs->requestread(stream, rangeList);
  log(" returned %s\n", NPErrorName(e));
  return e;
}

NPError 
wrap_NPN_NewStream (NPP npp, NPMIMEType type, const char* window, 
    NPStream** stream) {
  log("NPN_NewStream(npp=%p, type=\"%s\", window=\"%s\", stream=%p)\n", 
      npp, type, window, stream);
  NPError e = gBrowserFuncs->newstream(npp, type, window, stream);
  log(" returned %s\n", NPErrorName(e));
  return e;
}

int32_t 
wrap_NPN_Write (NPP npp, NPStream* stream, int32_t len, void* buffer) {
  log("NPN_Write(npp=%p, stream=%p, len=%d, buffer=%p\n", 
      npp, stream, len, buffer);
  int32_t r = gBrowserFuncs->write(npp, stream, len, buffer);
  log(" returned %d\n", r);
  return r;
}

NPError 
wrap_NPN_DestroyStream (NPP npp, NPStream* stream, NPReason reason) {
  log("NPN_DestroyStream(npp=%p, stream=%p, reason=%d)\n", 
      npp, stream, reason);
  NPError e = gBrowserFuncs->destroystream(npp, stream, reason);
  log(" returned %s\n", NPErrorName(e));
  return e;
}

void 
wrap_NPN_Status (NPP npp, const char* message) {
  log("NPN_Status(npp=%p, message=\"%s\"\n", npp, message);
  gBrowserFuncs->status(npp, message);
  return;
}

const char* 
wrap_NPN_UserAgent (NPP npp) {
  log("NPN_UserAgent(npp=%p)\n", npp);
  const char* r = gBrowserFuncs->uagent(npp);
  log(" returned \"%s\"\n", r);
  return r;
}

void* 
wrap_NPN_MemAlloc (uint32_t size) {
  log("NPN_MemAlloc(size=%d)\n", size);
  void* r = gBrowserFuncs->memalloc(size);
  log(" returned \"%p\"\n", r);
  return r;
}

void 
wrap_NPN_MemFree (void* ptr) {
  log("NPN_MemFree(ptr=%p)\n", ptr);
  gBrowserFuncs->memfree(ptr);
  return;
}

uint32_t 
wrap_NPN_MemFlush (uint32_t size) {
  log("NPN_MemFlush(size=%d)\n", size);
  uint32_t r = gBrowserFuncs->memflush(size);
  log(" returned %d\n", r);
  return r;
}

void 
wrap_NPN_ReloadPlugins (NPBool reloadPages) {
  log("NPN_ReloadPlugins(reloadPages=%d)\n", reloadPages);
  gBrowserFuncs->reloadplugins(reloadPages);
}

void* 
wrap_NPN_GetJavaEnv () {
  log("NPN_GetJavaEnv()\n");
  void* r = gBrowserFuncs->getJavaEnv();
  log(" returned %p\n", r);
  return r;
}

void* 
wrap_NPN_GetJavaPeer (NPP npp) {
  log("NPN_GetJavaPeer(npp=%p)\n", npp);
  void* r = gBrowserFuncs->getJavaPeer(npp);
  log(" returned %p\n", r);
  return r;
}

void 
wrap_NPN_InvalidateRect (NPP npp, NPRect *rect) {
  log("NPN_InvalidateRect(npp=%p rect={top=%d, left=%d, bottom=%d, "
      "right=%d})\n", npp, rect->top, rect->left, rect->bottom, rect->right);
  gBrowserFuncs->invalidaterect(npp, rect);
  return;
}

void 
wrap_NPN_InvalidateRegion (NPP npp, NPRegion region) {
  log("NPN_InvalidateRegion(npp=%p, region=%p\n", npp, region);
  gBrowserFuncs->invalidateregion(npp, region);
  return;
}

void 
wrap_NPN_ForceRedraw (NPP npp) {
  log("NPN_ForceRedraw(npp=%p)\n", npp);
  gBrowserFuncs->forceredraw(npp);
  return;
}

NPIdentifier 
wrap_NPN_GetStringIdentifier (const NPUTF8* name) {
  log("NPN_GetStringIdentifier(name=\"%s\")\n", name);
  NPIdentifier r = gBrowserFuncs->getstringidentifier(name);
  log(" returned %p\n", r);
  return r;
}

void 
wrap_NPN_GetStringIdentifiers (const NPUTF8** names, int32_t nameCount, 
    NPIdentifier* identifiers) {
  log("NPN_GetStringIdentifiers(nameCount=%d)\n", nameCount);
  gBrowserFuncs->getstringidentifiers(names, nameCount, identifiers);
  log(" returned: \n");
  for (int i=0; i<nameCount; i++) {
    log("  \"%s\" -> %p\n", names[i], identifiers[i]);
  }
}

NPIdentifier 
wrap_NPN_GetIntIdentifier (int32_t intid) {
  log("NPN_GetIntIdentifier(intid=%d)\n", intid);
  NPIdentifier r = gBrowserFuncs->getintidentifier(intid);
  log(" returned %p\n", r);
  return r;
}

bool 
wrap_NPN_IdentifierIsString (NPIdentifier identifier) {
  log("NPN_IdentifierIsString(identifier=%p)\n", identifier);
  bool r = gBrowserFuncs->identifierisstring(identifier);
  log(" returned %d\n", r);
  return r;
}

NPUTF8* 
wrap_NPN_UTF8FromIdentifier (NPIdentifier identifier) {
  log("NPN_UTF8FromIdentifier(identifier=%p)\n", identifier);
  NPUTF8* r = gBrowserFuncs->utf8fromidentifier(identifier);
  log(" returned \"%s\"\n", r);
  return r;
}

int32_t 
wrap_NPN_IntFromIdentifier (NPIdentifier identifier) {
  log("NPN_IntFromIdentifier(identifier=%p)\n", identifier);
  int32_t r = gBrowserFuncs->intfromidentifier(identifier);
  log(" returned %d\n", r);
  return r;
}

NPObject* 
wrap_NPN_CreateObject (NPP npp, NPClass *aClass) {
  log("NPN_CreateObject(npp=%p, class=%p)\n", npp, aClass);
  NPObject* r = gBrowserFuncs->createobject(npp, aClass);
  // the plugin is requesting that the browser create an object
  // so I think it belongs on the plugin side. we will see...
  NPObjectTracker::getTracker(r, ORIGIN_PLUGIN);
  log(" returned %s\n", NPObjectTracker::c_str(r));
  return r;
}

NPObject* 
wrap_NPN_RetainObject (NPObject *obj) {
  log("NPN_RetainObject(obj=%s)\n", NPObjectTracker::c_str(obj));
  NPObject* r = gBrowserFuncs->retainobject(obj);
  log(" returned %s\n", NPObjectTracker::c_str(r));
  return r;
}

void 
wrap_NPN_ReleaseObject (NPObject *obj) {
  log("NPN_ReleaseObject(obj=%p)\n", NPObjectTracker::c_str(obj));
  // FIXME: should we remove it from the tracker if refcount==0?
  gBrowserFuncs->releaseobject(obj);
  return;
}

bool 
wrap_NPN_Invoke (NPP npp, NPObject* obj, NPIdentifier methodName, 
    const NPVariant *args, uint32_t argCount, NPVariant *result) {
  log("NPN_Invoke(npp=%p, obj=%s, methodName=%s)\n", npp, 
      NPObjectTracker::c_str(obj), NPIdentifierPrintable(methodName).c_str());
  for (uint32_t i=0; i<argCount; i++) {
    log("  args[%d] = %s\n", i, NPVariantPrintable(&args[i]).c_str());
  }
  bool r = gBrowserFuncs->invoke(npp, obj, methodName, args, argCount, 
      result);
  if (r) {
    log(" returned true, result=%s\n", NPVariantPrintable(result).c_str());
  } else {
    log(" returned false\n");
  }
  return r;
}

bool 
wrap_NPN_InvokeDefault (NPP npp, NPObject* obj, const NPVariant *args, 
    uint32_t argCount, NPVariant *result) {
  log("NPN_InvokeDefault(npp=%p, obj=%s)\n", npp, NPObjectTracker::c_str(obj));
  for (uint32_t i=0; i<argCount; i++) {
    log("  args[%d] = %s\n", i, NPVariantPrintable(&args[i]).c_str());
  }
  bool r = gBrowserFuncs->invokeDefault(npp, obj, args, argCount, result);
  // FIXME: if the return value is an object we want to track that
  log(" returned %d, result=%s\n", r, NPVariantPrintable(result).c_str());
  return r;
}

bool 
wrap_NPN_Evaluate (NPP npp, NPObject *obj, NPString *script, 
    NPVariant *result) {
  log("NPN_Evaluate(npp=%p, obj=%s, script=%s)\n", npp, 
      NPObjectTracker::c_str(obj), NPStringPrintable(script).c_str());
  bool r = gBrowserFuncs->evaluate(npp, obj, script, result);
  // FIXME: if the return value is an object we want to track that
  log(" returned %d, result=%s\n", r, NPVariantPrintable(result).c_str());
  return r;
}

bool 
wrap_NPN_GetProperty (NPP npp, NPObject *obj, NPIdentifier propertyName, 
    NPVariant *result) {
  log("NPN_GetProperty(npp=%p, obj=%s, propertyName=%s)\n", npp, 
      NPObjectTracker::c_str(obj), NPIdentifierPrintable(propertyName).c_str());
  bool r = gBrowserFuncs->getproperty(npp, obj, propertyName, result);
  if (r) {
    // if the return value is an object we want to track that
    if (NPVARIANT_IS_OBJECT(*result)) {
      NPObjectTracker::getTracker(obj)->trackChild(
          NPVARIANT_TO_OBJECT(*result), propertyName);
    }
    log(" returned true, result=%s\n", NPVariantPrintable(result).c_str());
  } else {
    log(" returned false\n");
  }
  return r;
}

bool 
wrap_NPN_SetProperty (NPP npp, NPObject *obj, NPIdentifier propertyName, 
    const NPVariant *value) {
  log("NPN_SetProperty(npp=%p, obj=%s, propertyName=%s, value=%p)\n", 
      npp, NPObjectTracker::c_str(obj), 
      NPIdentifierPrintable(propertyName).c_str(), 
      NPVariantPrintable(value).c_str());
  bool r = gBrowserFuncs->setproperty(npp, obj, propertyName, value);
  log(" returned %d\n", r);
  return r;
}

bool 
wrap_NPN_RemoveProperty (NPP npp, NPObject *obj, 
    NPIdentifier propertyName) {
  log("NPN_RemoveProperty(npp=%p, obj=%s, properyName=%s)\n", npp, 
      NPObjectTracker::c_str(obj), NPIdentifierPrintable(propertyName).c_str());
  bool r = gBrowserFuncs->removeproperty(npp, obj, propertyName);
  log(" returned %d\n", r);
  return r;
}

bool 
wrap_NPN_HasProperty (NPP npp, NPObject *obj, NPIdentifier propertyName) {
  log("NPN_HasProperty(npp=%p, obj=%s, propertyName=%s)\n", npp, 
      NPObjectTracker::c_str(obj), NPIdentifierPrintable(propertyName).c_str());
  bool r = gBrowserFuncs->hasproperty(npp, obj, propertyName);
  log(" returned %d\n", r);
  return r;
}

bool 
wrap_NPN_HasMethod (NPP npp, NPObject *obj, NPIdentifier propertyName) {
  log("NPN_HasMethod(npp=%p, obj=%s, propertyName=%s)\n", npp, 
      NPObjectTracker::c_str(obj), NPIdentifierPrintable(propertyName).c_str());
  bool r = gBrowserFuncs->hasmethod(npp, obj, propertyName);
  log(" returned %d\n", r);
  return r;
}

void 
wrap_NPN_ReleaseVariantValue (NPVariant *variant) {
  log("NPN_ReleaseVariantValue(variant=%s)\n", 
      NPVariantPrintable(variant).c_str());
  gBrowserFuncs->releasevariantvalue(variant);
  return;
}

void 
wrap_NPN_SetException (NPObject *obj, const NPUTF8 *message) {
  log("NPN_SetException(obj=%s, message=\"%s\")\n", 
      NPObjectTracker::c_str(obj), message);
  gBrowserFuncs->setexception(obj, message);
  return;
}

bool 
wrap_NPN_PushPopupsEnabledState (NPP npp, NPBool enabled) {
  log("NPN_PushPopupsEnabledState(npp=%p, enabled=%d)\n", npp, enabled);
  bool r = gBrowserFuncs->pushpopupsenabledstate(npp, enabled);
  log(" returned %d\n", r);
  return r;
}

bool 
wrap_NPN_PopPopupsEnabledState (NPP npp) {
  log("NPN_PopPopupsEnabledState(npp=%p)\n", npp);
  bool r = gBrowserFuncs->poppopupsenabledstate(npp);
  log(" returned %d\n", r);
  return r;
}

bool 
wrap_NPN_Enumerate (NPP npp, NPObject *obj, NPIdentifier **identifier, 
    uint32_t *count) {
  log("NPN_Enumerate(npp=%p, obj=%s)\n", npp, NPObjectTracker::c_str(obj));
  bool r = gBrowserFuncs->enumerate(npp, obj, identifier, count);
  if (r) {
    for (uint32_t i = 0; i < *count; i++) {
      log("  %s\n", NPIdentifierPrintable(*identifier[i]).c_str());
    }
  }
  log(" returned %d\n", r);
  return r;
}

void 
wrap_NPN_PluginThreadAsyncCall (NPP npp, void (*func)(void *), 
    void *userData) {
  log("NPN_PluginThreadAsyncCall(npp=%p, func=%p, userData=%p)\n", 
      npp, func, userData);
  gBrowserFuncs->pluginthreadasynccall(npp, func, userData);
}

bool 
wrap_NPN_Construct (NPP npp, NPObject* obj, const NPVariant *args, 
    uint32_t argCount, NPVariant *result) {
  log("NPN_Construct(npp=%p, obj=%s)\n", npp, NPObjectTracker::c_str(obj));
  for (uint32_t i = 0; i<argCount; i++) {
    log("  arg[%d] = %s\n", i, NPVariantPrintable(&args[i]).c_str());
  }
  bool r = gBrowserFuncs->construct(npp, obj, args, argCount, result);
  // FIXME: check return value before showing result?
  log(" returned %d, result=%s\n", r, NPVariantPrintable(result).c_str());
  return r;
}

NPError 
wrap_NPN_GetValueForURL (NPP npp, NPNURLVariable variable, 
    const char *url, char **value, uint32_t *len) {
  log("NPN_GetValueForURL(npp=%p variable=%s, url=\"%s\")\n", npp, 
      (variable==NPNURLVCookie)?"cookie": 
      ((variable==NPNURLVProxy)?"proxy":"unknown"), url);
  NPError e = gBrowserFuncs->getvalueforurl(npp, variable, url, value, len);
  // FIXME: check return before printing values?
  for (uint32_t i=0; i<*len; i++) {
    log("  value[%d] = \"%s\"\n", i, value[i]);
  }
  log(" returned %s\n", NPErrorName(e));
  return e;
}

NPError 
wrap_NPN_SetValueForURL (NPP npp, NPNURLVariable variable, 
    const char *url, const char *value, uint32_t len) {
  log("NPN_SetValueForURL(npp=%p, variable=%s, url=\"%s\", value=\"%s\", "
      "len=%d)\n", npp, 
      (variable==NPNURLVCookie)?"cookie":
      ((variable==NPNURLVProxy)?"proxy":"unknown"), url, value, len);
  NPError e = gBrowserFuncs->setvalueforurl(npp, variable, url, value, len);
  log(" returned %s\n", NPErrorName(e));
  return e;
}

NPError 
wrap_NPN_GetAuthenticationInfo (NPP npp, const char *protocol, 
    const char *host, int32_t port, const char *scheme, 
    const char *realm, char **username, uint32_t *ulen, 
    char **password, uint32_t *plen) {
  log("NPN_GetAuthenticationInfo(npp=%p, protocol=\"%s\", host=\"%s\", "
      "port=%d, scheme=\"%s\", realm=\"%s\")\n", 
      npp, protocol, host, port, scheme, realm);
  NPError e = gBrowserFuncs->getauthenticationinfo(npp, protocol, host, 
      port, scheme, realm, username, ulen, password, plen);
  // FIXME: check return value before printing username & password?
  // FIXME: copy & truncate username & password
  log(" returned %s, username=\"%s\", password=\"%s\"\n", 
      NPErrorName(e), *username, *password);
  return e;
}

uint32_t 
wrap_NPN_ScheduleTimer (NPP npp, uint32_t interval, NPBool repeat, 
    void (*timerFunc)(NPP npp, uint32_t timerID)) {
  log("NPN_ScheduleTimer(npp=%p, interval=%d, repeat=%d, timerFunc=%p)\n", 
      npp, interval, repeat, timerFunc);
  uint32_t r = gBrowserFuncs->scheduletimer(npp, interval, repeat, timerFunc);
  log(" returned %d\n", r);
  return r;
}

void 
wrap_NPN_UnscheduleTimer (NPP npp, uint32_t timerID) {
  log("NPN_UnscheduleTimer(npp=%p, timerID=%d)\n", npp, timerID);
  gBrowserFuncs->unscheduletimer(npp, timerID);
  return;
}

NPError 
wrap_NPN_PopUpContextMenu (NPP npp, NPMenu* menu) {
  log("NPN_PopUpContextMenu(npp=%p, NPMenu=%p)\n", npp, menu);
  NPError e = gBrowserFuncs->popupcontextmenu(npp, menu);
  log(" returned %s\n", NPErrorName(e));
  return e;
}

NPBool 
wrap_NPN_ConvertPoint (NPP npp, 
    double sourceX, double sourceY, NPCoordinateSpace sourceSpace, 
    double *destX, double *destY, NPCoordinateSpace destSpace) {
  log("NPN_ConvertPoint(npp=%p, sourceX=%lf, sourceY=%lf, sourceSpace=%d, "
      "destSpace=%d)\n", npp, sourceX, sourceY, sourceSpace, destSpace);
  NPBool r = gBrowserFuncs->convertpoint(npp, sourceX, sourceY, sourceSpace, 
      destX, destY, destSpace);
  // FIXME: should we print destX and destY on r==FALSE?
  // how can I tell? it's not documented anywhere
  log(" returned %d, destX=%lf, destY=%lf\n", r, *destX, *destY);
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
  log(" returned %s\n", NPErrorName(e));
  return e;
}


NPError
wrap_NPP_Destroy(NPP instance, NPSavedData** save) {
  log("NPP_Destroy(instance=%p, save=%p)\n", instance, save);
  NPError e = gPluginFuncs->destroy(instance, save);
  log(" returned %s\n", NPErrorName(e));
  return e;
}

NPError
wrap_NPP_SetWindow(NPP instance, NPWindow* window) {
  log("NPP_SetWindow(instance=%p, window=%p)\n", instance, window);
  NPError e = gPluginFuncs->setwindow(instance, window);
  log(" returned %s\n", NPErrorName(e));
  return e;
};

NPError
wrap_NPP_NewStream(NPP instance, NPMIMEType type, NPStream* stream, 
    NPBool seekable, uint16_t* stype) {
  log("NPP_NewStream(instance=%p, type=\"%s\", stream=%p, seekable=%d, "
      "stype=%p)\n", instance, type, stream, seekable, stype);
  NPError e = gPluginFuncs->newstream(instance, type, stream, seekable, stype);
  log(" returned %s\n", NPErrorName(e));
  return e;
}

NPError
wrap_NPP_DestroyStream(NPP instance, NPStream* stream, NPReason reason) {
  log("NPP_DestroyStream(instance=%p, stream=%p, reason=%d)\n", 
      instance, stream, reason);
  NPError e = gPluginFuncs->destroystream(instance, stream, reason);
  log(" returned %s\n", NPErrorName(e));
  return e;
};

void
wrap_NPP_StreamAsFile(NPP instance, NPStream* stream, const char* fname) {
  log("NPP_StreamAsFile(instance=%p, stream=%p, fname=\"%s\")\n", 
      instance, stream, fname);
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
wrap_NPP_Write(NPP instance, NPStream* stream, int32_t offset, int32_t len, 
    void* buffer) {
  log("NPP_Write(instance=%p, stream=%p, offset=%d, len=%d, buffer=%p)\n", 
      instance, stream, offset, len, buffer);
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
wrap_NPP_URLNotify(NPP instance, const char* url, NPReason reason, 
    void* notifyData) {
  log("NPP_URLNotify(instance=%p, url=\"%s\", reason=%d, notifyData=%p)\n",
      instance, url, reason, notifyData);
  gPluginFuncs->urlnotify(instance, url, reason, notifyData);
  return;
}

NPError
wrap_NPP_GetValue(NPP instance, NPPVariable variable, void* ret) {
  log("NPP_GetValue(instance=%p, variable=%s, ret=%p)\n", 
      instance, NPPVariableName(variable), ret);
  NPError e = gPluginFuncs->getvalue(instance, variable, ret);
  log(" returned %s\n", NPErrorName(e));
  return e;
}

NPError
wrap_NPP_SetValue(NPP instance, NPNVariable variable, void* ret) {
  log("NPP_SetValue(instance=%p, variable=%s, ret=%p)\n", 
      instance, NPNVariableName(variable), ret);
  NPError e = gPluginFuncs->setvalue(instance, variable, ret);
  log(" returned %s\n", NPErrorName(e));
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
  gExportedPluginFunctions.initialize = (NP_Initialize_Func)
    dlsym(gPlugin, "NP_Initialize");
  gExportedPluginFunctions.getPluginVersion = (NP_GetPluginVersion_Func)
    dlsym(gPlugin, "NP_GetPluginVersion");
  gExportedPluginFunctions.getMIMEDescription = (NP_GetMIMEDescription_Func)
    dlsym(gPlugin, "NP_GetMIMEDescription");
  gExportedPluginFunctions.getValue = (NP_GetValue_Func)
    dlsym(gPlugin, "NP_GetValue");
  gExportedPluginFunctions.shutdown = (NP_Shutdown_Func)
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
  NPError e = gExportedPluginFunctions.initialize(gWrappedBrowserFuncs, 
      gPluginFuncs);
  log(" returning %s\n", NPErrorName(e));
  return e;
}

NP_EXPORT(char*)
NP_GetPluginVersion() {
  if (!gInitialized) initialize();
  log("NP_GetPluginVersion()\n");
  if (gExportedPluginFunctions.getPluginVersion != NULL) {
    char* v = gExportedPluginFunctions.getPluginVersion();
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
  char* md = gExportedPluginFunctions.getMIMEDescription();
  log(" returned %s\n", md);
  return md;
}

NP_EXPORT(NPError)
NP_GetValue(void* future, NPPVariable aVariable, void* aValue) {
  if (!gInitialized) initialize();
  log("NP_GetValue(%s)\n", NPPVariableName(aVariable));
  NPError e = gExportedPluginFunctions.getValue(future, aVariable, aValue);
  // FIXME: only print on success?
  switch (aVariable) {
    case NPPVpluginNameString:
      log(" pluginName=%s\n", *(const char**)aValue);
      break;
    case NPPVpluginDescriptionString:
      log(" pluginDescription=%s\n", *(const char**)aValue);
      break;
    case NPPVpluginWindowBool:
      log(" pluginWindowBool=%d\n", *(bool*)aValue);
      break;
    case NPPVpluginTransparentBool:
      log(" pluginTransparentBool=%d\n", *(bool*)aValue);
      break;
    default:
      // we only print values we might care about
      break;
  }
  log("NP_GetValue returned %s\n", NPErrorName(e));

  return e;
}

NP_EXPORT(NPError)
NP_Shutdown()
{
  log("NP_Shutdown()\n");
  NPError e = gExportedPluginFunctions.shutdown();
  log(" returned %s\n", NPErrorName(e));
  return e;
}

