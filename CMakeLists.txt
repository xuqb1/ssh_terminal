function(cui_example name)
    add_executable(${name} ${ARGN})
    target_link_libraries(${name} PRIVATE cui::cui)
    if(MSVC)
        target_compile_options(${name} PRIVATE /utf-8)
    endif()
    if(WIN32)
        set_target_properties(${name} PROPERTIES WIN32_EXECUTABLE ON)
        if(MSVC)
            target_link_options(${name} PRIVATE /ENTRY:mainCRTStartup)
        else()
            target_link_options(${name} PRIVATE -mwindows)
        endif()
    endif()
    if(APPLE)
        set_target_properties(${name} PROPERTIES MACOSX_BUNDLE OFF)
    endif()
    if(APPLE)
        target_link_options(${name} PRIVATE $<$<CONFIG:Release,MinSizeRel>:-Wl,-dead_strip>)
    elseif(MSVC)
        target_link_options(${name} PRIVATE $<$<CONFIG:Release,MinSizeRel>:/OPT:REF /OPT:ICF>)
    elseif(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
        target_link_options(${name} PRIVATE $<$<CONFIG:Release,MinSizeRel>:-Wl,--gc-sections>)
        if(MINGW)
            target_link_options(${name} PRIVATE $<$<CONFIG:Release,MinSizeRel>:-flto=auto>)
        endif()
    endif()
endfunction()


cui_example(hello hello.c)
cui_example(gallery gallery.c gallery_ui.c gallery_more.c)
cui_example(dashboard dashboard.c)
cui_example(settings settings.c)
cui_example(notes notes.c)
cui_example(mail mail.c)
cui_example(tracker tracker.c)
cui_example(music music.c)
cui_example(finance finance.c)
cui_example(assistant assistant.c)
cui_example(ssh_terminal ssh_terminal.c my_terminal.c)
# ---- libssh 集成（仅 ssh_terminal） ----
set(LIBSSH_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/libssh")
set(OPENSSL_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/openssl")

target_include_directories(ssh_terminal PRIVATE
    "${LIBSSH_DIR}/include"
    "${OPENSSL_DIR}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/../src"
)
target_compile_definitions(ssh_terminal PRIVATE LIBSSH_STATIC)
target_link_libraries(ssh_terminal PRIVATE
    "${LIBSSH_DIR}/lib/ssh.lib"
    "${OPENSSL_DIR}/lib/libcrypto.lib"
    "${OPENSSL_DIR}/lib/libssl.lib"
    ws2_32.lib advapi32.lib shell32.lib user32.lib
)

# 构建后把 OpenSSL DLL 复制到 exe 旁边
add_custom_command(TARGET ssh_terminal POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${OPENSSL_DIR}/bin/libcrypto-3-x64.dll"
        "${OPENSSL_DIR}/bin/libssl-3-x64.dll"
        "$<TARGET_FILE_DIR:ssh_terminal>"
)

add_custom_command(TARGET ssh_terminal POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/ssh_terminal.ico"
        "$<TARGET_FILE_DIR:ssh_terminal>"
)

# 确保和 ssh.lib 相同的 CRT（/MD）
if(MSVC)
    set_property(TARGET ssh_terminal PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
endif()

