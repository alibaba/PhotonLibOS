file(REMOVE_RECURSE
  "output/libphoton.pdb"
  "output/libphoton.so"
)

# Per-language clean rules from dependency scanning.
foreach(lang CXX)
  include(CMakeFiles/photon_shared.dir/cmake_clean_${lang}.cmake OPTIONAL)
endforeach()
