/* config.h.  Generated automatically by configure.  */
/* config.h.in.  Generated automatically from configure.in by autoheader 2.13.  */

/* Define to empty if the keyword does not work.  */
/* #undef const */

/* Define to one of _getb67, GETB67, getb67 for Cray-2 and Cray-YMP systems.
   This function is required for alloca.c support on those systems.  */
/* #undef CRAY_STACKSEG_END */

/* Define to the type of elements in the array set by `getgroups'.
   Usually this is either `int' or `gid_t'.  */
/* #undef GETGROUPS_T */

/* Define if the `getloadavg' function needs to be run setuid or setgid.  */
/* #undef GETLOADAVG_PRIVILEGED */

/* Define if the `getpgrp' function takes no argument.  */
/* #undef GETPGRP_VOID */

/* Define if you don't have vprintf but do have _doprnt.  */
/* #undef HAVE_DOPRNT */

/* Define if you have the strftime function.  */
#define HAVE_STRFTIME 1

/* Define if you have <sys/wait.h> that is POSIX.1 compatible.  */
#define HAVE_SYS_WAIT_H 1

/* Define if you have the vprintf function.  */
#define HAVE_VPRINTF 1

/* Define as __inline if that's what the C compiler calls it.  */
/* #undef inline */

/* Define to `int' if <sys/types.h> doesn't define.  */
/* #undef pid_t */

/* Define as the return type of signal handlers (int or void).  */
#define RETSIGTYPE int

/* Define to `unsigned' if <sys/types.h> doesn't define.  */
/* #undef size_t */

/* Define if you have the ANSI C header files.  */
#define STDC_HEADERS 1

/* Define if you can safely include both <sys/time.h> and <time.h>.  */
#define TIME_WITH_SYS_TIME 1

/* Define if your <sys/time.h> declares struct tm.  */
/* #undef TM_IN_SYS_TIME */

/* The number of bytes in a unsigned.  */
#define SIZEOF_UNSIGNED 4

/* The number of bytes in a unsigned long.  */
#define SIZEOF_UNSIGNED_LONG 8

/* The number of bytes in a unsigned long long.  */
#define SIZEOF_UNSIGNED_LONG_LONG 8

/* The number of bytes in a unsigned short.  */
#define SIZEOF_UNSIGNED_SHORT 2

/* Define if you have the ASN1_STRING_get0_data function.  */
#define HAVE_ASN1_STRING_GET0_DATA 1

/* Define if you have the ASN1_STRING_to_UTF8 function.  */
#define HAVE_ASN1_STRING_TO_UTF8 1

/* Define if you have the FT_Init_FreeType function.  */
#define HAVE_FT_INIT_FREETYPE 1

/* Define if you have the FT_Library_Version function.  */
#define HAVE_FT_LIBRARY_VERSION 1

/* Define if you have the FcGetVersion function.  */
#define HAVE_FCGETVERSION 1

/* Define if you have the FcInit function.  */
#define HAVE_FCINIT 1

/* Define if you have the Gpm_GetLibVersion function.  */
#define HAVE_GPM_GETLIBVERSION 1

/* Define if you have the MouOpen function.  */
/* #undef HAVE_MOUOPEN */

/* Define if you have the OPENSSL_cleanup function.  */
#define HAVE_OPENSSL_CLEANUP 1

/* Define if you have the OPENSSL_init_ssl function.  */
#define HAVE_OPENSSL_INIT_SSL 1

/* Define if you have the RAND_add function.  */
#define HAVE_RAND_ADD 1

/* Define if you have the RAND_egd function.  */
/* #undef HAVE_RAND_EGD */

/* Define if you have the RAND_file_name function.  */
#define HAVE_RAND_FILE_NAME 1

/* Define if you have the RAND_load_file function.  */
#define HAVE_RAND_LOAD_FILE 1

/* Define if you have the RAND_write_file function.  */
#define HAVE_RAND_WRITE_FILE 1

/* Define if you have the SSL_SESSION_is_resumable function.  */
#define HAVE_SSL_SESSION_IS_RESUMABLE 1

/* Define if you have the SSL_get0_verified_chain function.  */
#define HAVE_SSL_GET0_VERIFIED_CHAIN 1

/* Define if you have the SSL_get1_peer_certificate function.  */
#define HAVE_SSL_GET1_PEER_CERTIFICATE 1

/* Define if you have the SSL_get1_session function.  */
#define HAVE_SSL_GET1_SESSION 1

/* Define if you have the SSL_load_error_strings function.  */
/* #undef HAVE_SSL_LOAD_ERROR_STRINGS */

/* Define if you have the SSL_set_security_level function.  */
#define HAVE_SSL_SET_SECURITY_LEVEL 1

