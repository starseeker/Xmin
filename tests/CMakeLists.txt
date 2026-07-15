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

