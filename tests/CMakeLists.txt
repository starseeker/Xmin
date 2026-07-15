add_test(NAME xmin.version COMMAND Xmin::server --version)
set_tests_properties(xmin.version PROPERTIES
  PASS_REGULAR_EXPRESSION "Xmin ${PROJECT_VERSION} \\(server stub\\)"
)

add_test(NAME xmin.help COMMAND Xmin::server --help)
set_tests_properties(xmin.help PROPERTIES
  PASS_REGULAR_EXPRESSION "currently a build-system stub"
)

# A bare invocation must fail so automation cannot mistake the placeholder for a
# listening X server.
add_test(NAME xmin.stub-refuses-to-start COMMAND Xmin::server)
set_tests_properties(xmin.stub-refuses-to-start PROPERTIES
  WILL_FAIL TRUE
)

add_executable(xmin_protocol_headers protocol_headers.c)
target_compile_features(xmin_protocol_headers PRIVATE c_std_11)
target_link_libraries(xmin_protocol_headers PRIVATE Xmin::xorgproto)
xmin_enable_warnings(xmin_protocol_headers)
add_test(NAME xmin.protocol-headers COMMAND xmin_protocol_headers)

add_executable(xmin_dix_atoms dix_atoms.c)
target_compile_features(xmin_dix_atoms PRIVATE c_std_11)
target_link_libraries(xmin_dix_atoms PRIVATE Xmin::dix)
xmin_enable_warnings(xmin_dix_atoms)
add_test(NAME xmin.dix-atoms COMMAND xmin_dix_atoms)

add_executable(xmin_pixman_regions pixman_regions.c)
target_compile_features(xmin_pixman_regions PRIVATE c_std_11)
target_link_libraries(xmin_pixman_regions PRIVATE Xmin::pixman)
xmin_enable_warnings(xmin_pixman_regions)
add_test(NAME xmin.pixman-regions COMMAND xmin_pixman_regions)
