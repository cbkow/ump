/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 8 -*- */
/*
 * soup-version.h: Version information
 *
 * Copyright (C) 2012 Igalia S.L.
 */

#pragma once

#include <glib.h>

G_BEGIN_DECLS

#define SOUP_MAJOR_VERSION (3)
#define SOUP_MINOR_VERSION (6)
#define SOUP_MICRO_VERSION (0)

#define SOUP_CHECK_VERSION(major, minor, micro) \
    (SOUP_MAJOR_VERSION > (major) || \
    (SOUP_MAJOR_VERSION == (major) && SOUP_MINOR_VERSION > (minor)) || \
    (SOUP_MAJOR_VERSION == (major) && SOUP_MINOR_VERSION == (minor) && \
     SOUP_MICRO_VERSION >= (micro)))

#ifndef _SOUP_EXTERN
#define _SOUP_EXTERN extern
#endif

/* We prefix variable declarations so they can
 * properly get exported in Windows DLLs.
 */
#ifndef SOUP_VAR
#  ifdef G_PLATFORM_WIN32
#    ifdef LIBSOUP_COMPILATION
#      ifdef DLL_EXPORT
#        define SOUP_VAR extern __declspec(dllexport)
#      else /* !DLL_EXPORT */
#        define SOUP_VAR extern
#      endif /* !DLL_EXPORT */
#    else /* !SOUP_COMPILATION */
#      define SOUP_VAR extern __declspec(dllimport)
#    endif /* !LIBSOUP_COMPILATION */
#  else /* !G_PLATFORM_WIN32 */
#    define SOUP_VAR _SOUP_EXTERN
#  endif /* !G_PLATFORM_WIN32 */
#endif /* SOUP_VAR */

/* Deprecation / Availability macros */
/**
 * SOUP_VERSION_3_0:
 *
 * A macro that evaluates to the 3.0 version of libsoup, in a format
 * that can be used by the C pre-processor.
 *
 * Since: 3.0
 */
#define SOUP_VERSION_3_0 (G_ENCODE_VERSION (3, 0))
/**
 * SOUP_VERSION_3_2:
 *
 * A macro that evaluates to the 3.2 version of libsoup, in a format
 * that can be used by the C pre-processor.
 *
 * Since: 3.2
 */
#define SOUP_VERSION_3_2 (G_ENCODE_VERSION (3, 2))
/**
 * SOUP_VERSION_3_4:
 *
 * A macro that evaluates to the 3.4 version of libsoup, in a format
 * that can be used by the C pre-processor.
 *
 * Since: 3.4
 */
#define SOUP_VERSION_3_4 (G_ENCODE_VERSION (3, 4))
/**
 * SOUP_VERSION_3_6:
 *
 * A macro that evaluates to the 3.6 version of libsoup, in a format
 * that can be used by the C pre-processor.
 *
 * Since: 3.6
 */
#define SOUP_VERSION_3_6 (G_ENCODE_VERSION (3, 6))


/* evaluates to the current stable version; for development cycles,
 * this means the next stable target
 */
#if (SOUP_MINOR_VERSION % 2)
#define SOUP_VERSION_CUR_STABLE (G_ENCODE_VERSION (SOUP_MAJOR_VERSION, SOUP_MINOR_VERSION + 1))
#else
#define SOUP_VERSION_CUR_STABLE (G_ENCODE_VERSION (SOUP_MAJOR_VERSION, SOUP_MINOR_VERSION))
#endif

/* evaluates to the previous stable version */
#if (SOUP_MINOR_VERSION == 0)
/* Can't have -1 for previous version */
# define SOUP_VERSION_PREV_STABLE (G_ENCODE_VERSION (SOUP_MAJOR_VERSION, 0))
#else
# if (SOUP_MINOR_VERSION % 2)
#  define SOUP_VERSION_PREV_STABLE (G_ENCODE_VERSION (SOUP_MAJOR_VERSION, SOUP_MINOR_VERSION - 1))
# else
#  define SOUP_VERSION_PREV_STABLE (G_ENCODE_VERSION (SOUP_MAJOR_VERSION, SOUP_MINOR_VERSION - 2))
# endif
#endif

