/* flashliar 0.1, a plugin wrapper for Adobe Flash Player */

/* system headers for useful things */
#include <dlfcn.h>
#include <stdio.h>
#include <stdarg.h>

/* load the npapi headers */
#include "npapi.h"
#include "npupp.h"
#include "npruntime.h"

#define VERSION "0.1.0.0"
#define MIME_DESCRIPTION "application/x-flash-liar:lie:Flash Liar"
#define NAME "Flash Liar"
#define DESCRIPTION "Tell me lies, tell me sweet little lies"

#define FLASHPLAYER "/usr/lib/flashplugin-installer/libflashplayer.so"

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
static NPNetscapeFuncs* gBrowserFuncs = NULL;
static void* gFlashPlugin = NULL;
static ExportedPluginFunctions gExportedFlashFunctions = { NULL };

static FILE* gLogFile = NULL;
static void log(const char* format, ...) {
  va_list argp;
  va_start(argp, format);
  vfprintf(gLogFile, format, argp);
  va_end(argp);
  fflush(gLogFile);
}

NPError
NPP_New(NPMIMEType   pluginType, 
        NPP          instance, 
        uint16_t     mode, 
        int16_t      argc, 
        char*        argn[], 
        char*        argv[], 
        NPSavedData* saved) {
  return NPERR_NO_ERROR;
}

NPError
NPP_Destroy(NPP instance, NPSavedData** save) {
  return NPERR_NO_ERROR;
}


static void
initialize() {
  // initialize the plugin when it's first called
  gLogFile = fopen("/tmp/liar.log", "w");

  log("loading the flash player so\n");

  // load the flash player shared object
  gFlashPlugin = dlopen(FLASHPLAYER, RTLD_LOCAL);
  log("loaded the flash player as %p\n", gFlashPlugin);
  if (gFlashPlugin == NULL) {
    log("dlerror returns: %s\n", dlerror());
  }
  // get handles to all of the global function pointers from the flash
  // plugin
  gExportedFlashFunctions.initialize = (NP_Initialize_Func)
    dlsym(gFlashPlugin, "NP_Initialize");
  gExportedFlashFunctions.getPluginVersion = (NP_GetPluginVersion_Func)
    dlsym(gFlashPlugin, "NP_GetPluginVersion");
  gExportedFlashFunctions.getMIMEDescription = (NP_GetMIMEDescription_Func)
    dlsym(gFlashPlugin, "NP_GetMIMEDescription");
  gExportedFlashFunctions.getValue = (NP_GetValue_Func)
    dlsym(gFlashPlugin, "NP_GetValue");
  gExportedFlashFunctions.shutdown = (NP_Shutdown_Func)
    dlsym(gFlashPlugin, "NP_Shutdown");

  gInitialized = true;

  log("initialized\n");
}


NP_EXPORT(NPError)
NP_Initialize(NPNetscapeFuncs* aBrowserFuncs, 
              NPPluginFuncs* aPluginFuncs) {

  // save off the browser functions
  gBrowserFuncs = aBrowserFuncs;

  if (!gInitialized) initialize();

  log("NP_Initialize()\n");

  return gExportedFlashFunctions.initialize(aBrowserFuncs, aPluginFuncs);

#if 0
  // fill the plugin functions struct
  aPluginFuncs->version = 11;
  aPluginFuncs->size = sizeof(*aPluginFuncs);
  aPluginFuncs->newp = NPP_New;
  aPluginFuncs->destroy = NPP_Destroy;
  aPluginFuncs->setwindow = NPP_SetWindow;
  aPluginFuncs->newstream = NPP_NewStream;
  aPluginFuncs->destroystream = NPP_DestroyStream;
  aPluginFuncs->asfile = NPP_StreamAsFile;
  aPluginFuncs->writeready = NPP_WriteReady;
  aPluginFuncs->write = NPP_Write;
  aPluginFuncs->print = NPP_Print;
  aPluginFuncs->event = NPP_HandleEvent;
  aPluginFuncs->urlnotify = NPP_URLNotify;
  aPluginFuncs->getvalue = NPP_GetValue;
  aPluginFuncs->setvalue = NPP_SetValue;
#endif
 
  //return NPERR_NO_ERROR;
}

NP_EXPORT(char*)
NP_GetPluginVersion() {
  if (!gInitialized) initialize();
  log("NP_GetPluginVersion()\n");
  return (char*)VERSION;
}

NP_EXPORT(char*)
NP_GetMIMEDescription() {
  if (!gInitialized) initialize();
  log("NP_GetGetMIMEDescription()\n");
  char* md = gExportedFlashFunctions.getMIMEDescription();
  log(" flash returned %s\n", md);
  //return md;
  return (char*)MIME_DESCRIPTION;
}

NP_EXPORT(NPError)
NP_GetValue(void* future, NPPVariable aVariable, void* aValue) {
  if (!gInitialized) initialize();
  log("NP_GetValue(%d)\n", aVariable);
  NPError e = NPERR_NO_ERROR;
  if (aVariable == NPPVpluginNameString) {
    *((char**)aValue) = (char*)NAME;
  } else {
    e = gExportedFlashFunctions.getValue(future, aVariable, aValue);
    log("NP_GetValue flash returned %d\n", e);
  }
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

