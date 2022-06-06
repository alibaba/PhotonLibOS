find_path(KRB5_INCLUDE_DIR krb5.h)

find_library(KRB5_LIBRARIES NAMES krb5 k5crypto com_err)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(krb5 DEFAULT_MSG KRB5_LIBRARIES KRB5_INCLUDE_DIR)

mark_as_advanced(KRB5_INCLUDE_DIR KRB5_LIBRARIES)