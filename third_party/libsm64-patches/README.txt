Mario Mode's changes to libsm64 (commit fd11813), applied by
tools/build_sm64dll.sh before it builds sm64.dll:

0001-mario-alpha-display-list.patch
    make_gfx_mario_alpha (the vanish cap's see-through Mario, and Mario's
    model fading) built N64 display-list commands that libsm64's own display
    list adapter doesn't understand: it read past the end of the list into
    the heap. With the vanish cap on, that crashed (seen with the real
    libsm64 and the user's ROM in tests/test_cheats.cpp). libsm64 has
    nothing to draw from those commands, so the function returns no list.

0002-draw-mario-butt.patch
    geo_process_master_list_sub skipped the first display list of Mario's
    master list, taking it for an uninitialised projection matrix. It is his
    butt (mario_butt, or mario_metal_butt), which also sets the blue light
    the torso's display list shares: the butt was never drawn (72 triangles
    missing) and the overalls' bib and buttons took the colour of the last
    light of the previous frame - the shoes' brown, or the wing cap's white.
    Checked with the real libsm64 and the user's ROM for every cap
    (tests/test_cheats.cpp).
