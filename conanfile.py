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
        "with_editor": False,   # M9
        "with_ui": False,       # M10, and the M6 spike
        "with_lua": True,       # M8. On, because the sandbox game runs on it
        "with_audio": True,     # M11. On, because the sandbox game plays sound

        # HarfBuzz takes glib by default, and glib drags elfutils, gettext,
        # libiconv, pcre2 and libffi in behind it. The engine calls no glib
        # function, so this turns it off. With it off, with_ui=True adds
        # exactly freetype, harfbuzz, libpng, brotli and bzip2. glib is also
        # LGPL, which every other package here is not. Conan reads this line
        # even when with_ui is False, and that costs nothing, because nothing
        # requires HarfBuzz then.
        "harfbuzz/*:with_glib": False,
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
        # branch is the one the M9 editor wants, so take it now and do not
        # change the version later.
        self.requires("imgui/1.92.8-docking")
        self.requires("nlohmann_json/3.12.0")

        # M3. The scene lives in an EnTT registry. See DESIGN.md section 10.
        self.requires("entt/3.16.0")

        # M4.3. tools/cooker reads a PNG with this. Nothing in src/ includes it,
        # because a cooked texture needs no decoder at run time. It is a header
        # library, so it costs the runtime nothing to have it in the graph.
        self.requires("stb/cci.20240531")

        # M4.4. tools/cooker reads glTF with cgltf and reorders the indices with
        # meshoptimizer. Both are cooker only, for the same reason as stb: a
        # cooked mesh needs no importer at run time. DESIGN.md section 5 rejects
        # assimp, so glTF is the only import format.
        self.requires("cgltf/1.15")
        self.requires("meshoptimizer/0.25")

        # M5.1. tools/cooker reflects the SPIR-V that glslc wrote, and stores the
        # descriptor layout in the cooked shader. The runtime reads that layout
        # and never links this, so the shipping binary carries no reflection.
        # See DESIGN.md section 9 "Shader pipeline".
        #
        # The version tracks volk above, because both follow the Vulkan SDK.
        # The version tracks the spirv-headers that shaderc brings through
        # spirv-tools, so the two agree.
        self.requires("spirv-reflect/1.4.313.0")

        # shaderc was a build tool in M1–M5.1, when the cooker spawned glslc as a
        # subprocess. It became a library in #43 so that a compile is a function
        # call rather than a process spawn. Only the cooker links it.
        self.requires("shaderc/2025.3")

        # ImGuizmo is not here on purpose. Every recipe on Conan Center requires
        # imgui/1.90.5, which conflicts with the docking branch above, so asking
        # for it stopped with_editor=True from resolving at all. It is a
        # submodule under third_party/ instead, and it compiles against 1.92
        # with no patch. See issue #308, rule 4.4, and DESIGN.md section 5.

        if self.options.with_lua:
            # sol2/3.5.0 requires lua/5.4.6, and it is the newest sol2 on Conan
            # Center. Asking for any other Lua gives a version conflict and the
            # graph will not resolve at all, so these two move together. This
            # asked for lua/5.4.8 until M8.1, which nothing noticed because the
            # option had never been turned on.
            self.requires("lua/5.4.6")
            self.requires("sol2/3.5.0")

        if self.options.with_audio:
            self.requires("miniaudio/0.11.22")

        if self.options.with_ui:
            # M6. moth_ui is not on Conan Center. Take it from the Artifactory
            # remote, or export it to the local cache first:
            #   conan create . --version=<v>   (from the moth_ui checkout)
            #
            # 1.1.3 is the first release this engine can build against on both
            # platforms. Before 1.1.2 moth_ui pinned nlohmann_json to a patch
            # line that conflicts with the 3.12 this engine takes, and its
            # logger failed to compile against fmt 12.2. 1.1.2 still ran
            # clang-tidy on an MSVC build, which cannot read that command line,
            # so building it from source failed every Windows CI job.
            #
            # Develop against a Conan editable when a change spans both
            # repositories, then release moth_ui and come back to this pin. A
            # build that needs an editable is a build nobody else can run. See
            # DESIGN.md section 8.5.
            # M10.1 needs moth_ui::AssetId, which 1.8.0 is the first release to
            # carry. M10.6 needs moth_ui::ILayoutProvider, which is 1.9.0: a
            # sub-layout reference reads its target off the filesystem without
            # one, and this engine hands moth_ui bytes rather than a directory.
            # The ceiling stays below 2, because the 2.x line is a separate fork
            # for a larger toolkit and not the next version of this one. See
            # DESIGN.md section 8.5.
            self.requires("moth_ui/[>=1.9 <2]")

            # M6.4. moth_ui::IFont declares no methods, so the backend owns
            # glyph rasterization, atlas packing, measurement, line breaking
            # and both alignments. FreeType rasterizes and HarfBuzz shapes.
            #
            # stb_truetype was the other option, and the engine already takes
            # stb. It was rejected because its shaping is partial: GPOS pair
            # kerning only, no ligatures and no complex scripts. M6 is a
            # diagnostic spike, so text that measures differently from
            # moth_editor would make every layout difference ambiguous. See
            # DESIGN.md section 8.3.
            #
            # HarfBuzz is the version moth_graphics pins, on purpose. A shaping
            # difference between the two projects must come from our code and
            # not from a library version. Bump this when moth_graphics bumps.
            self.requires("harfbuzz/[~8.3]")

            # FreeType is an exact pin rather than a range, because harfbuzz
            # 8.3.0 requires exactly 2.13.2. A range here resolves to the newest
            # 2.13 the remote carries and then conflicts with that.
            #
            # A warm local cache hides this. Conan resolves a range from the
            # cache first, so a machine that already holds 2.13.2 picks it and
            # the graph agrees. CI starts empty, finds 2.13.3, and fails the
            # install. Check a version change here against an empty cache, or
            # with `conan graph info --update`.
            #
            # Bump this together with harfbuzz, to whatever the new harfbuzz
            # requires.
            self.requires("freetype/2.13.2")

    def build_requirements(self):
        pass

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
