CXX ?= g++
BUILD_DIR := build
OBJ_DIR := $(BUILD_DIR)/obj
BIN_DIR := $(BUILD_DIR)/bin
VERSION := $(strip $(shell cat VERSION))
VERSION_PARTS := $(subst ., ,$(VERSION))
VERSION_MAJOR := $(word 1,$(VERSION_PARTS))
VERSION_MINOR := $(word 2,$(VERSION_PARTS))
VERSION_PATCH := $(word 3,$(VERSION_PARTS))

CPPFLAGS := -Iinclude -DLOCKING_GLASS_VERSION_STR=\"$(VERSION)\"
CXXFLAGS ?= -std=c++20 -Wall -Wextra -Werror -pedantic -g -MMD -MP
LDFLAGS :=
LDLIBS :=
APP_RESOURCE_OBJECTS :=
WINDRES ?= windres

ifeq ($(OS),Windows_NT)
EXE_EXT := .exe
CPPFLAGS += -DUNICODE -D_UNICODE -DWINVER=0x0601 -D_WIN32_WINNT=0x0601 -D_WIN32_IE=0x0600
LDFLAGS += -static-libgcc -static-libstdc++
LDLIBS += -ladvapi32 -lgdi32 -lole32 -lshell32 -luuid -luser32
LOCKING_GLASS_FILE_VERSION := $(VERSION_MAJOR),$(VERSION_MINOR),$(VERSION_PATCH),0
APP_RESOURCE_OBJECTS := $(OBJ_DIR)/src/windows_version.res.o
else
EXE_EXT :=
endif

APP_SOURCES := $(shell find src -name '*.cpp' | sort)
TEST_SOURCES := $(shell find tests -name '*.cpp' | sort)
APP_OBJECTS := $(patsubst %.cpp,$(OBJ_DIR)/%.o,$(APP_SOURCES))
TEST_OBJECTS := $(patsubst %.cpp,$(OBJ_DIR)/%.o,$(TEST_SOURCES))
APP_LIB_OBJECTS := $(filter-out $(OBJ_DIR)/src/main.o,$(APP_OBJECTS))
APP_LINK_OBJECTS := $(APP_OBJECTS) $(APP_RESOURCE_OBJECTS)
DEPFILES := $(APP_OBJECTS:.o=.d) $(TEST_OBJECTS:.o=.d)

APP := $(BIN_DIR)/locking_glass$(EXE_EXT)
TEST_BIN := $(BIN_DIR)/locking_glass_tests$(EXE_EXT)

.PHONY: all clean test smoke prototype

all: $(APP) $(TEST_BIN)

$(APP): $(APP_LINK_OBJECTS)
	mkdir -p $(dir $@)
	$(CXX) $(APP_LINK_OBJECTS) -o $@ $(LDFLAGS) $(LDLIBS)

$(TEST_BIN): $(APP_LIB_OBJECTS) $(TEST_OBJECTS)
	mkdir -p $(dir $@)
	$(CXX) $(APP_LIB_OBJECTS) $(TEST_OBJECTS) -o $@ $(LDFLAGS) $(LDLIBS)

$(OBJ_DIR)/%.o: %.cpp VERSION
	mkdir -p $(dir $@)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

$(OBJ_DIR)/src/windows_version.res.o: src/windows_version.rc VERSION
	mkdir -p $(dir $@)
	$(WINDRES) -O coff \
		--define LOCKING_GLASS_VERSION_MAJOR=$(VERSION_MAJOR) \
		--define LOCKING_GLASS_VERSION_MINOR=$(VERSION_MINOR) \
		--define LOCKING_GLASS_VERSION_PATCH=$(VERSION_PATCH) \
		--define LOCKING_GLASS_FILE_VERSION=$(LOCKING_GLASS_FILE_VERSION) \
		$< $@

test: $(TEST_BIN)
	$(TEST_BIN)

smoke: $(APP)
	$(APP) --self-check

prototype: $(APP)
	$(APP) --prototype-windows-apis

clean:
	rm -rf $(BUILD_DIR)

-include $(DEPFILES)
