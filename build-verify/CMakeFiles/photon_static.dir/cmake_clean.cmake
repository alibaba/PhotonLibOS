file(REMOVE_RECURSE
  "output/libphoton_sole.a"
  "output/libphoton_sole.pdb"
)

# Per-language clean rules from dependency scanning.
foreach(lang CXX)
  include(CMakeFiles/photon_static.dir/cmake_clean_${lang}.cmake OPTIONAL)
endforeach()