#ifndef SOUP_VERSION_MIN_REQUIRED
# define SOUP_VERSION_MIN_REQUIRED (SOUP_VERSION_CUR_STABLE)
#elif SOUP_VERSION_MIN_REQUIRED == 0
# undef  SOUP_VERSION_MIN_REQUIRED
# define SOUP_VERSION_MIN_REQUIRED (SOUP_VERSION_CUR_STABLE + 2)
#endif

#if !defined (SOUP_VERSION_MAX_ALLOWED) || (SOUP_VERSION_MAX_ALLOWED == 0)
# undef SOUP_VERSION_MAX_ALLOWED
# define SOUP_VERSION_MAX_ALLOWED (SOUP_VERSION_CUR_STABLE)
#endif

/* sanity checks */
#if SOUP_VERSION_MIN_REQUIRED > SOUP_VERSION_CUR_STABLE
#error "SOUP_VERSION_MIN_REQUIRED must be <= SOUP_VERSION_CUR_STABLE"
#endif
#if SOUP_VERSION_MAX_ALLOWED < SOUP_VERSION_MIN_REQUIRED
#error "SOUP_VERSION_MAX_ALLOWED must be >= SOUP_VERSION_MIN_REQUIRED"
#endif
#if SOUP_VERSION_MIN_REQUIRED < SOUP_VERSION_3_0
#error "SOUP_VERSION_MIN_REQUIRED must be >= SOUP_VERSION_3_0"
#endif

/**
 * SOUP_DISABLE_DEPRECATION_WARNINGS:
 *
 * A macro that should be defined before including the `soup.h` header.
 *
 * If this symbol is defined, no compiler warnings will be produced for
 * uses of deprecated libsoup APIs.
 */

/**
 * SOUP_DEPRECATED:
 *
 * Marks a symbol as deprecated.
 *
 * You should use `SOUP_DEPRECATED_IN_*` in order to handle versioning.
 */

/**
 * SOUP_DEPRECATED_FOR:
 * @f: the symbol that replaces the deprecated one
 *
 * Marks a symbol as deprecated in favor of another symbol.
 *
 * You should use `SOUP_DEPRECATED_FOR_IN_*` in order to handle versioning.
 */

/**
 * SOUP_UNAVAILABLE
 * @maj: the major version that introduced the symbol
 * @min: the minor version that introduced the symbol
 *
 * Marks a symbol unavailable before the given major and minor version.
 *
 * You should use `SOUP_AVAILABLE_IN_*` in order to handle versioning.
 */

#ifdef SOUP_DISABLE_DEPRECATION_WARNINGS
#define SOUP_DEPRECATED _SOUP_EXTERN
#define SOUP_DEPRECATED_FOR(f) _SOUP_EXTERN
#define SOUP_UNAVAILABLE(maj,min) _SOUP_EXTERN
#else
#define SOUP_DEPRECATED G_DEPRECATED _SOUP_EXTERN
#define SOUP_DEPRECATED_FOR(f) G_DEPRECATED_FOR(f) _SOUP_EXTERN
#define SOUP_UNAVAILABLE(maj,min) G_UNAVAILABLE(maj,min) _SOUP_EXTERN
#endif

#define SOUP_AVAILABLE_IN_ALL                   _SOUP_EXTERN

/**
 * SOUP_DEPRECATED_IN_3_0:
 * A macro used to indicate a symbol was deprecated in this version.
 */
/**
 * SOUP_DEPRECATED_IN_3_0_FOR:
 * @f: The recommended replacement function.
 *
 * A macro used to indicate a symbol was deprecated in this version with a replacement.
 */
#if SOUP_VERSION_MIN_REQUIRED >= SOUP_VERSION_3_0
# define SOUP_DEPRECATED_IN_3_0                SOUP_DEPRECATED
# define SOUP_DEPRECATED_IN_3_0_FOR(f)         SOUP_DEPRECATED_FOR(f)
#else
# define SOUP_DEPRECATED_IN_3_0                _SOUP_EXTERN
# define SOUP_DEPRECATED_IN_3_0_FOR(f)         _SOUP_EXTERN
#endif

/**
 * SOUP_AVAILABLE_IN_3_0:
 * A macro used to indicate a symbol is available in this version or later.
 */
#if SOUP_VERSION_MAX_ALLOWED < SOUP_VERSION_3_0
# define SOUP_AVAILABLE_IN_3_0                 SOUP_UNAVAILABLE(3, 0)
#else
# define SOUP_AVAILABLE_IN_3_0                 _SOUP_EXTERN
#endif
/**
 * SOUP_DEPRECATED_IN_3_2:
 * A macro used to indicate a symbol was deprecated in this version.
 */
