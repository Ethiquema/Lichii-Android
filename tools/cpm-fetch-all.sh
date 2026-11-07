#!/bin/sh -e

# SPDX-FileCopyrightText: Copyright 2025 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later

# SPDX-FileCopyrightText: 2025 crueter
# SPDX-License-Identifier: GPL-3.0-or-later

LIBS=$(find . externals src/qt_common src/dynarmic -maxdepth 2 -name cpmfile.json -exec jq -j 'keys_unsorted | join(" ")' {} \; -printf " ")

find tools/ -name '*.sh' -type f -exec chmod +x {} \;

# shellcheck disable=SC2086
tools/cpmutil.sh package fetch -a
