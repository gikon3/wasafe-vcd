import os

from conan import ConanFile
from conan.tools.build import check_min_cppstd
from conan.tools.cmake import CMakeToolchain, CMakeDeps, CMake, cmake_layout
from conan.tools.files import copy, rmdir

required_conan_version = ">=2.28"


class WaSafeVcdConan(ConanFile):
    name = "wasafe-vcd"
    version = "0.1.0"

    url = "https://github.com/gikon3/wasafe-vcd"
    homepage = "https://github.com/gikon3/wasafe-vcd"
    description = "Waveform database backend"
    license = "Apache-2.0"
    topics = ("waveform", "vcd", "eda")

    package_type = "library"
    settings = "os", "compiler", "build_type", "arch"

    implements = ["auto_shared_fpic"]

    options = {
        "shared": [True, False],
        "fPIC": [True, False]
    }
    default_options = {
        "shared": False,
        "fPIC": True
    }

    exports_sources = (
        "bench/*",
        "cmake/*",
        "include/*",
        "src/*",
        "tests/*",
        "CMakeLists.txt",
        "LICENSE*",
    )

    @property
    def _skip_test(self):
        return self.conf.get("tools.build:skip_test", default=False, check_type=bool)

    def layout(self):
        cmake_layout(self)
        self.cpp.source.includedirs = ["include"]
        self.cpp.build.includedirs = ["src/export"]
        self.cpp.build.bindirs = ["bin"]
        self.cpp.build.libdirs = ["lib"]

    def requirements(self):
        self.requires("wasafe/[~0]", transitive_headers=True, transitive_libs=True)

    def build_requirements(self):
        if not self._skip_test:
            self.test_requires("gtest/1.18.0")

    def validate(self):
        check_min_cppstd(self, "23")

    def generate(self):
        deps = CMakeDeps(self)
        deps.generate()
        tc = CMakeToolchain(self)
        tc.cache_variables["WASAFE_VCD_VERSION"] = self.version
        tc.cache_variables["WASAFE_VCD_DESCRIPTION"] = self.description
        tc.cache_variables["WASAFE_VCD_HOMEPAGE_URL"] = self.homepage
        tc.generate()

    def build(self):
        cmake = CMake(self)
        cmake.configure()
        cmake.build()
        cmake.test()

    def package(self):
        copy(self, "LICENSE*", src=self.source_folder, dst=os.path.join(self.package_folder, "licenses"))
        CMake(self).install()
        rmdir(self, os.path.join(self.package_folder, "lib", "cmake"))

    def package_info(self):
        self.cpp_info.libs = ["wasafe-vcd"]
        self.cpp_info.set_property("cmake_target_name", "wasafe::wasafe_vcd")
        self.cpp_info.set_property("cmake_file_name", "wasafe_vcd")
        if not self.options.shared:
            self.cpp_info.defines.append("WASAFE_VCD_STATIC_DEFINE")
