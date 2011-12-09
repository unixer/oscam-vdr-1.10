#ifndef OSCAM_CONFIG_H_
#define OSCAM_CONFIG_H_

//
// ADDONS
//

#ifndef WEBIF
#define WEBIF
#endif

#ifndef WITH_SSL
//#define WITH_SSL
#endif

#ifndef HAVE_DVBAPI
#if !defined(OS_CYGWIN32) && !defined(OS_HPUX) && !defined(OS_FREEBSD) && !defined(OS_MACOSX)
//#define HAVE_DVBAPI
#endif
#endif

#ifdef HAVE_DVBAPI
#ifndef WITH_STAPI
//#define WITH_STAPI
#endif
#endif

#ifndef IRDETO_GUESSING
#define IRDETO_GUESSING
#endif

#ifndef CS_ANTICASC
#define CS_ANTICASC
#endif

#ifndef WITH_DEBUG
#define WITH_DEBUG
#endif

#ifndef CS_LED
//#define CS_LED
#endif

#ifndef CS_WITH_DOUBLECHECK
//#define CS_WITH_DOUBLECHECK
#endif

#ifndef WITH_LB
#define WITH_LB
#endif

#ifndef LCDSUPPORT
//#define LCDSUPPORT
#endif

//
// MODULES
//

#ifndef MODULE_MONITOR
#define MODULE_MONITOR
#endif

#ifndef MODULE_CAMD33
#define MODULE_CAMD33
#endif

#ifndef MODULE_CAMD35
#define MODULE_CAMD35
#endif

#ifndef MODULE_CAMD35_TCP
#define MODULE_CAMD35_TCP
#endif

#ifndef MODULE_NEWCAMD
#define MODULE_NEWCAMD
#endif

#ifndef MODULE_CCCAM
#define MODULE_CCCAM
#endif

#ifndef MODULE_GBOX
#define MODULE_GBOX
#endif

#ifndef MODULE_RADEGAST
#define MODULE_RADEGAST
#endif

#ifndef MODULE_SERIAL
#define MODULE_SERIAL
#endif

#ifndef MODULE_CONSTCW
#define MODULE_CONSTCW
#endif

//
// CARDREADER
//

#ifndef WITH_CARDREADER
#define WITH_CARDREADER
#endif

#ifdef WITH_CARDREADER
#ifndef READER_NAGRA
#define READER_NAGRA
#endif

#ifndef READER_IRDETO
#define READER_IRDETO
#endif

#ifndef READER_CONAX
#define READER_CONAX
#endif

#ifndef READER_CRYPTOWORKS
#define READER_CRYPTOWORKS
#endif

#ifndef READER_SECA
#define READER_SECA
#endif

#ifndef READER_VIACCESS
#define READER_VIACCESS
#endif

#ifndef READER_VIDEOGUARD
#define READER_VIDEOGUARD
#endif

#ifndef READER_DRE
#define READER_DRE
#endif

#ifndef READER_TONGFANG
#define READER_TONGFANG
#endif
#endif

#ifndef QBOXHD_LED
//#define QBOXHD_LED
#endif

#ifndef CS_LOGHISTORY
#define CS_LOGHISTORY
#endif

#ifdef OS_FREEBSD
#  define NO_FTIME
#endif

#ifdef TUXBOX
#  ifdef MIPSEL
#    define CS_LOGFILE "/dev/null"
#  else
#    define CS_LOGFILE "/dev/tty"
#  endif
#  define CS_EMBEDDED
#  define NO_FTIME
#  if !defined(COOL) && !defined(SCI_DEV)
#    define SCI_DEV 1
#  endif
#  ifndef HAVE_DVBAPI
#    define HAVE_DVBAPI
#  endif
#endif

#if defined(WITH_SSL) && !defined(WITH_LIBCRYPTO)
#  define WITH_LIBCRYPTO
#endif

#ifdef UCLIBC
#  define CS_EMBEDDED
#  define NO_FTIME
#endif

#ifdef OS_CYGWIN32
#  define CS_LOGFILE "/dev/tty"
#  define NO_ENDIAN_H
#endif

#ifdef OS_SOLARIS
#  define NO_ENDIAN_H
#  define NEED_DAEMON
#endif

#ifdef OS_OSF
#  define NO_ENDIAN_H
#  define NEED_DAEMON
#endif

#ifdef OS_AIX
#  define NO_ENDIAN_H
#  define NEED_DAEMON
#  define socklen_t unsigned long
#endif

#ifdef OS_IRIX
#  define NO_ENDIAN_H
#  define NEED_DAEMON
#  define socklen_t unsigned long
#endif

#ifdef OS_HPUX
#  define NO_ENDIAN_H
#  define NEED_DAEMON
#endif

#ifdef ARM
#  define CS_EMBEDDED
#  define NO_FTIME
#endif

//#ifdef ALIGNMENT
//#  define STRUCTS_PACKED
//#endif
#endif //OSCAM_CONFIG_H_
