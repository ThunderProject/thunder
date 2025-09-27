from conan import ConanFile
from conan.tools.cmake import cmake_layout

class ThunderRecipe(ConanFile):
    settings = "os", "compiler", "build_type", "arch"
    generators = "CMakeDeps", "CMakeToolchain"

    def requirements(self):
        self.requires("catch2/3.9.0")
        self.requires("benchmark/1.9.4")
        self.requires("libassert/2.2.1")

    def layout(self):
        cmake_layout(self)