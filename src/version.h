#ifndef BATTERYSCOPE_VERSION_H
#define BATTERYSCOPE_VERSION_H
/* Single source of truth for the release version. CMakeLists.txt parses these
   lines and verifies that app.manifest carries the same four-part version.
   Also included by app.rc, so keep it to plain preprocessor definitions. */
#define BS_VERSION_MAJOR 0
#define BS_VERSION_MINOR 24
#define BS_VERSION_PATCH 3
#define BS_VERSION_STRING "0.24.3"
#endif
