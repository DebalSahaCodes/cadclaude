# pmodel — lightweight parametric modeller

A single-file (~900 line) C++17 app built on **Dear ImGui** (immediate-mode GUI) +
**GLFW** + fixed-function **OpenGL** (no GL loader, no shaders to ship).

## Build

Dependencies (GLFW and ImGui) are fetched automatically by CMake.

```
# Linux (Debian/Ubuntu) prerequisites
sudo apt install build-essential cmake libgl1-mesa-dev xorg-dev

cmake -S . -B build
cmake --build build -j
./build/pmodel
```
If CMake complains about `wayland-scanner` or RandR headers, either install
`libwayland-dev wayland-protocols libxkbcommon-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev`
or configure with `-DGLFW_BUILD_WAYLAND=OFF` (X11 only).

Windows (Visual Studio) and macOS (Xcode CLT) work with the same two cmake commands.

## How it works

* **Parameters** – named values or expressions (`ball_r = post_r*1.6`). Drag the
  number to tweak a value live; everything that references it updates.
* **Objects** – Box, Cylinder/Cone, Sphere, Torus. Every dimension, position,
  rotation and array setting is an expression referencing parameters.
  Invalid expressions turn red and keep their last good value.
* **Linear array** – `Count` (can be a parameter) and `Offset per copy`.
* **Lighting** – ambient + 4 lights (directional or point with falloff),
  specular strength and shininess, light gizmos in the viewport.
* **File** – save/load projects (`.pm` text), export **OBJ** (Z-up or Y-up).

Expressions: `+ - * / ^ ( )`, `pi`, `sin cos tan asin acos atan atan2` (degrees),
`sqrt abs floor ceil round min(a,b) max(a,b) pow(a,b)`.

Controls: left-drag orbit · right/middle-drag or Shift+left-drag pan · wheel zoom.

Note: lighting is per-vertex (classic fixed-function Phong); primitives are
tessellated finely enough that it looks smooth. Raise **Resolution** on an object
for sharper highlights.
