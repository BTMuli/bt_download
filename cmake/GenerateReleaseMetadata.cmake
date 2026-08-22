cmake_minimum_required(VERSION 3.25)

foreach(required_variable
        BT_RELEASE_DIR
        BT_VCPKG_INSTALLED_DIR
        BT_VCPKG_TRIPLET
        BT_PROJECT_VERSION
        BT_MSVC_VERSION)
    if(NOT DEFINED ${required_variable} OR "${${required_variable}}" STREQUAL "")
        message(FATAL_ERROR "${required_variable} is required")
    endif()
endforeach()

cmake_path(CONVERT "${BT_RELEASE_DIR}" TO_CMAKE_PATH_LIST BT_RELEASE_DIR NORMALIZE)
cmake_path(CONVERT "${BT_VCPKG_INSTALLED_DIR}" TO_CMAKE_PATH_LIST BT_VCPKG_INSTALLED_DIR NORMALIZE)

set(status_path "${BT_VCPKG_INSTALLED_DIR}/vcpkg/status")
set(share_dir "${BT_VCPKG_INSTALLED_DIR}/${BT_VCPKG_TRIPLET}/share")
if(NOT EXISTS "${status_path}")
    message(FATAL_ERROR "vcpkg status file not found: ${status_path}")
endif()

file(READ "${status_path}" vcpkg_status)

function(read_package_version package_name output_variable)
    string(REGEX MATCH
        "(^|\n)Package: ${package_name}\r?\nVersion: ([^\r\n]+)(\r?\nPort-Version: ([0-9]+))?"
        package_record
        "${vcpkg_status}"
    )
    if(NOT package_record)
        message(FATAL_ERROR "Required vcpkg package is not installed: ${package_name}")
    endif()
    set(package_version "${CMAKE_MATCH_2}")
    if(CMAKE_MATCH_4 AND NOT CMAKE_MATCH_4 STREQUAL "0")
        string(APPEND package_version "#${CMAKE_MATCH_4}")
    endif()
    set(${output_variable} "${package_version}" PARENT_SCOPE)
endfunction()

read_package_version("libtorrent" libtorrent_version)
read_package_version("curl" curl_version)
read_package_version("zlib" zlib_version)
read_package_version("openssl" openssl_version)
read_package_version("nlohmann-json" nlohmann_json_version)
read_package_version("boost-headers" boost_version)

set(license_dir "${BT_RELEASE_DIR}/licenses")
file(MAKE_DIRECTORY "${license_dir}")

set(license_sources
    "libtorrent|${share_dir}/libtorrent/copyright"
    "curl|${share_dir}/curl/copyright"
    "zlib|${share_dir}/zlib/copyright"
    "openssl|${share_dir}/openssl/copyright"
    "nlohmann-json|${share_dir}/nlohmann-json/copyright"
    "boost|${share_dir}/boost-headers/copyright"
)

set(notices
    "bt_download third-party notices\n"
    "================================\n\n"
    "The authoritative package versions are also recorded in sbom.spdx.json.\n"
)
foreach(license_entry IN LISTS license_sources)
    string(REPLACE "|" ";" license_fields "${license_entry}")
    list(GET license_fields 0 license_name)
    list(GET license_fields 1 license_path)
    if(NOT EXISTS "${license_path}")
        message(FATAL_ERROR "License file not found: ${license_path}")
    endif()
    file(READ "${license_path}" license_text)
    file(WRITE "${license_dir}/${license_name}.txt" "${license_text}")
    string(APPEND notices
        "\n\n------------------------------------------------------------------------\n"
        "${license_name}\n"
        "------------------------------------------------------------------------\n\n"
        "${license_text}"
    )
endforeach()
file(WRITE "${BT_RELEASE_DIR}/THIRD_PARTY_NOTICES.txt" "${notices}")

set(required_release_files
    "bt_download.exe"
    "torrent-rasterbar.dll"
    "libcurl.dll"
    "z.dll"
    "libssl-3-x64.dll"
    "libcrypto-3-x64.dll"
)
foreach(required_release_file IN LISTS required_release_files)
    if(NOT EXISTS "${BT_RELEASE_DIR}/${required_release_file}")
        message(FATAL_ERROR "Required release file not found: ${BT_RELEASE_DIR}/${required_release_file}")
    endif()
endforeach()

file(GLOB release_paths
    LIST_DIRECTORIES false
    "${BT_RELEASE_DIR}/*.exe"
    "${BT_RELEASE_DIR}/*.dll"
)
set(release_files "")
foreach(release_path IN LISTS release_paths)
    get_filename_component(release_file "${release_path}" NAME)
    list(APPEND release_files "${release_file}")
endforeach()
if(NOT release_files)
    message(FATAL_ERROR "No release executables or libraries were found in ${BT_RELEASE_DIR}")
