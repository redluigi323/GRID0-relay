# Windows uses SDK declarations and loads the installed Npcap DLL at runtime.
# No Npcap runtime or obsolete WinPcap download is bundled by this build.
include(FindPackageHandleStandardArgs)
if(WIN32)
    set(NPCAP_SDK_DIR "" CACHE PATH "Extracted Npcap SDK directory (contains Include and Lib)")
    find_path(PCAP_INCLUDE_DIR NAMES pcap/pcap.h
        HINTS "${NPCAP_SDK_DIR}/Include" "$ENV{NPCAP_SDK_DIR}/Include"
        NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
    find_package_handle_standard_args(PCAP REQUIRED_VARS PCAP_INCLUDE_DIR
        REASON_FAILURE_MESSAGE "Download the Npcap SDK from https://npcap.com and set -DNPCAP_SDK_DIR=<extracted SDK>.")
else()
    find_path(PCAP_INCLUDE_DIR NAMES pcap/pcap.h HINTS "${PCAP_ROOT_DIR}/include")
    find_library(PCAP_LIBRARY NAMES pcap HINTS "${PCAP_ROOT_DIR}/lib")
    find_package_handle_standard_args(PCAP REQUIRED_VARS PCAP_INCLUDE_DIR PCAP_LIBRARY)
endif()
mark_as_advanced(PCAP_INCLUDE_DIR PCAP_LIBRARY)
