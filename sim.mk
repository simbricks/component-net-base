# Copyright 2021 Max Planck Institute for Software Systems, and
# National University of Singapore
#
# Permission is hereby granted, free of charge, to any person obtaining
# a copy of this software and associated documentation files (the
# "Software"), to deal in the Software without restriction, including
# without limitation the rights to use, copy, modify, merge, publish,
# distribute, sublicense, and/or sell copies of the Software, and to
# permit persons to whom the Software is furnished to do so, subject to
# the following conditions:
#
# The above copyright notice and this permission notice shall be
# included in all copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
# EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
# MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
# IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
# CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
# TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
# SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

# Shared build recipe for a single net-base simulator. A sim's Makefile sets a
# few variables and then `include ../sim.mk`:
#   SIM_BIN     - output binary name             (e.g. net_switch)
#   SIM_SRC     - single source file             (e.g. net_switch.cc / net_wire.c)
#   SIM_INSTALL - installed name in $(PREFIX)/bin (e.g. simb_net_switch)
#   SIM_LIBS    - simbricks archive base names, in link order (e.g. nicif network base)
#   SIM_LDLIBS  - extra link libraries, optional (e.g. -lpcap)
#
# Each sim directory is a standalone makefile: build it directly with
#   make -C <dir> <target>      (or:  cd <dir> && make <target>)
# GNU make runs in <dir>/, so relative paths (SIM_SRC, the output binary, the
# "../netproto/..." quoted include some sims use, and this "../sim.mk" include)
# resolve against <dir>/. The source extension selects the compiler; linking
# always uses the C++ driver because the simbricks archives pull in C++ symbols.

# --- Configuration (normally overridden on the top-level command line) -----
PREFIX            ?= /usr/local
SIMBRICKS_INC_DIR ?= $(PREFIX)/include
SIMBRICKS_LIB_DIR ?= $(PREFIX)/lib/simbricks

CC       ?= cc
CXX      ?= c++

# Append to (not overwrite) any incoming flags so a conda build's CPPFLAGS /
# CFLAGS / CXXFLAGS / LDFLAGS from the environment are honored — notably
# LDFLAGS carries -L$(PREFIX)/lib so the linker finds host libs like libpcap.
CPPFLAGS += -I$(SIMBRICKS_INC_DIR)
CFLAGS   += -Wall -Wextra -Wno-unused-parameter -O3 -fPIC -std=gnu11
CXXFLAGS += -Wall -Wextra -Wno-unused-parameter -O3 -fPIC -std=gnu++17

# Pick the compiler + flags from the source extension: .c -> C, else C++.
ifeq ($(suffix $(SIM_SRC)),.c)
  SIM_COMPILE := $(CC) $(CFLAGS)
else
  SIM_COMPILE := $(CXX) $(CXXFLAGS)
endif

SIM_OBJ       := $(basename $(SIM_SRC)).o
# Expand archive base names to full paths under $(SIMBRICKS_LIB_DIR).
SIM_LIB_PATHS := $(patsubst %,$(SIMBRICKS_LIB_DIR)/lib%.a,$(SIM_LIBS))

.PHONY: all install clean

all: $(SIM_BIN)

$(SIM_OBJ): $(SIM_SRC)
	$(SIM_COMPILE) $(CPPFLAGS) -c -o $@ $<

$(SIM_BIN): $(SIM_OBJ) $(SIM_LIB_PATHS)
	$(CXX) $(LDFLAGS) -o $@ $(SIM_OBJ) $(SIM_LIB_PATHS) $(SIM_LDLIBS)

install: $(SIM_BIN)
	install -d $(PREFIX)/bin
	install -m 755 $(SIM_BIN) $(PREFIX)/bin/$(SIM_INSTALL)

clean:
	rm -f $(SIM_BIN) $(SIM_OBJ)