endif()
list(SORT release_files)
set(file_entries "")
set(file_relationships "")
set(package_verification_hashes "")
foreach(release_file IN LISTS release_files)
    set(release_path "${BT_RELEASE_DIR}/${release_file}")
    file(SHA256 "${release_path}" release_sha256)
    file(SHA1 "${release_path}" release_sha1)
    list(APPEND package_verification_hashes "${release_sha1}")
    string(MAKE_C_IDENTIFIER "${release_file}" file_id)
    string(APPEND file_entries
        "    {\n"
        "      \"fileName\": \"./${release_file}\",\n"
        "      \"SPDXID\": \"SPDXRef-File-${file_id}\",\n"
        "      \"checksums\": [{\"algorithm\": \"SHA256\", \"checksumValue\": \"${release_sha256}\"}],\n"
        "      \"licenseConcluded\": \"NOASSERTION\",\n"
        "      \"copyrightText\": \"NOASSERTION\"\n"
        "    },\n"
    )
    string(APPEND file_relationships
        "    {\"spdxElementId\": \"SPDXRef-Package-bt-download\", \"relationshipType\": \"CONTAINS\", \"relatedSpdxElement\": \"SPDXRef-File-${file_id}\"},\n"
    )
    if(release_file MATCHES "^(concrt|msvcp|vcruntime).*[.]dll$")
        string(APPEND file_relationships
            "    {\"spdxElementId\": \"SPDXRef-File-${file_id}\", \"relationshipType\": \"GENERATED_FROM\", \"relatedSpdxElement\": \"SPDXRef-Package-msvc-runtime\"},\n"
        )
    endif()
endforeach()
string(REGEX REPLACE ",\n$" "\n" file_entries "${file_entries}")
list(SORT package_verification_hashes)
string(JOIN "" package_verification_input ${package_verification_hashes})
string(SHA1 package_verification_code "${package_verification_input}")

string(TIMESTAMP created_at "%Y-%m-%dT%H:%M:%SZ" UTC)
string(SHA256 namespace_hash
    "bt_download-${BT_PROJECT_VERSION}-${BT_VCPKG_TRIPLET}-${package_verification_code}-${libtorrent_version}-${curl_version}-${zlib_version}-${openssl_version}-${nlohmann_json_version}-${boost_version}-${created_at}"
)

