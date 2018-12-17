mkdir -p build

cd build && rm -rf *

cmake -G 'Unix Makefiles' -DCMAKE_BUILD_TYPE=Debug ..

make

echo "Build finish."
