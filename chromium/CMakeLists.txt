cmake_minimum_required(VERSION 3.25)
project(YOBROChromium VERSION 0.6.3 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)

include(CTest)
include(cmake/Options.cmake)
find_package(Threads REQUIRED)

add_library(yobro_engine_api INTERFACE)
target_include_directories(yobro_engine_api INTERFACE
    "${CMAKE_CURRENT_SOURCE_DIR}"
)
target_compile_features(yobro_engine_api INTERFACE cxx_std_20)

add_library(yobro_chromium_core STATIC
    core/src/Json.cpp
    core/src/ProfilePaths.cpp
    core/src/WebKitImport.cpp
)
target_include_directories(yobro_chromium_core PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/core/include"
)
target_compile_features(yobro_chromium_core PUBLIC cxx_std_20)

add_library(yobro_protocol_v2 STATIC
    controller/src/BrowserSession.cpp
    controller/src/ProtocolV2Controller.cpp
)
target_include_directories(yobro_protocol_v2 PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/controller/include"
)
target_link_libraries(yobro_protocol_v2 PUBLIC
    yobro_chromium_core
    yobro_engine_api
)
target_compile_features(yobro_protocol_v2 PUBLIC cxx_std_20)

if(UNIX)
    add_library(yobro_local_transport STATIC
        transport/src/UnixSocketServer.cpp
    )
    target_include_directories(yobro_local_transport PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/transport/include"
    )
    target_link_libraries(yobro_local_transport PUBLIC
        Threads::Threads
    )
    target_compile_features(yobro_local_transport PUBLIC cxx_std_20)
endif()

if(MSVC)
    target_compile_options(yobro_chromium_core PRIVATE /W4 /permissive-)
    target_compile_options(yobro_protocol_v2 PRIVATE /W4 /permissive-)
elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(yobro_chromium_core PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(yobro_protocol_v2 PRIVATE -Wall -Wextra -Wpedantic)
    if(TARGET yobro_local_transport)
        target_compile_options(yobro_local_transport PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endif()

if(BUILD_TESTING)
    add_executable(yobro-webkit-import-tests
        tests/core/WebKitImportTests.cpp
    )
    target_link_libraries(yobro-webkit-import-tests PRIVATE yobro_chromium_core)
    add_test(NAME chromium-webkit-import COMMAND yobro-webkit-import-tests)
    set_tests_properties(chromium-webkit-import PROPERTIES TIMEOUT 30)

    add_executable(yobro-protocol-v2-tests
        tests/protocol/ProtocolV2ControllerTests.cpp
    )
    target_link_libraries(yobro-protocol-v2-tests PRIVATE
        yobro_protocol_v2
        yobro_engine_api
    )
    add_test(NAME chromium-protocol-v2-controller COMMAND yobro-protocol-v2-tests)
    set_tests_properties(chromium-protocol-v2-controller PROPERTIES TIMEOUT 30)

    if(UNIX)
        add_executable(yobro-unix-socket-tests
            tests/transport/UnixSocketServerTests.cpp
        )
        target_link_libraries(yobro-unix-socket-tests PRIVATE
            yobro_protocol_v2
            yobro_local_transport
        )
        add_test(NAME chromium-protocol-v2-unix-socket COMMAND yobro-unix-socket-tests)
        set_tests_properties(chromium-protocol-v2-unix-socket PROPERTIES TIMEOUT 30)
    endif()
endif()

if(YOBRO_BUILD_QT_SPIKE)
    find_package(Qt6 ${YOBRO_QT_VERSION} EXACT REQUIRED COMPONENTS
        Core Gui Network PrintSupport Test WebChannel Widgets WebEngineCore WebEngineWidgets
    )
    qt_standard_project_setup()

    set(AGENT_BRIDGE_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/../Sources/YOBRO/Resources/AgentBridge.js")
    if(NOT EXISTS "${AGENT_BRIDGE_SOURCE}")
        message(FATAL_ERROR "Shared AgentBridge.js is missing: ${AGENT_BRIDGE_SOURCE}")
    endif()
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/resources")
    configure_file("${AGENT_BRIDGE_SOURCE}" "${CMAKE_CURRENT_BINARY_DIR}/resources/AgentBridge.js" COPYONLY)
    set(AGENT_BRIDGE_RESOURCE "${CMAKE_CURRENT_BINARY_DIR}/resources/AgentBridge.js")
    set_source_files_properties("${AGENT_BRIDGE_RESOURCE}" PROPERTIES QT_RESOURCE_ALIAS "AgentBridge.js")

    set(PASSKEY_SHIM_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/spike/PasskeyShim.js")
    if(NOT EXISTS "${PASSKEY_SHIM_SOURCE}")
        message(FATAL_ERROR "PasskeyShim.js is missing: ${PASSKEY_SHIM_SOURCE}")
    endif()
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/resources")
    configure_file("${PASSKEY_SHIM_SOURCE}" "${CMAKE_CURRENT_BINARY_DIR}/resources/PasskeyShim.js" COPYONLY)
    set(PASSKEY_SHIM_RESOURCE "${CMAKE_CURRENT_BINARY_DIR}/resources/PasskeyShim.js")
    set_source_files_properties("${PASSKEY_SHIM_RESOURCE}" PROPERTIES QT_RESOURCE_ALIAS "PasskeyShim.js")

    set(LOGIN_AUTOFILL_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/spike/LoginAutofill.js")
    if(NOT EXISTS "${LOGIN_AUTOFILL_SOURCE}")
        message(FATAL_ERROR "LoginAutofill.js is missing: ${LOGIN_AUTOFILL_SOURCE}")
    endif()
    configure_file("${LOGIN_AUTOFILL_SOURCE}" "${CMAKE_CURRENT_BINARY_DIR}/resources/LoginAutofill.js" COPYONLY)
    set(LOGIN_AUTOFILL_RESOURCE "${CMAKE_CURRENT_BINARY_DIR}/resources/LoginAutofill.js")
    set_source_files_properties("${LOGIN_AUTOFILL_RESOURCE}" PROPERTIES QT_RESOURCE_ALIAS "LoginAutofill.js")

    # Translations are shared with the WebKit build so both engines show the
    # same English wording.
    set(ENGLISH_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/../Sources/YOBRO/Resources/English.json")
    if(NOT EXISTS "${ENGLISH_SOURCE}")
        message(FATAL_ERROR "Shared English.json is missing: ${ENGLISH_SOURCE}")
    endif()
    configure_file("${ENGLISH_SOURCE}" "${CMAKE_CURRENT_BINARY_DIR}/resources/English.json" COPYONLY)
    set(ENGLISH_RESOURCE "${CMAKE_CURRENT_BINARY_DIR}/resources/English.json")
    set_source_files_properties("${ENGLISH_RESOURCE}" PROPERTIES QT_RESOURCE_ALIAS "English.json")
    configure_file("${CMAKE_CURRENT_SOURCE_DIR}/spike/EnglishChromium.json"
                   "${CMAKE_CURRENT_BINARY_DIR}/resources/EnglishChromium.json" COPYONLY)
    set(ENGLISH_CHROMIUM_RESOURCE "${CMAKE_CURRENT_BINARY_DIR}/resources/EnglishChromium.json")
    set_source_files_properties("${ENGLISH_CHROMIUM_RESOURCE}" PROPERTIES QT_RESOURCE_ALIAS "EnglishChromium.json")
    set(LOCALIZATION_RESOURCES "${ENGLISH_RESOURCE}" "${ENGLISH_CHROMIUM_RESOURCE}")

    # The website appearance engine is shared with the WebKit build so both
    # engines darken pages the same way.
    set(APPEARANCE_RESOURCES)
    foreach(appearance_script IN ITEMS "DarkReader.js" "WebAppearance.js")
        set(appearance_source "${CMAKE_CURRENT_SOURCE_DIR}/../Sources/YOBRO/Resources/${appearance_script}")
        if(NOT EXISTS "${appearance_source}")
            message(FATAL_ERROR "Shared appearance script is missing: ${appearance_source}")
        endif()
        configure_file("${appearance_source}" "${CMAKE_CURRENT_BINARY_DIR}/resources/${appearance_script}" COPYONLY)
        set(appearance_resource "${CMAKE_CURRENT_BINARY_DIR}/resources/${appearance_script}")
        set_source_files_properties("${appearance_resource}" PROPERTIES QT_RESOURCE_ALIAS "${appearance_script}")
        list(APPEND APPEARANCE_RESOURCES "${appearance_resource}")
    endforeach()

    # The cosmetic and page-level ad filters are shared with the WebKit build so
    # both engines hide the same elements. Only the network rules are reimplemented.
    set(ADBLOCK_RESOURCES)
    foreach(adblock_script IN ITEMS "AdBlocker.js" "AdBlockerPage.js")
        set(adblock_source "${CMAKE_CURRENT_SOURCE_DIR}/../Sources/YOBRO/Resources/${adblock_script}")
        if(NOT EXISTS "${adblock_source}")
            message(FATAL_ERROR "Shared ad-block script is missing: ${adblock_source}")
        endif()
        configure_file("${adblock_source}" "${CMAKE_CURRENT_BINARY_DIR}/resources/${adblock_script}" COPYONLY)
        set(adblock_resource "${CMAKE_CURRENT_BINARY_DIR}/resources/${adblock_script}")
        set_source_files_properties("${adblock_resource}" PROPERTIES QT_RESOURCE_ALIAS "${adblock_script}")
        list(APPEND ADBLOCK_RESOURCES "${adblock_resource}")
    endforeach()

    # The import helper is shared with the WebKit build so both engines read the
    # same browser profiles with the same rules.
    set(IMPORT_WORKER_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/../Sources/YOBRO/Resources/BrowserImport.py")
    if(NOT EXISTS "${IMPORT_WORKER_SOURCE}")
        message(FATAL_ERROR "Shared BrowserImport.py is missing: ${IMPORT_WORKER_SOURCE}")
    endif()
    configure_file("${IMPORT_WORKER_SOURCE}" "${CMAKE_CURRENT_BINARY_DIR}/resources/BrowserImport.py" COPYONLY)
    set(IMPORT_WORKER_RESOURCE "${CMAKE_CURRENT_BINARY_DIR}/resources/BrowserImport.py")
    set_source_files_properties("${IMPORT_WORKER_RESOURCE}" PROPERTIES QT_RESOURCE_ALIAS "BrowserImport.py")

    # The IMAP and SMTP helper is shared with the WebKit build too, so both
    # shells talk to a mail provider in exactly the same way.
    set(MAIL_WORKER_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/../Sources/YOBRO/Resources/MailWorker.py")
    if(NOT EXISTS "${MAIL_WORKER_SOURCE}")
        message(FATAL_ERROR "Shared MailWorker.py is missing: ${MAIL_WORKER_SOURCE}")
    endif()
    configure_file("${MAIL_WORKER_SOURCE}" "${CMAKE_CURRENT_BINARY_DIR}/resources/MailWorker.py" COPYONLY)
    set(MAIL_WORKER_RESOURCE "${CMAKE_CURRENT_BINARY_DIR}/resources/MailWorker.py")
    set_source_files_properties("${MAIL_WORKER_RESOURCE}" PROPERTIES QT_RESOURCE_ALIAS "MailWorker.py")

    add_library(yobro_bridge_policy STATIC
        spike/BridgePolicyStore.cpp
        spike/BridgePolicyStore.hpp
        spike/PasswordVault.cpp
        spike/PasswordVault.hpp
        spike/PageZoomStore.cpp
        spike/PageZoomStore.hpp
        spike/WebAppearanceStore.cpp
        spike/WebAppearanceStore.hpp
        spike/Theme.cpp
        spike/Theme.hpp
        spike/Localization.cpp
        spike/Localization.hpp
        spike/ProfileRegistry.cpp
        spike/ProfileRegistry.hpp
        spike/PrivacySettings.cpp
        spike/PrivacySettings.hpp
        spike/AdBlockRules.cpp
        spike/AdBlockRules.hpp
        spike/AdBlockSettings.cpp
        spike/AdBlockSettings.hpp
        spike/CertificateTrust.cpp
        spike/CertificateTrust.hpp
        spike/SpaceProxyStore.cpp
        spike/SpaceProxyStore.hpp
        spike/SpaceProxyController.cpp
        spike/SpaceProxyController.hpp
        spike/ImportWorker.cpp
        spike/ImportWorker.hpp
        spike/NativePasswordImport.cpp
        spike/NativePasswordImport.hpp
        spike/BrowserDataImport.cpp
        spike/BrowserDataImport.hpp
        spike/ChromeStore.cpp
        spike/ChromeStore.hpp
        spike/SpaceChat.cpp
        spike/SpaceChat.hpp
        spike/PythonWorker.cpp
        spike/PythonWorker.hpp
        spike/MailWorker.cpp
        spike/MailWorker.hpp
        spike/MailStore.cpp
        spike/MailStore.hpp
        spike/MailDiscovery.cpp
        spike/MailDiscovery.hpp
        spike/OnboardingProgress.cpp
        spike/OnboardingProgress.hpp
        spike/ChaCha20Poly1305.cpp
        spike/ChaCha20Poly1305.hpp
        spike/KeyDerivation.cpp
        spike/KeyDerivation.hpp
        spike/BrowserSync.cpp
        spike/BrowserSync.hpp
        spike/SyncAccount.cpp
        spike/SyncAccount.hpp
    )
    target_include_directories(yobro_bridge_policy PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}"
    )
    target_link_libraries(yobro_bridge_policy PUBLIC
        yobro_chromium_core
        Qt6::Core
        Qt6::Network
    )
    if(APPLE)
        # Credentials live in the macOS Keychain via Security Services.
        target_link_libraries(yobro_bridge_policy PUBLIC "-framework Security" "-framework CoreFoundation")
    endif()
    target_compile_features(yobro_bridge_policy PUBLIC cxx_std_20)

    # The window is one class spread over several translation units, so every
    # target that builds the shell needs all of its parts. Listing them once
    # keeps a new part from silently missing in one target.
    set(YOBRO_SPIKE_WINDOW_SOURCES
        spike/SpikeWindow.cpp
        spike/SpikeWindowAssistant.cpp
        spike/SpikeWindowDownloads.cpp
        spike/SpikeWindowLayout.cpp
        spike/SpikeWindowLibrary.cpp
        spike/SpikeWindowSession.cpp
        spike/SpikeWindowCredentials.cpp
        spike/SpikeWindowExtensions.cpp
        spike/SpikeWindowImport.cpp
        spike/SpikeWindowOnboarding.cpp
        spike/SpikeWindowPasskeys.cpp
        spike/SpikeWindowProfiles.cpp
        spike/SpikeWindowSettings.cpp
        spike/SpikeWindowSupport.cpp
        spike/SpikeWindowSync.cpp
        spike/SpikeWindowWorkspace.cpp
        spike/SpikeWindowInternal.hpp
    )
    add_library(yobro_qtwebengine STATIC
        engine/qtwebengine/QtBrowserLibrary.cpp
        engine/qtwebengine/QtBrowserLibrary.hpp
        engine/qtwebengine/QtDownloadFilename.cpp
        engine/qtwebengine/QtDownloadFilename.hpp
        engine/qtwebengine/QtBrowserPage.cpp
        engine/qtwebengine/QtBrowserPage.hpp
        engine/qtwebengine/QtBrowserProfile.cpp
        engine/qtwebengine/QtBrowserProfile.hpp
        engine/qtwebengine/QtEventLoop.cpp
        engine/qtwebengine/QtEventLoop.hpp
        spike/AdBlockInterceptor.cpp
        spike/AdBlockInterceptor.hpp
        spike/LoginChannel.cpp
        spike/LoginChannel.hpp
        spike/NoteEditor.cpp
        spike/NoteEditor.hpp
        spike/SpaceChatPanel.cpp
        spike/SpaceChatPanel.hpp
        spike/MailPanel.cpp
        spike/MailPanel.hpp
        spike/OnboardingDialog.cpp
        spike/OnboardingDialog.hpp
    )
    target_include_directories(yobro_qtwebengine PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}"
    )
    target_link_libraries(yobro_qtwebengine PUBLIC
        yobro_protocol_v2
        yobro_engine_api
        yobro_bridge_policy
        Qt6::Core Qt6::Gui Qt6::Network Qt6::PrintSupport Qt6::Widgets
        Qt6::WebChannel Qt6::WebEngineCore Qt6::WebEngineWidgets
    )
    target_compile_features(yobro_qtwebengine PUBLIC cxx_std_20)

    qt_add_executable(yobro-chromium-spike
        spike/main.cpp
        ${YOBRO_SPIKE_WINDOW_SOURCES}
        spike/SpikeWindow.hpp
        spike/SmokeRunner.cpp
        spike/SmokeRunner.hpp
    )
    qt_add_resources(yobro-chromium-spike "yobro_shared_resources"
        PREFIX "/yobro"
        FILES "${AGENT_BRIDGE_RESOURCE}" "${LOGIN_AUTOFILL_RESOURCE}" "${PASSKEY_SHIM_RESOURCE}" ${APPEARANCE_RESOURCES} ${ADBLOCK_RESOURCES} "${IMPORT_WORKER_RESOURCE}" "${MAIL_WORKER_RESOURCE}" ${LOCALIZATION_RESOURCES}
    )
    target_include_directories(yobro-chromium-spike PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}"
    )
    target_link_libraries(yobro-chromium-spike PRIVATE
        yobro_bridge_policy
        yobro_qtwebengine
        yobro_local_transport
    )
    target_compile_definitions(yobro-chromium-spike PRIVATE
        YOBRO_CHROMIUM_VERSION="${PROJECT_VERSION}"
    )

    if(MSVC)
        target_compile_options(yobro_bridge_policy PRIVATE /W4 /permissive-)
        target_compile_options(yobro_qtwebengine PRIVATE /W4 /permissive-)
        target_compile_options(yobro-chromium-spike PRIVATE /W4 /permissive-)
    else()
        target_compile_options(yobro_bridge_policy PRIVATE -Wall -Wextra -Wpedantic)
        target_compile_options(yobro_qtwebengine PRIVATE -Wall -Wextra -Wpedantic)
        target_compile_options(yobro-chromium-spike PRIVATE -Wall -Wextra -Wpedantic)
    endif()

    if(APPLE)
        set_target_properties(yobro-chromium-spike PROPERTIES
            OUTPUT_NAME "YOBRO Chromium Feasibility"
            MACOSX_BUNDLE TRUE
            MACOSX_BUNDLE_GUI_IDENTIFIER "bro.yobro.chromium.feasibility"
            MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
            MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
        )
    elseif(WIN32)
        set_target_properties(yobro-chromium-spike PROPERTIES
            OUTPUT_NAME "YOBRO-Chromium-Feasibility"
            WIN32_EXECUTABLE TRUE
        )
    else()
        set_target_properties(yobro-chromium-spike PROPERTIES
            OUTPUT_NAME "yobro-chromium-feasibility"
        )
    endif()

    if(BUILD_TESTING)
        add_executable(yobro-bridge-policy-tests
            tests/qt/BridgePolicyStoreTests.cpp
        )
        target_include_directories(yobro-bridge-policy-tests PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}"
        )
        target_link_libraries(yobro-bridge-policy-tests PRIVATE
            yobro_bridge_policy
            Qt6::Core
        )
        add_test(
            NAME chromium-bridge-policy
            COMMAND $<TARGET_FILE:yobro-bridge-policy-tests>
        )
        set_tests_properties(chromium-bridge-policy PROPERTIES TIMEOUT 30)

        add_executable(yobro-password-vault-tests
            tests/qt/PasswordVaultTests.cpp
        )
        target_include_directories(yobro-password-vault-tests PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}"
        )
        target_link_libraries(yobro-password-vault-tests PRIVATE
            yobro_bridge_policy
            Qt6::Core
        )
        add_test(
            NAME chromium-password-vault
            COMMAND $<TARGET_FILE:yobro-password-vault-tests>
        )
        set_tests_properties(chromium-password-vault PROPERTIES TIMEOUT 60)

        add_test(
            NAME chromium-phase1b-smoke
            COMMAND $<TARGET_FILE:yobro-chromium-spike> --smoke
        )
        set_tests_properties(chromium-phase1b-smoke PROPERTIES
            ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
            TIMEOUT 240
        )

        add_executable(yobro-qt-webkit-import-tests
            tests/qt/QtWebKitImportIntegrationTests.cpp
            ${YOBRO_SPIKE_WINDOW_SOURCES}
            spike/SpikeWindow.hpp
        )
        qt_add_resources(yobro-qt-webkit-import-tests "yobro_import_test_resources"
            PREFIX "/yobro"
            FILES "${AGENT_BRIDGE_RESOURCE}" "${LOGIN_AUTOFILL_RESOURCE}" "${PASSKEY_SHIM_RESOURCE}" ${APPEARANCE_RESOURCES} ${ADBLOCK_RESOURCES} "${IMPORT_WORKER_RESOURCE}" "${MAIL_WORKER_RESOURCE}" ${LOCALIZATION_RESOURCES}
        )
        target_include_directories(yobro-qt-webkit-import-tests PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}"
        )
        target_link_libraries(yobro-qt-webkit-import-tests PRIVATE
            yobro_bridge_policy
            yobro_qtwebengine
            Qt6::Test
        )
        add_test(
            NAME chromium-webkit-import-ui
            COMMAND $<TARGET_FILE:yobro-qt-webkit-import-tests>
        )
        set_tests_properties(chromium-webkit-import-ui PROPERTIES
            ENVIRONMENT "YOBRO_LANGUAGE=de;QT_QPA_PLATFORM=offscreen;QTWEBENGINE_CHROMIUM_FLAGS=--disable-gpu"
            TIMEOUT 120
        )

        add_executable(yobro-qt-bookmark-tests
            tests/qt/QtBookmarkTests.cpp
        )
        target_include_directories(yobro-qt-bookmark-tests PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}"
        )
        target_link_libraries(yobro-qt-bookmark-tests PRIVATE
            yobro_qtwebengine
            Qt6::Test
        )
        add_test(
            NAME chromium-bookmarks
            COMMAND $<TARGET_FILE:yobro-qt-bookmark-tests>
        )
        set_tests_properties(chromium-bookmarks PROPERTIES
            ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QTWEBENGINE_CHROMIUM_FLAGS=--disable-gpu"
            TIMEOUT 90
        )

        qt_add_executable(yobro-chrome-store-tests
            tests/qt/ChromeStoreTests.cpp
        )
        qt_add_resources(yobro-chrome-store-tests "yobro_chrome_store_test_resources"
            PREFIX "/yobro"
            FILES ${LOCALIZATION_RESOURCES}
        )
        target_include_directories(yobro-chrome-store-tests PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}"
        )
        target_link_libraries(yobro-chrome-store-tests PRIVATE
            yobro_bridge_policy
            Qt6::Core Qt6::Network
        )
        add_test(
            NAME chromium-chrome-store
            COMMAND $<TARGET_FILE:yobro-chrome-store-tests>
        )
        set_tests_properties(chromium-chrome-store PROPERTIES
            ENVIRONMENT "YOBRO_LANGUAGE=de"
            TIMEOUT 120
        )

        qt_add_executable(yobro-sync-tests
            tests/qt/SyncTests.cpp
        )
        qt_add_resources(yobro-sync-tests "yobro_sync_test_resources"
            PREFIX "/yobro"
            FILES ${LOCALIZATION_RESOURCES}
        )
        target_include_directories(yobro-sync-tests PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}"
        )
        target_link_libraries(yobro-sync-tests PRIVATE
            yobro_bridge_policy
            Qt6::Core Qt6::Network
        )
        add_test(
            NAME chromium-sync
            COMMAND $<TARGET_FILE:yobro-sync-tests>
        )
        set_tests_properties(chromium-sync PROPERTIES
            ENVIRONMENT "YOBRO_LANGUAGE=de"
            TIMEOUT 120
        )

        qt_add_executable(yobro-mail-tests
            tests/qt/MailTests.cpp
        )
        qt_add_resources(yobro-mail-tests "yobro_mail_test_resources"
            PREFIX "/yobro"
            FILES "${MAIL_WORKER_RESOURCE}" ${LOCALIZATION_RESOURCES}
        )
        target_include_directories(yobro-mail-tests PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}"
        )
        target_compile_definitions(yobro-mail-tests PRIVATE
            YOBRO_WEBKIT_MAIL_SCRIPT="${MAIL_WORKER_SOURCE}"
        )
        target_link_libraries(yobro-mail-tests PRIVATE
            yobro_bridge_policy
            Qt6::Core Qt6::Network
        )
        add_test(
            NAME chromium-mail
            COMMAND $<TARGET_FILE:yobro-mail-tests>
        )
        set_tests_properties(chromium-mail PROPERTIES
            ENVIRONMENT "YOBRO_LANGUAGE=de"
            TIMEOUT 120
        )

        qt_add_executable(yobro-space-chat-tests
            tests/qt/SpaceChatTests.cpp
        )
        qt_add_resources(yobro-space-chat-tests "yobro_space_chat_test_resources"
            PREFIX "/yobro"
            FILES ${LOCALIZATION_RESOURCES}
        )
        target_include_directories(yobro-space-chat-tests PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}"
        )
        target_compile_definitions(yobro-space-chat-tests PRIVATE
            YOBRO_WEBKIT_SPACE_CHAT_SOURCE="${CMAKE_CURRENT_SOURCE_DIR}/../Sources/YOBRO/SpaceChat.swift"
        )
        target_link_libraries(yobro-space-chat-tests PRIVATE
            yobro_bridge_policy
            Qt6::Core Qt6::Network
        )
        add_test(
            NAME chromium-space-chat
            COMMAND $<TARGET_FILE:yobro-space-chat-tests>
        )
        set_tests_properties(chromium-space-chat PROPERTIES
            ENVIRONMENT "YOBRO_LANGUAGE=de"
            TIMEOUT 120
        )

        qt_add_executable(yobro-browser-data-import-tests
            tests/qt/BrowserDataImportTests.cpp
        )
        qt_add_resources(yobro-browser-data-import-tests "yobro_browser_data_import_test_resources"
            PREFIX "/yobro"
            FILES "${IMPORT_WORKER_RESOURCE}" "${MAIL_WORKER_RESOURCE}" ${LOCALIZATION_RESOURCES}
        )
        target_include_directories(yobro-browser-data-import-tests PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}"
        )
        target_link_libraries(yobro-browser-data-import-tests PRIVATE
            yobro_bridge_policy
            Qt6::Core
        )
        add_test(
            NAME chromium-browser-data-import
            COMMAND $<TARGET_FILE:yobro-browser-data-import-tests>
        )
        set_tests_properties(chromium-browser-data-import PROPERTIES
            ENVIRONMENT "YOBRO_LANGUAGE=de"
            TIMEOUT 120
        )

        qt_add_executable(yobro-password-import-tests
            tests/qt/PasswordImportTests.cpp
        )
        qt_add_resources(yobro-password-import-tests "yobro_password_import_test_resources"
            PREFIX "/yobro"
            FILES "${IMPORT_WORKER_RESOURCE}" "${MAIL_WORKER_RESOURCE}" ${LOCALIZATION_RESOURCES}
        )
        target_include_directories(yobro-password-import-tests PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}"
        )
        target_compile_definitions(yobro-password-import-tests PRIVATE
            YOBRO_WEBKIT_IMPORT_SCRIPT="${IMPORT_WORKER_SOURCE}"
        )
        target_link_libraries(yobro-password-import-tests PRIVATE
            yobro_bridge_policy
            Qt6::Core
        )
        add_test(
            NAME chromium-password-import
            COMMAND $<TARGET_FILE:yobro-password-import-tests>
        )
        set_tests_properties(chromium-password-import PROPERTIES
            ENVIRONMENT "YOBRO_LANGUAGE=de"
            TIMEOUT 120
        )

        add_executable(yobro-adblock-tests
            tests/qt/AdBlockTests.cpp
        )
        target_include_directories(yobro-adblock-tests PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}"
        )
        target_compile_definitions(yobro-adblock-tests PRIVATE
            YOBRO_WEBKIT_ADBLOCK_SOURCE="${CMAKE_CURRENT_SOURCE_DIR}/../Sources/YOBRO/AdBlocker.swift"
        )
        target_link_libraries(yobro-adblock-tests PRIVATE
            yobro_bridge_policy
            Qt6::Core
        )
        add_test(
            NAME chromium-adblock
            COMMAND $<TARGET_FILE:yobro-adblock-tests>
        )
        set_tests_properties(chromium-adblock PROPERTIES TIMEOUT 60)

        add_executable(yobro-qt-space-proxy-tests
            tests/qt/QtSpaceProxyIntegrationTests.cpp
        )
        target_include_directories(yobro-qt-space-proxy-tests PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}"
        )
        target_link_libraries(yobro-qt-space-proxy-tests PRIVATE
            yobro_qtwebengine
            Qt6::Test
        )
        add_test(
            NAME chromium-space-proxy-network
            COMMAND $<TARGET_FILE:yobro-qt-space-proxy-tests>
        )
        set_tests_properties(chromium-space-proxy-network PROPERTIES
            ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QTWEBENGINE_CHROMIUM_FLAGS=--disable-gpu"
            TIMEOUT 120
        )

        add_executable(yobro-qt-certificate-tests
            tests/qt/QtCertificateIntegrationTests.cpp
        )
        target_include_directories(yobro-qt-certificate-tests PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}"
        )
        target_link_libraries(yobro-qt-certificate-tests PRIVATE
            yobro_qtwebengine
            Qt6::Test
        )
        add_test(
            NAME chromium-certificate-trust
            COMMAND $<TARGET_FILE:yobro-qt-certificate-tests>
        )
        set_tests_properties(chromium-certificate-trust PROPERTIES
            ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QTWEBENGINE_CHROMIUM_FLAGS=--disable-gpu"
            TIMEOUT 120
        )

        add_executable(yobro-qt-adblock-tests
            tests/qt/QtAdBlockIntegrationTests.cpp
        )
        target_include_directories(yobro-qt-adblock-tests PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}"
        )
        target_link_libraries(yobro-qt-adblock-tests PRIVATE
            yobro_qtwebengine
            Qt6::Test
        )
        add_test(
            NAME chromium-adblock-network
            COMMAND $<TARGET_FILE:yobro-qt-adblock-tests>
        )
        set_tests_properties(chromium-adblock-network PROPERTIES
            ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QTWEBENGINE_CHROMIUM_FLAGS=--disable-gpu"
            TIMEOUT 120
        )

        add_executable(yobro-qt-privacy-tests
            tests/qt/QtPrivacyTests.cpp
        )
        target_include_directories(yobro-qt-privacy-tests PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}"
        )
        target_link_libraries(yobro-qt-privacy-tests PRIVATE
            yobro_qtwebengine
            yobro_bridge_policy
            Qt6::Test
        )
        add_test(
            NAME chromium-privacy
            COMMAND $<TARGET_FILE:yobro-qt-privacy-tests>
        )
        set_tests_properties(chromium-privacy PROPERTIES
            ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QTWEBENGINE_CHROMIUM_FLAGS=--disable-gpu"
            TIMEOUT 90
        )

        add_executable(yobro-profile-registry-tests
            tests/qt/ProfileRegistryTests.cpp
        )
        target_include_directories(yobro-profile-registry-tests PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}"
        )
        target_link_libraries(yobro-profile-registry-tests PRIVATE
            yobro_bridge_policy
            Qt6::Core
        )
        add_test(
            NAME chromium-profile-registry
            COMMAND $<TARGET_FILE:yobro-profile-registry-tests>
        )
        set_tests_properties(chromium-profile-registry PROPERTIES TIMEOUT 60)

        qt_add_executable(yobro-localization-tests
            tests/qt/LocalizationTests.cpp
        )
        qt_add_resources(yobro-localization-tests "yobro_localization_test_resources"
            PREFIX "/yobro"
            FILES ${LOCALIZATION_RESOURCES}
        )
        target_include_directories(yobro-localization-tests PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}"
        )
        target_link_libraries(yobro-localization-tests PRIVATE
            yobro_bridge_policy
            Qt6::Core
        )
        add_test(
            NAME chromium-localization-german
            COMMAND $<TARGET_FILE:yobro-localization-tests> de
        )
        set_tests_properties(chromium-localization-german PROPERTIES
            ENVIRONMENT "YOBRO_LANGUAGE=de"
            TIMEOUT 60
        )
        add_test(
            NAME chromium-localization-english
            COMMAND $<TARGET_FILE:yobro-localization-tests> en
        )
        set_tests_properties(chromium-localization-english PROPERTIES
            ENVIRONMENT "YOBRO_LANGUAGE=en"
            TIMEOUT 60
        )

        add_executable(yobro-qt-web-appearance-tests
            tests/qt/QtWebAppearanceIntegrationTests.cpp
        )
        qt_add_resources(yobro-qt-web-appearance-tests "yobro_appearance_test_resources"
            PREFIX "/yobro"
            FILES ${APPEARANCE_RESOURCES} ${ADBLOCK_RESOURCES} ${LOCALIZATION_RESOURCES}
        )
        target_include_directories(yobro-qt-web-appearance-tests PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}"
        )
        target_link_libraries(yobro-qt-web-appearance-tests PRIVATE
            yobro_bridge_policy
            yobro_qtwebengine
            Qt6::Test
        )
        add_test(
            NAME chromium-web-appearance
            COMMAND $<TARGET_FILE:yobro-qt-web-appearance-tests>
        )
        set_tests_properties(chromium-web-appearance PROPERTIES
            ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QTWEBENGINE_CHROMIUM_FLAGS=--disable-gpu"
            TIMEOUT 180
        )

        add_executable(yobro-qt-session-restore-tests
            tests/qt/QtSessionRestoreIntegrationTests.cpp
            ${YOBRO_SPIKE_WINDOW_SOURCES}
            spike/SpikeWindow.hpp
        )
        qt_add_resources(yobro-qt-session-restore-tests "yobro_session_restore_test_resources"
            PREFIX "/yobro"
            FILES "${AGENT_BRIDGE_RESOURCE}" "${LOGIN_AUTOFILL_RESOURCE}" "${PASSKEY_SHIM_RESOURCE}" ${APPEARANCE_RESOURCES} ${ADBLOCK_RESOURCES} "${IMPORT_WORKER_RESOURCE}" "${MAIL_WORKER_RESOURCE}" ${LOCALIZATION_RESOURCES}
        )
        target_include_directories(yobro-qt-session-restore-tests PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}"
        )
        target_link_libraries(yobro-qt-session-restore-tests PRIVATE
            yobro_bridge_policy
            yobro_qtwebengine
            Qt6::Test
        )
        add_test(
            NAME chromium-session-restore
            COMMAND $<TARGET_FILE:yobro-qt-session-restore-tests>
        )
        set_tests_properties(chromium-session-restore PROPERTIES
            ENVIRONMENT "YOBRO_LANGUAGE=de;QT_QPA_PLATFORM=offscreen;QTWEBENGINE_CHROMIUM_FLAGS=--disable-gpu"
            TIMEOUT 150
        )

        add_executable(yobro-qt-passkey-tests
            tests/qt/QtPasskeyIntegrationTests.cpp
            ${YOBRO_SPIKE_WINDOW_SOURCES}
            spike/SpikeWindow.hpp
        )
        qt_add_resources(yobro-qt-passkey-tests "yobro_passkey_test_resources"
            PREFIX "/yobro"
            FILES "${AGENT_BRIDGE_RESOURCE}" "${LOGIN_AUTOFILL_RESOURCE}" "${PASSKEY_SHIM_RESOURCE}" ${APPEARANCE_RESOURCES} ${ADBLOCK_RESOURCES} "${IMPORT_WORKER_RESOURCE}" "${MAIL_WORKER_RESOURCE}" ${LOCALIZATION_RESOURCES}
        )
        target_include_directories(yobro-qt-passkey-tests PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}"
        )
        target_link_libraries(yobro-qt-passkey-tests PRIVATE
            yobro_bridge_policy
            yobro_qtwebengine
            Qt6::Test
        )
        add_test(
            NAME chromium-passkeys
            COMMAND $<TARGET_FILE:yobro-qt-passkey-tests>
        )
        set_tests_properties(chromium-passkeys PROPERTIES
            ENVIRONMENT "YOBRO_LANGUAGE=de;QT_QPA_PLATFORM=offscreen;QTWEBENGINE_CHROMIUM_FLAGS=--disable-gpu"
            TIMEOUT 180
        )

        add_executable(yobro-qt-sync-ui-tests
            tests/qt/QtSyncIntegrationTests.cpp
            ${YOBRO_SPIKE_WINDOW_SOURCES}
            spike/SpikeWindow.hpp
        )
        qt_add_resources(yobro-qt-sync-ui-tests "yobro_sync_ui_test_resources"
            PREFIX "/yobro"
            FILES "${AGENT_BRIDGE_RESOURCE}" "${LOGIN_AUTOFILL_RESOURCE}" "${PASSKEY_SHIM_RESOURCE}" ${APPEARANCE_RESOURCES} ${ADBLOCK_RESOURCES} "${IMPORT_WORKER_RESOURCE}" "${MAIL_WORKER_RESOURCE}" ${LOCALIZATION_RESOURCES}
        )
        target_include_directories(yobro-qt-sync-ui-tests PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}"
        )
        target_link_libraries(yobro-qt-sync-ui-tests PRIVATE
            yobro_bridge_policy
            yobro_qtwebengine
            Qt6::Test
        )
        add_test(
            NAME chromium-sync-ui
            COMMAND $<TARGET_FILE:yobro-qt-sync-ui-tests>
        )
        set_tests_properties(chromium-sync-ui PROPERTIES
            ENVIRONMENT "YOBRO_LANGUAGE=de;QT_QPA_PLATFORM=offscreen;QTWEBENGINE_CHROMIUM_FLAGS=--disable-gpu"
            TIMEOUT 180
        )

        add_executable(yobro-qt-onboarding-tests
            tests/qt/QtOnboardingIntegrationTests.cpp
            ${YOBRO_SPIKE_WINDOW_SOURCES}
            spike/SpikeWindow.hpp
        )
        qt_add_resources(yobro-qt-onboarding-tests "yobro_onboarding_test_resources"
            PREFIX "/yobro"
            FILES "${AGENT_BRIDGE_RESOURCE}" "${LOGIN_AUTOFILL_RESOURCE}" "${PASSKEY_SHIM_RESOURCE}" ${APPEARANCE_RESOURCES} ${ADBLOCK_RESOURCES} "${IMPORT_WORKER_RESOURCE}" "${MAIL_WORKER_RESOURCE}" ${LOCALIZATION_RESOURCES}
        )
        target_include_directories(yobro-qt-onboarding-tests PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}"
        )
        target_link_libraries(yobro-qt-onboarding-tests PRIVATE
            yobro_bridge_policy
            yobro_qtwebengine
            Qt6::Test
        )
        add_test(
            NAME chromium-onboarding
            COMMAND $<TARGET_FILE:yobro-qt-onboarding-tests>
        )
        set_tests_properties(chromium-onboarding PROPERTIES
            ENVIRONMENT "YOBRO_LANGUAGE=de;QT_QPA_PLATFORM=offscreen;QTWEBENGINE_CHROMIUM_FLAGS=--disable-gpu"
            TIMEOUT 150
        )

        add_executable(yobro-qt-mail-ui-tests
            tests/qt/QtMailIntegrationTests.cpp
            ${YOBRO_SPIKE_WINDOW_SOURCES}
            spike/SpikeWindow.hpp
        )
        qt_add_resources(yobro-qt-mail-ui-tests "yobro_mail_ui_test_resources"
            PREFIX "/yobro"
            FILES "${AGENT_BRIDGE_RESOURCE}" "${LOGIN_AUTOFILL_RESOURCE}" "${PASSKEY_SHIM_RESOURCE}" ${APPEARANCE_RESOURCES} ${ADBLOCK_RESOURCES} "${IMPORT_WORKER_RESOURCE}" "${MAIL_WORKER_RESOURCE}" ${LOCALIZATION_RESOURCES}
        )
        target_include_directories(yobro-qt-mail-ui-tests PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}"
        )
        target_link_libraries(yobro-qt-mail-ui-tests PRIVATE
            yobro_bridge_policy
            yobro_qtwebengine
            Qt6::Test
        )
        add_test(
            NAME chromium-mail-ui
            COMMAND $<TARGET_FILE:yobro-qt-mail-ui-tests>
        )
        set_tests_properties(chromium-mail-ui PROPERTIES
            ENVIRONMENT "YOBRO_LANGUAGE=de;QT_QPA_PLATFORM=offscreen;QTWEBENGINE_CHROMIUM_FLAGS=--disable-gpu"
            TIMEOUT 150
        )

        add_executable(yobro-qt-space-chat-ui-tests
            tests/qt/QtSpaceChatIntegrationTests.cpp
            ${YOBRO_SPIKE_WINDOW_SOURCES}
            spike/SpikeWindow.hpp
        )
        qt_add_resources(yobro-qt-space-chat-ui-tests "yobro_space_chat_ui_test_resources"
            PREFIX "/yobro"
            FILES "${AGENT_BRIDGE_RESOURCE}" "${LOGIN_AUTOFILL_RESOURCE}" "${PASSKEY_SHIM_RESOURCE}" ${APPEARANCE_RESOURCES} ${ADBLOCK_RESOURCES} "${IMPORT_WORKER_RESOURCE}" "${MAIL_WORKER_RESOURCE}" ${LOCALIZATION_RESOURCES}
        )
        target_include_directories(yobro-qt-space-chat-ui-tests PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}"
        )
        target_link_libraries(yobro-qt-space-chat-ui-tests PRIVATE
            yobro_bridge_policy
            yobro_qtwebengine
            Qt6::Test
        )
        add_test(
            NAME chromium-space-chat-ui
            COMMAND $<TARGET_FILE:yobro-qt-space-chat-ui-tests>
        )
        set_tests_properties(chromium-space-chat-ui PROPERTIES
            ENVIRONMENT "YOBRO_LANGUAGE=de;QT_QPA_PLATFORM=offscreen;QTWEBENGINE_CHROMIUM_FLAGS=--disable-gpu"
            TIMEOUT 150
        )

        add_executable(yobro-qt-screen-share-tests
            tests/qt/QtScreenShareIntegrationTests.cpp
            ${YOBRO_SPIKE_WINDOW_SOURCES}
        )
        qt_add_resources(yobro-qt-screen-share-tests "yobro_screen_share_test_resources"
            PREFIX "/yobro"
            FILES "${AGENT_BRIDGE_RESOURCE}" "${LOGIN_AUTOFILL_RESOURCE}" "${PASSKEY_SHIM_RESOURCE}" ${APPEARANCE_RESOURCES} ${ADBLOCK_RESOURCES} "${IMPORT_WORKER_RESOURCE}" "${MAIL_WORKER_RESOURCE}" ${LOCALIZATION_RESOURCES}
        )
        target_include_directories(yobro-qt-screen-share-tests PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}"
        )
        target_link_libraries(yobro-qt-screen-share-tests PRIVATE
            yobro_bridge_policy
            yobro_qtwebengine
            Qt6::Test
        )
        add_test(
            NAME chromium-screen-share
            COMMAND $<TARGET_FILE:yobro-qt-screen-share-tests>
        )
        set_tests_properties(chromium-screen-share PROPERTIES
            ENVIRONMENT "YOBRO_LANGUAGE=de;QT_QPA_PLATFORM=offscreen;QTWEBENGINE_CHROMIUM_FLAGS=--disable-gpu"
            TIMEOUT 200
        )
        add_executable(yobro-qt-login-autofill-tests
            tests/qt/QtLoginAutofillIntegrationTests.cpp
            ${YOBRO_SPIKE_WINDOW_SOURCES}
            spike/SpikeWindow.hpp
        )
        qt_add_resources(yobro-qt-login-autofill-tests "yobro_login_autofill_test_resources"
            PREFIX "/yobro"
            FILES "${AGENT_BRIDGE_RESOURCE}" "${LOGIN_AUTOFILL_RESOURCE}" "${PASSKEY_SHIM_RESOURCE}" ${APPEARANCE_RESOURCES} ${ADBLOCK_RESOURCES} "${IMPORT_WORKER_RESOURCE}" "${MAIL_WORKER_RESOURCE}" ${LOCALIZATION_RESOURCES}
        )
        target_include_directories(yobro-qt-login-autofill-tests PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}"
        )
        target_link_libraries(yobro-qt-login-autofill-tests PRIVATE
            yobro_bridge_policy
            yobro_qtwebengine
            Qt6::Test
        )
        add_test(
            NAME chromium-login-autofill
            COMMAND $<TARGET_FILE:yobro-qt-login-autofill-tests>
        )
        # The fixture serves HTTPS with a self-signed certificate. Accepting it
        # for this run keeps the certificate dialog out of a test that is about
        # credential handling, not about certificate trust.
        set_tests_properties(chromium-login-autofill PROPERTIES
            ENVIRONMENT "YOBRO_LANGUAGE=de;QT_QPA_PLATFORM=offscreen;QTWEBENGINE_CHROMIUM_FLAGS=--disable-gpu --ignore-certificate-errors"
            TIMEOUT 240
        )
        add_executable(yobro-qt-agent-log-tests
            tests/qt/QtAgentLogIntegrationTests.cpp
            ${YOBRO_SPIKE_WINDOW_SOURCES}
            spike/SpikeWindow.hpp
        )
        qt_add_resources(yobro-qt-agent-log-tests "yobro_agent_log_test_resources"
            PREFIX "/yobro"
            FILES "${AGENT_BRIDGE_RESOURCE}" "${LOGIN_AUTOFILL_RESOURCE}" "${PASSKEY_SHIM_RESOURCE}" ${APPEARANCE_RESOURCES} ${ADBLOCK_RESOURCES} "${IMPORT_WORKER_RESOURCE}" "${MAIL_WORKER_RESOURCE}" ${LOCALIZATION_RESOURCES}
        )
        target_include_directories(yobro-qt-agent-log-tests PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}"
        )
        target_link_libraries(yobro-qt-agent-log-tests PRIVATE
            yobro_bridge_policy
            yobro_qtwebengine
            Qt6::Test
        )
        add_test(
            NAME chromium-agent-log
            COMMAND $<TARGET_FILE:yobro-qt-agent-log-tests>
        )
        set_tests_properties(chromium-agent-log PROPERTIES
            ENVIRONMENT "YOBRO_LANGUAGE=de;QT_QPA_PLATFORM=offscreen;QTWEBENGINE_CHROMIUM_FLAGS=--disable-gpu"
            TIMEOUT 180
        )
        add_executable(yobro-qt-popup-ownership-tests
            tests/qt/QtPopupIntegrationTests.cpp
        )
        qt_add_resources(yobro-qt-popup-ownership-tests "yobro_popup_test_resources"
            PREFIX "/yobro"
            FILES "${AGENT_BRIDGE_RESOURCE}"
        )
        target_include_directories(yobro-qt-popup-ownership-tests PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}"
        )
        target_link_libraries(yobro-qt-popup-ownership-tests PRIVATE
            yobro_qtwebengine
            Qt6::Test
        )
        add_test(
            NAME chromium-phase1d-popup-ownership
            COMMAND $<TARGET_FILE:yobro-qt-popup-ownership-tests>
        )
        set_tests_properties(chromium-phase1d-popup-ownership PROPERTIES
            ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QTWEBENGINE_CHROMIUM_FLAGS=--disable-gpu"
            TIMEOUT 90
        )

        add_executable(yobro-qt-permission-tests
            tests/qt/QtPermissionIntegrationTests.cpp
            ${YOBRO_SPIKE_WINDOW_SOURCES}
            spike/SpikeWindow.hpp
        )
        qt_add_resources(yobro-qt-permission-tests "yobro_permission_test_resources"
            PREFIX "/yobro"
            FILES "${AGENT_BRIDGE_RESOURCE}" "${LOGIN_AUTOFILL_RESOURCE}" "${PASSKEY_SHIM_RESOURCE}" ${APPEARANCE_RESOURCES} ${ADBLOCK_RESOURCES} "${IMPORT_WORKER_RESOURCE}" "${MAIL_WORKER_RESOURCE}" ${LOCALIZATION_RESOURCES}
        )
        target_include_directories(yobro-qt-permission-tests PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}"
        )
        target_link_libraries(yobro-qt-permission-tests PRIVATE
            yobro_bridge_policy
            yobro_qtwebengine
            Qt6::Test
        )
        add_test(
            NAME chromium-permission-media
            COMMAND $<TARGET_FILE:yobro-qt-permission-tests>
        )
        set_tests_properties(chromium-permission-media PROPERTIES
            ENVIRONMENT "YOBRO_LANGUAGE=de;QT_QPA_PLATFORM=offscreen;QTWEBENGINE_CHROMIUM_FLAGS=--disable-gpu --use-fake-device-for-media-stream"
            TIMEOUT 120
        )

        add_executable(yobro-qt-extension-tests
            tests/qt/QtExtensionIntegrationTests.cpp
        )
        target_include_directories(yobro-qt-extension-tests PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}"
        )
        target_link_libraries(yobro-qt-extension-tests PRIVATE
            yobro_qtwebengine
            Qt6::Test
        )
        add_test(
            NAME chromium-extension-mv3
            COMMAND $<TARGET_FILE:yobro-qt-extension-tests>
        )
        set_tests_properties(chromium-extension-mv3 PROPERTIES
            ENVIRONMENT "QTWEBENGINE_CHROMIUM_FLAGS=--disable-gpu"
            TIMEOUT 90
        )

        if(UNIX)
            add_executable(yobro-qt-renderer-recovery-tests
                tests/qt/QtRendererRecoveryIntegrationTests.cpp
            )
            qt_add_resources(yobro-qt-renderer-recovery-tests "yobro_renderer_recovery_test_resources"
                PREFIX "/yobro"
                FILES "${AGENT_BRIDGE_RESOURCE}"
            )
            target_include_directories(yobro-qt-renderer-recovery-tests PRIVATE
                "${CMAKE_CURRENT_SOURCE_DIR}"
            )
            target_link_libraries(yobro-qt-renderer-recovery-tests PRIVATE
                yobro_qtwebengine
                Qt6::Test
            )
            add_test(
                NAME chromium-renderer-recovery
                COMMAND $<TARGET_FILE:yobro-qt-renderer-recovery-tests>
            )
            set_tests_properties(chromium-renderer-recovery PROPERTIES
                ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QTWEBENGINE_CHROMIUM_FLAGS=--disable-gpu --use-fake-device-for-media-stream"
                TIMEOUT 150
            )

            add_executable(yobro-qt-download-lifecycle-tests
                tests/qt/QtDownloadLifecycleIntegrationTests.cpp
                ${YOBRO_SPIKE_WINDOW_SOURCES}
                spike/SpikeWindow.hpp
            )
            qt_add_resources(yobro-qt-download-lifecycle-tests "yobro_download_lifecycle_test_resources"
                PREFIX "/yobro"
                FILES "${AGENT_BRIDGE_RESOURCE}" "${LOGIN_AUTOFILL_RESOURCE}" "${PASSKEY_SHIM_RESOURCE}" ${APPEARANCE_RESOURCES} ${ADBLOCK_RESOURCES} "${IMPORT_WORKER_RESOURCE}" "${MAIL_WORKER_RESOURCE}" ${LOCALIZATION_RESOURCES}
            )
            target_include_directories(yobro-qt-download-lifecycle-tests PRIVATE
                "${CMAKE_CURRENT_SOURCE_DIR}"
            )
            target_link_libraries(yobro-qt-download-lifecycle-tests PRIVATE
                yobro_bridge_policy
                yobro_qtwebengine
                Qt6::Test
            )
            add_test(
                NAME chromium-download-lifecycle
                COMMAND $<TARGET_FILE:yobro-qt-download-lifecycle-tests>
            )
            set_tests_properties(chromium-download-lifecycle PROPERTIES
                ENVIRONMENT "YOBRO_LANGUAGE=de;QT_QPA_PLATFORM=offscreen;QTWEBENGINE_CHROMIUM_FLAGS=--disable-gpu"
                TIMEOUT 150
            )

            add_executable(yobro-qt-protocol-integration-tests
                tests/qt/QtProtocolIntegrationTests.cpp
            )
            qt_add_resources(yobro-qt-protocol-integration-tests "yobro_protocol_test_resources"
                PREFIX "/yobro"
                FILES "${AGENT_BRIDGE_RESOURCE}"
            )
            target_include_directories(yobro-qt-protocol-integration-tests PRIVATE
                "${CMAKE_CURRENT_SOURCE_DIR}"
            )
            target_link_libraries(yobro-qt-protocol-integration-tests PRIVATE
                yobro_bridge_policy
                yobro_qtwebengine
                yobro_local_transport
            )
            add_test(
                NAME chromium-protocol-v2-qt-socket
                COMMAND $<TARGET_FILE:yobro-qt-protocol-integration-tests>
            )
            set_tests_properties(chromium-protocol-v2-qt-socket PROPERTIES
                ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QTWEBENGINE_CHROMIUM_FLAGS=--disable-gpu"
                TIMEOUT 90
            )
        endif()
    endif()

    if(APPLE AND YOBRO_PACKAGE_APP)
        if(NOT YOBRO_QT_WEBENGINE_PREFIX)
            message(FATAL_ERROR
                "YOBRO_QT_WEBENGINE_PREFIX is required for packaging license evidence."
            )
        endif()
        if(NOT EXISTS "${YOBRO_QT_PLUGIN_DIR}/platforms/libqcocoa.dylib")
            message(FATAL_ERROR
                "YOBRO_QT_PLUGIN_DIR must contain platforms/libqcocoa.dylib."
            )
        endif()
        file(REAL_PATH
            "${YOBRO_QT_PLUGIN_DIR}/platforms/libqcocoa.dylib"
            YOBRO_QT_COCOA_PLUGIN_REAL
        )

        set(YOBRO_CHROMIUM_APP_BUNDLE "YOBRO Chromium Feasibility.app")
        set(YOBRO_CHROMIUM_LICENSE_DESTINATION
            "${YOBRO_CHROMIUM_APP_BUNDLE}/Contents/Resources/licenses"
        )
        get_filename_component(YOBRO_QT_PREFIX "${Qt6_DIR}/../../.." ABSOLUTE)

        set(YOBRO_LICENSE_CHROMIUM "${YOBRO_QT_WEBENGINE_PREFIX}/LICENSE.Chromium")
        set(YOBRO_LICENSE_QT_WEBENGINE
            "${YOBRO_QT_WEBENGINE_PREFIX}/share/qt/sbom/qtwebengine-${YOBRO_QT_VERSION}.spdx"
        )
        set(YOBRO_SBOM_QT_WEBENGINE "${YOBRO_QT_WEBENGINE_PREFIX}/sbom.spdx.json")
        set(YOBRO_SBOM_QT "${YOBRO_QT_PREFIX}/sbom.spdx.json")
        set(YOBRO_QT_MODULE_SBOMS
            "${YOBRO_QT_PREFIX}/share/qt/sbom/qtbase-${YOBRO_QT_VERSION}.spdx"
            "${YOBRO_QT_PREFIX}/share/qt/sbom/qtdeclarative-${YOBRO_QT_VERSION}.spdx"
            "${YOBRO_QT_PREFIX}/share/qt/sbom/qtimageformats-${YOBRO_QT_VERSION}.spdx"
            "${YOBRO_QT_PREFIX}/share/qt/sbom/qtpdf-${YOBRO_QT_VERSION}.spdx"
            "${YOBRO_QT_PREFIX}/share/qt/sbom/qtpositioning-${YOBRO_QT_VERSION}.spdx"
            "${YOBRO_QT_PREFIX}/share/qt/sbom/qtserialport-${YOBRO_QT_VERSION}.spdx"
            "${YOBRO_QT_PREFIX}/share/qt/sbom/qtsvg-${YOBRO_QT_VERSION}.spdx"
            "${YOBRO_QT_PREFIX}/share/qt/sbom/qtvirtualkeyboard-${YOBRO_QT_VERSION}.spdx"
            "${YOBRO_QT_PREFIX}/share/qt/sbom/qtwebchannel-${YOBRO_QT_VERSION}.spdx"
        )
        foreach(evidence IN ITEMS
            "${YOBRO_LICENSE_CHROMIUM}"
            "${YOBRO_LICENSE_QT_WEBENGINE}"
            "${YOBRO_SBOM_QT_WEBENGINE}"
            "${YOBRO_SBOM_QT}"
            ${YOBRO_QT_MODULE_SBOMS}
        )
            if(NOT EXISTS "${evidence}")
                message(FATAL_ERROR "Required packaging license evidence is missing: ${evidence}")
            endif()
        endforeach()

        file(REAL_PATH "${YOBRO_LICENSE_CHROMIUM}" YOBRO_LICENSE_CHROMIUM_REAL)
        file(REAL_PATH "${YOBRO_LICENSE_QT_WEBENGINE}" YOBRO_LICENSE_QT_WEBENGINE_REAL)
        file(REAL_PATH "${YOBRO_SBOM_QT_WEBENGINE}" YOBRO_SBOM_QT_WEBENGINE_REAL)
        file(REAL_PATH "${YOBRO_SBOM_QT}" YOBRO_SBOM_QT_REAL)
        set(YOBRO_QT_MODULE_SBOMS_REAL)
        foreach(evidence IN LISTS YOBRO_QT_MODULE_SBOMS)
            file(REAL_PATH "${evidence}" evidence_real)
            list(APPEND YOBRO_QT_MODULE_SBOMS_REAL "${evidence_real}")
        endforeach()

        install(TARGETS yobro-chromium-spike BUNDLE DESTINATION .)
        install(
            DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/licenses/"
            DESTINATION "${YOBRO_CHROMIUM_LICENSE_DESTINATION}"
        )
        install(FILES "${YOBRO_LICENSE_CHROMIUM_REAL}"
            DESTINATION "${YOBRO_CHROMIUM_LICENSE_DESTINATION}"
        )
        install(FILES "${YOBRO_LICENSE_QT_WEBENGINE_REAL}" ${YOBRO_QT_MODULE_SBOMS_REAL}
            DESTINATION "${YOBRO_CHROMIUM_LICENSE_DESTINATION}"
        )
        install(FILES "${YOBRO_SBOM_QT_WEBENGINE_REAL}"
            DESTINATION "${YOBRO_CHROMIUM_LICENSE_DESTINATION}"
            RENAME "homebrew-qtwebengine-${YOBRO_QT_VERSION}.spdx.json"
        )
        install(FILES "${YOBRO_SBOM_QT_REAL}"
            DESTINATION "${YOBRO_CHROMIUM_LICENSE_DESTINATION}"
            RENAME "homebrew-qt-${YOBRO_QT_VERSION}.spdx.json"
        )

        qt_generate_deploy_app_script(
            TARGET yobro-chromium-spike
            OUTPUT_SCRIPT YOBRO_CHROMIUM_DEPLOY_SCRIPT
            NO_PLUGINS
            DEPLOY_TOOL_OPTIONS
                -verbose=0
        )
        install(SCRIPT "${YOBRO_CHROMIUM_DEPLOY_SCRIPT}")

        find_program(YOBRO_MACDEPLOYQT
            NAMES macdeployqt
            HINTS "${YOBRO_QT_PREFIX}/bin"
            REQUIRED
        )
        configure_file(
            "${CMAKE_CURRENT_SOURCE_DIR}/cmake/DeployWebEngineHelper.cmake.in"
            "${CMAKE_CURRENT_BINARY_DIR}/DeployWebEngineHelper.cmake"
            @ONLY
        )
        install(SCRIPT "${CMAKE_CURRENT_BINARY_DIR}/DeployWebEngineHelper.cmake")
    endif()
endif()
