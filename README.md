# ESMAT (Electric Sail Mission Analysis Tool)

ESMAT is a high-precision, interactive orbital mechanics and mission design simulation environment specifically focused on modeling and analyzing Electric Solar Wind Sail (E-Sail) spacecraft.

## Purpose
The primary purpose of ESMAT is to provide a comprehensive, N-body physics engine combined with a 3D visualizer to calculate and simulate interplanetary trajectories. It leverages the robust NASA SPICE toolkit for ephemeris data and models the complex interactions between the solar wind and E-Sail tethers in real-time. This tool is designed to help researchers and mission planners evaluate the feasibility, performance, and optimal trajectories for E-Sail missions across the solar system.

## Features
- **N-Body Gravity Integration**: High-fidelity trajectory propagation using dynamic RK4 step limits to eliminate drift.
- **Electric Sail Physics**: Real-time modeling of E-Sail thrust utilizing 4D ENLIL solar wind grids.
- **NASA SPICE Integration**: Direct use of standard `.bsp`, `.tpc`, and `.tls` kernels for accurate celestial body states.
- **Interactive 3D Visualizer**: Real-time rendering of orbits, planets, and spacecraft utilizing OpenGL and ImGui.
- **Mission Planning**: Create and execute impulsive burns and positional propagation commands dynamically.

## Installation and Setup

### Prerequisites
- C++17 compatible compiler (e.g., MSVC, GCC, Clang)
- CMake (version 3.10 or higher)

### Building the Project
1. Clone the repository:
   ```bash
   git clone https://github.com/sfga12/ESMAT.git
   cd ESMAT
   ```

2. Create a build directory:
   ```bash
   mkdir build
   cd build
   ```

3. Generate the build files and compile:
   ```bash
   cmake ..
   cmake --build . --config Release
   ```

4. Run the executable generated in the `build` directory. Ensure that the `data/` and `dependencies/` folders are accessible relative to the executable path.

## Acknowledgements
Founded By The Scientific and Technological Research Council of Turkey (TÜBİTAK)
