# Doxygen API documentation, following the moth_ui setup.
#
# This is opt-in, unlike moth_ui, which always configures it. The stylesheet
# arrives through FetchContent, which needs the network at configure time. Paying
# that on every configure is not worth it when you build docs occasionally.
#
#   cmake --preset conan-relwithdebinfo -DCAMINA_BUILD_DOCS=ON
#   cmake --build --preset conan-relwithdebinfo --target docs
#
# Doxygen runs with EXTRACT_ALL off and WARN_IF_UNDOCUMENTED on, so an
# undocumented public entity is a warning, and FAIL_ON_WARNINGS turns it into an
# error. This keeps the header documentation complete from the start instead of
# letting a backlog build up.

option(CAMINA_BUILD_DOCS "Configure the Doxygen docs target" OFF)

# A docs-only configure skips every dependency, so Doxygen runs without a Conan
# install. CI uses this, and it makes a local documentation check quick:
#
#   cmake -S . -B build-docs -DCAMINA_DOCS_ONLY=ON
#   cmake --build build-docs --target docs
option(CAMINA_DOCS_ONLY "Configure only the docs target, with no dependencies" OFF)

function(camina_add_docs)
    if(NOT CAMINA_BUILD_DOCS)
        add_custom_target(docs
            COMMAND ${CMAKE_COMMAND} -E echo
                "docs target disabled. Reconfigure with -DCAMINA_BUILD_DOCS=ON."
        )
        return()
    endif()

    find_package(Doxygen OPTIONAL_COMPONENTS dot)
    if(NOT DOXYGEN_FOUND)
        add_custom_target(docs
            COMMAND ${CMAKE_COMMAND} -E echo
                "docs target unavailable: doxygen not found. Install doxygen and reconfigure."
        )
        return()
    endif()

    include(FetchContent)
    FetchContent_Declare(
        doxygen-awesome-css
        GIT_REPOSITORY https://github.com/jothepro/doxygen-awesome-css.git
        GIT_TAG v2.4.2
    )
    FetchContent_MakeAvailable(doxygen-awesome-css)

    set(DOXYGEN_PROJECT_NAME "Camina Engine")
    set(DOXYGEN_PROJECT_BRIEF "A small 3D game engine")
    set(DOXYGEN_PROJECT_NUMBER "${PROJECT_VERSION_FULL}")

    set(DOXYGEN_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/docs)
    set(DOXYGEN_RECURSIVE YES)
    set(DOXYGEN_GENERATE_TREEVIEW YES)
    # moth_ui uses AUTO, which Doxygen 1.9.8 rejects. AUTO_LIGHT is the same intent:
    # follow the system preference, and fall back to light.
    set(DOXYGEN_HTML_COLORSTYLE AUTO_LIGHT)
    set(DOXYGEN_HTML_EXTRA_STYLESHEET ${doxygen-awesome-css_SOURCE_DIR}/doxygen-awesome.css)

    # Headers only. The .cpp files hold implementation comments, not API docs.
    set(DOXYGEN_FILE_PATTERNS *.h)
    set(DOXYGEN_EXCLUDE_PATTERNS "*/version.h")

    # Report anything public that carries no documentation, and fail on it.
    set(DOXYGEN_EXTRACT_ALL NO)
    set(DOXYGEN_EXTRACT_PRIVATE NO)
    set(DOXYGEN_EXTRACT_STATIC YES)
    set(DOXYGEN_WARN_IF_UNDOCUMENTED YES)
    set(DOXYGEN_WARN_IF_DOC_ERROR YES)
    set(DOXYGEN_WARN_NO_PARAMDOC YES)
    set(DOXYGEN_WARN_AS_ERROR FAIL_ON_WARNINGS)

    set(DOXYGEN_MACRO_EXPANSION YES)
    set(DOXYGEN_PREDEFINED "ENGINE_ENABLE_ASSERTS")

    doxygen_add_docs(docs
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        COMMENT "Generating API documentation"
    )
endfunction()
