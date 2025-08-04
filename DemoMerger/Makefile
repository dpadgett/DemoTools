#
# Linux Makefile for GCC for Jedi Academy SDK
#
# Credits
#         - JA Coders (http://jacoders.co.cc)
#         - Raz0r
#         - TrippHop
#         - Xycaleth
#         - Sil
#

# Compiler - GCC
CC = gcc
#CC = /opt/intel/composer_xe_2013_sp1.0.080/bin/ia32/icc

# Compiler Flags
# I***          		include paths
# D***          		preprocessor definitions
# fPIC          		required to build shared object
# w             		disable warnings
# O3            		optimisation (warning: may cause issues)
# m32           		force 32-bit compilation
# -masm=intel   		assembler syntax.


# Compiler Flags - swap CONFIG for release/debug builds
CONFIG = -O3 -DNDEBUG
#CONFIG = -O0 -g -DNDEBUG
#CONFIG = -g3 -D_DEBUG
INCLUDES = -I../OpenJK/codemp -I../OpenJK/shared -I../JKDemoMetadata -I../DemoTrimmer
DEFINES = -DARCH_STRING=\"arm\" -DDEDICATED -DFINAL_BUILD -D_CONSOLE
OPTIONS = -std=c++11 -Wall -Wno-invalid-offsetof -Wno-write-strings -Wno-comment -fsigned-char -fvisibility=hidden
FLAGS = $(INCLUDES) $(DEFINES) $(OPTIONS) $(CONFIG)
LIBS = -lm -lstdc++

# Target file
TARGET = demomerger

# Object Files
OJKSRCS = \
	codemp/qcommon/huffman.o \
	codemp/qcommon/msg.o \
	shared/qcommon/q_math.o \
	codemp/qcommon/q_shared.o \
	shared/qcommon/q_string.o
OJKFILES = $(notdir $(OJKSRCS))
LOCALFILES = \
	demo_merger.o \
	../JKDemoMetadata/cl_parse.o \
	../JKDemoMetadata/cmd.o \
	../JKDemoMetadata/deps.o \
	../JKDemoMetadata/utils.o \
	../DemoTrimmer/demo_utils.o
FILES = $(OJKFILES) $(LOCALFILES)

# "make"
$(TARGET):  $(FILES)
	$(CC) -o $(TARGET) $(FILES) $(LIBS)
	chmod +x $(TARGET)

# "make .c.o"
.c.o:
	$(CC) $(FLAGS) -c $< -o $@

.cpp.o:
	$(CC) $(FLAGS) -c $< -o $@

$(OJKFILES):
	$(eval OFILE := $(foreach var,$(OJKSRCS),$(if $(filter $(notdir $(var)),$@),$(var))))
	$(eval FILE := $(if $(filter shared/%,$(OFILE)),$(patsubst %.o,%.c,$(OFILE)),$(patsubst %.o,%.cpp,$(OFILE))))
	$(CC) $(FLAGS) -c ../OpenJK/$(FILE) -o $@

# "make clean"
clean:
	rm -f $(FILES) $(TARGET)
