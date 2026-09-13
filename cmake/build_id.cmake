# Writes the build id header, and is re-run on every build.
#
# __DATE__ and __TIME__ are the time the translation unit was compiled, which is
# not the time the build was made: an incremental build that does not touch
# main.c leaves the banner reading whenever main.c last changed. Two different
# builds went out this way stamped with the same time, and a log that was in
# fact from the older of the two could not be told apart from one that was not.
#
# The git description is the part that actually identifies the code. The clock
# is only there to separate two builds of the same commit.
execute_process(COMMAND git -C "${SRC}" describe --always --dirty --abbrev=8
                OUTPUT_VARIABLE ID OUTPUT_STRIP_TRAILING_WHITESPACE
                ERROR_QUIET RESULT_VARIABLE FAILED)
if(FAILED OR ID STREQUAL "")
  set(ID "no-git")
endif()
string(TIMESTAMP WHEN "%Y-%m-%d %H:%M:%S" UTC)
file(WRITE "${OUT}" "#define LOADER_BUILD_ID \"${ID} ${WHEN} UTC\"\n")
