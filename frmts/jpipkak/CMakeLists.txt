add_gdal_driver(TARGET gdal_JPIPKAK
                SOURCES jpipkak_headers.h
                        jpipkakdataset.h
                        jpipkakdataset.cpp
                CORE_SOURCES jpipkakdrivercore.cpp
                PLUGIN_CAPABLE
                NO_SHARED_SYMBOL_WITH_CORE
  NO_CXX_WFLAGS_EFFCXX
  NO_WFLAG_OLD_STYLE_CAST
)

if(NOT TARGET gdal_JPIPKAK)
    return()
endif()

gdal_standard_includes(gdal_JPIPKAK)
gdal_target_link_libraries(gdal_JPIPKAK PRIVATE KDU::KDU)
