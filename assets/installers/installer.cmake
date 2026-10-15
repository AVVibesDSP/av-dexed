set(DIST_DIR ${CMAKE_BINARY_DIR}/dist)
file(MAKE_DIRECTORY ${DIST_DIR})

add_custom_target(dist)
add_custom_target(installer)

function(package format)
    get_target_property(output_dir ${PROJECT_NAME} RUNTIME_OUTPUT_DIRECTORY)

    if(TARGET ${PROJECT_NAME}_${format})
        add_dependencies(dist ${PROJECT_NAME}_${format})
        add_custom_command(
                TARGET dist
                POST_BUILD
                WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
                COMMAND echo "Installing ${output_dir}/${format} to ${DIST_DIR}"
                COMMAND ${CMAKE_COMMAND} -E copy_directory ${output_dir}/${format}/ ${DIST_DIR}/
        )
    endif()
endfunction()

package(VST3)
package(Standalone)
package(CLAP)
if (APPLE)
    package(AU)
endif()

if (WIN32)
    # The VST3 and CLAP folders also hold the linker's import library (.lib, .exp): not for users.
    get_target_property(product_name ${PROJECT_NAME} JUCE_PRODUCT_NAME)
    add_custom_command(
            TARGET dist
            POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E rm -f "${DIST_DIR}/${product_name}.lib" "${DIST_DIR}/${product_name}.exp"
            VERBATIM
    )
endif()

add_dependencies(installer dist)

if (APPLE)
    set(ARCH_NAME "macOS")
elseif (WIN32)
    set(ARCH_NAME "win")
else()
    set(ARCH_NAME "lnx")
endif()


get_property(multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if (WIN32 AND multi_config)
    # Visual Studio picks the configuration when building (--config Release), not with CMAKE_BUILD_TYPE.
    set(VERSION_NAME "$<IF:$<CONFIG:Release>,${PROJECT_VERSION},${PROJECT_VERSION}-NIGHTLY-${BUILD_ID}>")
elseif(CMAKE_BUILD_TYPE STREQUAL "Release")
    set(VERSION_NAME ${PROJECT_VERSION})
else()
    set(VERSION_NAME ${PROJECT_VERSION}-NIGHTLY-${BUILD_ID})
endif()

set(PACKAGE_NAME ${PROJECT_NAME}-${VERSION_NAME}-${ARCH_NAME})

add_custom_command(
        TARGET installer
        POST_BUILD
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_SOURCE_DIR}/LICENSE ${DIST_DIR}/
)

add_custom_command(
        TARGET installer
        POST_BUILD
        WORKING_DIRECTORY ${DIST_DIR}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/installer
        COMMAND ${CMAKE_COMMAND} -E tar cvf ${CMAKE_BINARY_DIR}/installer/${PACKAGE_NAME}.zip --format=zip .
        COMMAND ${CMAKE_COMMAND} -E echo "Artifact in: installer/${PACKAGE_NAME}.zip")

if (APPLE)
    message(STATUS "Configuring for mac installer")
    add_custom_command(
            TARGET installer
            POST_BUILD
            USES_TERMINAL
            WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
            COMMAND ${CMAKE_COMMAND} -E make_directory installer
            COMMAND ${CMAKE_SOURCE_DIR}/assets/installers/make_macos_pkg.sh ${PROJECT_NAME} ${DIST_DIR} ${PROJECT_VERSION} ${CMAKE_BINARY_DIR}/installer ${PACKAGE_NAME}
    )
elseif (WIN32)
    message(STATUS "Configuring for windows installer")
    find_program(INNOSETUP_COMPILER_EXECUTABLE iscc
            PATHS "C:/Program Files (x86)/Inno Setup 6" "$ENV{LOCALAPPDATA}/Programs/Inno Setup 6")

    if(
            NOT INNOSETUP_COMPILER_EXECUTABLE
            OR "${INNOSETUP_COMPILER_EXECUTABLE}" MATCHES "NOTFOUND"
            OR NOT EXISTS "${INNOSETUP_COMPILER_EXECUTABLE}"
    )
        message(STATUS "Inno Setup compiler not found")
    else()
        message(
                STATUS
                "Inno Setup compiler found: ${INNOSETUP_COMPILER_EXECUTABLE}"
        )
        add_executable(innosetup::compiler IMPORTED GLOBAL)

        set_target_properties(
                innosetup::compiler
                PROPERTIES
                IMPORTED_LOCATION "${INNOSETUP_COMPILER_EXECUTABLE}"
                INSTALL_SCRIPT "${CMAKE_SOURCE_DIR}/assets/installers/windows/installer.iss"
        )

        # Spanish licence page: the unofficial summary, then the official English text (as on macOS).
        set(license_es "${CMAKE_BINARY_DIR}/installer-license-es.txt")
        file(READ "${CMAKE_SOURCE_DIR}/packaging/macos/resources/license-es.txt" license_es_summary)
        file(READ "${CMAKE_SOURCE_DIR}/LICENSE" license_gpl)
        file(WRITE "${license_es}" "${license_es_summary}${license_gpl}")

        get_target_property(generated_sources ${PROJECT_NAME} JUCE_GENERATED_SOURCES_DIRECTORY)
        set(icon_arg "")
        if (EXISTS "${generated_sources}/icon.ico")
            set(icon_arg "/DIcon=${generated_sources}/icon.ico")
        endif()

        # The files are named after the product ("AV Dexed.vst3"), the setup file after the project.
        add_custom_command(
                TARGET installer
                POST_BUILD
                WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
                COMMAND ${CMAKE_COMMAND} -E make_directory installer
                COMMAND innosetup::compiler
                "/O${CMAKE_BINARY_DIR}/installer" "/DName=${product_name}"
                "/DNameCondensed=${PROJECT_NAME}" "/DVersion=${VERSION_NAME}"
                /DVST3 /DSA /DCLAP
                "/DLicense=${CMAKE_SOURCE_DIR}/LICENSE"
                "/DLicenseEs=${license_es}"
                ${icon_arg}
                "/DStagedAssets=${DIST_DIR}"
                "$<TARGET_PROPERTY:innosetup::compiler,INSTALL_SCRIPT>"
                VERBATIM
        )
    endif()
endif()