set(sbom "{\n")
string(APPEND sbom
    "  \"$schema\": \"https://raw.githubusercontent.com/spdx/spdx-spec/v2.3/schemas/spdx-schema.json\",\n"
    "  \"spdxVersion\": \"SPDX-2.3\",\n"
    "  \"dataLicense\": \"CC0-1.0\",\n"
    "  \"SPDXID\": \"SPDXRef-DOCUMENT\",\n"
    "  \"name\": \"bt_download-${BT_PROJECT_VERSION}-${BT_VCPKG_TRIPLET}\",\n"
    "  \"documentNamespace\": \"https://bangumi.today/spdx/bt-download/${namespace_hash}\",\n"
    "  \"creationInfo\": {\n"
    "    \"created\": \"${created_at}\",\n"
    "    \"creators\": [\"Tool: bt_download CMake release metadata generator\"]\n"
    "  },\n"
    "  \"documentDescribes\": [\"SPDXRef-Package-bt-download\"],\n"
    "  \"packages\": [\n"
    "    {\"name\": \"bt_download\", \"SPDXID\": \"SPDXRef-Package-bt-download\", \"versionInfo\": \"${BT_PROJECT_VERSION}\", \"downloadLocation\": \"NOASSERTION\", \"filesAnalyzed\": true, \"packageVerificationCode\": {\"packageVerificationCodeValue\": \"${package_verification_code}\"}, \"licenseConcluded\": \"NOASSERTION\", \"licenseDeclared\": \"NOASSERTION\", \"copyrightText\": \"NOASSERTION\"},\n"
    "    {\"name\": \"libtorrent\", \"SPDXID\": \"SPDXRef-Package-libtorrent\", \"versionInfo\": \"${libtorrent_version}\", \"downloadLocation\": \"https://github.com/arvidn/libtorrent\", \"filesAnalyzed\": false, \"licenseConcluded\": \"BSD-2-Clause\", \"licenseDeclared\": \"BSD-2-Clause\", \"copyrightText\": \"NOASSERTION\"},\n"
    "    {\"name\": \"curl\", \"SPDXID\": \"SPDXRef-Package-curl\", \"versionInfo\": \"${curl_version}\", \"downloadLocation\": \"https://curl.se/\", \"filesAnalyzed\": false, \"licenseConcluded\": \"curl\", \"licenseDeclared\": \"curl\", \"copyrightText\": \"NOASSERTION\"},\n"
    "    {\"name\": \"zlib\", \"SPDXID\": \"SPDXRef-Package-zlib\", \"versionInfo\": \"${zlib_version}\", \"downloadLocation\": \"https://zlib.net/\", \"filesAnalyzed\": false, \"licenseConcluded\": \"Zlib\", \"licenseDeclared\": \"Zlib\", \"copyrightText\": \"NOASSERTION\"},\n"
    "    {\"name\": \"OpenSSL\", \"SPDXID\": \"SPDXRef-Package-openssl\", \"versionInfo\": \"${openssl_version}\", \"downloadLocation\": \"https://github.com/openssl/openssl\", \"filesAnalyzed\": false, \"licenseConcluded\": \"Apache-2.0\", \"licenseDeclared\": \"Apache-2.0\", \"copyrightText\": \"NOASSERTION\"},\n"
    "    {\"name\": \"nlohmann-json\", \"SPDXID\": \"SPDXRef-Package-nlohmann-json\", \"versionInfo\": \"${nlohmann_json_version}\", \"downloadLocation\": \"https://github.com/nlohmann/json\", \"filesAnalyzed\": false, \"licenseConcluded\": \"MIT\", \"licenseDeclared\": \"MIT\", \"copyrightText\": \"NOASSERTION\"},\n"
    "    {\"name\": \"Boost\", \"SPDXID\": \"SPDXRef-Package-boost\", \"versionInfo\": \"${boost_version}\", \"downloadLocation\": \"https://github.com/boostorg/boost\", \"filesAnalyzed\": false, \"licenseConcluded\": \"BSL-1.0\", \"licenseDeclared\": \"BSL-1.0\", \"copyrightText\": \"NOASSERTION\"},\n"
    "    {\"name\": \"Microsoft Visual C++ Runtime\", \"SPDXID\": \"SPDXRef-Package-msvc-runtime\", \"versionInfo\": \"${BT_MSVC_VERSION}\", \"supplier\": \"Organization: Microsoft Corporation\", \"downloadLocation\": \"NONE\", \"filesAnalyzed\": false, \"licenseConcluded\": \"NOASSERTION\", \"licenseDeclared\": \"NOASSERTION\", \"copyrightText\": \"Copyright Microsoft Corporation\"}\n"
    "  ],\n"
    "  \"files\": [\n${file_entries}  ],\n"
    "  \"relationships\": [\n"
    "    {\"spdxElementId\": \"SPDXRef-DOCUMENT\", \"relationshipType\": \"DESCRIBES\", \"relatedSpdxElement\": \"SPDXRef-Package-bt-download\"},\n"
    "    {\"spdxElementId\": \"SPDXRef-Package-bt-download\", \"relationshipType\": \"DEPENDS_ON\", \"relatedSpdxElement\": \"SPDXRef-Package-libtorrent\"},\n"
    "    {\"spdxElementId\": \"SPDXRef-Package-bt-download\", \"relationshipType\": \"DEPENDS_ON\", \"relatedSpdxElement\": \"SPDXRef-Package-curl\"},\n"
    "    {\"spdxElementId\": \"SPDXRef-Package-bt-download\", \"relationshipType\": \"DEPENDS_ON\", \"relatedSpdxElement\": \"SPDXRef-Package-nlohmann-json\"},\n"
    "    {\"spdxElementId\": \"SPDXRef-Package-bt-download\", \"relationshipType\": \"DEPENDS_ON\", \"relatedSpdxElement\": \"SPDXRef-Package-msvc-runtime\"},\n"
    "    {\"spdxElementId\": \"SPDXRef-Package-libtorrent\", \"relationshipType\": \"DEPENDS_ON\", \"relatedSpdxElement\": \"SPDXRef-Package-openssl\"},\n"
    "    {\"spdxElementId\": \"SPDXRef-Package-libtorrent\", \"relationshipType\": \"DEPENDS_ON\", \"relatedSpdxElement\": \"SPDXRef-Package-boost\"},\n"
    "    {\"spdxElementId\": \"SPDXRef-Package-curl\", \"relationshipType\": \"DEPENDS_ON\", \"relatedSpdxElement\": \"SPDXRef-Package-zlib\"},\n"
    "${file_relationships}"
    "    {\"spdxElementId\": \"SPDXRef-File-torrent_rasterbar_dll\", \"relationshipType\": \"GENERATED_FROM\", \"relatedSpdxElement\": \"SPDXRef-Package-libtorrent\"},\n"
    "    {\"spdxElementId\": \"SPDXRef-File-libcurl_dll\", \"relationshipType\": \"GENERATED_FROM\", \"relatedSpdxElement\": \"SPDXRef-Package-curl\"},\n"
    "    {\"spdxElementId\": \"SPDXRef-File-z_dll\", \"relationshipType\": \"GENERATED_FROM\", \"relatedSpdxElement\": \"SPDXRef-Package-zlib\"},\n"
    "    {\"spdxElementId\": \"SPDXRef-File-libssl_3_x64_dll\", \"relationshipType\": \"GENERATED_FROM\", \"relatedSpdxElement\": \"SPDXRef-Package-openssl\"},\n"
    "    {\"spdxElementId\": \"SPDXRef-File-libcrypto_3_x64_dll\", \"relationshipType\": \"GENERATED_FROM\", \"relatedSpdxElement\": \"SPDXRef-Package-openssl\"}\n"
    "  ]\n"
    "}\n"
)
file(WRITE "${BT_RELEASE_DIR}/sbom.spdx.json" "${sbom}")
