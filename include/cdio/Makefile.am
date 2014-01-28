#   Copyright (C) 2003, 2004, 2006, 2008, 2011 Rocky Bernstein <rocky@gnu.org>
#
#   This program is free software: you can redistribute it and/or modify
#   it under the terms of the GNU General Public License as published by
#   the Free Software Foundation, either version 3 of the License, or
#   (at your option) any later version.
#
#   This program is distributed in the hope that it will be useful,
#   but WITHOUT ANY WARRANTY; without even the implied warranty of
#   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
#   GNU General Public License for more details.
#
#   You should have received a copy of the GNU General Public License
#   along with this program.  If not, see <http://www.gnu.org/licenses/>.

########################################################
# Things to make the install (public) libcdio headers
########################################################
#

if BUILD_CD_PARANOIA
paranoiaheaders = cdda.h cdtext.h 
endif

cdio_config.h: $(top_srcdir)/config.h
	echo  '#ifndef __CDIO_CONFIG_H__' > cdio_config.h
	echo  '#define __CDIO_CONFIG_H__' >> cdio_config.h
	cat  $(top_builddir)/config.h >>cdio_config.h
	echo '#endif /* #ifndef CDIO_CONFIG_H */' >>cdio_config.h

libcdioincludedir=$(includedir)/cdio
dist_libcdioinclude_HEADERS = \
	cdio_unconfig.h \
	paranoia.h \
	udf_file.h \
	cdda.h

#nodist_libcdioinclude_HEADERS = cdio_config.h

EXTRA_DIST = version.h.in
BUILT_SOURCES = version.h

DISTCLEANFILES = cdio_config.h
