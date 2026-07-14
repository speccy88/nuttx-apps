/****************************************************************************
 * apps/interpreters/berry/include/berry_coc_p2.h
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Berry's constant-object generator scans plain #define directives; it does
 * not run the C preprocessor.  Keep this host-side table in sync with the P2
 * branch in berry_conf.h so it emits the native-module headers used by the
 * target build without mistaking the non-P2 branch for the active one.
 ****************************************************************************/

#ifndef __APPS_INTERPRETERS_BERRY_INCLUDE_BERRY_COC_P2_H
#define __APPS_INTERPRETERS_BERRY_INCLUDE_BERRY_COC_P2_H

#define BE_USE_STRING_MODULE 0
#define BE_USE_JSON_MODULE 0
#define BE_USE_MATH_MODULE 0
#define BE_USE_TIME_MODULE 0
#define BE_USE_OS_MODULE 0
#define BE_USE_GLOBAL_MODULE 0
#define BE_USE_SYS_MODULE 0
#define BE_USE_DEBUG_MODULE 0
#define BE_USE_GC_MODULE 0
#define BE_USE_SOLIDIFY_MODULE 0
#define BE_USE_INTROSPECT_MODULE 0
#define BE_USE_STRICT_MODULE 0
#define BE_USE_FILE_CLASS 0
#define BE_USE_BYTES_CLASS 0

#endif /* __APPS_INTERPRETERS_BERRY_INCLUDE_BERRY_COC_P2_H */