/* Define if you have the WebPDecodeRGBA function.  */
#define HAVE_WEBPDECODERGBA 1

/* Define if you have the WebPFree function.  */
#define HAVE_WEBPFREE 1

/* Define if you have the X509_VERIFY_PARAM_set_flags function.  */
#define HAVE_X509_VERIFY_PARAM_SET_FLAGS 1

/* Define if you have the X509_check_host function.  */
#define HAVE_X509_CHECK_HOST 1

/* Define if you have the X509_check_ip function.  */
#define HAVE_X509_CHECK_IP 1

/* Define if you have the XCloseIM function.  */
/* #undef HAVE_XCLOSEIM */

/* Define if you have the XCreateIC function.  */
/* #undef HAVE_XCREATEIC */

/* Define if you have the XDestroyIC function.  */
/* #undef HAVE_XDESTROYIC */

/* Define if you have the XOpenDisplay function.  */
/* #undef HAVE_XOPENDISPLAY */

/* Define if you have the XOpenIM function.  */
/* #undef HAVE_XOPENIM */

/* Define if you have the XSupportsLocale function.  */
/* #undef HAVE_XSUPPORTSLOCALE */

/* Define if you have the XmbTextListToTextProperty function.  */
/* #undef HAVE_XMBTEXTLISTTOTEXTPROPERTY */

/* Define if you have the Xutf8LookupString function.  */
/* #undef HAVE_XUTF8LOOKUPSTRING */

/* Define if you have the XwcLookupString function.  */
/* #undef HAVE_XWCLOOKUPSTRING */

/* Define if you have the ZSTD_getErrorCode function.  */
#define HAVE_ZSTD_GETERRORCODE 1

/* Define if you have the __strtoll function.  */
/* #undef HAVE___STRTOLL */

/* Define if you have the _heapmin function.  */
/* #undef HAVE__HEAPMIN */

/* Define if you have the _msize function.  */
/* #undef HAVE__MSIZE */

/* Define if you have the _read_kbd function.  */
/* #undef HAVE__READ_KBD */

/* Define if you have the _ucreate function.  */
/* #undef HAVE__UCREATE */

/* Define if you have the _udefault function.  */
/* #undef HAVE__UDEFAULT */

/* Define if you have the _udestroy function.  */
/* #undef HAVE__UDESTROY */

/* Define if you have the _uopen function.  */
/* #undef HAVE__UOPEN */

/* Define if you have the bcmp function.  */
#define HAVE_BCMP 1

/* Define if you have the bcopy function.  */
#define HAVE_BCOPY 1

/* Define if you have the bzero function.  */
#define HAVE_BZERO 1

/* Define if you have the calloc function.  */
#define HAVE_CALLOC 1

/* Define if you have the cfmakeraw function.  */
#define HAVE_CFMAKERAW 1

/* Define if you have the chmod function.  */
#define HAVE_CHMOD 1

/* Define if you have the clock_gettime function.  */
#define HAVE_CLOCK_GETTIME 1

/* Define if you have the cygwin_conv_path function.  */
/* #undef HAVE_CYGWIN_CONV_PATH */

/* Define if you have the cygwin_conv_to_full_win32_path function.  */
/* #undef HAVE_CYGWIN_CONV_TO_FULL_WIN32_PATH */

/* Define if you have the dirfd function.  */
#define HAVE_DIRFD 1

/* Define if you have the event_base_free function.  */
#define HAVE_EVENT_BASE_FREE 1

/* Define if you have the event_base_get_method function.  */
#define HAVE_EVENT_BASE_GET_METHOD 1

/* Define if you have the event_base_new function.  */
#define HAVE_EVENT_BASE_NEW 1

/* Define if you have the event_base_set function.  */
#define HAVE_EVENT_BASE_SET 1

/* Define if you have the event_config_set_flag function.  */
#define HAVE_EVENT_CONFIG_SET_FLAG 1

/* Define if you have the event_get_method function.  */
#define HAVE_EVENT_GET_METHOD 1

/* Define if you have the event_get_struct_event_size function.  */
#define HAVE_EVENT_GET_STRUCT_EVENT_SIZE 1

/* Define if you have the event_get_version function.  */
#define HAVE_EVENT_GET_VERSION 1

/* Define if you have the event_reinit function.  */
#define HAVE_EVENT_REINIT 1

/* Define if you have the fallocate function.  */
#define HAVE_FALLOCATE 1

/* Define if you have the freeaddrinfo function.  */
#define HAVE_FREEADDRINFO 1

/* Define if you have the freelocale function.  */
#define HAVE_FREELOCALE 1

