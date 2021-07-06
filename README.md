# indexed_bzip2

[![PyPI version](https://badge.fury.io/py/indexed-bzip2.svg)](https://badge.fury.io/py/indexed-bzip2)
[![Downloads](https://pepy.tech/badge/indexed-bzip2/month)](https://pepy.tech/project/indexed-bzip2)
[![License](https://img.shields.io/badge/license-MIT-blue.svg)](http://opensource.org/licenses/MIT)
[![Build Status](https://travis-ci.org/mxmlnkn/indexed_bzip2.svg?branch=master)](https://travis-ci.com/mxmlnkn/indexed_bzip2)
[![codecov](https://codecov.io/gh/mxmlnkn/indexed_bzip2/branch/master/graph/badge.svg?token=94ZD4UTZQW)](https://codecov.io/gh/mxmlnkn/indexed_bzip2)
![C++17](https://img.shields.io/badge/C++-17-blue.svg?style=flat-square)


This module provides an IndexedBzip2File class, which can be used to seek inside bzip2 files without having to decompress them first.
It's based on an improved version of the bzip2 decoder [bzcat](https://github.com/landley/toybox/blob/c77b66455762f42bb824c1aa8cc60e7f4d44bdab/toys/other/bzcat.c) from [toybox](https://landley.net/code/toybox/), which was refactored and extended to be able to export and import bzip2 block offsets and seek to them and to add support for threaded parallel decoding of blocks.

Seeking inside a block is only emulated, so IndexedBzip2File will only speed up seeking when there are more than one block, which should almost always be the cause for archives larger than 1 MB.

Since version 1.2.0, parallel decoding of blocks is supported!
Per default, the older serial implementation is used.
To use the parallel implementation you need to specify a `parallelization` other than 1 argument to `IndexedBzip2File`.


# Installation

You can simply install it from PyPI:
```
pip install indexed_bzip2
```

# Usage

## Example 1

```python3
from indexed_bzip2 import IndexedBzip2File

file = IndexedBzip2File( "example.bz2", parallelization = os.cpu_count() )

# You can now use it like a normal file
file.seek( 123 )
data = file.read( 100 )
```

The first call to seek will ensure that the block offset list is complete and therefore might create them first.
Because of this the first call to seek might take a while.

## Example 2

The creation of the list of bzip2 blocks can take a while because it has to decode the bzip2 file completely.
To avoid this setup when opening a bzip2 file, the block offset list can be exported and imported.

```python3
from indexed_bzip2 import IndexedBzip2File
import pickle

# Calculate and save bzip2 block offsets
file = IndexedBzip2File( "example.bz2", parallelization = os.cpu_count() )
block_offsets = file.block_offsets() # can take a while
# block_offsets is a simple dictionary where the keys are the bzip2 block offsets in bits(!)
# and the values are the corresponding offsets in the decoded data in bytes. E.g.:
# block_offsets = {32: 0, 14920: 4796}
with open( "offsets.dat", 'wb' ) as offsets_file:
    pickle.dump( block_offsets, offsets_file )
file.close()

# Load bzip2 block offsets for fast seeking
with open( "offsets.dat", 'rb' ) as offsets_file:
    block_offsets = pickle.load( offsets_file )
file2 = IndexedBzip2File( "example.bz2", parallelization = os.cpu_count() )
file2.set_block_offsets( block_offsets ) # should be fast
file2.seek( 123 )
data = file2.read( 100 )
file2.close()
```


# Tracing the decoder

Performance profiling and tracing is done with [Score-P](https://www.vi-hps.org/projects/score-p/) for instrumentation and [Vampir](https://vampir.eu/) for visualization.
This is one way, you could install Score-P with most of the functionalities on Debian 10.

```bash
# Install PAPI
wget http://icl.utk.edu/projects/papi/downloads/papi-5.7.0.tar.gz
tar -xf papi-5.7.0.tar.gz
cd papi-5.7.0/src
./configure
make -j
sudo make install

# Install Dependencies
sudo apt-get install libopenmpi-dev openmpi gcc-8-plugin-dev llvm-dev libclang-dev libunwind-dev libopen-trace-format-dev otf-trace

# Install Score-P (to /opt/scorep)
wget https://www.vi-hps.org/cms/upload/packages/scorep/scorep-6.0.tar.gz
tar -xf scorep-6.0.tar.gz
cd scorep-6.0
./configure --with-mpi=openmpi --enable-shared
make -j
make install

# Add /opt/scorep to your path variables on shell start
cat <<EOF >> ~/.bashrc
if test -d /opt/scorep; then
    export SCOREP_ROOT=/opt/scorep
    export PATH=$SCOREP_ROOT/bin:$PATH
    export LD_LIBRARY_PATH=$SCOREP_ROOT/lib:$LD_LIBRARY_PATH
fi
EOF

# Check whether it works
scorep --version
scorep-info config-summary

# Actually do the tracing
cd tools
bash trace.sh
```
