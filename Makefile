CXX ?= g++
CXXFLAGS ?= -std=gnu++17 -Wall -Wextra -pthread
CPPFLAGS ?= -D_GNU_SOURCE

# AI 直连（OpenSSL TLS）：macOS 用 Homebrew 的 openssl，Linux 用系统 libssl-dev
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
OPENSSL_PREFIX ?= /opt/homebrew/opt/openssl
CPPFLAGS += -I$(OPENSSL_PREFIX)/include
LDFLAGS += -L$(OPENSSL_PREFIX)/lib
endif
LDLIBS += -lssl -lcrypto

SRC_DIR := ReactorHttp-Cpp
BUILD_DIR := build
TARGET := $(BUILD_DIR)/reactor-http
TEST_TARGET := $(BUILD_DIR)/http-request-test
DRIVE_TEST_TARGET := $(BUILD_DIR)/drive-test
AI_TEST_TARGET := $(BUILD_DIR)/ai-test
SOURCES := $(wildcard $(SRC_DIR)/*.cpp)
HEADERS := $(wildcard $(SRC_DIR)/*.h)
TEST_SOURCES := tests/http_request_test.cpp \
	$(SRC_DIR)/Buffer.cpp \
	$(SRC_DIR)/Channel.cpp \
	$(SRC_DIR)/Config.cpp \
	$(SRC_DIR)/Dispatcher.cpp \
	$(SRC_DIR)/EventLoop.cpp \
	$(SRC_DIR)/HttpRequest.cpp \
	$(SRC_DIR)/Httpresponse.cpp \
	$(SRC_DIR)/Log.cpp \
	$(SRC_DIR)/PollDispatcher.cpp \
	$(SRC_DIR)/SelectDispatcher.cpp \
	$(SRC_DIR)/ServerMetrics.cpp \
	$(SRC_DIR)/DriveServer.cpp \
	$(SRC_DIR)/SessionStore.cpp \
	$(SRC_DIR)/Sha256.cpp \
	$(SRC_DIR)/SidecarClient.cpp \
	$(SRC_DIR)/AiClient.cpp \
	$(SRC_DIR)/ThreadPool.cpp \
	$(SRC_DIR)/UserStore.cpp \
	$(SRC_DIR)/WorkerThread.cpp
DRIVE_TEST_SOURCES := tests/drive_test.cpp \
	$(SRC_DIR)/Buffer.cpp \
	$(SRC_DIR)/Channel.cpp \
	$(SRC_DIR)/Config.cpp \
	$(SRC_DIR)/Dispatcher.cpp \
	$(SRC_DIR)/DriveServer.cpp \
	$(SRC_DIR)/EventLoop.cpp \
	$(SRC_DIR)/HttpRequest.cpp \
	$(SRC_DIR)/Httpresponse.cpp \
	$(SRC_DIR)/Log.cpp \
	$(SRC_DIR)/PollDispatcher.cpp \
	$(SRC_DIR)/SelectDispatcher.cpp \
	$(SRC_DIR)/ServerMetrics.cpp \
	$(SRC_DIR)/SessionStore.cpp \
	$(SRC_DIR)/Sha256.cpp \
	$(SRC_DIR)/SidecarClient.cpp \
	$(SRC_DIR)/AiClient.cpp \
	$(SRC_DIR)/UserStore.cpp

AI_TEST_SOURCES := tests/ai_test.cpp \
	$(SRC_DIR)/AiClient.cpp \
	$(SRC_DIR)/Log.cpp

ifneq ($(shell uname -s),Linux)
SOURCES := $(filter-out $(SRC_DIR)/EpollDispatcher.cpp,$(SOURCES))
endif

.PHONY: all release test clean

all: CPPFLAGS += -DDEBUG=1
all: CXXFLAGS += -O0 -g
all: $(TARGET)

release: CPPFLAGS += -DDEBUG=0
release: CXXFLAGS += -O2 -DNDEBUG
release: $(TARGET)

test: CPPFLAGS += -DDEBUG=0
test: CXXFLAGS += -O2 -DNDEBUG
test: $(TEST_TARGET) $(DRIVE_TEST_TARGET) $(AI_TEST_TARGET)
	./$(TEST_TARGET)
	./$(DRIVE_TEST_TARGET)
	./$(AI_TEST_TARGET)

$(TARGET): $(SOURCES) $(HEADERS)
	@mkdir -p $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(SOURCES) -o $@ $(LDFLAGS) $(LDLIBS)

$(TEST_TARGET): $(TEST_SOURCES) $(HEADERS)
	@mkdir -p $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -I$(SRC_DIR) $(TEST_SOURCES) -o $@ $(LDFLAGS) $(LDLIBS)

$(DRIVE_TEST_TARGET): $(DRIVE_TEST_SOURCES) $(HEADERS)
	@mkdir -p $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -I$(SRC_DIR) $(DRIVE_TEST_SOURCES) -o $@ $(LDFLAGS) $(LDLIBS)

$(AI_TEST_TARGET): $(AI_TEST_SOURCES) $(HEADERS)
	@mkdir -p $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -I$(SRC_DIR) $(AI_TEST_SOURCES) -o $@ $(LDFLAGS) $(LDLIBS)

clean:
	rm -rf $(BUILD_DIR)