/* Define if you have the gai_strerror function.  */
#define HAVE_GAI_STRERROR 1

/* Define if you have the getaddrinfo function.  */
#define HAVE_GETADDRINFO 1

/* Define if you have the getcwd function.  */
#define HAVE_GETCWD 1

/* Define if you have the getgrgid function.  */
#define HAVE_GETGRGID 1

/* Define if you have the gethostname function.  */
#define HAVE_GETHOSTNAME 1

/* Define if you have the getpagesize function.  */
#define HAVE_GETPAGESIZE 1

/* Define if you have the getpid function.  */
#define HAVE_GETPID 1

/* Define if you have the getpwuid function.  */
#define HAVE_GETPWUID 1

/* Define if you have the getrlimit function.  */
#define HAVE_GETRLIMIT 1

/* Define if you have the gettimeofday function.  */
#define HAVE_GETTIMEOFDAY 1

/* Define if you have the gmtime function.  */
#define HAVE_GMTIME 1

/* Define if you have the herror function.  */
#define HAVE_HERROR 1

/* Define if you have the inet_ntop function.  */
#define HAVE_INET_NTOP 1

/* Define if you have the inet_pton function.  */
#define HAVE_INET_PTON 1

/* Define if you have the kqueue function.  */
/* #undef HAVE_KQUEUE */

/* Define if you have the malloc_trim function.  */
#define HAVE_MALLOC_TRIM 1

/* Define if you have the mallopt function.  */
#define HAVE_MALLOPT 1

/* Define if you have the memchr function.  */
#define HAVE_MEMCHR 1

/* Define if you have the memcmp function.  */
#define HAVE_MEMCMP 1

/* Define if you have the memcpy function.  */
#define HAVE_MEMCPY 1

/* Define if you have the memmem function.  */
#define HAVE_MEMMEM 1

/* Define if you have the memmove function.  */
#define HAVE_MEMMOVE 1

/* Define if you have the memrchr function.  */
#define HAVE_MEMRCHR 1

/* Define if you have the memset function.  */
#define HAVE_MEMSET 1

/* Define if you have the mktime function.  */
#define HAVE_MKTIME 1

/* Define if you have the mouse_getposition_6d function.  */
/* #undef HAVE_MOUSE_GETPOSITION_6D */

/* Define if you have the newlocale function.  */
#define HAVE_NEWLOCALE 1

/* Define if you have the nl_langinfo function.  */
#define HAVE_NL_LANGINFO 1

/* Define if you have the png_create_info_struct function.  */
#define HAVE_PNG_CREATE_INFO_STRUCT 1

/* Define if you have the png_get_bit_depth function.  */
#define HAVE_PNG_GET_BIT_DEPTH 1

/* Define if you have the png_get_color_type function.  */
#define HAVE_PNG_GET_COLOR_TYPE 1

/* Define if you have the png_get_gAMA function.  */
#define HAVE_PNG_GET_GAMA 1

/* Define if you have the png_get_image_height function.  */
#define HAVE_PNG_GET_IMAGE_HEIGHT 1

/* Define if you have the png_get_image_width function.  */
#define HAVE_PNG_GET_IMAGE_WIDTH 1

/* Define if you have the png_get_libpng_ver function.  */
#define HAVE_PNG_GET_LIBPNG_VER 1

/* Define if you have the png_get_sRGB function.  */
#define HAVE_PNG_GET_SRGB 1

/* Define if you have the png_get_valid function.  */
#define HAVE_PNG_GET_VALID 1

/* Define if you have the png_set_rgb_to_gray function.  */
#define HAVE_PNG_SET_RGB_TO_GRAY 1

/* Define if you have the png_set_strip_alpha function.  */
#define HAVE_PNG_SET_STRIP_ALPHA 1

/* Define if you have the poll function.  */
#define HAVE_POLL 1

/* Define if you have the popen function.  */
#define HAVE_POPEN 1

/* Define if you have the posix_fallocate function.  */
#define HAVE_POSIX_FALLOCATE 1

/* Define if you have the pthread_sigmask function.  */
#define HAVE_PTHREAD_SIGMASK 1

/* Define if you have the raise function.  */
#define HAVE_RAISE 1

/* Define if you have the regcomp function.  */
/* #undef HAVE_REGCOMP */

/* Define if you have the resume_thread function.  */
/* #undef HAVE_RESUME_THREAD */

/* Define if you have the rsvg_handle_read_stream_sync function.  */
#define HAVE_RSVG_HANDLE_READ_STREAM_SYNC 1

/* Define if you have the rsvg_handle_render_cairo function.  */
#define HAVE_RSVG_HANDLE_RENDER_CAIRO 1