/**
 * SOUP_DEPRECATED_IN_3_2_FOR:
 * @f: The recommended replacement function.
 *
 * A macro used to indicate a symbol was deprecated in this version with a replacement.
 */
#if SOUP_VERSION_MIN_REQUIRED >= SOUP_VERSION_3_2
# define SOUP_DEPRECATED_IN_3_2                SOUP_DEPRECATED
# define SOUP_DEPRECATED_IN_3_2_FOR(f)         SOUP_DEPRECATED_FOR(f)
#else
# define SOUP_DEPRECATED_IN_3_2                _SOUP_EXTERN
# define SOUP_DEPRECATED_IN_3_2_FOR(f)         _SOUP_EXTERN
#endif

/**
 * SOUP_AVAILABLE_IN_3_2:
 * A macro used to indicate a symbol is available in this version or later.
 */
#if SOUP_VERSION_MAX_ALLOWED < SOUP_VERSION_3_2
# define SOUP_AVAILABLE_IN_3_2                 SOUP_UNAVAILABLE(3, 2)
#else
# define SOUP_AVAILABLE_IN_3_2                 _SOUP_EXTERN
#endif
/**
 * SOUP_DEPRECATED_IN_3_4:
 * A macro used to indicate a symbol was deprecated in this version.
 */
/**
 * SOUP_DEPRECATED_IN_3_4_FOR:
 * @f: The recommended replacement function.
 *
 * A macro used to indicate a symbol was deprecated in this version with a replacement.
 */
#if SOUP_VERSION_MIN_REQUIRED >= SOUP_VERSION_3_4
# define SOUP_DEPRECATED_IN_3_4                SOUP_DEPRECATED
# define SOUP_DEPRECATED_IN_3_4_FOR(f)         SOUP_DEPRECATED_FOR(f)
#else
# define SOUP_DEPRECATED_IN_3_4                _SOUP_EXTERN
# define SOUP_DEPRECATED_IN_3_4_FOR(f)         _SOUP_EXTERN
#endif

/**
 * SOUP_AVAILABLE_IN_3_4:
 * A macro used to indicate a symbol is available in this version or later.
 */
#if SOUP_VERSION_MAX_ALLOWED < SOUP_VERSION_3_4
# define SOUP_AVAILABLE_IN_3_4                 SOUP_UNAVAILABLE(3, 4)
#else
# define SOUP_AVAILABLE_IN_3_4                 _SOUP_EXTERN
#endif
/**
 * SOUP_DEPRECATED_IN_3_6:
 * A macro used to indicate a symbol was deprecated in this version.
 */
/**
 * SOUP_DEPRECATED_IN_3_6_FOR:
 * @f: The recommended replacement function.
 *
 * A macro used to indicate a symbol was deprecated in this version with a replacement.
 */
#if SOUP_VERSION_MIN_REQUIRED >= SOUP_VERSION_3_6
# define SOUP_DEPRECATED_IN_3_6                SOUP_DEPRECATED
# define SOUP_DEPRECATED_IN_3_6_FOR(f)         SOUP_DEPRECATED_FOR(f)
#else
# define SOUP_DEPRECATED_IN_3_6                _SOUP_EXTERN
# define SOUP_DEPRECATED_IN_3_6_FOR(f)         _SOUP_EXTERN
#endif

/**
 * SOUP_AVAILABLE_IN_3_6:
 * A macro used to indicate a symbol is available in this version or later.
 */
#if SOUP_VERSION_MAX_ALLOWED < SOUP_VERSION_3_6
# define SOUP_AVAILABLE_IN_3_6                 SOUP_UNAVAILABLE(3, 6)
#else
# define SOUP_AVAILABLE_IN_3_6                 _SOUP_EXTERN
#endif


SOUP_AVAILABLE_IN_ALL
guint    soup_get_major_version (void);

SOUP_AVAILABLE_IN_ALL
guint    soup_get_minor_version (void);

SOUP_AVAILABLE_IN_ALL
guint    soup_get_micro_version (void);

SOUP_AVAILABLE_IN_ALL
gboolean soup_check_version     (guint major,
				 guint minor,
				 guint micro);

G_END_DECLS
