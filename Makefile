NT_API_PATH := ../disting_pulsar/distingNT_API
INCLUDE_PATH := $(NT_API_PATH)/include

inputs := $(wildcard src/*.cpp)
outputs := $(patsubst src/%.cpp,plugins/%.o,$(inputs))

all: $(outputs)

clean:
	rm -f $(outputs)

plugins/%.o: src/%.cpp
	mkdir -p $(@D)
	arm-none-eabi-c++ -std=c++11 -mcpu=cortex-m7 -mfpu=fpv5-d16 -mfloat-abi=hard -mthumb -fno-rtti -fno-exceptions -Os -ffast-math -fPIC -Wall -I$(INCLUDE_PATH) -c -o $@ $<
