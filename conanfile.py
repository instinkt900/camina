from conan import ConanFile
from conan.tools.cmake import CMakeToolchain, CMakeDeps, CMake, cmake_layout
from conan.tools.files import load


class CaminaConan(ConanFile):
    # The package is called camina. The C++ namespace stays "engine" on purpose.
    # See DESIGN.md section 2.1.
    name = "camina"
    license = "MIT"
    description = "Camina Engine, a small 3D game engine"

    settings = "os", "compiler", "build_type", "arch"

    def set_version(self):
        # version.txt at the repository root is the single source of the version.
        # CMakeLists.txt reads the same file. Changing it and pushing to main
        # tags the release. See .github/workflows/release.yml.
        if not self.version:
            self.version = load(self, "version.txt").strip()

    options = {
        "with_editor": [True, False],
        "with_ui": [True, False],
        "with_lua": [True, False],
        "with_audio": [True, False],
    }

    # Every optional subsystem starts off. Each one turns on at its milestone.
    # See DESIGN.md section 10.
    default_options = {
        "with_editor": False,   # M8
        "with_ui": False,       # M9, and the M5.5 spike
        "with_lua": False,      # M7
        "with_audio": False,    # M10
    }

    def requirements(self):
        # M0
        self.requires("sdl/3.4.8")
        self.requires("spdlog/1.17.0")
        self.requires("glm/1.0.3")
        self.requires("enkits/1.12")
        self.requires("tracy/0.13.1")

        # M1. Added now so that M1 needs no change to this file.
        self.requires("volk/1.4.350.0")
        self.requires("vulkan-memory-allocator/3.3.0")

        # M2. Both consumers of the reflection descriptors. See DESIGN.md
        # section 7.
        #
        # ImGui is not gated behind with_editor. Hard rule 4.3 says the editor is
        # an application, not a build mode, and the inspector runs as a debug
        # overlay in the runtime long before apps/editor exists. The docking
        # branch is the one the M8 editor wants, so take it now and do not
        # change the version later.
        self.requires("imgui/1.92.8-docking")
        self.requires("nlohmann_json/3.12.0")

        if self.options.with_editor:
            self.requires("imguizmo/1.83")

        if self.options.with_lua:
            self.requires("lua/5.4.8")
            self.requires("sol2/3.5.0")

        if self.options.with_audio:
            self.requires("miniaudio/0.11.22")

        if self.options.with_ui:
            # moth_ui is not on Conan Center. Export it to the local cache first:
            #   conan create . --version=<v>   (from the moth_ui checkout)
            self.requires("moth_ui/[>=0.1]")

    def build_requirements(self):
        # shaderc is a build tool here, not a library. The engine compiles GLSL to
        # SPIR-V during the build and embeds the result, so the runtime carries no
        # shader compiler. See cmake/Shaders.cmake and DESIGN.md section 2.
        self.tool_requires("shaderc/2025.3")

    def layout(self):
        cmake_layout(self)

    def generate(self):
        deps = CMakeDeps(self)
        deps.generate()
        tc = CMakeToolchain(self)
        tc.variables["ENGINE_WITH_EDITOR"] = bool(self.options.with_editor)
        tc.variables["ENGINE_WITH_UI"] = bool(self.options.with_ui)
        tc.variables["ENGINE_WITH_LUA"] = bool(self.options.with_lua)
        tc.variables["ENGINE_WITH_AUDIO"] = bool(self.options.with_audio)
        tc.generate()

    def build(self):
        cmake = CMake(self)
        cmake.configure()
        cmake.build()
