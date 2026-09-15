#pragma once
//
// Single source of truth for the product version.
//
// Consumed by res\app.rc and installer\installer.rc (VERSIONINFO blocks) and
// by the installer code (Add/Remove Programs DisplayVersion). The .manifest
// files keep a static assemblyIdentity version on purpose: the linker embeds
// manifests verbatim (no RC preprocessing), and Windows ignores that field
// functionally.
//
#define PSCAM_VERSION_MAJOR 4
#define PSCAM_VERSION_MINOR 0
#define PSCAM_VERSION_PATCH 0
#define PSCAM_VERSION_REV   0

#define PSCAM_STR_(x) #x
#define PSCAM_STR(x)  PSCAM_STR_(x)

// 4,0,0,0 -- FILEVERSION / PRODUCTVERSION
#define PSCAM_VERSION_COMMA \
    PSCAM_VERSION_MAJOR,PSCAM_VERSION_MINOR,PSCAM_VERSION_PATCH,PSCAM_VERSION_REV
// "4.0.0.0" -- FileVersion / ProductVersion string values
#define PSCAM_VERSION_STRING \
    PSCAM_STR(PSCAM_VERSION_MAJOR) "." PSCAM_STR(PSCAM_VERSION_MINOR) "." \
    PSCAM_STR(PSCAM_VERSION_PATCH) "." PSCAM_STR(PSCAM_VERSION_REV)
// "4.0.0" -- user-facing (Add/Remove Programs, wizard header)
#define PSCAM_VERSION_DISPLAY \
    PSCAM_STR(PSCAM_VERSION_MAJOR) "." PSCAM_STR(PSCAM_VERSION_MINOR) "." \
    PSCAM_STR(PSCAM_VERSION_PATCH)
