The intent of this repository is to define a minimalist but complete, stand-alone
xvfb implementation that can provide headless X11 graphics rendering portably.

It is similar in spirit to https://github.com/starseeker/osmesa - intended to make
X11 graphics rendering universally available in a self-contained codbase with minimal
or (ideally) no external dependencies or reliance on grahics hardware.  It should remove
any code not needed for that purpose.

We want to be portable, standards compliant, use CMake for our build, and can use
C11/C++17 for our implementations.

https://gitlab.freedesktop.org/xorg will be our source for code, but we will pull
in only what we need.  Likely sources we will need subsets from include (but probably
are not limited to):

https://gitlab.freedesktop.org/xorg/lib/libx11
https://gitlab.freedesktop.org/xorg/lib/libxcb
https://gitlab.freedesktop.org/xorg/xserver/-/tree/main/hw/vfb

Our initial target is to map out an implementation plan - we will likely need to handle a few
modern extensions like randr if we can to allow Qt to talk to us successfully, but in cases
where something would pull in a large dependency stack (worried about fonts, but probably other
cases) we should look to using things like https://github.com/starseeker/struetype or
https://github.com/starseeker/osmesa and accept a possibly slightly more limited feature set
rather than shooting for absolute matching fidelity.  I remember a concern about keyboards being
an issue - a sane minimal default should be acceptable there rather than embracing huge complexity

The initial architecture, required component inventory, extension profile, and implementation
sequence are documented in DESIGN.md.