/* Define if you have the select function.  */
#define HAVE_SELECT 1

/* Define if you have the setlocale function.  */
#define HAVE_SETLOCALE 1

/* Define if you have the setpgid function.  */
#define HAVE_SETPGID 1

/* Define if you have the setrlimit function.  */
#define HAVE_SETRLIMIT 1

/* Define if you have the setsid function.  */
#define HAVE_SETSID 1

/* Define if you have the sigaction function.  */
#define HAVE_SIGACTION 1

/* Define if you have the sigblock function.  */
#define HAVE_SIGBLOCK 1

/* Define if you have the sigdelset function.  */
#define HAVE_SIGDELSET 1

/* Define if you have the sigprocmask function.  */
#define HAVE_SIGPROCMASK 1

/* Define if you have the sigsetmask function.  */
#define HAVE_SIGSETMASK 1

/* Define if you have the snprintf function.  */
#define HAVE_SNPRINTF 1

/* Define if you have the spawn_thread function.  */
/* #undef HAVE_SPAWN_THREAD */

/* Define if you have the strchr function.  */
#define HAVE_STRCHR 1

/* Define if you have the strcmp function.  */
#define HAVE_STRCMP 1

/* Define if you have the strcpy function.  */
#define HAVE_STRCPY 1

/* Define if you have the strcspn function.  */
#define HAVE_STRCSPN 1

/* Define if you have the strdup function.  */
#define HAVE_STRDUP 1

/* Define if you have the strerror function.  */
#define HAVE_STRERROR 1

/* Define if you have the strerror_l function.  */
#define HAVE_STRERROR_L 1

/* Define if you have the strlen function.  */
#define HAVE_STRLEN 1

/* Define if you have the strncmp function.  */
#define HAVE_STRNCMP 1

/* Define if you have the strncpy function.  */
#define HAVE_STRNCPY 1

/* Define if you have the strnlen function.  */
#define HAVE_STRNLEN 1

/* Define if you have the strptime function.  */
#define HAVE_STRPTIME 1

/* Define if you have the strrchr function.  */
#define HAVE_STRRCHR 1

/* Define if you have the strspn function.  */
#define HAVE_STRSPN 1

/* Define if you have the strstr function.  */
#define HAVE_STRSTR 1

/* Define if you have the strtod function.  */
#define HAVE_STRTOD 1

/* Define if you have the strtoimax function.  */
#define HAVE_STRTOIMAX 1

/* Define if you have the strtol function.  */
#define HAVE_STRTOL 1

/* Define if you have the strtoll function.  */
#define HAVE_STRTOLL 1

/* Define if you have the strtoq function.  */
#define HAVE_STRTOQ 1

/* Define if you have the strtoul function.  */
#define HAVE_STRTOUL 1

/* Define if you have the sysconf function.  */
#define HAVE_SYSCONF 1

/* Define if you have the tdelete function.  */
#define HAVE_TDELETE 1

/* Define if you have the tempnam function.  */
#define HAVE_TEMPNAM 1

/* Define if you have the tfind function.  */
#define HAVE_TFIND 1

/* Define if you have the timegm function.  */
#define HAVE_TIMEGM 1

/* Define if you have the tsearch function.  */
#define HAVE_TSEARCH 1

/* Define if you have the uname function.  */
#define HAVE_UNAME 1

/* Define if you have the unixpath2win function.  */
/* #undef HAVE_UNIXPATH2WIN */

/* Define if you have the utime function.  */
#define HAVE_UTIME 1

/* Define if you have the utimes function.  */
#define HAVE_UTIMES 1

/* Define if you have the uwin_path function.  */
/* #undef HAVE_UWIN_PATH */

/* Define if you have the vga_runinbackground_version function.  */
/* #undef HAVE_VGA_RUNINBACKGROUND_VERSION */

/* Define if you have the winpath2unix function.  */
/* #undef HAVE_WINPATH2UNIX */

/* Define if you have the <X11/X.h> header file.  */
/* #undef HAVE_X11_X_H */

/* Define if you have the <X11/Xatom.h> header file.  */
/* #undef HAVE_X11_XATOM_H */

/* Define if you have the <X11/Xlib.h> header file.  */
/* #undef HAVE_X11_XLIB_H */

/* Define if you have the <X11/Xlocale.h> header file.  */
/* #undef HAVE_X11_XLOCALE_H */

/* Define if you have the <X11/Xutil.h> header file.  */
/* #undef HAVE_X11_XUTIL_H */

/* Define if you have the <X11/keysymdef.h> header file.  */
/* #undef HAVE_X11_KEYSYMDEF_H */

/* Define if you have the <alloca.h> header file.  */
#define HAVE_ALLOCA_H 1

