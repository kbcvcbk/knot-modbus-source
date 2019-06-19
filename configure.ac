AC_PREREQ(2.60)
AC_INIT(knot-modbus, 0.01)

AM_INIT_AUTOMAKE([silent-rules subdir-objects])

AC_CONFIG_HEADERS(config.h)

m4_ifdef([AM_SILENT_RULES], [AM_SILENT_RULES([yes])])

AM_MAINTAINER_MODE

AC_PREFIX_DEFAULT(/usr/local)

PKG_PROG_PKG_CONFIG

COMPILER_WARNING_CFLAGS
COMPILER_BUILD_CFLAGS

AC_LANG_C

AC_PROG_CC
AC_PROG_CC_PIE

m4_define([_LT_AC_TAGCONFIG], [])
m4_ifdef([AC_LIBTOOL_TAGS], [AC_LIBTOOL_TAGS([])])

AC_DISABLE_STATIC
AC_PROG_LIBTOOL

PKG_CHECK_MODULES(TINYCBOR, tinycbor >= 0.5.3, dummy=yes,
		AC_MSG_ERROR("required tinycbor >= 0.5.3"))
AC_SUBST(TINYCBOR_CFLAGS)
AC_SUBST(TINYCBOR_LIBS)

PKG_CHECK_MODULES(ELL, ell >= 0.19, dummy=yes,
		AC_MSG_ERROR("required libell >= 0.19"))
AC_SUBST(ELL_CFLAGS)
AC_SUBST(ELL_LIBS)

PKG_CHECK_MODULES(MODBUS, libmodbus >= 3.1.4, dummy=yes,
  AC_MSG_ERROR("required libmodbus >= 3.1.4"))
AC_SUBST(MODBUS_CFLAGS)
AC_SUBST(MODBUS_LIBS)

if (test "${prefix}" = "NONE"); then
        if (test "$localstatedir" = '${prefix}/var'); then
                AC_SUBST([localstatedir], ['/var'])
        fi
        prefix="${ac_default_prefix}"
fi

if (test "$localstatedir" = '${prefix}/var'); then
        storagedir="${prefix}/var/lib/modbus"
else
        storagedir="${localstatedir}/lib/modbus"
fi

AC_DEFINE_UNQUOTED(STORAGEDIR, "${storagedir}",
                        [Directory for storing settings])

if (test "${sysconfdir}" = '${prefix}/etc'); then
	configdir="${prefix}/etc/modbus"
else
	configdir="${sysconfdir}/modbus"
fi

AC_DEFINE_UNQUOTED(CONFIGDIR, "${configdir}",
			[Directory for the config files])
AC_SUBST(CONFIGDIR, "${configdir}")

AC_ARG_ENABLE(configfiles, AC_HELP_STRING([--enable-configfiles],
			[enable to install config files [default=yes]]),
					[enable_configfiles=${enableval}],
					[enable_configfiles=yes])
AM_CONDITIONAL(CONFIGFILES, test "${enable_configfiles}" = "yes")

AC_OUTPUT(Makefile)
