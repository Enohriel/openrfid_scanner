CXX = g++
CC = gcc
CXXFLAGS = -std=c++14 -DNATIVE_TEST -Wall -Wextra
CFLAGS = -std=c11 -DNATIVE_TEST -Wall -Wextra
INCLUDES = -I ../../include -I ../../src -I .
JSON_INCLUDES = -I ../../.pio/libdeps/esp32dev/htcw_json/src -I ../../.pio/libdeps/esp32dev/htcw_io/src -I ../../.pio/libdeps/esp32dev/htcw_bits

# OpenSpool sources
COMMON_NATIVE_SRCS = NativePlatform.cpp ../../src/OpenSpoolParser.cpp ../../src/ConversionUtils.cpp

# App flow tests (uses TestNFCManager stub)
APP_FLOW_SRCS = test_app_flow.cpp $(COMMON_NATIVE_SRCS)
APP_FLOW_TARGET = test_runner

# NFC read tests (uses real NFCManager with StubNFCConnection)
NFC_READ_SRCS = test_nfc_read.cpp $(COMMON_NATIVE_SRCS)
NFC_READ_TARGET = test_nfc_read_runner

# Bgcode parser tests
BGCODE_SRCS = test_bgcode_parser.cpp NativePlatform.cpp
BGCODE_TARGET = test_bgcode_parser

LCD_MANAGER_SRCS = test_lcd_manager.cpp NativePlatform.cpp
LCD_MANAGER_TARGET = test_lcd_manager_runner

JSON_PULL_HELPERS_SRCS = test_json_pull_helpers.cpp ../../src/JsonPullHelpers.cpp ../../.pio/libdeps/esp32dev/htcw_io/src/source/io_stream.cpp
JSON_PULL_HELPERS_TARGET = test_json_pull_helpers_runner

# Raw write tests (uses real NFCManager with StubNFCConnection)
RAW_WRITE_SRCS = test_raw_write.cpp $(COMMON_NATIVE_SRCS)
RAW_WRITE_TARGET = test_raw_write_runner

# Printer strategy tests (Moonraker + PrusaLink)
MOONRAKER_SRCS = test_moonraker_strategy.cpp ../../src/MoonrakerAPIStrategy.cpp ../../src/PrusaLinkAPIStrategy.cpp ../../src/BgcodeParser.cpp ../../src/ConversionUtils.cpp ../../.pio/libdeps/esp32dev/htcw_io/src/source/io_stream.cpp
MOONRAKER_TARGET = test_moonraker_runner

all: $(APP_FLOW_TARGET) $(NFC_READ_TARGET) $(BGCODE_TARGET) $(LCD_MANAGER_TARGET) $(JSON_PULL_HELPERS_TARGET) $(RAW_WRITE_TARGET) $(MOONRAKER_TARGET)
	./$(APP_FLOW_TARGET)
	./$(NFC_READ_TARGET)
	./$(BGCODE_TARGET)
	./$(LCD_MANAGER_TARGET)
	./$(JSON_PULL_HELPERS_TARGET)
	./$(RAW_WRITE_TARGET)
	./$(MOONRAKER_TARGET)

$(APP_FLOW_TARGET): $(APP_FLOW_SRCS)
	$(CXX) $(CXXFLAGS) $(INCLUDES) $(APP_FLOW_SRCS) -o $(APP_FLOW_TARGET)

$(NFC_READ_TARGET): $(NFC_READ_SRCS)
	$(CXX) $(CXXFLAGS) $(INCLUDES) $(NFC_READ_SRCS) -o $(NFC_READ_TARGET)

$(BGCODE_TARGET): $(BGCODE_SRCS)
	$(CXX) $(CXXFLAGS) $(INCLUDES) $(BGCODE_SRCS) -o $(BGCODE_TARGET)

$(LCD_MANAGER_TARGET): $(LCD_MANAGER_SRCS)
	$(CXX) $(CXXFLAGS) $(INCLUDES) $(LCD_MANAGER_SRCS) -o $(LCD_MANAGER_TARGET)

$(JSON_PULL_HELPERS_TARGET): $(JSON_PULL_HELPERS_SRCS)
	$(CXX) $(CXXFLAGS) $(INCLUDES) $(JSON_INCLUDES) $(JSON_PULL_HELPERS_SRCS) -o $(JSON_PULL_HELPERS_TARGET)

$(RAW_WRITE_TARGET): $(RAW_WRITE_SRCS)
	$(CXX) $(CXXFLAGS) $(INCLUDES) $(RAW_WRITE_SRCS) -o $(RAW_WRITE_TARGET)

$(MOONRAKER_TARGET): $(MOONRAKER_SRCS)
	$(CXX) $(CXXFLAGS) $(INCLUDES) $(JSON_INCLUDES) $(MOONRAKER_SRCS) -o $(MOONRAKER_TARGET)

run-app-flow: $(APP_FLOW_TARGET)
	./$(APP_FLOW_TARGET)

run-nfc-read: $(NFC_READ_TARGET)
	./$(NFC_READ_TARGET)

run-bgcode: $(BGCODE_TARGET)
	./$(BGCODE_TARGET)

run-lcd-manager: $(LCD_MANAGER_TARGET)
	./$(LCD_MANAGER_TARGET)

run-json-pull-helpers: $(JSON_PULL_HELPERS_TARGET)
	./$(JSON_PULL_HELPERS_TARGET)

run-raw-write: $(RAW_WRITE_TARGET)
	./$(RAW_WRITE_TARGET)

run-moonraker: $(MOONRAKER_TARGET)
	./$(MOONRAKER_TARGET)

clean:
	rm -f $(APP_FLOW_TARGET) $(NFC_READ_TARGET) $(BGCODE_TARGET) $(LCD_MANAGER_TARGET) $(JSON_PULL_HELPERS_TARGET) $(RAW_WRITE_TARGET) $(MOONRAKER_TARGET)

.PHONY: all clean run-app-flow run-nfc-read run-bgcode run-lcd-manager run-json-pull-helpers run-raw-write run-moonraker