/* Define if you have the <app/Application.h> header file.  */
/* #undef HAVE_APP_APPLICATION_H */

/* Define if you have the <arpa/inet.h> header file.  */
#define HAVE_ARPA_INET_H 1

/* Define if you have the <atheos/threads.h> header file.  */
/* #undef HAVE_ATHEOS_THREADS_H */

/* Define if you have the <avif/avif.h> header file.  */
#define HAVE_AVIF_AVIF_H 1

/* Define if you have the <brotli/decode.h> header file.  */
#define HAVE_BROTLI_DECODE_H 1

/* Define if you have the <bsd/string.h> header file.  */
#define HAVE_BSD_STRING_H 1

/* Define if you have the <bzlib.h> header file.  */
#define HAVE_BZLIB_H 1

/* Define if you have the <cairo.h> header file.  */
#define HAVE_CAIRO_H 1

/* Define if you have the <cygwin/process.h> header file.  */
/* #undef HAVE_CYGWIN_PROCESS_H */

/* Define if you have the <cygwin/version.h> header file.  */
/* #undef HAVE_CYGWIN_VERSION_H */

/* Define if you have the <dirent.h> header file.  */
#define HAVE_DIRENT_H 1

/* Define if you have the <ev-event.h> header file.  */
/* #undef HAVE_EV_EVENT_H */

/* Define if you have the <event.h> header file.  */
#define HAVE_EVENT_H 1

/* Define if you have the <fcntl.h> header file.  */
#define HAVE_FCNTL_H 1

/* Define if you have the <fontconfig/fontconfig.h> header file.  */
#define HAVE_FONTCONFIG_FONTCONFIG_H 1

/* Define if you have the <ft2build.h> header file.  */
#define HAVE_FT2BUILD_H 1

/* Define if you have the <gpm.h> header file.  */
#define HAVE_GPM_H 1

/* Define if you have the <grp.h> header file.  */
#define HAVE_GRP_H 1

/* Define if you have the <grx20.h> header file.  */
/* #undef HAVE_GRX20_H */

/* Define if you have the <gui/bitmap.h> header file.  */
/* #undef HAVE_GUI_BITMAP_H */

/* Define if you have the <gui/desktop.h> header file.  */
/* #undef HAVE_GUI_DESKTOP_H */

/* Define if you have the <gui/view.h> header file.  */
/* #undef HAVE_GUI_VIEW_H */

/* Define if you have the <gui/window.h> header file.  */
/* #undef HAVE_GUI_WINDOW_H */

/* Define if you have the <ieee.h> header file.  */
/* #undef HAVE_IEEE_H */

/* Define if you have the <interface/Bitmap.h> header file.  */
/* #undef HAVE_INTERFACE_BITMAP_H */

/* Define if you have the <interface/Screen.h> header file.  */
/* #undef HAVE_INTERFACE_SCREEN_H */

/* Define if you have the <interface/View.h> header file.  */
/* #undef HAVE_INTERFACE_VIEW_H */

/* Define if you have the <interface/Window.h> header file.  */
/* #undef HAVE_INTERFACE_WINDOW_H */

/* Define if you have the <interix/interix.h> header file.  */
/* #undef HAVE_INTERIX_INTERIX_H */

/* Define if you have the <inttypes.h> header file.  */
#define HAVE_INTTYPES_H 1

/* Define if you have the <io.h> header file.  */
/* #undef HAVE_IO_H */

/* Define if you have the <jpeglib.h> header file.  */
#define HAVE_JPEGLIB_H 1

/* Define if you have the <langinfo.h> header file.  */
#define HAVE_LANGINFO_H 1

/* Define if you have the <libpng/png.h> header file.  */
/* #undef HAVE_LIBPNG_PNG_H */

/* Define if you have the <librsvg/librsvg-features.h> header file.  */
/* #undef HAVE_LIBRSVG_LIBRSVG_FEATURES_H */

/* Define if you have the <librsvg/rsvg-cairo.h> header file.  */
/* #undef HAVE_LIBRSVG_RSVG_CAIRO_H */

/* Define if you have the <librsvg/rsvg.h> header file.  */
#define HAVE_LIBRSVG_RSVG_H 1

/* Define if you have the <limits.h> header file.  */
#define HAVE_LIMITS_H 1

/* Define if you have the <linux/falloc.h> header file.  */
#define HAVE_LINUX_FALLOC_H 1

/* Define if you have the <linux/fb.h> header file.  */
#define HAVE_LINUX_FB_H 1

/* Define if you have the <linux/kd.h> header file.  */
#define HAVE_LINUX_KD_H 1

