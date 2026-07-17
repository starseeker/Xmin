include_guard(GLOBAL)

function(xmin_verify_protocol_profile policy report xml_directory)
  foreach(path IN ITEMS "${policy}" "${report}")
    if(NOT EXISTS "${path}")
      message(FATAL_ERROR "Missing protocol profile input: ${path}")
    endif()
  endforeach()

  file(READ "${report}" profile_json)
  string(JSON schema GET "${profile_json}" schema_version)
  if(NOT schema EQUAL 2)
    message(FATAL_ERROR "Unsupported generated protocol profile schema: ${schema}")
  endif()

  file(SHA256 "${policy}" actual_policy_hash)
  string(JSON recorded_policy_hash GET "${profile_json}"
    generated_from policy_sha256)
  if(NOT actual_policy_hash STREQUAL recorded_policy_hash)
    message(FATAL_ERROR
      "profiles/protocol-coverage.json is stale; run "
      "tools/generate-protocol-coverage.py"
    )
  endif()

  string(JSON xml_count LENGTH "${profile_json}" generated_from xml)
  math(EXPR xml_last "${xml_count} - 1")
  foreach(index RANGE 0 ${xml_last})
    string(JSON xml_file GET "${profile_json}" generated_from xml ${index} file)
    string(JSON recorded_hash GET "${profile_json}"
      generated_from xml ${index} sha256)
    set(xml_path "${xml_directory}/${xml_file}")
    if(NOT EXISTS "${xml_path}")
      message(FATAL_ERROR "Missing protocol XML: ${xml_path}")
    endif()
    file(SHA256 "${xml_path}" actual_hash)
    if(NOT actual_hash STREQUAL recorded_hash)
      message(FATAL_ERROR
        "${xml_file} changed without regenerating protocol-coverage.json"
      )
    endif()
    if(xml_file STREQUAL "xproto.xml")
      set(core_xml_hash "${actual_hash}")
    endif()
  endforeach()

  if(ARGC GREATER 3)
    set(generated_header "${ARGV3}")
    if(NOT EXISTS "${generated_header}")
      message(FATAL_ERROR
        "Missing generated core protocol header: ${generated_header}"
      )
    endif()
    file(READ "${generated_header}" generated_core)
    string(FIND "${generated_core}" "sha256: ${core_xml_hash}" hash_position)
    if(hash_position EQUAL -1)
      message(FATAL_ERROR
        "Generated core protocol header is stale; run "
        "tools/generate-protocol-coverage.py with --cpp-header"
      )
    endif()
  endif()

  string(JSON core_count LENGTH "${profile_json}" core_requests)
  if(NOT core_count EQUAL 127)
    message(FATAL_ERROR
      "Protocol profile must enumerate 127 core slots, found ${core_count}"
    )
  endif()
  math(EXPR core_last "${core_count} - 1")
  foreach(index RANGE 0 ${core_last})
    string(JSON opcode GET "${profile_json}" core_requests ${index} opcode)
    math(EXPR expected_opcode "${index} + 1")
    if(NOT opcode EQUAL expected_opcode)
      message(FATAL_ERROR
        "Protocol profile slot ${index} has opcode ${opcode}; "
        "expected ${expected_opcode}"
      )
    endif()
  endforeach()

  string(JSON extension_count LENGTH "${profile_json}" extensions)
  if(extension_count LESS 1)
    message(FATAL_ERROR "Protocol profile contains no extensions")
  endif()
  math(EXPR extension_last "${extension_count} - 1")
  foreach(index RANGE 0 ${extension_last})
    string(JSON extension_name GET "${profile_json}" extensions ${index} name)
    string(JSON request_count LENGTH "${profile_json}"
      extensions ${index} requests)
    if(request_count LESS 1)
      message(FATAL_ERROR "${extension_name} has no request-level contract")
    endif()
  endforeach()

  string(JSON extension_requests GET "${profile_json}" summary extension_requests)
  message(STATUS
    "Protocol contract: ${core_count} core slots, ${extension_count} extensions, "
    "${extension_requests} extension requests"
  )
endfunction()
