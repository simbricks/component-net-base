#!/bin/bash
set -eo pipefail

# Install every simulator binary into the conda build prefix. The switch (and
# any future simulators aggregated under sims-install) links against the
# simbricks-lib headers/archives provided in $PREFIX.
make sims-install
