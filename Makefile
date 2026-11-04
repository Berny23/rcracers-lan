# rclan.asi - build & deploy
#
#   make            build build/rclan.asi
#   make install    copy ASI loader (as winmm.dll) + plugin into the game dir
#   make uninstall  remove everything `install` added
#   make run        install, then launch the game via Proton (see run.sh: PROTON, PREFIX)
#   make release    build/rclan-$(VERSION).zip: unpack into the game folder

CXX      := i686-w64-mingw32-g++
CXXFLAGS := -std=c++17 -O2 -Wall -Wextra -fno-exceptions -fno-rtti
LDFLAGS  := -shared -static -static-libgcc -static-libstdc++ -s
LDLIBS   := -lws2_32 -lgdi32

VERSION  := 1.0.0

# Game folder (the one with RCRacers.exe), e.g. make install GAME_DIR=/path/to/RCRacersDeluxe
GAME_DIR ?=

UAL_VERSION := v10.0.2
UAL_SHA256  := c8cb367641de962cb10be44786359a23be013c4f96962eb8120ff237f1561139
UAL_DIR     := third_party/ual-$(UAL_VERSION)
UAL_ZIP     := $(UAL_DIR)/Ultimate-ASI-Loader-NoPDB.zip
UAL_DLL     := $(UAL_DIR)/extracted/dinput8.dll

SRCS := $(wildcard src/*.cpp)
OBJS := $(SRCS:src/%.cpp=build/%.o)
ASI  := build/rclan.asi

.PHONY: all install uninstall run clean peer release

all: $(ASI)

build/%.o: src/%.cpp $(wildcard src/*.h) | build
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(ASI): $(OBJS)
	$(CXX) $(LDFLAGS) $^ -o $@ $(LDLIBS)

build:
	mkdir -p $@

# Headless lobby peer for testing (run under Wine, see tools/peer.cpp).
PEER := build/rclan-peer.exe
peer: $(PEER)
$(PEER): tools/peer.cpp build/net.o build/race.o build/game.o build/build.o build/config.o build/log.o
	$(CXX) $(CXXFLAGS) -static -static-libgcc -static-libstdc++ -s $^ -o $@ $(LDLIBS)

# Ultimate ASI Loader (x86, no PDB). The release ships it as dinput8.dll;
# it picks its proxy role from its own file name, so we install it as winmm.dll.
$(UAL_ZIP):
	mkdir -p $(UAL_DIR)
	curl -sSL -o $@ https://github.com/ThirteenAG/Ultimate-ASI-Loader/releases/download/$(UAL_VERSION)/Ultimate-ASI-Loader-NoPDB.zip
	echo "$(UAL_SHA256)  $@" | sha256sum -c -

$(UAL_DLL): $(UAL_ZIP)
	unzip -o -q $< -d $(UAL_DIR)/extracted
	touch $@

install: $(ASI) $(UAL_DLL)
	@test -n "$(GAME_DIR)" || { echo "set GAME_DIR to the game folder: make install GAME_DIR=/path/to/RCRacersDeluxe"; exit 1; }
	@test -f "$(GAME_DIR)/RCRacers.exe" || { echo "RCRacers.exe not found in GAME_DIR=$(GAME_DIR)"; exit 1; }
	install -m 644 $(UAL_DLL) "$(GAME_DIR)/winmm.dll"
	install -d "$(GAME_DIR)/scripts"
	install -m 644 $(ASI) "$(GAME_DIR)/scripts/rclan.asi"
	test -e "$(GAME_DIR)/scripts/rclan.ini" || install -m 644 dist/rclan.ini "$(GAME_DIR)/scripts/rclan.ini"

# Release zip, laid out like the game folder: winmm.dll at the top, the rest
# in scripts/. Rebuilt from scratch so the build stamp is current.
RELEASE := build/rclan-$(VERSION)
release: $(UAL_DLL)
	$(MAKE) clean
	$(MAKE) all
	rm -rf $(RELEASE) $(RELEASE).zip
	install -d $(RELEASE)/scripts
	install -m 644 $(UAL_DLL) $(RELEASE)/winmm.dll
	install -m 644 $(ASI) $(RELEASE)/scripts/rclan.asi
	sed 's/$$/\r/' dist/rclan.ini > $(RELEASE)/scripts/rclan.ini
	install -m 644 README.md $(RELEASE)/scripts/rclan-README.md
	install -m 644 dist/LICENSE-UltimateASILoader.txt $(RELEASE)/scripts/LICENSE-UltimateASILoader.txt
	cd $(RELEASE) && zip -q -r -X ../rclan-$(VERSION).zip winmm.dll scripts
	@echo "-> $(RELEASE).zip"; unzip -l $(RELEASE).zip

uninstall:
	@test -n "$(GAME_DIR)" || { echo "set GAME_DIR to the game folder"; exit 1; }
	rm -f "$(GAME_DIR)/winmm.dll" "$(GAME_DIR)/scripts/rclan.asi" "$(GAME_DIR)/rclan.log"
	-rmdir "$(GAME_DIR)/scripts" 2>/dev/null

run: install
	GAME_DIR="$(GAME_DIR)" ./run.sh

clean:
	rm -rf build
