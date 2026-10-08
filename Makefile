NAME = hyprdrop
CXX ?= g++
CXXFLAGS = -fPIC --no-gnu-unique -std=c++2b -O2 \
	`pkg-config --cflags pixman-1 libdrm hyprland pangocairo libinput libudev wayland-server xkbcommon`

SRC = $(wildcard src/*.cpp)
OBJ = $(SRC:src/%.cpp=build/%.o)

all: $(NAME).so

$(NAME).so: $(OBJ)
	$(CXX) -shared $(OBJ) -o $@

build/%.o: src/%.cpp src/hyprdrop.hpp
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	rm -rf build $(NAME).so

.PHONY: all clean
