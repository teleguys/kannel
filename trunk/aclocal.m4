dnl aclocal.m4 -- local include for for autoconf
dnl
dnl This file is processed while autoconf generates configure.
dnl This file is part of the Kannel WAP and SMS gateway project.


dnl Check if installed version string is equal or higher then required. 
dnl This is used in a couple of tests to ensude we have a valid version 
dnl of a software package installed. The basic idea is to split the 
dnl version sequences into three parts and then test against eachother
dnl in a whole complex if statement. 
dnl
dnl AC_CHECK_VERSION(installed, required, [do-if-success], [do-if-tail])
dnl
dnl Written by Stipe Tolj <tolj@wapme-systems.de> 
 
AC_DEFUN(AC_CHECK_VERSION, 
[ 
  dnl split installed version string 
  ac_inst_ver_maj=`echo $1 | sed -e 's/^\(.*\)\..*\..*$/\1/'` 
  ac_inst_ver_mid=`echo $1 | sed -e 's/^.*\.\(.*\)\..*$/\1/'` 
  ac_inst_ver_min=`echo $1 | sed -e 's/^.*\..*\.\(.*\)$/\1/'` 
 
  dnl split required version string 
  ac_req_ver_maj=`echo $2 | sed -e 's/^\(.*\)\..*\..*$/\1/'` 
  ac_req_ver_mid=`echo $2 | sed -e 's/^.*\.\(.*\)\..*$/\1/'` 
  ac_req_ver_min=`echo $2 | sed -e 's/^.*\..*\.\(.*\)$/\1/'` 

  dnl now perform the test 
  if test "$ac_inst_ver_maj" -lt "$ac_req_ver_maj" || \
    ( test "$ac_inst_ver_maj" -eq "$ac_req_ver_maj" && \
      test "$ac_inst_ver_mid" -lt "$ac_req_ver_mid" ) || \
    ( test "$ac_inst_ver_mid" -eq "$ac_req_ver_mid" && \
      test "$ac_inst_ver_min" -lt "$ac_req_ver_min" )
  then 
    ac_ver_fail=yes 
  else 
    ac_ver_fail=no 
  fi 
 
  dnl now see if we have to do something 
  ifelse([$3],,, 
  [if test $ac_ver_fail = no; then 
    $3 
   fi]) 
  ifelse([$4],,, 
  [if test $ac_ver_fail = yes; then 
    $4 
   fi]) 
]) 
    

dnl Some optional terminal sequences for configure
dnl Taken from the mod_ssl package by Ralf S. Engelschall.

AC_DEFUN(AC_SET_TERMINAL_SEQUENCES,
[
  case $TERM in
    xterm|xterm*|vt220|vt220*|cygwin)
        T_MD=`echo dummy | awk '{ printf("%c%c%c%c", 27, 91, 49, 109); }'`
        T_ME=`echo dummy | awk '{ printf("%c%c%c", 27, 91, 109); }'`
        ;;
    vt100|vt100*)
        T_MD=`echo dummy | awk '{ printf("%c%c%c%c%c%c", 27, 91, 49, 109, 0, 0); }'`
        T_ME=`echo dummy | awk '{ printf("%c%c%c%c%c", 27, 91, 109, 0, 0); }'`
        ;;
    default)
        T_MD=''
        T_ME=''
        ;;
  esac
])


dnl Display configure section name in bold white letters
dnl if available on the terminal

AC_DEFUN(AC_CONFIG_SECTION,
[
  nl='
'
  echo "${nl}${T_MD}$1 ...${T_ME}"
])


dnl Check which cvs checkout date this tree is any apply
dnl the value to the given variable

AC_DEFUN(AC_CVS_DATE,
[
  cvs_date=`grep ChangeLog CVS/Entries | cut -f4 -d/`
  day=`echo $cvs_date | cut -f3 -d' '`
  month=`echo $cvs_date | cut -f2 -d' '`
  case $month in
    "Jan") month="01" ;;
    "Feb") month="02" ;;
    "Mar") month="03" ;;
    "Apr") month="04" ;;
    "May") month="05" ;;
    "Jun") month="06" ;;
    "Jul") month="07" ;;
    "Aug") month="08" ;;
    "Sep") month="09" ;;
    "Oct") month="10" ;;
    "Nov") month="11" ;;
    "Dec") month="12" ;;
  esac
  year=`echo $cvs_date | cut -f5 -d' '`
  $1="$year$month$day"
])

