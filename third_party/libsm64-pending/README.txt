Not applied by tools/build_sm64dll.sh yet: the custom Mario model (DynOS)
support, kept for the release that adds models. To use it, move the .patch
files into third_party/libsm64-patches/ and swap in the patched libsm64.h
(the patch changes src/libsm64.h; copy it to third_party/libsm64/).
