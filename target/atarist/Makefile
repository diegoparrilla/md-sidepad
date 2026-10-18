# Version from the repository's version.txt, the one source of truth:
# build.sh mounts the repository root, so it is two levels up from here.
VERSION := $(shell cat ../../version.txt)
VERSION_MAJOR := $(shell echo $(VERSION) | awk -F'.' '{print $$1}')
VERSION_MINOR := $(shell echo $(VERSION) | awk -F'.' '{print $$2}')
VERSION_PATCH := $(shell echo $(VERSION) | awk -F'.' '{print $$3}')


# GEMDOS Date: YYYYMMDD format
YEAR = $(shell date +'%Y')
MONTH = $(shell date +'%-m')   # %-m gives the month without leading zeros
DAY = $(shell date +'%-d')     # %-d gives the day without leading zeros


# Create the GEMDOS date format
GEMDOS_DATE_FORMATTED := $(shell echo "$$(($(YEAR) - 1980 << 9 | $(MONTH) << 5 | $(DAY)))")


# GEMDOS Time: HHMMSS format
HOUR = $(shell date +'%-H')    # %-H gives the hour without leading zeros
MINUTE = $(shell date +'%-M')  # %-M gives the minute without leading zeros
SECOND = $(shell date +'%-S')  # %-S gives the second without leading zeros

# Halve the seconds for GEMDOS format
HALVED_SECOND = $(shell echo "$$(( $(SECOND) / 2 ))")


# Create the GEMDOS time format
GEMDOS_TIME_FORMATTED :=  $(shell echo "$$(( $(HOUR) << 11 | $(MINUTE) << 5 | $(HALVED_SECOND) ))")

# Folder and file names
ODIR = ./obj
SOURCES_DIR = ./src
BUILD_DIR = ./build
DIST_DIR = ./dist
BIN = BOOT.BIN

# VASM PARAMETERS
# _DEBUG: 1 to enable debug, 0 to disable them
# To disable debug, make target DEBUG_MODE=0
DEBUG_MODE = 1
# The xpad submodule lives at the repository root, which is what the
# container mounts (see build.sh): -I here means userfw.s can include
# the generated equates by name instead of transcribing them.
XPAD_INC = ../../xpad/src

VASMFLAGS=-Faout -quiet -x -m68000 -spaces -showopt -devpac -I$(XPAD_INC) -D_DEBUG=$(DEBUG_MODE) -DGEMDOS_DATE=$(GEMDOS_DATE_FORMATTED) -DGEMDOS_TIME=$(GEMDOS_TIME_FORMATTED) -DVERSION_MAJOR=\"$(VERSION_MAJOR)\" -DVERSION_MINOR=\"$(VERSION_MINOR)\" -DVERSION_PATCH=\"$(VERSION_PATCH)\"
VASM = vasm
VLINK =  vlink
LINKER_SCRIPT = $(SOURCES_DIR)/userfw.ld
VLINKFLAGS = -brawbin1 -T $(LINKER_SCRIPT)

.PHONY: all
all: prepare dist

.PHONY: release
release: prepare dist

.PHONY: prepare
prepare: clean
	mkdir -p $(BUILD_DIR)

clean-compile : clean main.o userfw.o

# Generate the credit banner (banner_txt) boot_gem prints, with the version
# substituted, as MD/Net and MD/DOOM do. Written here rather than through
# vasm -D string macros, which vasm will not concatenate inside a dc.b, so
# the banner always matches the build version.
main.o: prepare
	printf 'banner_txt:\tdc.b "MD/Sidepad $(VERSION) (c)2026 Neil Rackett",13,10\n\tdc.b "GPLv3 neilrackett.com/atarist",13,10,0\n\teven\n' > $(SOURCES_DIR)/version.inc
	$(VASM) $(VASMFLAGS) $(SOURCES_DIR)/main.s -o $(BUILD_DIR)/main.o

userfw.o: prepare
	$(VASM) $(VASMFLAGS) $(SOURCES_DIR)/userfw.s -o $(BUILD_DIR)/userfw.o

.PHONY: build
build: main.o userfw.o
	$(VLINK) $(addprefix $(BUILD_DIR)/,$^) $(VLINKFLAGS) -o $(BUILD_DIR)/$(BIN)


.PHONY: dist
dist: build
	mkdir -p $(DIST_DIR)
	cp $(BUILD_DIR)/$(BIN) $(DIST_DIR) 	

.PHONY: clean
clean:
	rm -rf $(BUILD_DIR)
	rm -rf $(DIST_DIR)

## Tag this version
.PHONY: tag
tag:
	git tag v$(VERSION) && git push origin v$(VERSION) && \
	echo "Tagged: $(VERSION)"
