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

# Run the test suite for a coverage build, and succeed whichever way it goes.
#
# A separate script rather than a COMMAND in the `coverage` target, because
# add_custom_target does not run its commands through a shell: `ctest || true`
# would hand ctest two arguments it does not understand rather than swallowing
# its exit status.
#
# Swallowing it is the point. A failing test still executed the code it
# executed, and the .gcda files it wrote are exactly as valid as a green run's —
# so stopping the target there would spend the expensive half of the job and
# throw away the answer at the moment it is most worth having. The failure is
# still on screen (--output-on-failure) and still in ctest's own log; what it
# does not do is take the report with it.
#
# LOFTAIL_CTEST and LOFTAIL_BUILD_DIR are passed in with -D by the caller.

execute_process(
    COMMAND "${LOFTAIL_CTEST}" --test-dir "${LOFTAIL_BUILD_DIR}" --output-on-failure
    RESULT_VARIABLE loftail_ctest_result)

if(NOT loftail_ctest_result EQUAL 0)
    message(STATUS
        "ctest exited ${loftail_ctest_result}; reporting coverage for the run anyway")
endif()
