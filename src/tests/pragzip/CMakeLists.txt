add_executable(testGzipIndexFormat)
target_sources(testGzipIndexFormat PRIVATE ${CMAKE_CURRENT_LIST_DIR}/testGzipIndexFormat.cpp)
target_link_libraries(testGzipIndexFormat PRIVATE libpragzip)
add_test(NAME Test-GzipIndexFormat COMMAND testGzipIndexFormat)
add_dependencies(all_tests testGzipIndexFormat)

add_executable(testHuffmanCoding)
target_sources(testHuffmanCoding PRIVATE ${CMAKE_CURRENT_LIST_DIR}/testHuffmanCoding.cpp)
target_link_libraries(testHuffmanCoding PRIVATE libhuffman)
add_test(NAME Test-HuffmanCoding COMMAND testHuffmanCoding)
add_dependencies(all_tests testHuffmanCoding)

add_executable(testDeflate)
target_sources(testDeflate PRIVATE ${CMAKE_CURRENT_LIST_DIR}/testDeflate.cpp)
target_link_libraries(testDeflate PRIVATE zlibstatic libpragzip)
add_test(NAME Test-Deflate COMMAND testDeflate)
add_dependencies(all_tests testDeflate)

add_executable(testGzip)
target_sources(testGzip PRIVATE ${CMAKE_CURRENT_LIST_DIR}/testGzip.cpp)
target_link_libraries(testGzip PRIVATE zlibstatic libpragzip)
add_test(NAME Test-Gzip COMMAND testGzip)
add_dependencies(all_tests testGzip)

add_executable(testParallelGzipReader)
target_sources(testParallelGzipReader PRIVATE ${CMAKE_CURRENT_LIST_DIR}/testParallelGzipReader.cpp)
target_link_libraries(testParallelGzipReader PRIVATE libpragzip)
add_test(NAME Test-Parallel-Gzip-Reader COMMAND testParallelGzipReader)
add_dependencies(all_tests testParallelGzipReader)

add_executable(testGzipBlockFetcher)
target_sources(testGzipBlockFetcher PRIVATE ${CMAKE_CURRENT_LIST_DIR}/testGzipBlockFetcher.cpp)
target_link_libraries(testGzipBlockFetcher PRIVATE libpragzip)
add_test(NAME Test-Gzip-Block-Fetcher COMMAND testGzipBlockFetcher)
add_dependencies(all_tests testGzipBlockFetcher)

add_executable(testGzipBlockFinder)
target_sources(testGzipBlockFinder PRIVATE ${CMAKE_CURRENT_LIST_DIR}/testGzipBlockFinder.cpp)
target_link_libraries(testGzipBlockFinder PRIVATE libpragzip)
add_test(NAME Test-Gzip-Block-Finder COMMAND testGzipBlockFinder)
add_dependencies(all_tests testGzipBlockFinder)

add_executable(testPrecodeCheck)
target_sources(testPrecodeCheck PRIVATE ${CMAKE_CURRENT_LIST_DIR}/testPrecodeCheck.cpp)
target_link_libraries(testPrecodeCheck PRIVATE libpragzip)
add_test(NAME Test-Precode-Check COMMAND testPrecodeCheck)
add_dependencies(all_tests testPrecodeCheck)
