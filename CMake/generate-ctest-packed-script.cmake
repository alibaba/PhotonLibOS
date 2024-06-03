function(__GenerateStandaloneCTestScript DIR FILEPATH)
    get_property(TGTS DIRECTORY "${DIR}" PROPERTY TESTS)
    foreach(TGT IN LISTS TGTS)
        file(APPEND ${FILEPATH} "add_test(${TGT} \"./${TGT}\")\n")
    endforeach()

    get_property(SUBDIRS DIRECTORY "${DIR}" PROPERTY SUBDIRECTORIES)
    foreach(SUBDIR IN LISTS SUBDIRS)
        __GenerateStandaloneCTestScript("${SUBDIR}" ${FILEPATH})
    endforeach()
endfunction()


function(GenerateStandaloneCTestScript DIR FILEPATH)
    file(WRITE ${FILEPATH} "")
    __GenerateStandaloneCTestScript(${DIR} ${FILEPATH})
endfunction()