/* Define if you have the <linux/vt.h> header file.  */
#define HAVE_LINUX_VT_H 1

/* Define if you have the <locale.h> header file.  */
#define HAVE_LOCALE_H 1

/* Define if you have the <lzlib.h> header file.  */
/* #undef HAVE_LZLIB_H */

/* Define if you have the <lzma.h> header file.  */
#define HAVE_LZMA_H 1

/* Define if you have the <malloc.h> header file.  */
#define HAVE_MALLOC_H 1

/* Define if you have the <math.h> header file.  */
#define HAVE_MATH_H 1

/* Define if you have the <ndir.h> header file.  */
/* #undef HAVE_NDIR_H */

/* Define if you have the <net/socket.h> header file.  */
/* #undef HAVE_NET_SOCKET_H */

/* Define if you have the <netinet/in_system.h> header file.  */
/* #undef HAVE_NETINET_IN_SYSTEM_H */

/* Define if you have the <netinet/in_systm.h> header file.  */
#define HAVE_NETINET_IN_SYSTM_H 1

/* Define if you have the <netinet/ip.h> header file.  */
#define HAVE_NETINET_IP_H 1

/* Define if you have the <openssl/x509v3.h> header file.  */
#define HAVE_OPENSSL_X509V3_H 1

/* Define if you have the <pcre.h> header file.  */
/* #undef HAVE_PCRE_H */

/* Define if you have the <png.h> header file.  */
#define HAVE_PNG_H 1

/* Define if you have the <poll.h> header file.  */
#define HAVE_POLL_H 1

/* Define if you have the <process.h> header file.  */
/* #undef HAVE_PROCESS_H */

/* Define if you have the <pwd.h> header file.  */
#define HAVE_PWD_H 1

/* Define if you have the <regex.h> header file.  */
/* #undef HAVE_REGEX_H */

/* Define if you have the <search.h> header file.  */
#define HAVE_SEARCH_H 1

/* Define if you have the <setjmp.h> header file.  */
#define HAVE_SETJMP_H 1

/* Define if you have the <sgtty.h> header file.  */
#define HAVE_SGTTY_H 1

/* Define if you have the <stdarg.h> header file.  */
#define HAVE_STDARG_H 1

/* Define if you have the <string.h> header file.  */
#define HAVE_STRING_H 1

/* Define if you have the <strings.h> header file.  */
#define HAVE_STRINGS_H 1

/* Define if you have the <support/Locker.h> header file.  */
/* #undef HAVE_SUPPORT_LOCKER_H */

/* Define if you have the <sys/cygwin.h> header file.  */
/* #undef HAVE_SYS_CYGWIN_H */

/* Define if you have the <sys/dir.h> header file.  */
/* #undef HAVE_SYS_DIR_H */

/* Define if you have the <sys/file.h> header file.  */
#define HAVE_SYS_FILE_H 1

/* Define if you have the <sys/fmutex.h> header file.  */
/* #undef HAVE_SYS_FMUTEX_H */

/* Define if you have the <sys/ioctl.h> header file.  */
#define HAVE_SYS_IOCTL_H 1

/* Define if you have the <sys/mman.h> header file.  */
#define HAVE_SYS_MMAN_H 1

/* Define if you have the <sys/ndir.h> header file.  */
/* #undef HAVE_SYS_NDIR_H */

/* Define if you have the <sys/resource.h> header file.  */
#define HAVE_SYS_RESOURCE_H 1

/* Define if you have the <sys/select.h> header file.  */
#define HAVE_SYS_SELECT_H 1

/* Define if you have the <sys/time.h> header file.  */
#define HAVE_SYS_TIME_H 1

/* Define if you have the <sys/un.h> header file.  */
#define HAVE_SYS_UN_H 1

/* Define if you have the <sys/utsname.h> header file.  */
#define HAVE_SYS_UTSNAME_H 1

/* Define if you have the <termios.h> header file.  */
#define HAVE_TERMIOS_H 1

/* Define if you have the <tiffio.h> header file.  */
#define HAVE_TIFFIO_H 1

/* Define if you have the <time.h> header file.  */
#define HAVE_TIME_H 1

/* Define if you have the <umalloc.h> header file.  */
/* #undef HAVE_UMALLOC_H */

/* Define if you have the <unistd.h> header file.  */
#define HAVE_UNISTD_H 1

/* Define if you have the <unixlib.h> header file.  */
/* #undef HAVE_UNIXLIB_H */

/* Define if you have the <util/application.h> header file.  */
/* #undef HAVE_UTIL_APPLICATION_H */

/* Define if you have the <util/locker.h> header file.  */
/* #undef HAVE_UTIL_LOCKER_H */

