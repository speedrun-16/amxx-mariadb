#ifndef __MODULECONFIG_H__
#define __MODULECONFIG_H__

#define MODULE_NAME    "AMXX MariaDB"
#define MODULE_VERSION "1.1.0"
#define MODULE_AUTHOR  "PWNED"
#define MODULE_URL     "https://github.com/speedrun-org/amxx-mariadb"
#define MODULE_LOGTAG  "MariaDB"
#define MODULE_LIBRARY "mariadb"
#define MODULE_LIBCLASS ""
#define MODULE_RELOAD_ON_MAPCHANGE

#ifdef __DATE__
#define MODULE_DATE __DATE__
#else
#define MODULE_DATE "Unknown"
#endif

#define USE_METAMOD

#define FN_AMXX_QUERY          on_amxx_query
#define FN_AMXX_ATTACH         on_amxx_attach
#define FN_AMXX_DETACH         on_amxx_detach
#define FN_AMXX_PLUGINSLOADED  on_plugins_loaded
#define FN_AMXX_PLUGINSUNLOADING on_plugins_unloading
#define FN_AMXX_PLUGINSUNLOADED  on_plugins_unloaded

#ifdef USE_METAMOD
#define FN_META_QUERY     on_meta_query
#define FN_META_ATTACH    on_meta_attach
#define FN_META_DETACH    on_meta_detach
#define FN_StartFrame     start_frame
#define FN_ServerDeactivate server_deactivate
#endif

#define MARIADB_ASYNC_WORKER_THREADS 4

// use simplified WINAPI calling convention for GiveFnptrsToDll instead of
// the original naked+__asm prolog/epilog approach, which is problematic on
// this Windows toolchain; remove when building for Linux
#ifdef _WIN32
#define AMXX_GIVEFNPTRS_WINAPI
#endif

#endif
