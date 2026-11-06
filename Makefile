CC ?= gcc
PYTHON ?= python3
CFLAGS ?= -std=c99 -O2 -Wall -Wextra
CPPFLAGS += -Ifirmware/include -Isimulation

FW_SRC = firmware/src/dsp.c firmware/src/event.c firmware/src/sampling.c \
         firmware/src/packet.c firmware/src/comms.c firmware/src/storage.c \
         firmware/src/sensor_manager.c firmware/src/telemetry.c firmware/src/energy.c \
         firmware/src/node.c firmware/drivers/analog_drivers.c \
         firmware/drivers/digital_drivers.c firmware/profiles/profile_water.c \
         firmware/profiles/profile_air.c firmware/profiles/profile_soil.c \
         firmware/profiles/profile_registry.c firmware/hal/hal_virtual.c
SIM_SRC = simulation/sensors/virtual_sensors.c simulation/radio/radio_sim.c

.PHONY: all test clean
all: build/sensor_node build/test_firmware

build:
	mkdir -p build

build/sensor_node: $(FW_SRC) $(SIM_SRC) simulation/main_sim.c | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $^ -lm -o $@

build/test_firmware: $(FW_SRC) $(SIM_SRC) tests/test_firmware.c | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $^ -lm -o $@

test: all
	./build/test_firmware
	$(PYTHON) python/generate_data.py
	$(PYTHON) python/run_experiments.py
	$(PYTHON) python/analyse_results.py
	$(PYTHON) python/plots.py
	$(PYTHON) python/seed_study.py
	$(PYTHON) -m unittest discover -s tests -p "test_*.py"

clean:
	rm -rf build out
