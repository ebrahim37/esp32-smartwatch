# SWCompanion.ipa: unsigned-for-distribution (ad-hoc pseudo signature) arm64 app for LiveContainer.
# Run through scripts/sw ipa, which passes SDK, OUT, DIST and ROOT.
APP := SWCompanion
CORE := $(ROOT)/firmware/companion/components/companion_core
PROTO := $(ROOT)/shared/protocol

SRCS := $(wildcard $(ROOT)/phone/ios/src/*.mm) \
	$(addprefix $(PROTO)/src/,wire.cpp frame.cpp transfer.cpp) \
	$(addprefix $(CORE)/src/,backoff.cpp md5.cpp ntfy.cpp play_queue.cpp quota.cpp subsonic.cpp sync_plan.cpp)
OBJS := $(patsubst $(ROOT)/%,$(OUT)/obj/%.o,$(SRCS))

COMMON := -target arm64-apple-ios15.0 -isysroot $(SDK) -resource-dir $(SW_CLANG_RESOURCE_DIR) \
	-isystem $(SDK)/usr/include/c++/v1 -I$(PROTO)/include -I$(CORE)/include -I$(ROOT)/phone/ios/src \
	-std=c++20 -O2 -g0 -Wall -Wno-deprecated-declarations -MMD -MP
FRAMEWORKS := Foundation UIKit CoreBluetooth CoreLocation AVFoundation AudioToolbox MediaPlayer Network

$(DIST)/$(APP).ipa: $(OUT)/Payload/$(APP).app/$(APP) $(OUT)/Payload/$(APP).app/Info.plist
	mkdir -p $(DIST)
	rm -f $@
	cd $(OUT) && zip -qr $(abspath $@) Payload

$(OUT)/Payload/$(APP).app/Info.plist: $(ROOT)/phone/ios/Info.plist
	mkdir -p $(dir $@)
	cp $< $@

# LiveContainer rewrites load commands to load the app as a library, so leave room for them.
$(OUT)/Payload/$(APP).app/$(APP): $(OBJS)
	mkdir -p $(dir $@)
	ld64.lld -arch arm64 -platform_version ios 15.0 27.2 -syslibroot $(SDK) -dead_strip -headerpad 0x4000 \
		-o $@ $(OBJS) -lSystem -lc++ -lobjc $(addprefix -framework ,$(FRAMEWORKS))
	ldid -S $@

$(OUT)/obj/%.mm.o: $(ROOT)/%.mm
	mkdir -p $(dir $@)
	clang++ $(COMMON) -fobjc-arc -c $< -o $@

$(OUT)/obj/%.cpp.o: $(ROOT)/%.cpp
	mkdir -p $(dir $@)
	clang++ $(COMMON) -c $< -o $@

-include $(OBJS:.o=.d)
