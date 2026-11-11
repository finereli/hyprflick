CXXFLAGS ?= -O2
CXXFLAGS += -shared -fPIC -std=c++2b

ifeq ($(CXX),g++)
    EXTRA_FLAGS = --no-gnu-unique
endif

all: hyprflick.so

hyprflick.so: main.cpp
	$(CXX) $(CXXFLAGS) $(LDFLAGS) $(EXTRA_FLAGS) main.cpp -o $@ `pkg-config --cflags pixman-1 libdrm hyprland pangocairo libinput libudev wayland-server xkbcommon`

clean:
	rm -f hyprflick.so
