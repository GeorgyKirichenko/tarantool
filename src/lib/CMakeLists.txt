add_subdirectory(bit)
add_subdirectory(bitset)
set(SMALL_EMBEDDED ON)
add_subdirectory(small)
add_subdirectory(salad)
add_subdirectory(csv)
add_subdirectory(json)
if(ENABLE_BUNDLED_MSGPUCK)
    add_subdirectory(msgpuck EXCLUDE_FROM_ALL)
endif()
