#include <iostream>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>

#include <common.hpp>
#include <IndexFileFormat.hpp>
#include <StandardFileReader.hpp>


[[nodiscard]] TemporaryDirectory
createTemporaryDirectory()
{
    const std::filesystem::path tmpFolderName = "pragzip.testGzipIndexFormat." + std::to_string( unixTime() );
    std::filesystem::create_directory( tmpFolderName );
    return tmpFolderName;
}


int
main( int    argc,
      char** argv )
{
    if ( argc == 0 ) {
        std::cerr << "Expected at least the launch command as the first argument!\n";
        return 1;
    }

    const std::string binaryFilePath( argv[0] );
    std::string binaryFolder = std::filesystem::path( binaryFilePath ).parent_path();
    if ( binaryFolder.empty() ) {
        binaryFolder = ".";
    }
    const auto rootFolder = findParentFolderContaining( binaryFolder, "tests/data/base64-256KiB.gz.index" );

    const auto index = readGzipIndex(
        std::make_unique<StandardFileReader>( rootFolder + "/tests/data/base64-256KiB.gz.index" ) );

    REQUIRE( index.compressedSizeInBytes == fileSize( rootFolder + "/tests/data/base64-256KiB.gz" ) );
    REQUIRE( index.uncompressedSizeInBytes == fileSize( rootFolder + "/tests/data/base64-256KiB" ) );

    REQUIRE( index.checkpointSpacing == 64 * 1024 );
    REQUIRE( index.checkpoints.size() == 5 );

    try
    {
        const auto tmpFolder = createTemporaryDirectory();
        const auto gzipIndexPath = tmpFolder.path() / "gzipindex";
        writeGzipIndex( index, gzipIndexPath );
        const auto rereadIndex = readGzipIndex( std::make_unique<StandardFileReader>( gzipIndexPath ) );
        REQUIRE( rereadIndex == index );
    }
    catch ( const std::exception& exception )
    {
        /* Note that the destructor for TemporaryDirectory might not be called for uncaught exceptions!
         * @see https://stackoverflow.com/questions/222175/why-destructor-is-not-called-on-exception */
        std::cerr << "Caught exception: " << exception.what() << "\n";
        gnTestErrors += 1;
    }

    std::cout << "Tests successful: " << ( gnTests - gnTestErrors ) << " / " << gnTests << "\n";

    return gnTestErrors;
}