# MIT License

# Copyright (c) 2026 SimBricks

# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software, and to permit persons to whom the Software is
# furnished to do so, subject to the following conditions:

# The above copyright notice and this permission notice shall be included in all
# copies or substantial portions of the Software.

# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
# SOFTWARE.

## --- Build configuration ---------------------------------------------------

# Install prefix for locally built artifacts, and where the simbricks-lib conda
# package provides the headers and static archives we build against. Inside a
# conda build this is the build prefix; for a local dev build override it, e.g.
# PREFIX=$(CURDIR)/out.
PREFIX            ?= $(CURDIR)/out
SIMBRICKS_INC_DIR ?= $(PREFIX)/include
SIMBRICKS_LIB_DIR ?= $(PREFIX)/lib/simbricks

# Compilers and python interpreter (overridable by conda / the environment).
CC     ?= cc
CXX    ?= c++
PYTHON ?= python

# Forward the toolchain + paths to every simulator sub-make via the environment,
# so the per-sim invocations below don't have to repeat them. (Conda's CFLAGS /
# CXXFLAGS / LDFLAGS already arrive from the environment and pass through too.)
export PREFIX SIMBRICKS_INC_DIR SIMBRICKS_LIB_DIR CC CXX

# Optional: redirect conda-build output, e.g. OUTPUT_FOLDER=./conda-out.
OUTPUT_FOLDER     ?=
OUTPUT_FLAG       := $(if $(OUTPUT_FOLDER),--output-folder $(OUTPUT_FOLDER))
# Conda channels searched by `conda build`. The SimBricks channel hosts external
# deps not built here (e.g. simbricks-lib, simbricks-orchestration); conda-forge
# provides the rest. Override to point at a different channel if needed.
SIMB_CONDA_CHANNEL:= -c https://conda.simbricks.io/latest
BASE_BUILD_CMD    := conda build $(SIMB_CONDA_CHANNEL) -m conda-recipes/conda_build_config.yaml $(OUTPUT_FLAG)

.PHONY: all sims-build sims-install python-develop python-conda sim-bin-conda \
        conda-packages pypi-build pypi-publish clean

## --- Simulators (local dev build, no conda) --------------------------------

# Every simulator in the repo. Each lives in a directory of the same name whose
# standalone Makefile (it `include`s ../sim.mk) exposes `all` (build), `install`,
# and `clean`. Adding a new simulator is a one-word edit here plus its Makefile.
SIMS := switch wire tap

# $(call sim_rules,<dir>) — generate build/install/clean targets for one sim.
# The toolchain + paths reach each sim's Makefile via the `export` above, so we
# only name the target to run in its directory.
define sim_rules
.PHONY: $(1)-build $(1)-install $(1)-clean

$(1)-build:
	$$(MAKE) -C $(1) all

$(1)-install: $(1)-build
	$$(MAKE) -C $(1) install

$(1)-clean:
	$$(MAKE) -C $(1) clean
endef

$(foreach s,$(SIMS),$(eval $(call sim_rules,$(s))))

# Aggregate over every simulator in the repo.
sims-build:   $(addsuffix -build,$(SIMS))
sims-install: $(addsuffix -install,$(SIMS))

## --- Python integration package (simbricks-net-base-python/) ---------------

# Editable install for local development (not used by the conda build).
python-develop:
	$(PYTHON) -m pip install -e ./simbricks-net-base-python

## --- Conda packages --------------------------------------------------------

# Build the noarch python conda package.
python-conda:
	$(BASE_BUILD_CMD) conda-recipes/simbricks-net-base-sim-py

# Build the compiled simulator conda package.
sim-bin-conda: python-conda
	$(BASE_BUILD_CMD) conda-recipes/simbricks-net-base-sim-bin

# Build both conda packages in dependency order.
conda-packages: python-conda sim-bin-conda

## --- PyPI packages ---------------------------------------------------------

pypi-build:
	poetry build -C ./simbricks-net-base-python

pypi-publish: pypi-build
	poetry publish -C ./simbricks-net-base-python

## --- Default target --------------------------------------------------------

# Default: local dev build of both halves.
all: conda-packages

## --- Housekeeping ----------------------------------------------------------

clean: $(addsuffix -clean,$(SIMS))