/* Define if you have the <utime.h> header file.  */
#define HAVE_UTIME_H 1

/* Define if you have the <uwin.h> header file.  */
/* #undef HAVE_UWIN_H */

/* Define if you have the <values.h> header file.  */
#define HAVE_VALUES_H 1

/* Define if you have the <webp/decode.h> header file.  */
#define HAVE_WEBP_DECODE_H 1

/* Define if you have the <windowsx.h> header file.  */
/* #undef HAVE_WINDOWSX_H */

/* Define if you have the <zlib.h> header file.  */
#define HAVE_ZLIB_H 1

/* Define if you have the <zstd.h> header file.  */
#define HAVE_ZSTD_H 1

/* Define if you have the Xau library (-lXau).  */
/* #undef HAVE_LIBXAU */

/* Define if you have the Xdmcp library (-lXdmcp).  */
/* #undef HAVE_LIBXDMCP */

/* Define if you have the atheos library (-latheos).  */
/* #undef HAVE_LIBATHEOS */

/* Define if you have the avif library (-lavif).  */
#define HAVE_LIBAVIF 1

/* Define if you have the be library (-lbe).  */
/* #undef HAVE_LIBBE */

/* Define if you have the brotlidec library (-lbrotlidec).  */
#define HAVE_LIBBROTLIDEC 1

/* Define if you have the bsd library (-lbsd).  */
#define HAVE_LIBBSD 1

/* Define if you have the bz2 library (-lbz2).  */
#define HAVE_LIBBZ2 1

/* Define if you have the dl library (-ldl).  */
/* #undef HAVE_LIBDL */

/* Define if you have the ev library (-lev).  */
/* #undef HAVE_LIBEV */

/* Define if you have the event library (-levent).  */
#define HAVE_LIBEVENT 1

/* Define if you have the fontconfig library (-lfontconfig).  */
/* #undef HAVE_LIBFONTCONFIG */

/* Define if you have the freetype library (-lfreetype).  */
/* #undef HAVE_LIBFREETYPE */

/* Define if you have the gpm library (-lgpm).  */
#define HAVE_LIBGPM 1

/* Define if you have the grx20 library (-lgrx20).  */
/* #undef HAVE_LIBGRX20 */

/* Define if you have the jbig library (-ljbig).  */
/* #undef HAVE_LIBJBIG */

/* Define if you have the jpeg library (-ljpeg).  */
#define HAVE_LIBJPEG 1

/* Define if you have the lz library (-llz).  */
/* #undef HAVE_LIBLZ */

/* Define if you have the lzma library (-llzma).  */
#define HAVE_LIBLZMA 1

/* Define if you have the m library (-lm).  */
#define HAVE_LIBM 1

/* Define if you have the network library (-lnetwork).  */
/* #undef HAVE_LIBNETWORK */

/* Define if you have the nsl library (-lnsl).  */
/* #undef HAVE_LIBNSL */

/* Define if you have the pcre library (-lpcre).  */
/* #undef HAVE_LIBPCRE */

/* Define if you have the png library (-lpng).  */
/* #undef HAVE_LIBPNG */

/* Define if you have the pthread library (-lpthread).  */
#define HAVE_LIBPTHREAD 1

/* Define if you have the rsvg-2 library (-lrsvg-2).  */
/* #undef HAVE_LIBRSVG_2 */

/* Define if you have the rt library (-lrt).  */
/* #undef HAVE_LIBRT */

/* Define if you have the socket library (-lsocket).  */
/* #undef HAVE_LIBSOCKET */

/* Define if you have the stdc++ library (-lstdc++).  */
/* #undef HAVE_LIBSTDC__ */

/* Define if you have the syllable library (-lsyllable).  */
/* #undef HAVE_LIBSYLLABLE */

/* Define if you have the tiff library (-ltiff).  */
#define HAVE_LIBTIFF 1

/* Define if you have the watt library (-lwatt).  */
/* #undef HAVE_LIBWATT */

/* Define if you have the webp library (-lwebp).  */
/* #undef HAVE_LIBWEBP */

/* Define if you have the x86 library (-lx86).  */
/* #undef HAVE_LIBX86 */

/* Define if you have the xcb library (-lxcb).  */
/* #undef HAVE_LIBXCB */

/* Define if you have the xcb-xlib library (-lxcb-xlib).  */
/* #undef HAVE_LIBXCB_XLIB */

/* Define if you have the xnet library (-lxnet).  */
/* #undef HAVE_LIBXNET */

/* Define if you have the z library (-lz).  */
#define HAVE_LIBZ 1

/* Define if you have the zstd library (-lzstd).  */
#define HAVE_LIBZSTD 1

