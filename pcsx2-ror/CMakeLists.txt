# pcsx2-ror.dll, built as a subproject of PCSX2's own CMake (PCSX2_EXTRA_PROJECT_DIR, tools/pcsx2-patches/0003) so
# it compiles with exactly PCSX2's flags and links its core (PCSX2 + PCSX2_FLAGS) the way pcsx2-gsrunner does.
add_library(pcsx2-ror SHARED
	ror_pcsx2.cpp
	gs.cpp
	vif1.cpp
	host.cpp
	ror_internal.h
	include/ror_pcsx2.h
)

target_compile_definitions(pcsx2-ror PRIVATE ROR_PCSX2_EXPORTS)

target_include_directories(pcsx2-ror
	PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include"
	PRIVATE "${CMAKE_BINARY_DIR}/common/include" "${CMAKE_SOURCE_DIR}" "${CMAKE_SOURCE_DIR}/pcsx2"
)

target_link_libraries(pcsx2-ror PRIVATE
	PCSX2_FLAGS
	PCSX2
)

if(MSVC)
	# A PDB for every configuration, so crashes inside the DLL can be debugged from the runner.
	target_compile_options(pcsx2-ror PRIVATE /Zi)
	target_link_options(pcsx2-ror PRIVATE /DEBUG /OPT:REF /OPT:ICF)
endif()

# ror-gsreplay: GS dumps replayed through the DLL's C API (the regression replays, tools/port/gs_regress.py). A client
# of include/ror_pcsx2.h only, like the runner: it links the import library, nothing of PCSX2's core.
add_executable(ror-gsreplay gsreplay.cpp)
target_link_libraries(ror-gsreplay PRIVATE pcsx2-ror)
if(MSVC)
	target_compile_options(ror-gsreplay PRIVATE /Zi)
	target_link_options(ror-gsreplay PRIVATE /DEBUG)
endif()
