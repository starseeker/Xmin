# Test organization

Keep fast project-owned unit tests directly under `tests/unit`, X11 wire tests under
`tests/protocol`, process and authentication tests under `tests/integration`, toolkit
acceptance tests under `tests/qt`, and GLX/OpenGL tests under `tests/glx`. External
Xlib, XCB, Qt, GTK, and GL implementations are test-only dependencies and must not
enter Xmin's installed link interface.