/* Name of package */
#define PACKAGE "links"

/* Version number of package */
#define VERSION "2.30"


/* */
#define VERSION "2.30"

/* */
#define HAVE_OPENMP 1

/* */
#define HAVE_LONG_LONG 1

/* */
/* #undef HAVE_POINTER_COMPARISON_BUG */

/* */
/* #undef HAVE_MAXINT_CONVERSION_BUG */

/* */
#define HAVE_STDLIB_H_X 1

/* */
#define HAVE_SOCKLEN_T 1

/* */
#define HAVE_VOLATILE 1

/* */
#define HAVE_RESTRICT 1

/* */
#define HAVE___RESTRICT 1

/* */
#define HAVE_ERRNO 1

/* */
/* #undef C_BIG_ENDIAN */

/* */
#define C_LITTLE_ENDIAN 1

/* */
#define RENAME_OVER_EXISTING_FILES 1

/* */
#define HAVE_STRLEN 1

/* */
#define HAVE_STRNLEN 1

/* */
#define HAVE_STRCPY 1

/* */
#define HAVE_STRNCPY 1

/* */
#define HAVE_STRCHR 1

/* */
#define HAVE_STRRCHR 1

/* */
#define HAVE_STRCMP 1

/* */
#define HAVE_STRNCMP 1

/* */
#define HAVE_STRCSPN 1

/* */
#define HAVE_STRSPN 1

/* */
#define HAVE_STRSTR 1

/* */
#define HAVE_MEMCMP 1

/* */
#define HAVE_MEMCHR 1

/* */
#define HAVE_MEMRCHR 1

/* */
#define HAVE_MEMCPY 1

/* */
#define HAVE_MEMMOVE 1

/* */
#define HAVE_MEMSET 1

/* */
#define HAVE_MEMMEM 1

/* */
#define HAVE_STRERROR 1

/* */
#define HAVE_SIGFILLSET 1

/* */
#define HAVE_SIGSETJMP 1

/* */
#define HAVE_GCC_ASSEMBLER 1

/* */
#define HAVE___BUILTIN_ADD_OVERFLOW 1

/* */
#define HAVE___BUILTIN_CLZ 1

/* */
#define DEBUGLEVEL 0

/* */
#define HAVE_CLOCK_GETTIME 1

/* */
#define HAVE_GETHOSTBYNAME 1

/* */
/* #undef HAVE_GETHOSTBYNAME_BUG */

/* */
#define SUPPORT_IPV6 1

/* */
#define SUPPORT_IPV6_SCOPE 1

/* */
#define HAVE_POW 1

/* */
#define HAVE_POWF 1

/* */
/* #undef JS */

/* */
/* #undef CHCEME_FLEXI_LIBU */

/* */
/* #undef HAVE_PCRE */

/* */
/* #undef HAVE_REGEX */

/* */
#define ENABLE_UTF8 1

/* */
/* #undef HAVE_BEGINTHREAD */

/* */
/* #undef HAVE_PTHREADS */

/* */
/* #undef X2 */

/* */
/* #undef HAVE_XSETLOCALE */

/* */
#define HAVE_SSL 1

/* */
#define HAVE_OPENSSL 1

/* */
/* #undef HAVE_NSS */

/* */
/* #undef HAVE_CRYPTO_SET_MEM_FUNCTIONS_1 */

/* */
#define HAVE_CRYPTO_SET_MEM_FUNCTIONS_2 1

/* */
#define HAVE_ZLIB 1

/* */
#define HAVE_BROTLI 1

/* */
#define HAVE_ZSTD 1

/* */
#define HAVE_BZIP2 1

/* */
#define HAVE_LZMA 1

/* */
/* #undef HAVE_LZIP */

/* */
#define G 1

/* */
/* #undef GRDRV_SVGALIB */

/* */
#define GRDRV_FB 1

/* */
/* #undef GRDRV_DIRECTFB */

/* */
/* #undef GRDRV_X */

/* */
/* #undef GRDRV_SDL */

/* */
/* #undef GRDRV_PMSHELL */

/* */
/* #undef GRDRV_ATHEOS */

/* */
/* #undef GRDRV_HAIKU */

/* */
/* #undef GRDRV_GRX */

/* Have freetype */
#define HAVE_FREETYPE 1

/* Jpeg by Clock */
#define HAVE_JPEG 1

/* Tiff by Brain */
#define HAVE_TIFF 1

/* SVG */
#define HAVE_SVG 1

/* WebP */
#define HAVE_WEBP 1

/* AVIF */
#define HAVE_AVIF 1

/* Gpm_Event has wdx and wdy */
#define HAVE_WDX_WDY 1
