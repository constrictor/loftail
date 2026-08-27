# loftail — a desktop viewer for log4cplus logs.
# Copyright (C) 2026 Valentyn Pavliuchenko
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program.  If not, see <https://www.gnu.org/licenses/>.
#
# SPDX-License-Identifier: GPL-3.0-or-later

# Debian/Ubuntu package (.deb) via CPack.
#
# This is the one Linux artifact that does NOT bundle Qt — the exact opposite of
# the AppImage next to it, and deliberately so. An AppImage carries its own Qt so
# it runs anywhere; a distro package links the distro's Qt so it stays small and
# gets security updates from the archive. That difference is the whole reason both
# exist, and it is why a .deb is built per Ubuntu release rather than once:
# 24.04 has Qt 6.4.2 and 26.04 a much newer one, with different sonames, so a
# single package cannot satisfy both. The release is stamped into the version
# (0.1.0~ubuntu24.04) so the two never collide in a release page or a repo.
#
# Nothing here duplicates the install layout: the FHS rules in src/CMakeLists.txt
# that stage the AppDir are the same ones CPack packs.

if(APPLE OR WIN32 OR NOT UNIX)
    return()
endif()

set(CPACK_GENERATOR "DEB")
set(CPACK_PACKAGE_NAME "loftail")
set(CPACK_PACKAGE_VENDOR "loftail")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "${PROJECT_DESCRIPTION}")
set(CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/constrictor/loftail")
set(CPACK_STRIP_FILES ON)

# Which Ubuntu/Debian release this package is FOR, read from the machine building
# it. Overridable, because a container-built package is named for the container,
# not the host, and a build outside a container has no other source of truth.
if(NOT LOFTAIL_DEB_DISTRO)
    set(LOFTAIL_DEB_DISTRO "")
    if(EXISTS /etc/os-release)
        file(READ /etc/os-release _osrel)
        if(_osrel MATCHES "\nID=\"?([a-z]+)\"?")
            set(_id "${CMAKE_MATCH_1}")
            if(_osrel MATCHES "\nVERSION_ID=\"?([0-9.]+)\"?")
                set(LOFTAIL_DEB_DISTRO "${_id}${CMAKE_MATCH_1}")
            endif()
        endif()
        unset(_osrel)
    endif()
endif()

# `~` sorts BEFORE the empty string in dpkg's version ordering, so
# 0.1.0~ubuntu24.04 < 0.1.0~ubuntu26.04 < 0.1.0. A `+` or `-` here would make the
# per-release packages sort above a plain upstream 0.1.0 and shadow it on upgrade.
if(LOFTAIL_DEB_DISTRO)
    set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}~${LOFTAIL_DEB_DISTRO}")
else()
    set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
endif()

# CPackDeb refuses to run without a maintainer; it falls back to CPACK_PACKAGE_CONTACT.
if(NOT CPACK_PACKAGE_CONTACT)
    set(CPACK_PACKAGE_CONTACT "Valentyn Pavliuchenko <valentyn.pavliuchenko@gmail.com>")
endif()
set(CPACK_DEBIAN_PACKAGE_MAINTAINER "${CPACK_PACKAGE_CONTACT}")
set(CPACK_DEBIAN_PACKAGE_SECTION "utils")
set(CPACK_DEBIAN_PACKAGE_HOMEPAGE "${CPACK_PACKAGE_HOMEPAGE_URL}")
# loftail_0.1.0~ubuntu24.04_amd64.deb — the canonical Debian file name, which
# encodes the architecture dpkg-shlibdeps just resolved against.
set(CPACK_DEBIAN_FILE_NAME "DEB-DEFAULT")

# Derive Depends: from the binary's actual DT_NEEDED entries rather than listing
# Qt/libssh2/libarchive by hand. Hand-written dependencies are wrong twice over
# here: the Qt package names differ between 24.04 and 26.04, and libssh2 and
# libarchive are OPTIONAL — a build configured without them must not produce a
# package that demands them. shlibdeps reads what was actually linked, so the
# dependency list follows the configuration for free.
#
# The corollary CI checks: shlibdeps reports a THIN dependency list just as
# happily when the optional libraries were auto-detected off, so the workflow
# asserts the resulting Depends names them (same failure shape the
# "SSH remote sources: ENABLED" grep guards against at configure time).
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)

# The one dependency shlibdeps CANNOT find, because it is not linked. QtKeychain
# reaches libsecret through QLibrary("secret-1") — a dlopen at runtime — so there is
# no DT_NEEDED entry to derive, and on a machine without it a remembered password
# quietly falls back to the plain-text file instead of the keychain.
#
# Recommends rather than Depends because that is the honest strength: loftail works
# without it and says so in the dialog, and apt installs Recommends by default, so
# the ordinary install gets the keychain and a deliberate --no-install-recommends
# still gets a working program. Hardcoded rather than derived for the reason above;
# it is also correct in a build with no keychain support, where it costs a package
# nobody uses rather than a missing one somebody needs.
set(CPACK_DEBIAN_PACKAGE_RECOMMENDS "libsecret-1-0")

# The extended description ONLY — the synopsis is CPACK_PACKAGE_DESCRIPTION_SUMMARY
# above, which CPackDeb puts on the Description: line before this body. Repeating
# the summary here would print it twice. Written without the Debian leading space;
# CPackDeb adds it per line, and adding our own gets both.
set(CPACK_DEBIAN_PACKAGE_DESCRIPTION
"loftail opens finished and still-being-written logs with no mode switch: every
file is watched, so a live log tails like tail -f and a finished one is simply a
log that never grows. It filters and highlights by subsystem, thread, priority,
time range and message text, splits a log into runs, and restores its session.
Logs on other machines open over SSH, and compressed and archived logs
(.gz .bz2 .xz .zst, zip and tar) open directly.")

# The DEB generator ignores this — a Debian package states its terms in
# /usr/share/doc/<pkg>/copyright, which the install() rules in src/CMakeLists.txt put
# there. It is set anyway because every other generator (NSIS, DragNDrop, WIX) shows it
# as the licence the user agrees to, and the day one of those is added is not the day to
# remember this line exists.
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_SOURCE_DIR}/LICENSE")

include(CPack)
