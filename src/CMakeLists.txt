set(src
    main.c
    lan-play.c
    dhcp-server.c
    native-udp.c
    pcaploop.cpp
    packet.c
    arp.c
    helper.c
    lan-client.c
    gateway.cpp
    proxy_direct.cpp
    proxy_socks5.cpp
    sha1.c
    ipv4/ipv4.c
    ipv4/tcp.c
    ipv4/udp.c
    ipv4/icmp.c
    rpc/rpc.cpp
    rpc/lan-play.cpp
    rpc/rpc-server.cpp
    rpc/rpc-ws-server.cpp
)

add_executable(grid0-relay ${src})

target_compile_options(
    grid0-relay
    PRIVATE "-DLANPLAY_VERSION=\"${LANPLAY_VERSION}\""
)
if (${CMAKE_CXX_COMPILER_ID} STREQUAL GNU)
    target_compile_options(
        grid0-relay
        PRIVATE "-Wall"
        PRIVATE "-Wno-unused-function"
    )
endif()

set_target_properties(grid0-relay PROPERTIES CXX_STANDARD 17)

if(NOT ((CMAKE_SYSTEM_NAME STREQUAL "Darwin") OR MSVC))
    target_link_libraries(grid0-relay atomic)
endif()
target_link_libraries(grid0-relay base)
target_link_libraries(grid0-relay uv_lwip)
target_link_libraries(grid0-relay uv_a)

find_package(PCAP REQUIRED)
if(NOT WIN32)
    target_link_libraries(grid0-relay ${PCAP_LIBRARY})
endif()
target_include_directories(grid0-relay PRIVATE ${PCAP_INCLUDE_DIR})

if (WIN32)
    target_sources(grid0-relay PRIVATE npcap-loader.c win-firewall.c)
    target_link_libraries(grid0-relay ws2_32 iphlpapi psapi userenv shell32)
    target_compile_definitions(grid0-relay PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00)
else()
    if(NOT CMAKE_SYSTEM_NAME STREQUAL "Android")
        target_link_libraries(grid0-relay pthread)
    endif()
endif()

INSTALL(TARGETS grid0-relay RUNTIME DESTINATION bin)
