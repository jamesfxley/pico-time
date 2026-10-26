# The README's and the usage guide's examples, compiled from the documents in
# the cross tree (EN-03.5): they call the firmware-only entry points and log
# through pico-logging, which only the firmware configure fetches. Each harness
# in docs/ supplies only what its example leaves out, and the example is read
# out of the document at configure time, so an API change that breaks one fails
# this build. They are compiled and never linked, so they take time_sync's
# headers rather than the library: linking it would compile lwIP and the SDK's
# own sources into this target under this repository's strict flags.
house_cxx_add_doc_examples(pico_time_firmware_doc_examples PACKAGE pico_time
    GROUP firmware GROUPS firmware
    DOCUMENTS ${PROJECT_SOURCE_DIR}/README.md
              ${PROJECT_SOURCE_DIR}/docs/usage-guide.md
    HARNESS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/docs
    LIBRARIES pico_time::ntp pico_logging::core)
target_include_directories(pico_time_firmware_doc_examples PRIVATE
    ${PROJECT_SOURCE_DIR}/src/time_sync/include)
