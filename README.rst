`Mesa <https://mesa3d.org>`_ - The 3D Graphics Library
======================================================

This Mesa fork was conceived to improve the Linux experience on Ivy Bridge and Haswell hardware. This involved touching the ELK compiler and the crocus and hasvk drivers as well as the common ISL code, though I've tried to put hasvk somewhat in its own sandbox.
I've implemented support for Vulkan H.264 hardware acceleration leveraging a complex VDPAU-based stack. It currently works well enough and no crashes are observed playing all of the test files at https://chromium.googlesource.com/chromium/src/media.git but there is some additional CPU overhead. Hopefully I didn't break Android, Bay Trail, or Broadwell, but I can't test. Note that generally performance is a little worse due to working around many bugs and hardware limitations.

Last I checked hasvk doesn't work at all on the BSDs, but your mileage may vary.
