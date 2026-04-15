CXX ?= g++
BUILD_DIR := build
OBJ_DIR := $(BUILD_DIR)/obj
BIN_DIR := $(BUILD_DIR)/bin
LIB_DIR := $(BUILD_DIR)/lib

CPPFLAGS := -Iinclude
CXXFLAGS ?= -std=c++20 -Wall -Wextra -Werror -pedantic -g -MMD -MP
LDLIBS :=

ifeq ($(OS),Windows_NT)
EXE_EXT := .exe
SHARED_EXT := dll
SHARED_FLAGS := -shared
CPPFLAGS += -DUNICODE -D_UNICODE -DWINVER=0x0601 -D_WIN32_WINNT=0x0601 -D_WIN32_IE=0x0600
LDFLAGS += -static-libgcc -static-libstdc++
LDLIBS += -ladvapi32 -lgdi32 -lole32 -lshell32 -luuid -luser32
else
EXE_EXT :=
SHARED_EXT := so
SHARED_FLAGS := -shared -fPIC
LDLIBS += -ldl
endif

APP_SOURCES := $(shell find src -name '*.cpp' | sort)
TEST_SOURCES := $(filter-out tests/fakes/fake_ffmpeg_avutil.cpp,$(shell find tests -name '*.cpp' | sort))
APP_OBJECTS := $(patsubst %.cpp,$(OBJ_DIR)/%.o,$(APP_SOURCES))
TEST_OBJECTS := $(patsubst %.cpp,$(OBJ_DIR)/%.o,$(TEST_SOURCES))
APP_LIB_OBJECTS := $(filter-out $(OBJ_DIR)/src/main.o,$(APP_OBJECTS))
DEPFILES := $(APP_OBJECTS:.o=.d) $(TEST_OBJECTS:.o=.d)

APP := $(BIN_DIR)/locking_glass$(EXE_EXT)
TEST_BIN := $(BIN_DIR)/locking_glass_tests$(EXE_EXT)
FAKE_FFMPEG_LIB := $(LIB_DIR)/libfakeavutil.$(SHARED_EXT)

.PHONY: all clean test smoke prototype

all: $(APP) $(TEST_BIN) $(FAKE_FFMPEG_LIB)

$(APP): $(APP_OBJECTS)
	mkdir -p $(dir $@)
	$(CXX) $(APP_OBJECTS) -o $@ $(LDFLAGS) $(LDLIBS)

$(TEST_BIN): $(APP_LIB_OBJECTS) $(TEST_OBJECTS)
	mkdir -p $(dir $@)
	$(CXX) $(APP_LIB_OBJECTS) $(TEST_OBJECTS) -o $@ $(LDFLAGS) $(LDLIBS)

$(FAKE_FFMPEG_LIB): tests/fakes/fake_ffmpeg_avutil.cpp
	mkdir -p $(dir $@)
	$(CXX) $(CPPFLAGS) -std=c++20 -Wall -Wextra -Werror -pedantic $(SHARED_FLAGS) $< -o $@

$(OBJ_DIR)/%.o: %.cpp
	mkdir -p $(dir $@)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

test: $(TEST_BIN) $(FAKE_FFMPEG_LIB)
	LOCKING_GLASS_FFMPEG_LIBRARY=$(abspath $(FAKE_FFMPEG_LIB)) $(TEST_BIN)

smoke: $(APP) $(FAKE_FFMPEG_LIB)
	LOCKING_GLASS_FFMPEG_LIBRARY=$(abspath $(FAKE_FFMPEG_LIB)) $(APP) --self-check

prototype: $(APP) $(FAKE_FFMPEG_LIB)
	LOCKING_GLASS_FFMPEG_LIBRARY=$(abspath $(FAKE_FFMPEG_LIB)) $(APP) --prototype-windows-apis

clean:
	rm -rf $(BUILD_DIR)

-include $(DEPFILES)
