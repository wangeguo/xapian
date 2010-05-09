SET(prefix "/home/olly/install")
SET(exec_prefix "${prefix}")
SET(XAPIAN_LIBRARIES "${exec_prefix}/lib/libxapian.so" CACHE FILEPATH "Libraries for Xapian")
SET(XAPIAN_INCLUDE_DIR "${prefix}/include" CACHE PATH "Include path for Xapian")
SET(XAPIAN_FOUND "TRUE")