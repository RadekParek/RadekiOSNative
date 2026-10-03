CXX ?= g++
CXXFLAGS ?= -std=c++20 -Wall -Wextra -O1 -g -I.
ifdef SAN
CXXFLAGS += -fsanitize=address,undefined -fno-sanitize-recover=undefined
endif
LIBSRC = mach_o/macho.cpp binary_analysis/analysis.cpp objc/objc.cpp loader/dyld.cpp relinker/relinker.cpp runtime/runtime.cpp runtime/compat_libsystem.cpp runtime/compat_cxx.cpp runtime/cxx_forward.cpp runtime/stub_dispatch.cpp runtime/framework_stubs.cpp compat/status.cpp compat/matrix.cpp ipa/inflate.cpp ipa/png.cpp ipa/plist.cpp ipa/bundle.cpp
BUILD = build
LIBOBJ = $(LIBSRC:%.cpp=$(BUILD)/%.o)

all: $(BUILD)/radeki $(BUILD)/radeki_tests $(BUILD)/dump_sections
$(BUILD)/%.o: %.cpp $(wildcard */*.h)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@
$(BUILD)/libradeki.a: $(LIBOBJ)
	ar rcs $@ $^
$(BUILD)/radeki: $(BUILD)/tools/radeki.o $(BUILD)/libradeki.a
	$(CXX) $(CXXFLAGS) $^ -o $@
$(BUILD)/radeki_tests: $(BUILD)/tests/test_main.o $(BUILD)/libradeki.a
	$(CXX) $(CXXFLAGS) $^ -o $@
$(BUILD)/dump_sections: $(BUILD)/tools/dump_sections.o $(BUILD)/libradeki.a
	$(CXX) $(CXXFLAGS) $^ -o $@
test: $(BUILD)/radeki_tests
	$(BUILD)/radeki_tests
clean:
	rm -rf $(BUILD)
