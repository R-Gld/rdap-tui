from conan import ConanFile
from conan.tools.build import check_min_cppstd
from conan.tools.cmake import CMake, CMakeDeps, CMakeToolchain, cmake_layout


class RdapTuiRecipe(ConanFile):
    name = "rdap-tui"
    version = "0.0.1"
    package_type = "application"
    settings = "os", "compiler", "build_type", "arch"
    options = {
        "build_tests": [True, False],
        "network_tests": [True, False],
    }
    default_options = {
        "build_tests": True,
        "network_tests": False,
        "libcurl/*:shared": False,
        "libcurl/*:build_executable": False,
    }

    def requirements(self):
        self.requires("ftxui/6.1.9")
        self.requires("libcurl/8.20.0")
        self.requires("nlohmann_json/3.12.0")
        if self.options.build_tests:
            self.test_requires("catch2/3.15.1")

    def validate(self):
        check_min_cppstd(self, "20")

    def layout(self):
        cmake_layout(self)

    def generate(self):
        dependencies = CMakeDeps(self)
        dependencies.generate()

        toolchain = CMakeToolchain(self)
        toolchain.variables["RDAP_BUILD_TESTS"] = bool(self.options.build_tests)
        toolchain.variables["RDAP_ENABLE_NETWORK_TESTS"] = bool(self.options.network_tests)
        toolchain.generate()

    def build(self):
        cmake = CMake(self)
        cmake.configure()
        cmake.build()
